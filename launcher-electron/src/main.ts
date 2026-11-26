import { app, BrowserWindow, dialog, ipcMain } from 'electron';
import { readFileSync } from 'node:fs';
import path from 'node:path';
import { parseAppearance, validAppearancePreference } from './appearance-protocol';
import { parseInstalledSettings, validSetting } from './installed-settings';
import { parseModAction, parseModsPage } from './mods-protocol';
import { parseCrashOffer, parseReportsView, readReportPreview, readWorkerBytes } from './reports-protocol';
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
  let questionOwner: WorkerClient | null = null;
  let crashOwner: WorkerClient | null = null;
  let closingReport = false;
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
    if (!result) return null;
    const editable = worker?.handshake.lock === 'held' && !result.job_marker_present;
    return { ...result, hide_while_playing: editable ? result.hide_while_playing : null,
      first_run_preferences: editable ? result.first_run_preferences : undefined };
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
        message: 'A memória disponível pode ser insuficiente.', detail: `Disponível: ${Math.trunc(memory.remaining)} MiB de memória comprometida (RAM e arquivo de paginação). Estimativa do jogo: ${estimate}. Feche programas que usam muita memória e ative ou aumente o arquivo de paginação do Windows. Para 16 GB de RAM, use 32 GB fixos (32768 MB) e reinicie o Windows. Para não mostrar este aviso novamente, desative o aviso de memória em Configurações ou ajuste mem_need_mb = 0 em nx2saria.ini.` });
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
        if (frame.event === 'update_checked') {
          log?.write('update', `check result=${frame.update.status}`);
          if (!window.isDestroyed()) window.webContents.send('update-checked', frame.update);
        } else if (frame.event === 'recovery_changed') {
          state = frame.inspection;
          log?.write('recovery', `state=${state?.recovery?.state ?? 'none'}`);
          if (!window.isDestroyed()) window.webContents.send('recovery-changed', settingsView(state));
        } else if (frame.event === 'job_started') {
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
  ipcMain.handle('first-run-preference', async (event, key, value) => {
    const valid = (key === 'channel' && (value === 'stable' || value === 'preview')) ||
      (key === 'auto_check' && typeof value === 'boolean') ||
      (key === 'desktop_shortcut' && process.platform === 'win32' && typeof value === 'boolean');
    const preferences = state?.first_run_preferences;
    if (event.sender !== window.webContents || !valid || playing || !worker ||
        worker.handshake.lock !== 'held' || state?.job_marker_present || !preferences ||
        !firstRunAvailable(state)) throw new Error('Unavailable');
    const active = worker;
    try {
      const changed = await active.request('first_run_preference', { key, value });
      if (worker !== active || changed?.key !== key || changed?.value !== value) throw new Error('Save unsuccessful');
      if (state) state.first_run_preferences = { ...preferences, [key]: value };
      log?.write('settings', `first_run key=${key} result=saved`);
      return value;
    } catch {
      log?.write('settings', `first_run key=${key} result=save_failed`);
      throw new Error('Save unsuccessful');
    }
  });
  ipcMain.handle('recover-build', async (event, action) => {
    if (event.sender !== window.webContents || !worker?.handshake.capabilities.includes('recovery') ||
        playing || !state?.job_marker_present || (action !== 'wait' && action !== 'stop' && action !== 'confirm')) {
      return 'unavailable';
    }
    const active = worker;
    if (action !== 'wait') {
      const stopping = action === 'stop';
      if (state.recovery?.state !== (stopping ? 'alive' : 'unverified')) return 'unavailable';
      const answer = await dialog.showMessageBox(window, { type: 'warning',
        buttons: ['Voltar', stopping ? 'Parar compilação' : 'Sei que nenhuma compilação está rodando'],
        defaultId: 0, cancelId: 0,
        message: stopping ? 'Parar a compilação anterior?' : 'Confirmar que a compilação anterior terminou?',
        detail: stopping ? 'Você poderá retomar a compilação depois que ela parar.' :
          'Confirme somente se você verificou que a compilação parou. Se tiver dúvida, reinicie o computador.' });
      if (answer.response !== 1 || worker !== active) return 'cancelled';
    }
    let status = 'unavailable';
    try {
      const result = await active.request('recover_build', { action });
      if (worker !== active) return status;
      if (result?.status === 'finished') {
        state = parseInspection(result.inspection);
        if (!window.isDestroyed()) window.webContents.send('recovery-changed', settingsView(state));
        status = 'finished';
      } else if (result?.status === 'waiting' || result?.status === 'stopping') {
        status = result.status;
      }
    } catch {}
    log?.write('recovery', `${action} result=${status}`);
    return status;
  });
  function gameWorker(event: Electron.IpcMainInvokeEvent, capability: 'saves' | 'mods' | 'steam') {
    if (event.sender !== window.webContents || !worker?.handshake.capabilities.includes(capability) ||
        worker.handshake.lock !== 'held' || state?.status !== 'ready' || state.job_marker_present ||
        (playing && !gameSpawned)) throw new Error('Game action unavailable');
    return worker;
  }
  async function saveOptions(active: WorkerClient) {
    const result = await active.request('saves_info');
    if (active !== worker || typeof result?.destination !== 'string' || !path.isAbsolute(result.destination) ||
        typeof result.with_log !== 'boolean' || typeof result.waiting !== 'boolean' ||
        typeof result.pending !== 'boolean') throw new Error('Saves unavailable');
    return { destination: result.destination, with_log: result.with_log, waiting: result.waiting, pending: result.pending };
  }
  ipcMain.handle('saves-info', event => saveOptions(gameWorker(event, 'saves')));
  ipcMain.handle('choose-save-export', async event => {
    const active = gameWorker(event, 'saves');
    const options = await saveOptions(active);
    const picked = await dialog.showOpenDialog(window, { title: 'Escolher pasta de exportação',
      properties: ['openDirectory'], defaultPath: options.destination });
    const destination = picked.filePaths[0];
    if (picked.canceled || !destination) return null;
    if (gameWorker(event, 'saves') !== active || !path.isAbsolute(destination)) throw new Error('Saves unavailable');
    const changed = await active.request('save_preference', { key: 'save_export_dir', value: destination });
    if (worker !== active || changed?.key !== 'save_export_dir' || changed?.value !== destination) {
      throw new Error('Save unsuccessful');
    }
    log?.write('saves', 'destination saved');
    return destination;
  });
  ipcMain.handle('set-save-report', async (event, value) => {
    const active = gameWorker(event, 'saves');
    if (typeof value !== 'boolean') throw new Error('Invalid preference');
    const changed = await active.request('save_preference', { key: 'export_with_log', value });
    if (worker !== active || changed?.key !== 'export_with_log' || changed?.value !== value) throw new Error('Save unsuccessful');
    log?.write('saves', `report=${value ? 'on' : 'off'}`);
    return value;
  });
  ipcMain.handle('export-save', async event => {
    const active = gameWorker(event, 'saves');
    const result = await active.request('export_save', {}, null);
    if (worker !== active) throw new Error('Saves unavailable');
    if (result?.status === 'no_save' || result?.status === 'failed') {
      log?.write('saves', `export result=${result.status}`);
      return { status: result.status };
    }
    if (result?.status !== 'ok' || typeof result.name !== 'string' || path.basename(result.name) !== result.name ||
        !/^NX2Saria-(?:save|log)-[0-9() _-]+(?:bug-report)?[0-9() _-]*\.zip$/.test(result.name) ||
        !Number.isSafeInteger(result.files) || result.files < 0 || typeof result.included_log !== 'boolean') {
      throw new Error('Export unsuccessful');
    }
    log?.write('saves', `export result=ok files=${result.files} report=${result.included_log}`);
    return { status: 'ok', name: result.name, files: result.files, included_log: result.included_log };
  });
  ipcMain.handle('import-save', async (event, kind) => {
    const active = gameWorker(event, 'saves');
    if (playing || state?.game_running || (kind !== 'archive' && kind !== 'folder')) throw new Error('Import unavailable');
    const options = await saveOptions(active);
    let choice: { source: string; path?: string } = { source: 'waiting' };
    if (kind === 'folder' || !options.waiting) {
      const picked = await dialog.showOpenDialog(window, {
        title: kind === 'folder' ? 'Escolher pasta do save' : 'Escolher o save',
        defaultPath: options.destination,
        properties: kind === 'folder' ? ['openDirectory'] : ['openFile'],
        ...(kind === 'archive' ? { filters: [{ name: 'Save do NX2Saria', extensions: ['zip'] },
          { name: 'Todos os arquivos', extensions: ['*'] }] } : {}),
      });
      const selected = picked.filePaths[0];
      if (picked.canceled || !selected) return null;
      if (!path.isAbsolute(selected)) throw new Error('Import unavailable');
      choice = { source: kind, path: selected };
    }
    if (gameWorker(event, 'saves') !== active || playing) throw new Error('Import unavailable');
    const result = await active.request('import_save', choice, null);
    if (worker !== active) throw new Error('Import unavailable');
    if (['game_running', 'invalid_source', 'invalid_save', 'failed'].includes(result?.status) &&
        (result.detail === undefined || (typeof result.detail === 'string' && result.detail.length <= 520))) {
      log?.write('saves', `import result=${result.status}`);
      return { status: result.status, detail: result.detail };
    }
    if (result?.status !== 'ok' || !Number.isSafeInteger(result.files) || result.files < 1 ||
        !(result.slot === null || (typeof result.slot === 'string' && /^slot_[0-9]{2}$/.test(result.slot))) ||
        !(result.backup === null || (typeof result.backup === 'string' && /^backup [0-9 .()-]+$/.test(result.backup)))) {
      throw new Error('Invalid import result');
    }
    log?.write('saves', `import result=ok files=${result.files} backup=${Boolean(result.backup)}`);
    return { status: 'ok', files: result.files, slot: result.slot, backup: result.backup };
  });
  ipcMain.handle('open-saves-dir', async event => {
    const active = gameWorker(event, 'saves');
    const result = await active.request('open_saves_dir');
    if (worker !== active || result?.status !== 'ok') throw new Error('Could not open saves folder');
    log?.write('saves', 'folder opened');
  });
  async function modAction(active: WorkerClient, method: string, args: Record<string, unknown>, timeout?: number | null) {
    const result = parseModAction(await active.request(method, args, timeout));
    if (worker !== active) throw new Error('Mods unavailable');
    log?.write('mods', `${method} result=${result.status}`);
    return result;
  }
  ipcMain.handle('mods-list', async (event, page) => {
    const active = gameWorker(event, 'mods');
    if (!Number.isSafeInteger(page) || page < 0) throw new Error('Invalid page');
    const result = parseModsPage(await active.request('mods_list', { page }));
    if (worker !== active) throw new Error('Mods unavailable');
    return result;
  });
  ipcMain.handle('mod-add', async (event, kind) => {
    const active = gameWorker(event, 'mods');
    if (kind !== 'archive' && kind !== 'folder') throw new Error('Invalid mod source');
    const picked = await dialog.showOpenDialog(window, {
      title: kind === 'folder' ? 'Escolher pasta do mod' : 'Escolher arquivo do mod',
      defaultPath: app.getPath('downloads'),
      properties: kind === 'folder' ? ['openDirectory'] : ['openFile'],
      ...(kind === 'archive' ? { filters: [{ name: 'Mods', extensions: ['zip', '7z', 'rar'] },
        { name: 'Todos os arquivos', extensions: ['*'] }] } : {}),
    });
    const selected = picked.filePaths[0];
    if (picked.canceled || !selected) return null;
    if (gameWorker(event, 'mods') !== active || !path.isAbsolute(selected)) throw new Error('Mods unavailable');
    return modAction(active, 'mod_add', { path: selected }, null);
  });
  ipcMain.handle('mod-enabled', (event, name, value) => {
    if (typeof name !== 'string' || name.length > 255 || typeof value !== 'boolean') throw new Error('Invalid mod setting');
    return modAction(gameWorker(event, 'mods'), 'mod_enabled', { name, value });
  });
  ipcMain.handle('mod-move', (event, name, direction) => {
    if (typeof name !== 'string' || name.length > 255 || (direction !== -1 && direction !== 1)) {
      throw new Error('Invalid load order');
    }
    return modAction(gameWorker(event, 'mods'), 'mod_move', { name, direction });
  });
  ipcMain.handle('open-mods-dir', event => modAction(gameWorker(event, 'mods'), 'open_mods_dir', {}));
  function appearanceWorker(event: Electron.IpcMainInvokeEvent, write = false) {
    if (event.sender !== window.webContents || !state || state.status === 'inspection_error' ||
        !worker?.handshake.capabilities.includes('appearance') ||
        (write && (worker.handshake.lock !== 'held' || state.job_marker_present || (playing && !gameSpawned)))) {
      return null;
    }
    return worker;
  }
  ipcMain.handle('appearance-info', async event => {
    const active = appearanceWorker(event);
    if (!active) return null;
    const result = parseAppearance(await active.request('appearance_info'));
    if (worker !== active) return null;
    log?.write('appearance', `theme=${result.theme} read`);
    return { ...result, writable: active.handshake.lock === 'held' };
  });
  ipcMain.handle('appearance-preference', async (event, key, value) => {
    const active = appearanceWorker(event, true);
    if (!active || !validAppearancePreference(key, value)) return null;
    try {
      const result = parseAppearance(await active.request('appearance_preference', { key, value }));
      if (worker !== active) return null;
      log?.write('appearance', `key=${key} result=saved`);
      return { ...result, writable: true };
    } catch {
      log?.write('appearance', `key=${key} result=failed`);
      return null;
    }
  });
  ipcMain.handle('appearance-image', async event => {
    const active = appearanceWorker(event);
    if (!active) return null;
    const first = await active.request('appearance_image', { offset: 0 });
    if (!first || worker !== active) return null;
    const { bytes } = await readWorkerBytes(offset => offset === 0
      ? Promise.resolve(first) : active.request('appearance_image', { offset }));
    return worker === active && bytes.subarray(0, 8).equals(Buffer.from('89504e470d0a1a0a', 'hex'))
      ? `data:image/png;base64,${bytes.toString('base64')}` : null;
  });
  ipcMain.handle('appearance-background', async (event, choose) => {
    const active = appearanceWorker(event, true);
    if (!active || typeof choose !== 'boolean') return null;
    let source: string | null = null;
    if (choose) {
      const picked = await dialog.showOpenDialog(window, { title: 'Escolher imagem de fundo',
        defaultPath: app.getPath('pictures'), properties: ['openFile'],
        filters: [{ name: 'Imagens', extensions: ['png', 'jpg', 'jpeg', 'webp', 'bmp', 'gif', 'tif', 'tiff'] },
          { name: 'Todos os arquivos', extensions: ['*'] }] });
      if (picked.canceled || !picked.filePaths[0]) return null;
      source = picked.filePaths[0];
      if (!path.isAbsolute(source) || appearanceWorker(event, true) !== active) return null;
    }
    const result = await active.request('appearance_background', { path: source });
    if (worker !== active || !['ok', 'invalid_picture', 'failed'].includes(result?.status)) return null;
    log?.write('appearance', `background ${choose ? 'choose' : 'remove'} result=${result.status}`);
    return { status: result.status,
      appearance: result.status === 'ok' ? { ...parseAppearance(result.appearance), writable: true } : null };
  });
  ipcMain.handle('appearance-assets', async event => {
    const active = appearanceWorker(event);
    if (!active) return null;
    let assets = await active.request('appearance_assets');
    while (worker === active && assets?.status === 'loading') {
      await new Promise(resolve => setTimeout(resolve, 150));
      assets = await active.request('appearance_assets');
    }
    if (worker !== active || assets?.status !== 'ready' || typeof assets.title !== 'boolean' ||
        !Array.isArray(assets.effects) || assets.effects.some((effect: unknown) => effect !== 'select' && effect !== 'back')) {
      return null;
    }
    const mediaWorker = active;
    async function readAsset(asset: 'title' | 'select' | 'back') {
      const first = await mediaWorker.request('appearance_asset', { asset, offset: 0 });
      if (!first || worker !== active) return null;
      const { bytes } = await readWorkerBytes(offset => offset === 0 ? Promise.resolve(first) :
        mediaWorker.request('appearance_asset', { asset, offset }), asset === 'title' ? 16 * 1024 * 1024 : 4 * 1024 * 1024);
      return worker === active ? bytes.toString('base64') : null;
    }
    const [title, select, back] = await Promise.all([
      assets.title ? readAsset('title') : null,
      assets.effects.includes('select') ? readAsset('select') : null,
      assets.effects.includes('back') ? readAsset('back') : null,
    ]);
    if (worker !== active) return null;
    log?.write('appearance', `assets title=${Boolean(title)} effects=${Number(Boolean(select)) + Number(Boolean(back))}`);
    return { title, select, back };
  });
  const steamStatuses = ['added', 'available', 'not_found', 'flatpak_unsupported', 'unavailable'];
  async function steamStatus(active: WorkerClient) {
    const result = await active.request('steam_info');
    if (worker !== active || !steamStatuses.includes(result?.status) || typeof result.running !== 'boolean') {
      throw new Error('Steam unavailable');
    }
    return { status: result.status, running: result.running, linux: process.platform === 'linux' };
  }
  ipcMain.handle('steam-info', async event => {
    const result = await steamStatus(gameWorker(event, 'steam'));
    log?.write('steam', `detection=${result.status} running=${result.running}`);
    return result;
  });
  ipcMain.handle('steam-add', async event => {
    const active = gameWorker(event, 'steam');
    let result = await active.request('steam_add');
    while (worker === active && result?.status === 'adding') {
      await new Promise(resolve => setTimeout(resolve, 200));
      result = await active.request('steam_result');
    }
    if (worker !== active || !['ok', 'failed', 'not_built', ...steamStatuses].includes(result?.status)) {
      throw new Error('Steam unavailable');
    }
    log?.write('steam', `add result=${result.status}`);
    const detail = typeof result.detail === 'string' ? result.detail.slice(0, 256) : undefined;
    return { status: result.status, detail, view: await steamStatus(active) };
  });
  function settingsWorker(event: Electron.IpcMainInvokeEvent, write = false) {
    if (event.sender !== window.webContents || !worker?.handshake.capabilities.includes('settings') ||
        state?.status !== 'ready' || (write && (worker.handshake.lock !== 'held' || playing || state.job_marker_present))) {
      throw new Error('Settings unavailable');
    }
    return worker;
  }
  ipcMain.handle('settings-info', async event => {
    const active = settingsWorker(event);
    const result = parseInstalledSettings(await active.request('settings_info'));
    if (active !== worker) throw new Error('Settings unavailable');
    return { ...result, dump: result.dump ? path.basename(result.dump) : '', writable: active.handshake.lock === 'held' };
  });
  ipcMain.handle('settings-preference', async (event, key, value) => {
    const active = settingsWorker(event, true);
    if (!validSetting(key, value, process.platform)) throw new Error('Invalid launcher preference');
    try {
      const result = await active.request('settings_preference', { key, value });
      if (active !== worker || result?.key !== key || result?.value !== value) throw new Error('Save unsuccessful');
      log?.write('settings', `key=${key} result=saved`);
      return value;
    } catch {
      log?.write('settings', `key=${key} result=save_failed`);
      throw new Error('Save unsuccessful');
    }
  });
  ipcMain.handle('settings-dump', async event => {
    const active = settingsWorker(event, true);
    const current = parseInstalledSettings(await active.request('settings_info'));
    if (settingsWorker(event, true) !== active) throw new Error('Settings unavailable');
    const chosen = await dialog.showOpenDialog(window, {
      defaultPath: path.isAbsolute(current.dump) ? path.dirname(current.dump) : app.getPath('home'),
      title: 'Escolher dump do jogo', properties: ['openFile'],
      filters: [{ name: 'Dump do jogo', extensions: ['nup'] }, { name: 'Todos os arquivos', extensions: ['*'] }],
    });
    if (chosen.canceled || !chosen.filePaths[0]) return null;
    const picked = chosen.filePaths[0];
    if (settingsWorker(event, true) !== active || !path.isAbsolute(picked)) throw new Error('Settings unavailable');
    try {
      const result = await active.request('settings_dump', { file: picked });
      if (active !== worker) throw new Error('Settings unavailable');
      if (result?.status === 'invalid_dump') {
        log?.write('settings', 'dump result=invalid_dump');
        return { status: 'invalid_dump' };
      }
      if (result?.file !== picked) throw new Error('Save unsuccessful');
      log?.write('settings', 'dump result=saved');
      return { status: 'ok', name: path.basename(picked) };
    } catch {
      log?.write('settings', 'dump result=failed');
      return { status: 'failed' };
    }
  });
  ipcMain.handle('settings-open', async (event, target) => {
    const active = settingsWorker(event);
    if (target !== 'game' && target !== 'logs') throw new Error('Invalid launcher folder');
    try {
      const result = await active.request('settings_open', { target });
      if (active !== worker || result?.status !== 'ok') throw new Error('Open unsuccessful');
      log?.write('settings', `open target=${target} result=ok`);
    } catch {
      log?.write('settings', `open target=${target} result=failed`);
      throw new Error('Open unsuccessful');
    }
  });
  function reportsWorker(event: Electron.IpcMainInvokeEvent) {
    if (event.sender !== window.webContents || state?.status !== 'ready' || playing ||
        !worker?.handshake.capabilities.includes('reports') || worker.handshake.lock !== 'held') return null;
    return worker;
  }
  async function reportState(active: WorkerClient, method: string, fields: Record<string, unknown> = {}) {
    const state = parseReportsView(await active.request(method, fields));
    if (active !== worker) throw new Error('Reports unavailable');
    questionOwner = state.question ? active : null;
    return state;
  }
  ipcMain.handle('reports-home', async (event, after) => {
    const active = reportsWorker(event);
    if (!active || (after !== '' && after !== 'installation' && after !== 'update')) return null;
    const result = await reportState(active, 'reports_home', { after });
    if (result.question) log?.write('reports', `question countdown=${result.seconds !== null}`);
    return result;
  });
  ipcMain.handle('reports-status', event => {
    const active = reportsWorker(event);
    return active ? reportState(active, 'reports_status') : null;
  });
  ipcMain.handle('reports-touch', event => {
    const active = reportsWorker(event);
    return active ? reportState(active, 'reports_touch') : null;
  });
  ipcMain.handle('reports-answer', async (event, answer) => {
    const active = reportsWorker(event);
    if (!active || (answer !== 'send' && answer !== "don't send" && answer !== 'closed')) return null;
    const result = await reportState(active, 'reports_answer', { answer });
    log?.write('reports', `question answer=${answer === "don't send" ? 'declined' : answer}`);
    return result;
  });
  ipcMain.handle('reports-consent', async (event, value) => {
    const active = reportsWorker(event);
    if (!active || typeof value !== 'boolean') return null;
    const result = await reportState(active, 'reports_consent', { value });
    log?.write('reports', `consent=${result.consent}`);
    return result;
  });
  ipcMain.handle('reports-preview', async event => {
    const active = reportsWorker(event);
    if (!active) return null;
    const result = await readReportPreview(offset => active.request('reports_preview', { offset }));
    if (active !== worker) return null;
    log?.write('reports', `preview example=${result.example}`);
    return result;
  });
  async function reportSubmission(active: WorkerClient, method: string, statusMethod: string,
                                  args: Record<string, unknown>) {
    let result = await active.request(method, args);
    while (worker === active && result?.status === 'sending') {
      await new Promise(resolve => setTimeout(resolve, 200));
      result = await active.request(statusMethod);
    }
    return worker === active ? result : null;
  }
  ipcMain.handle('reports-comment', async (event, text, attach) => {
    const active = reportsWorker(event);
    if (!active || typeof text !== 'string' || text.length > 8000 || [...text].length > 4000 ||
        typeof attach !== 'boolean') return { status: 'failed' };
    const result = await reportSubmission(active, 'reports_comment', 'reports_comment_status', { text, attach });
    if (result?.status !== 'ok' && result?.status !== 'failed') return { status: 'failed' };
    log?.write('reports', `comment result=${result.status} attachment=${attach}`);
    return { status: result.status, reason: typeof result.reason === 'string' ? result.reason.slice(0, 240) : undefined };
  });
  ipcMain.handle('reports-crash-open', async event => {
    const active = reportsWorker(event);
    if (!active) return null;
    const offer = parseCrashOffer(await active.request('reports_crash_open'));
    if (worker !== active) return null;
    crashOwner = offer ? active : null;
    if (offer) log?.write('reports', `crash offer=shown send_enabled=${offer.can_send}`);
    return offer;
  });
  ipcMain.handle('reports-crash-preview', async event => {
    const active = reportsWorker(event);
    if (!active || active !== crashOwner) return null;
    const result = await readReportPreview(offset => active.request('reports_crash_preview', { offset }));
    if (worker !== active || crashOwner !== active) return null;
    log?.write('reports', 'crash preview=shown');
    return result;
  });
  ipcMain.handle('reports-crash-preference', async (event, value) => {
    const active = reportsWorker(event);
    if (!active || typeof value !== 'boolean') return null;
    const result = await reportState(active, 'reports_crash_preference', { value });
    log?.write('reports', `crash ask=${value ? 'on' : 'off'}`);
    return result;
  });
  ipcMain.handle('reports-crash-answer', async (event, answer) => {
    const active = reportsWorker(event);
    if (!active || active !== crashOwner || !['send', 'decline', 'closed', 'comment'].includes(answer)) {
      return { status: 'none' };
    }
    const result = await reportSubmission(active, 'reports_crash_answer', 'reports_crash_status', { answer });
    if (!result || !['ok', 'failed', 'none', 'declined', 'comment', 'guarded', 'disabled'].includes(result.status)) {
      return { status: 'none' };
    }
    if (result.status !== 'guarded' && result.status !== 'disabled') crashOwner = null;
    log?.write('reports', `crash answer=${answer} result=${result.status}`);
    return { status: result.status, reason: typeof result.reason === 'string' ? result.reason.slice(0, 240) : undefined };
  });
  ipcMain.handle('check-updates', async event => {
    if (event.sender !== window.webContents || !worker?.handshake.capabilities.includes('check_updates') ||
        worker.handshake.lock !== 'held' || !state || state.job_marker_present) return 'unavailable';
    let status = 'unavailable';
    try {
      const active = worker;
      const response = await active.request('check_updates');
      if (worker === active && (response?.status === 'checking' || response?.status === 'recent_check')) {
        status = response.status;
      }
    } catch {}
    log?.write('update', `check request=${status}`);
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
  window.on('close', event => {
    const active = questionOwner ?? crashOwner;
    if (!active || active !== worker) return;
    event.preventDefault();
    if (closingReport) return;
    closingReport = true;
    const action = questionOwner ? 'question' : 'crash';
    void active.request('reports_close').then(() => {
      log?.write('reports', `${action} answer=closed`);
    }).catch(() => {}).finally(() => {
      questionOwner = crashOwner = null;
      closingReport = false;
      if (!window.isDestroyed()) window.close();
    });
  });
  window.on('closed', () => {
    log?.write('window', 'closed');
    if (worker) void worker.close();
  });
  app.once('quit', (_, code) => log?.write('exit', `code=${code} play_active=${playing}`));
  window.loadFile(path.join(__dirname, 'index.html'));
});
app.on('window-all-closed', () => app.quit());
