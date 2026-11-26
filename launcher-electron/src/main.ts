import { app, BrowserWindow, dialog, ipcMain } from 'electron';
import { readFileSync } from 'node:fs';
import path from 'node:path';
import { parseInspection, type Inspection } from './inspection';
import { openWorker, WorkerRequestError, type WorkerClient } from './worker-client';
import { parsePlayResult, type PlayResult } from './worker-protocol';
import { defaultRoots, findDefaultRoot } from './discovery';
import { InstallationRuntimeError, runtimePaths } from './runtime-paths';
import { createDevelopmentLog, type DevelopmentLog } from './dev-log';

const buildActions = ['install', 'rebuild'] as const;
type BuildAction = typeof buildActions[number];

const startedAt = new Date();
let build = 'unknown';
try { build = readFileSync(path.join(__dirname, 'build-id.txt'), 'utf8').trim(); } catch {}
app.whenReady().then(() => {
  const window = new BrowserWindow({ width: 1040, height: 720, minWidth: 720, minHeight: 500, webPreferences: { preload: path.join(__dirname, 'preload.js'), contextIsolation: true, sandbox: true, nodeIntegration: false } });
  let worker: WorkerClient | null = null;
  let state: Inspection | null = null;
  let playing = false;
  let buildAction: BuildAction | null = null;
  let gameSpawned = false;
  let hidden = false;
  let shown = window.isVisible();
  let log: DevelopmentLog | null = null;
  let logRoot: string | null = null;
  let messageSequence = 0;
  let pendingPlayMessage: number | null = null;
  window.once('show', () => {
    if (!shown) log?.write('window', 'shown');
    shown = true;
  });
  function attachLog(root: string) {
    if (logRoot !== root) {
      log = null;
      logRoot = root;
    }
    if (!log && findDefaultRoot([root])) {
      log = createDevelopmentLog(root, process.platform, build, startedAt);
      if (shown) log?.write('window', 'shown');
    }
  }
  function restoreWindow() {
    if (!window.isDestroyed() && hidden) {
      window.show();
      window.focus();
      log?.write('window', window.isVisible() ? 'restored' : 'restore_failed');
    }
    hidden = false;
  }
  function expectPlayMessage(): number {
    pendingPlayMessage = ++messageSequence;
    return pendingPlayMessage;
  }
  function notifyPlayEnded(result: object) {
    if (!window.isDestroyed()) window.webContents.send('play-ended', { ...result, messageId: expectPlayMessage() });
  }
  async function finishBuild(active: WorkerClient, outcome: string) {
    const action = buildAction;
    if (!action) return;
    if (worker === active) {
      let refreshed: Inspection | null = null;
      try { refreshed = parseInspection(await active.request('inspect')); } catch {}
      if (worker !== active) return;
      state = refreshed;
    }
    log?.write(action, `ended outcome=${outcome}`);
    buildAction = null;
    playing = false;
    if (!window.isDestroyed()) window.webContents.send('build-ended', { action, outcome, state: settingsView(state) });
  }
  function firstRunAvailable(selected: Inspection | null) {
    return selected?.status === 'not_configured' || selected?.status === 'not_ready';
  }
  function settingsView(result: Inspection | null): Inspection | null {
    return result && { ...result, hide_while_playing: worker?.handshake.lock === 'held' && !result.job_marker_present ? result.hide_while_playing : null };
  }
  function playRefused(error: string) {
    return { error, messageId: expectPlayMessage() };
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
    pendingPlayMessage = null;
    if (previous) await previous.close();
    attachLog(root);
    let current: WorkerClient | null = null;
    function inspected(result: Inspection, outcome: string): Inspection {
      attachLog(root);
      const name = path.basename(root).toLowerCase();
      const brand = name === 'nx2saria' ? 'NX2Saria' : name === 'nx2navi' ? 'NX2navi' : 'other';
      const version = current?.handshake.version ?? '';
      const installedVersion = /^v?\d+(?:\.\d+)+(?:-[0-9A-Za-z.-]+)?(?:\+[0-9A-Za-z.-]+)?$/.test(version) && version.length <= 40
        ? version : 'unknown';
      log?.write('root', `picked origin=${origin} brand=${brand} installed_version=${installedVersion}`);
      log?.write('inspection', outcome);
      return result;
    }
    try {
      const { source, python, installed } = runtimePaths(app.isPackaged, process.resourcesPath, __dirname, process.env, process.platform, root);
      current = await openWorker(root, python, source, installed);
      if (!current.handshake.capabilities.includes('inspect') || !current.handshake.capabilities.includes('play')) throw new Error('Unsupported worker');
      const active = current;
      worker = active;
      active.on('disconnect', () => {
        if (worker !== active) return;
        worker = null;
        if (buildAction) {
          state = null;
          void finishBuild(active, 'worker_disconnected');
          return;
        }
        playing = false;
        restoreWindow();
        log?.write('play', 'worker_disconnected');
        notifyPlayEnded({ error: 'worker_disconnected', outcome: gameSpawned ? 'monitor_lost' : undefined });
        gameSpawned = false;
      });
      active.on('event', async frame => {
        if (worker !== active) return;
        if (frame.event === 'job_started') {
          if (!window.isDestroyed()) window.webContents.send('job-status', { status: 'running', task: buildAction ?? 'mods' });
        } else if (frame.event === 'job_progress') {
          if (!window.isDestroyed()) window.webContents.send('job-status', { status: 'running', task: buildAction ?? 'mods', fraction: frame.fraction });
        } else if (frame.event === 'job_ended') {
          if (buildAction) {
            await finishBuild(active, frame.outcome);
            return;
          }
          log?.write('play', `job_ended outcome=${frame.outcome}`);
          if (!window.isDestroyed()) window.webContents.send('job-status', { status: frame.outcome === 'resume' ? 'resuming' : frame.outcome });
          if (frame.outcome !== 'resume') {
            playing = false;
            try { state = parseInspection(await active.request('inspect')); } catch { state = null; }
            notifyPlayEnded({ state: settingsView(state), error: frame.outcome === 'failed' ? 'job_failed' : 'job_cancelled' });
          }
        } else if (frame.event === 'memory_required') {
          try {
            const decision = parsePlayResult(frame.decision);
            if (decision.status !== 'memory') throw new Error('Invalid decision');
            const result = await answerMemory(active, decision);
            log?.write('play', `resumed result=${result.status}`);
            if (result.status !== 'started') {
              playing = false;
              notifyPlayEnded({ error: result.status });
            }
          } catch {
            playing = false;
            log?.write('play', 'resumed result=worker_failed');
            notifyPlayEnded({ error: 'worker_failed' });
          }
        } else if (frame.event === 'play_failed') {
          playing = false;
          log?.write('play', 'resumed result=play_failed');
          notifyPlayEnded({ error: 'play_failed' });
        } else if (frame.event === 'game_started') {
          gameSpawned = true;
        } else if (frame.event === 'game_window_ready' && !window.isDestroyed()) {
          log?.write('play', 'window_ready');
          window.webContents.send('game-window-ready');
          if (state?.hide_while_playing && window.isVisible() && !window.isMinimized()) {
            window.hide();
            hidden = true;
            log?.write('window', window.isVisible() ? 'hide_failed' : 'hidden_for_game');
          }
        } else if (frame.event === 'game_ended') {
          restoreWindow();
          playing = false;
          gameSpawned = false;
          log?.write('play', `game_ended code=${frame.code ?? 'unknown'} alarm=${Boolean(frame.alarm)}`);
          try { state = parseInspection(await active.request('inspect')); } catch { state = null; }
          notifyPlayEnded({ state: settingsView(state), code: frame.code, alarm: frame.alarm, message: frame.message, outcome: 'game_ended' });
          if (frame.update_waiting && !window.isDestroyed()) void dialog.showMessageBox(window, { type: 'info', message: 'Há uma atualização esperando. Abra o launcher instalado para aplicá-la.' });
        }
      });
      state = parseInspection(await active.request('inspect'));
      if (active.handshake.lock === 'unavailable' && !window.isDestroyed()) void dialog.showMessageBox(window, { type: 'warning', message: 'Não foi possível bloquear a instalação. Evite abrir outro launcher ao mesmo tempo.' });
      if (active.handshake.update_waiting && !window.isDestroyed()) void dialog.showMessageBox(window, { type: 'info', message: 'Há uma atualização esperando. Abra o launcher instalado para aplicá-la.' });
      return inspected(settingsView(state)!, `result=${state.status}`);
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
  ipcMain.handle('install-destination', async (event, pick) => {
    if (event.sender !== window.webContents || !worker || playing || typeof pick !== 'boolean' ||
        !firstRunAvailable(state)) throw new Error('Unavailable');
    const active = worker;
    let options = {};
    if (pick) {
      const chosen = await dialog.showOpenDialog(window, { properties: ['openDirectory'], title: 'Escolher pasta do jogo' });
      if (chosen.canceled || !chosen.filePaths[0]) return null;
      options = { folder: chosen.filePaths[0] };
    }
    if (worker !== active || playing) throw new Error('Unavailable');
    const result = await active.request('install_destination', options);
    if (worker !== active || typeof result?.folder !== 'string' || !path.isAbsolute(result.folder) ||
        !['default', 'saved', 'selected'].includes(result.source) ||
        !Number.isSafeInteger(result.available_gb) || result.available_gb < 0 ||
        !Number.isSafeInteger(result.required_gb) || result.required_gb <= 0 ||
        typeof result.complete_image !== 'boolean' || typeof result.path_too_long !== 'boolean') {
      throw new Error('Unavailable');
    }
    return { folder: result.folder, available: result.available_gb, required: result.required_gb,
      completeImage: result.complete_image, pathTooLong: result.path_too_long };
  });
  ipcMain.handle('install-dump', async (event, pick) => {
    if (event.sender !== window.webContents || !worker || playing || typeof pick !== 'boolean' ||
        !firstRunAvailable(state)) throw new Error('Unavailable');
    const active = worker;
    let options = {};
    if (pick) {
      const chosen = await dialog.showOpenDialog(window, {
        properties: ['openFile'], title: 'Escolher dump do jogo',
        filters: [{ name: 'Dump do jogo', extensions: ['nup'] }, { name: 'Todos os arquivos', extensions: ['*'] }],
      });
      if (chosen.canceled || !chosen.filePaths[0]) return null;
      options = { file: chosen.filePaths[0] };
    }
    if (worker !== active || playing) throw new Error('Unavailable');
    const result = await active.request('install_dump', options);
    if (worker !== active || (result?.file !== null && (typeof result?.file !== 'string' ||
        !path.isAbsolute(result.file)))) throw new Error('Unavailable');
    return result.file;
  });
  ipcMain.handle('search-install-dump', async event => {
    if (event.sender !== window.webContents || !worker || playing || !firstRunAvailable(state)) throw new Error('Unavailable');
    const active = worker;
    const result = await active.request('search_install_dump');
    if (worker !== active || !['searching', 'complete'].includes(result?.status) ||
        (result.file !== null && (typeof result.file !== 'string' || !path.isAbsolute(result.file)))) {
      throw new Error('Unavailable');
    }
    return { status: result.status, file: result.file };
  });
  for (const action of buildActions) ipcMain.handle(action, async event => {
    if (event.sender !== window.webContents) return 'unavailable';
    log?.write(action, 'requested');
    const eligible = action === 'install' ? firstRunAvailable(state) : state?.status === 'ready';
    if (!worker?.handshake.capabilities.includes(action) || playing || state?.job_marker_present || !eligible) {
      log?.write(action, 'result=unavailable');
      return 'unavailable';
    }
    const active = worker;
    playing = true;
    buildAction = action;
    let status = 'unavailable';
    try {
      const response = await active.request(action);
      if (['building', 'choose_source', 'invalid_dump', 'invalid_destination', 'path_too_long',
        'already_installed', 'not_built', 'game_running', 'need_space'].includes(response?.status)) status = response.status;
    } catch {}
    if (status !== 'building') {
      playing = false;
      buildAction = null;
    }
    log?.write(action, `result=${status}`);
    return status;
  });
  ipcMain.handle('play', async event => {
    if (event.sender !== window.webContents) return { error: 'unavailable' };
    log?.write('play', 'requested');
    if (!worker || playing) {
      log?.write('play', 'result=unavailable');
      return playRefused('unavailable');
    }
    const active = worker;
    playing = true;
    gameSpawned = false;
    try {
      state = parseInspection(await active.request('inspect'));
      if (state.status !== 'ready' || state.job_marker_present || state.game_running) throw new Error('Not ready');
      let result = parsePlayResult(await active.request('play'));
      if (result.status === 'memory') result = await answerMemory(active, result);
      log?.write('play', `result=${result.status}`);
      if (result.status === 'building') return { started: true, building: true };
      if (result.status !== 'started') { playing = false; return playRefused(result.status); }
      return { started: true };
    } catch { playing = false; log?.write('play', 'result=unavailable'); return playRefused('unavailable'); }
  });
  ipcMain.handle('cancel-job', async event => {
    if (event.sender !== window.webContents || !worker || !playing) return { error: 'unavailable' };
    try { await worker.request('cancel_job'); return { cancelling: true }; }
    catch { return { error: 'unavailable' }; }
  });
  ipcMain.handle('set-hide-while-playing', async (event, value) => {
    if (event.sender !== window.webContents || typeof value !== 'boolean' || playing || !worker ||
        worker.handshake.lock !== 'held' || state?.job_marker_present || typeof state?.hide_while_playing !== 'boolean') throw new Error('Unavailable');
    const active = worker;
    try {
      const result = await active.request('set_hide_while_playing', { value });
      if (result?.hide_while_playing !== value || worker !== active) throw new Error('Save unsuccessful');
      if (state) state.hide_while_playing = value;
      log?.write('settings', `hide_while_playing=${value ? 'on' : 'off'}`);
      return value;
    } catch {
      log?.write('settings', 'hide_while_playing=save_failed');
      throw new Error('Save unsuccessful');
    }
  });
  ipcMain.on('play-message-shown', (event, id, kind) => {
    if (event.sender !== window.webContents || pendingPlayMessage === null || id !== pendingPlayMessage ||
        !['could_not_start', 'ended_with_error', 'game_ended', 'monitor_lost'].includes(kind)) return;
    pendingPlayMessage = null;
    log?.write('window', `message_shown kind=${kind}`);
  });
  window.on('closed', () => {
    log?.write('window', 'closed');
    if (worker) void worker.close();
  });
  app.once('quit', (_, code) => log?.write('exit', `code=${code} play_active=${playing}`));
  window.loadFile(path.join(__dirname, 'index.html'));
});
app.on('window-all-closed', () => app.quit());
