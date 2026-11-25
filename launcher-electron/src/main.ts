import { app, BrowserWindow, dialog, ipcMain } from 'electron';
import { readFileSync } from 'node:fs';
import path from 'node:path';
import { parseInspection, type Inspection } from './inspection';
import { openWorker, WorkerRequestError, type WorkerClient } from './worker-client';
import { parsePlayResult, type PlayResult } from './worker-protocol';
import { defaultRoots, findDefaultRoot } from './discovery';
import { InstallationRuntimeError, runtimePaths } from './runtime-paths';
import { createDevelopmentLog, type DevelopmentLog } from './dev-log';

const startedAt = new Date();
let build = 'unknown';
try { build = readFileSync(path.join(__dirname, 'build-id.txt'), 'utf8').trim(); } catch {}
app.whenReady().then(() => {
  const window = new BrowserWindow({ width: 1040, height: 720, minWidth: 720, minHeight: 500, webPreferences: { preload: path.join(__dirname, 'preload.js'), contextIsolation: true, sandbox: true, nodeIntegration: false } });
  let worker: WorkerClient | null = null;
  let state: Inspection | null = null;
  let playing = false;
  let hidden = false;
  let shown = window.isVisible();
  let log: DevelopmentLog | null = null;
  let logRoot: string | null = null;
  window.once('show', () => {
    if (!shown) log?.write('window', 'shown');
    shown = true;
  });
  function logSelection(root: string, origin: 'default' | 'manual'): boolean {
    if (logRoot !== root) {
      log = null;
      logRoot = root;
    }
    if (!log && findDefaultRoot([root])) {
      log = createDevelopmentLog(root, app.getVersion(), process.platform, build, startedAt);
      if (shown) log?.write('window', 'shown');
    }
    const name = path.basename(root).toLowerCase();
    const brand = name === 'nx2saria' ? 'NX2Saria' : name === 'nx2navi' ? 'NX2navi' : 'other';
    log?.write('root', `picked origin=${origin} brand=${brand}`);
    return Boolean(log);
  }
  function restoreWindow() {
    if (!window.isDestroyed() && hidden) {
      window.show();
      window.focus();
      log?.write('window', window.isVisible() ? 'restored' : 'restore_failed');
    }
    hidden = false;
  }
  async function answerMemory(active: WorkerClient, result: Extract<PlayResult, { status: 'memory' }>): Promise<PlayResult> {
    const memory = result.memory;
    const estimate = memory.unbounded ? 'ilimitada' : `${memory.need} MiB`;
    let answer: { accept: boolean } | { dialog_error: true };
    try {
      const response = await dialog.showMessageBox(window, { type: 'warning', buttons: ['Cancelar', 'Jogar'], defaultId: 1, cancelId: 0,
        message: 'A memória disponível pode ser insuficiente.', detail: `Disponível: ${Math.trunc(memory.remaining)} MiB de memória comprometida (RAM e arquivo de paginação). Estimativa do jogo: ${estimate}. Feche programas que usam muita memória e ative ou aumente o arquivo de paginação do Windows. Para 16 GB de RAM, use 32 GB fixos (32768 MB) e reinicie o Windows. Para não mostrar este aviso novamente, desative a opção de memória no launcher instalado ou ajuste mem_need_mb = 0 em nx2saria.ini.` });
      answer = { accept: response.response === 1 };
    } catch { answer = { dialog_error: true }; }
    return parsePlayResult(await active.request('memory_response', { attempt: result.attempt, ...answer }));
  }
  async function selectRoot(root: string, origin: 'default' | 'manual'): Promise<Inspection> {
    const previous = worker;
    worker = null;
    if (previous) await previous.close();
    const selectionLogged = logSelection(root, origin);
    function inspected(result: Inspection, outcome: string): Inspection {
      if (!selectionLogged) logSelection(root, origin);
      log?.write('inspection', outcome);
      return result;
    }
    let current: WorkerClient | null = null;
    try {
      const { source, python, installed } = runtimePaths(app.isPackaged, process.resourcesPath, __dirname, process.env, process.platform, root);
      current = await openWorker(root, python, source, installed);
      if (!current.handshake.capabilities.includes('inspect') || !current.handshake.capabilities.includes('play')) throw new Error('Unsupported worker');
      const active = current;
      worker = active;
      active.on('disconnect', () => {
        if (worker !== active) return;
        worker = null;
        playing = false;
        restoreWindow();
        log?.write('play', 'worker_disconnected');
        if (!window.isDestroyed()) window.webContents.send('play-ended', { error: 'worker_disconnected' });
      });
      active.on('event', async frame => {
        if (worker !== active) return;
        if (frame.event === 'job_started') {
          if (!window.isDestroyed()) window.webContents.send('job-status', { status: 'running' });
        } else if (frame.event === 'job_progress') {
          if (!window.isDestroyed()) window.webContents.send('job-status', { status: 'running', fraction: frame.fraction });
        } else if (frame.event === 'job_ended') {
          log?.write('play', `job_ended outcome=${frame.outcome}`);
          if (!window.isDestroyed()) window.webContents.send('job-status', { status: frame.outcome === 'resume' ? 'resuming' : frame.outcome });
          if (frame.outcome !== 'resume') {
            playing = false;
            try { state = parseInspection(await active.request('inspect')); } catch { state = null; }
            if (!window.isDestroyed()) window.webContents.send('play-ended', { state, error: frame.outcome === 'failed' ? 'job_failed' : 'job_cancelled' });
          }
        } else if (frame.event === 'memory_required') {
          try {
            const decision = parsePlayResult(frame.decision);
            if (decision.status !== 'memory') throw new Error('Invalid decision');
            const result = await answerMemory(active, decision);
            log?.write('play', `resumed result=${result.status}`);
            if (result.status !== 'started') {
              playing = false;
              if (!window.isDestroyed()) window.webContents.send('play-ended', { error: result.status });
            }
          } catch {
            playing = false;
            log?.write('play', 'resumed result=worker_failed');
            if (!window.isDestroyed()) window.webContents.send('play-ended', { error: 'worker_failed' });
          }
        } else if (frame.event === 'play_failed') {
          playing = false;
          log?.write('play', 'resumed result=play_failed');
          if (!window.isDestroyed()) window.webContents.send('play-ended', { error: 'play_failed' });
        } else if (frame.event === 'game_started') {
          if (state?.hide_while_playing && !window.isDestroyed() && window.isVisible() && !window.isMinimized()) {
            window.hide();
            hidden = true;
            log?.write('window', window.isVisible() ? 'hide_failed' : 'hidden_for_game');
          }
        } else if (frame.event === 'game_ended') {
          restoreWindow();
          playing = false;
          log?.write('play', `game_ended code=${frame.code ?? 'unknown'} alarm=${Boolean(frame.alarm)}`);
          try { state = parseInspection(await active.request('inspect')); } catch { state = null; }
          if (!window.isDestroyed()) window.webContents.send('play-ended', { state, code: frame.code, error: frame.alarm ? 'game_failed' : undefined, update_waiting: frame.update_waiting });
          if (frame.update_waiting && !window.isDestroyed()) void dialog.showMessageBox(window, { type: 'info', message: 'Há uma atualização esperando. Abra o launcher instalado para aplicá-la.' });
        }
      });
      state = parseInspection(await active.request('inspect'));
      if (active.handshake.lock === 'unavailable' && !window.isDestroyed()) void dialog.showMessageBox(window, { type: 'warning', message: 'Não foi possível bloquear a instalação. Evite abrir outro launcher ao mesmo tempo.' });
      if (active.handshake.update_waiting && !window.isDestroyed()) void dialog.showMessageBox(window, { type: 'info', message: 'Há uma atualização esperando. Abra o launcher instalado para aplicá-la.' });
      return inspected(state, `result=${state.status}`);
    } catch (error) {
      if (current) await current.close();
      if (worker === current) worker = null;
      if (error instanceof InstallationRuntimeError || (error instanceof WorkerRequestError && error.code === 'runtime_incompatible')) {
        return inspected({ status: 'inspection_error', reason: error.code }, `result=${error.code}`);
      }
      const reason = error instanceof WorkerRequestError && error.code === 'busy' ? 'worker_busy' : 'worker_failed';
      if (reason === 'worker_busy') log?.write('window', 'second_instance_refused');
      return inspected({ status: 'inspection_error', reason }, `result=${reason}`);
    }
  }
  ipcMain.handle('discover-installation', async event => {
    if (event.sender !== window.webContents || playing) throw new Error('Unavailable');
    const root = findDefaultRoot(defaultRoots(process.platform, app.getPath('home'), process.env));
    return root ? selectRoot(root, 'default') : null;
  });
  ipcMain.handle('select-installation', async event => {
    if (event.sender !== window.webContents || playing) throw new Error('Unavailable');
    const previous = worker;
    worker = null;
    state = null;
    if (previous) await previous.close();
    const chosen = await dialog.showOpenDialog(window, { properties: ['openDirectory'], title: 'Select NX2Saria launcher folder' });
    if (!chosen.canceled && chosen.filePaths[0]) return selectRoot(chosen.filePaths[0], 'manual');
    log?.write('inspection', 'selection_cancelled');
    return null;
  });
  ipcMain.handle('play', async event => {
    if (event.sender !== window.webContents) return { error: 'unavailable' };
    log?.write('play', 'requested');
    if (!worker || playing) {
      log?.write('play', 'result=unavailable');
      return { error: 'unavailable' };
    }
    const active = worker;
    playing = true;
    try {
      state = parseInspection(await active.request('inspect'));
      if (state.status !== 'ready' || state.job_marker_present || state.game_running) throw new Error('Not ready');
      let result = parsePlayResult(await active.request('play'));
      if (result.status === 'memory') result = await answerMemory(active, result);
      log?.write('play', `result=${result.status}`);
      if (result.status === 'building') return { started: true, building: true };
      if (result.status !== 'started') { playing = false; return { error: result.status }; }
      return { started: true };
    } catch { playing = false; log?.write('play', 'result=unavailable'); return { error: 'unavailable' }; }
  });
  ipcMain.handle('cancel-job', async event => {
    if (event.sender !== window.webContents || !worker || !playing) return { error: 'unavailable' };
    try { await worker.request('cancel_job'); return { cancelling: true }; }
    catch { return { error: 'unavailable' }; }
  });
  window.on('closed', () => {
    log?.write('window', 'closed');
    if (worker) void worker.close();
  });
  app.once('quit', (_, code) => log?.write('exit', `code=${code} play_active=${playing}`));
  window.loadFile(path.join(__dirname, 'index.html'));
});
app.on('window-all-closed', () => app.quit());
