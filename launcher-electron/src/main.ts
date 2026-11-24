import { app, BrowserWindow, dialog, ipcMain } from 'electron';
import path from 'node:path';
import { parseInspection, type Inspection } from './inspection';
import { openWorker, type WorkerClient } from './worker-client';
import { parsePlayResult } from './worker-protocol';
import { defaultRoots, findDefaultRoot } from './discovery';
import { runtimePaths } from './runtime-paths';

const { source, python } = runtimePaths(app.isPackaged, process.resourcesPath, __dirname, process.env, process.platform);
app.whenReady().then(() => {
  const window = new BrowserWindow({ width: 1040, height: 720, minWidth: 720, minHeight: 500, webPreferences: { preload: path.join(__dirname, 'preload.js'), contextIsolation: true, sandbox: true, nodeIntegration: false } });
  let worker: WorkerClient | null = null;
  let state: Inspection | null = null;
  let playing = false;
  let hidden = false;
  async function selectRoot(root: string) {
    const previous = worker;
    worker = null;
    if (previous) await previous.close();
    let current: WorkerClient | null = null;
    try {
      current = await openWorker(root, python, source);
      if (!current.handshake.capabilities.includes('inspect') || !current.handshake.capabilities.includes('play')) throw new Error('Unsupported worker');
      const active = current;
      worker = active;
      active.on('disconnect', () => {
        if (worker !== active) return;
        worker = null;
        playing = false;
        if (!window.isDestroyed() && hidden) { window.show(); window.focus(); }
        hidden = false;
        if (!window.isDestroyed()) window.webContents.send('play-ended', { error: 'worker_disconnected' });
      });
      active.on('event', async frame => {
        if (worker !== active) return;
        if (frame.event === 'game_started') {
          if (state?.hide_while_playing && !window.isDestroyed() && window.isVisible() && !window.isMinimized()) { window.hide(); hidden = true; }
        } else if (frame.event === 'game_ended') {
          if (!window.isDestroyed() && hidden) { window.show(); window.focus(); }
          hidden = false;
          playing = false;
          try { state = parseInspection(await active.request('inspect')); } catch { state = null; }
          if (!window.isDestroyed()) window.webContents.send('play-ended', { state, code: frame.code, error: frame.alarm ? 'game_failed' : undefined, update_waiting: frame.update_waiting });
          if (frame.update_waiting && !window.isDestroyed()) void dialog.showMessageBox(window, { type: 'info', message: 'Há uma atualização esperando. Abra o launcher instalado para aplicá-la.' });
        }
      });
      state = parseInspection(await active.request('inspect'));
      if (active.handshake.lock === 'unavailable' && !window.isDestroyed()) void dialog.showMessageBox(window, { type: 'warning', message: 'Não foi possível bloquear a instalação. Evite abrir outro launcher ao mesmo tempo.' });
      if (active.handshake.update_waiting && !window.isDestroyed()) void dialog.showMessageBox(window, { type: 'info', message: 'Há uma atualização esperando. Abra o launcher instalado para aplicá-la.' });
      return state;
    } catch {
      if (current) await current.close();
      if (worker === current) worker = null;
      return { status: 'inspection_error', reason: 'worker_failed' };
    }
  }
  ipcMain.handle('discover-installation', async event => {
    if (event.sender !== window.webContents || playing) throw new Error('Unavailable');
    const root = findDefaultRoot(defaultRoots(process.platform, app.getPath('home'), process.env));
    return root ? selectRoot(root) : null;
  });
  ipcMain.handle('select-installation', async event => {
    if (event.sender !== window.webContents || playing) throw new Error('Unavailable');
    const previous = worker;
    worker = null;
    state = null;
    if (previous) await previous.close();
    const chosen = await dialog.showOpenDialog(window, { properties: ['openDirectory'], title: 'Select NX2Saria launcher folder' });
    return chosen.canceled || !chosen.filePaths[0] ? null : selectRoot(chosen.filePaths[0]);
  });
  ipcMain.handle('play', async event => {
    if (event.sender !== window.webContents || !worker || playing) return { error: 'unavailable' };
    const active = worker;
    playing = true;
    try {
      state = parseInspection(await active.request('inspect'));
      if (state.status !== 'ready' || state.job_marker_present || state.game_running) throw new Error('Not ready');
      let result = parsePlayResult(await active.request('play'));
      if (result.status === 'memory') {
        const memory = result.memory;
        const estimate = memory.unbounded ? 'ilimitada' : `${memory.need} MiB`;
        let answer: { accept: boolean } | { dialog_error: true };
        try {
          const response = await dialog.showMessageBox(window, { type: 'warning', buttons: ['Cancelar', 'Jogar'], defaultId: 1, cancelId: 0,
            message: 'A memória disponível pode ser insuficiente.', detail: `Disponível: ${Math.trunc(memory.remaining)} MiB de memória comprometida (RAM e arquivo de paginação). Estimativa do jogo: ${estimate}. Feche programas que usam muita memória e ative ou aumente o arquivo de paginação do Windows. Para 16 GB de RAM, use 32 GB fixos (32768 MB) e reinicie o Windows. Para não mostrar este aviso novamente, desative a opção de memória no launcher instalado ou ajuste mem_need_mb = 0 em nx2saria.ini.` });
          answer = { accept: response.response === 1 };
        } catch { answer = { dialog_error: true }; }
        result = parsePlayResult(await active.request('memory_response', { attempt: result.attempt, ...answer }));
      }
      if (result.status !== 'started') { playing = false; return { error: result.status }; }
      return { started: true };
    } catch { playing = false; return { error: 'unavailable' }; }
  });
  window.on('closed', () => { if (worker) void worker.close(); });
  window.loadFile(path.join(__dirname, 'index.html'));
});
app.on('window-all-closed', () => app.quit());
