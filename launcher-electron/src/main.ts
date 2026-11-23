import { app, BrowserWindow, dialog, ipcMain } from 'electron';
import path from 'node:path';
import { inspect } from './inspection';
import { startPlay } from './play';
import { createMonitor } from './monitor';
import { defaultRoots, findDefaultRoot } from './discovery';
import { runtimePaths } from './runtime-paths';

const { source, python } = runtimePaths(app.isPackaged, process.resourcesPath, __dirname, process.env, process.platform);
app.whenReady().then(() => {
  const window = new BrowserWindow({ width: 1040, height: 720, minWidth: 720, minHeight: 500, webPreferences: { preload: path.join(__dirname, 'preload.js'), contextIsolation: true, sandbox: true, nodeIntegration: false } });
  let selectedRoot: string | null = null;
  let playing = false;
  ipcMain.handle('discover-installation', async event => {
    if (event.sender !== window.webContents || playing) throw new Error('Unavailable');
    const root = findDefaultRoot(defaultRoots(process.platform, app.getPath('home'), process.env));
    if (!root) return null;
    const result = await inspect(root, python, source);
    selectedRoot = result.status === 'ready' ? root : null;
    return result;
  });
  ipcMain.handle('select-installation', async event => {
    if (event.sender !== window.webContents || playing) throw new Error('Unavailable');
    selectedRoot = null;
    const chosen = await dialog.showOpenDialog(window, { properties: ['openDirectory'], title: 'Select NX2Saria launcher folder' });
    if (chosen.canceled || !chosen.filePaths[0]) return null;
    const result = await inspect(chosen.filePaths[0], python, source);
    selectedRoot = result.status === 'ready' ? chosen.filePaths[0] : null;
    return result;
  });
  ipcMain.handle('play', async event => {
    if (event.sender !== window.webContents || !selectedRoot || playing) return { error: 'unavailable' };
    playing = true;
    const root = selectedRoot;
    const result = await inspect(root, python, source);
    if (result.status !== 'ready' || result.job_marker_present || result.game_running) { playing = false; return { error: 'not_ready' }; }
    const session = startPlay(root);
    const started = await session.started;
    if ('error' in started) { playing = false; return started; }
    const monitor = createMonitor(
      () => inspect(root, python, source, session.pid),
      () => {
        if (window.isDestroyed() || !window.isVisible() || window.isMinimized()) return false;
        window.hide();
        return true;
      }
    );
    const timer = (process.platform === 'linux' || process.platform === 'win32') && session.pid !== undefined
      ? setInterval(() => { void monitor.check(); }, 300) : null;
    if (timer) void monitor.check();
    void session.done.then(end => {
      if (timer) clearInterval(timer);
      const hidden = monitor.stop();
      playing = false;
      if (!window.isDestroyed()) {
        if (hidden) { window.show(); window.focus(); }
        window.webContents.send('play-ended', end);
      }
    });
    return started;
  });
  window.loadFile(path.join(__dirname, 'index.html'));
});
app.on('window-all-closed', () => app.quit());
