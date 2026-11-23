import { app, BrowserWindow, dialog, ipcMain } from 'electron';
import path from 'node:path';
import { inspect } from './inspection';
import { startPlay } from './play';
import { createMonitor } from './monitor';
import { defaultRoots, findDefaultRoot } from './discovery';
import { runtimePaths } from './runtime-paths';
import { createDevelopmentLog, type DevelopmentLog } from './dev-log';

const { source, python } = runtimePaths(app.isPackaged, process.resourcesPath, __dirname, process.env, process.platform);
app.whenReady().then(() => {
  const window = new BrowserWindow({ width: 1040, height: 720, minWidth: 720, minHeight: 500, webPreferences: { preload: path.join(__dirname, 'preload.js'), contextIsolation: true, sandbox: true, nodeIntegration: false } });
  let selectedRoot: string | null = null;
  let playing = false;
  let log: DevelopmentLog | null = null;
  async function selectRoot(root: string, origin: 'default' | 'manual') {
    const basename = path.basename(root).toLowerCase();
    const brand = basename === 'nx2saria' ? 'NX2Saria' : basename === 'nx2navi' ? 'NX2navi' : 'other';
    log = findDefaultRoot([root]) ? await createDevelopmentLog(root, origin, brand, app.getVersion(), process.platform) : null;
    const result = await inspect(root, python, source);
    selectedRoot = result.status === 'ready' ? root : null;
    await log?.write('inspection', result.status);
    return result;
  }
  ipcMain.handle('discover-installation', async event => {
    if (event.sender !== window.webContents || playing) throw new Error('Unavailable');
    const root = findDefaultRoot(defaultRoots(process.platform, app.getPath('home'), process.env));
    if (!root) return null;
    return selectRoot(root, 'default');
  });
  ipcMain.handle('select-installation', async event => {
    if (event.sender !== window.webContents || playing) throw new Error('Unavailable');
    selectedRoot = null;
    await log?.write('inspection', 'selection requested');
    const chosen = await dialog.showOpenDialog(window, { properties: ['openDirectory'], title: 'Select NX2Saria launcher folder' });
    if (chosen.canceled || !chosen.filePaths[0]) { await log?.write('inspection', 'selection cancelled'); log = null; return null; }
    return selectRoot(chosen.filePaths[0], 'manual');
  });
  ipcMain.handle('play', async event => {
    if (event.sender !== window.webContents || !selectedRoot || playing) return { error: 'unavailable' };
    playing = true;
    const root = selectedRoot;
    await log?.write('play', 'requested');
    const result = await inspect(root, python, source);
    if (result.status !== 'ready' || result.job_marker_present || result.game_running) { playing = false; await log?.write('play', 'refused'); return { error: 'not_ready' }; }
    const session = startPlay(root);
    const started = await session.started;
    if ('error' in started) { playing = false; await log?.write('play', 'start failed'); return started; }
    await log?.write('play', 'started');
    const monitor = createMonitor(
      () => inspect(root, python, source, session.pid),
      () => {
        if (window.isDestroyed() || !window.isVisible() || window.isMinimized()) return false;
        window.hide();
        void log?.write('window', 'hidden');
        return true;
      }
    );
    const timer = (process.platform === 'linux' || process.platform === 'win32') && session.pid !== undefined
      ? setInterval(() => { void monitor.check(); }, 300) : null;
    if (timer) void monitor.check();
    void session.done.then(async end => {
      if (timer) clearInterval(timer);
      const hidden = monitor.stop();
      await log?.write('play', 'error' in end ? 'start failed' : end.signal ? `ended signal=${end.signal}` : `ended code=${end.code ?? 'unknown'}`);
      if (!window.isDestroyed() && hidden) { window.show(); window.focus(); await log?.write('window', 'restored'); }
      const state = await inspect(root, python, source);
      await log?.write('inspection', state.status);
      selectedRoot = state.status === 'ready' ? root : null;
      playing = false;
      if (!window.isDestroyed()) window.webContents.send('play-ended', { ...end, state });
    });
    return started;
  });
  window.loadFile(path.join(__dirname, 'index.html'));
});
app.on('window-all-closed', () => app.quit());
