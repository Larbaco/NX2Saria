type FirstRunPreferences = { channel: 'stable' | 'preview'; auto_check: boolean; desktop_shortcut: boolean | null };
type Result = { status: string; root?: string; game_dir?: string | null; reason?: string; job_marker_present?: boolean; game_running?: true | null; hide_while_playing?: boolean | null; recovery?: { state: 'alive' | 'interrupted' | 'unverified'; kind: string }; first_run_preferences?: FirstRunPreferences; first_run_requirements?: { status: string; message?: string } };
type PlayFeedback = { messageId?: number; outcome?: 'game_ended' | 'monitor_lost'; code?: number | null; alarm?: boolean; message?: string; state?: Result };
type SaveOptions = { destination: string; with_log: boolean; waiting: boolean; pending: boolean };
type SaveImport = { status: string; files?: number; slot?: string | null; backup?: string | null; detail?: string };
type SaveExport = { status: string; name?: string; files?: number; included_log?: boolean };
type ModRow = {
  name: string; enabled: boolean; usable: boolean; waiting: boolean; files: number;
  patches: number; cheats: boolean; other_version: number; problem: string;
  overlaps: { name: string; files: number; wins: boolean }[];
};
type ModPage = { page: number; pages: number; count: number; items: ModRow[] };
type ModAction = { status: string; name?: string; problem?: string; detail?: string; page?: number };
type SteamView = { status: string; running: boolean; linux: boolean };
type InstalledKey = 'channel' | 'auto_check' | 'auto_install' | 'keep_cache' | 'warn_low_commit';
type InstalledSettings = { game_dir: string; dump: string; channel: 'stable' | 'preview'; auto_check: boolean;
  auto_install: boolean; keep_cache: boolean; warn_low_commit: boolean | null; writable: boolean };
type UpdateView = { status: string; version?: string; notes?: string[] };
type BuildAction = 'install' | 'rebuild';
type RecoveryAction = 'wait' | 'stop' | 'confirm';
type BuildFeedback = { action: BuildAction; outcome: string; state: Result | null };
type InstallDestination = { folder: string; available: number; required: number; completeImage: boolean; pathTooLong: boolean };
interface Window { launcher: { discoverInstallation(): Promise<Result | null>; selectInstallation(): Promise<Result | null>; installDestination(pick: boolean): Promise<InstallDestination | null>; installDump(pick: boolean): Promise<string | null>; searchInstallDump(): Promise<{ status: 'searching' | 'complete'; file: string | null }>; install(): Promise<string>; rebuild(): Promise<string>; recoverBuild(action: RecoveryAction): Promise<string>; onRecoveryChanged(callback: (state: Result | null) => void): void; onBuildEnded(callback: (result: BuildFeedback) => void): void; setFirstRunPreference(key: keyof FirstRunPreferences, value: string | boolean): Promise<string | boolean>; setHideWhilePlaying(value: boolean): Promise<boolean>; play(): Promise<{ started?: boolean; building?: boolean; error?: string; messageId?: number }>; cancelJob(): Promise<unknown>; onJobStatus(callback: (result: { status: string; task?: BuildAction | 'mods'; fraction?: number }) => void): void; onGameWindowReady(callback: () => void): void; savesInfo(): Promise<SaveOptions>; chooseSaveExport(): Promise<string | null>; setSaveReport(value: boolean): Promise<boolean>; exportSave(): Promise<SaveExport>; importSave(source: 'archive' | 'folder'): Promise<SaveImport | null>; openSavesDir(): Promise<void>; modsList(page: number): Promise<ModPage>; modAdd(kind: 'archive' | 'folder'): Promise<ModAction | null>; modEnabled(name: string, value: boolean): Promise<ModAction>; modMove(name: string, direction: -1 | 1): Promise<ModAction>; openModsDir(): Promise<ModAction>; steamInfo(): Promise<SteamView>; steamAdd(): Promise<{ status: string; detail?: string; view: SteamView }>; settingsInfo(): Promise<InstalledSettings>; settingsPreference(key: InstalledKey, value: string | boolean): Promise<string | boolean>; settingsDump(): Promise<{ status: string; name?: string } | null>; settingsOpen(target: 'game' | 'logs'): Promise<void>; reportsHome(after: string): Promise<ReportsState | null>; reportsStatus(): Promise<ReportsState | null>; reportsTouch(): Promise<ReportsState | null>; reportsAnswer(answer: string): Promise<ReportsState | null>; reportsConsent(value: boolean): Promise<ReportsState | null>; reportsPreview(): Promise<{ text: string; example: boolean } | null>; reportsComment(text: string, attach: boolean): Promise<{ status: string; reason?: string }>; reportsCrashOpen(): Promise<CrashOffer | null>; reportsCrashPreview(): Promise<{ text: string; example: boolean } | null>; reportsCrashPreference(value: boolean): Promise<ReportsState | null>; reportsCrashAnswer(answer: string): Promise<{ status: string; reason?: string }>; checkUpdates(): Promise<string>; onUpdateChecked(callback: (update: UpdateView) => void): void; onPlayEnded(callback: (result: PlayFeedback) => void): void; playMessageShown(id: number, kind: string): void } }
const statusElement = document.querySelector<HTMLElement>('#status')!;
const detail = document.querySelector<HTMLElement>('#detail')!;
const button = document.querySelector<HTMLButtonElement>('#select')!;
const rebuildButton = document.querySelector<HTMLButtonElement>('#rebuild')!;
const playButton = document.querySelector<HTMLButtonElement>('#play')!;
const cancelJob = document.querySelector<HTMLButtonElement>('#cancel-job')!;
const hideWhilePlaying = document.querySelector<HTMLInputElement>('#hide-while-playing')!;
const savesControl = document.querySelector<HTMLElement>('#saves-control')!;
const savesButton = document.querySelector<HTMLButtonElement>('#saves-button')!;
const savesPanel = document.querySelector<HTMLElement>('#saves-panel')!;
const saveDestination = document.querySelector<HTMLElement>('#save-export-destination')!;
const chooseSaveExport = document.querySelector<HTMLButtonElement>('#choose-save-export')!;
const includeSaveReport = document.querySelector<HTMLInputElement>('#include-save-report')!;
const exportSaveButton = document.querySelector<HTMLButtonElement>('#export-save')!;
const saveResult = document.querySelector<HTMLElement>('#save-result')!;
const importSaveButton = document.querySelector<HTMLButtonElement>('#import-save')!;
const importSaveFolder = document.querySelector<HTMLButtonElement>('#import-save-folder')!;
const openSavesDir = document.querySelector<HTMLButtonElement>('#open-saves-dir')!;
const pendingSaveImport = document.querySelector<HTMLElement>('#pending-save-import')!;
const modsControl = document.querySelector<HTMLElement>('#mods-control')!;
const modsButton = document.querySelector<HTMLButtonElement>('#mods-button')!;
const modsPanel = document.querySelector<HTMLElement>('#mods-panel')!;
const modsList = document.querySelector<HTMLOListElement>('#mods-list')!;
const modsEmpty = document.querySelector<HTMLElement>('#mods-empty')!;
const openModsDir = document.querySelector<HTMLButtonElement>('#open-mods-dir')!;
const addModFolder = document.querySelector<HTMLButtonElement>('#add-mod-folder')!;
const addModArchive = document.querySelector<HTMLButtonElement>('#add-mod-archive')!;
const modsPagination = document.querySelector<HTMLElement>('#mods-pagination')!;
const previousModPage = document.querySelector<HTMLButtonElement>('#previous-mod-page')!;
const nextModPage = document.querySelector<HTMLButtonElement>('#next-mod-page')!;
const modsPageNumber = document.querySelector<HTMLElement>('#mods-page-number')!;
const modsResult = document.querySelector<HTMLElement>('#mods-result')!;
const steamControl = document.querySelector<HTMLElement>('#steam-control')!;
const steamButton = document.querySelector<HTMLButtonElement>('#steam-button')!;
const steamPanel = document.querySelector<HTMLElement>('#steam-panel')!;
const steamStatus = document.querySelector<HTMLElement>('#steam-status')!;
const steamRequirement = document.querySelector<HTMLElement>('#steam-requirement')!;
const steamAdd = document.querySelector<HTMLButtonElement>('#steam-add')!;
const steamResult = document.querySelector<HTMLElement>('#steam-result')!;
const settingsControl = document.querySelector<HTMLElement>('#settings-control')!;
const settingsButton = document.querySelector<HTMLButtonElement>('#settings-button')!;
const settingsPanel = document.querySelector<HTMLElement>('#settings-panel')!;
const settingsFields = document.querySelector<HTMLFieldSetElement>('#settings-fields')!;
const settingsChannel = document.querySelector<HTMLSelectElement>('#settings-channel')!;
const settingsChecks = {
  auto_check: document.querySelector<HTMLInputElement>('#settings-auto-check')!,
  auto_install: document.querySelector<HTMLInputElement>('#settings-auto-install')!,
  keep_cache: document.querySelector<HTMLInputElement>('#settings-keep-cache')!,
  warn_low_commit: document.querySelector<HTMLInputElement>('#settings-warn-commit')!,
};
const settingsCommitOption = document.querySelector<HTMLElement>('#settings-commit-option')!;
const settingsGameDir = document.querySelector<HTMLElement>('#settings-game-dir')!;
const settingsDumpName = document.querySelector<HTMLElement>('#settings-dump-name')!;
const settingsDump = document.querySelector<HTMLButtonElement>('#settings-dump')!;
const settingsOpenGame = document.querySelector<HTMLButtonElement>('#settings-open-game')!;
const settingsOpenLogs = document.querySelector<HTMLButtonElement>('#settings-open-logs')!;
const settingsResult = document.querySelector<HTMLElement>('#settings-result')!;
const updateControl = document.querySelector<HTMLElement>('#update-control')!;
const checkUpdatesButton = document.querySelector<HTMLButtonElement>('#check-updates')!;
const updateResult = document.querySelector<HTMLElement>('#update-result')!;
const updateNotes = document.querySelector<HTMLElement>('#update-notes')!;
const settingError = document.querySelector<HTMLElement>('#setting-error')!;
const installButton = document.querySelector<HTMLButtonElement>('#begin-install')!;
const recoveryControls = document.querySelector<HTMLElement>('#recovery-controls')!;
const waitBuild = document.querySelector<HTMLButtonElement>('#wait-build')!;
const confirmRecovery = document.querySelector<HTMLButtonElement>('#confirm-recovery')!;
const stopBuild = document.querySelector<HTMLButtonElement>('#stop-build')!;
const firstRunRequirements = document.querySelector<HTMLElement>('#first-run-requirements')!;
const firstRun = document.querySelector<HTMLElement>('#first-run')!;
const gameFolderSpace = document.querySelector<HTMLElement>('#game-folder-space')!;
const gameFolderProposal = document.querySelector<HTMLElement>('#game-folder-proposal')!;
const pickGameFolder = document.querySelector<HTMLButtonElement>('#pick-game-folder')!;
const gameFolderError = document.querySelector<HTMLElement>('#game-folder-error')!;
const gameDumpProposal = document.querySelector<HTMLElement>('#game-dump-proposal')!;
const pickGameDump = document.querySelector<HTMLButtonElement>('#pick-game-dump')!;
const gameDumpError = document.querySelector<HTMLElement>('#game-dump-error')!;
const firstRunOptions = document.querySelector<HTMLFieldSetElement>('#first-run-preferences')!;
const firstRunChannel = document.querySelector<HTMLSelectElement>('#first-run-channel')!;
const firstRunAutoCheck = document.querySelector<HTMLInputElement>('#first-run-auto-check')!;
const firstRunDesktop = document.querySelector<HTMLInputElement>('#first-run-desktop')!;
const firstRunDesktopOption = document.querySelector<HTMLElement>('#first-run-desktop-option')!;
const firstRunSettingsError = document.querySelector<HTMLElement>('#first-run-settings-error')!;
const steamLabels: Record<string, string> = {
  added: 'Atalho disponível na Steam',
  available: 'Steam encontrada',
  flatpak_unsupported: 'Use a Steam nativa. O sandbox padrão do Flatpak impede o acesso ao launcher.',
  not_found: 'Steam não encontrada.',
  not_built: 'O jogo precisa estar compilado para receber um atalho.',
  unavailable: 'Não foi possível verificar a Steam.',
};
const messages: Record<string, string> = { ready: 'Instalação encontrada', not_ready: 'Jogo não está pronto', not_configured: 'Jogo não configurado', inspection_error: 'Não foi possível verificar' };
let installation: Result | null = null;
let selecting = false;
let playing = false;
let gameWindowReady = false;
let savesBusy = false;
let saveOptions: SaveOptions | null = null;
let modPage: ModPage | null = null;
let modsBusy = false;
let steamView: SteamView | null = null;
let steamBusy = false;
let launcherSettings: InstalledSettings | null = null;
let settingsBusy = false;
let savingPreference = false;
let playFailed = false;
let choosingSource = false;
let checkingUpdates = false;
let recoveryAction: 'wait' | 'stop' | null = null;
let askingRecovery = false;
let proposedFolder: InstallDestination | null = null;
let proposedDump: string | null = null;

function updateControls() {
  const busy = selecting || playing || savingPreference || choosingSource || savesBusy || modsBusy || steamBusy || settingsBusy;
  button.disabled = busy || askingRecovery || savesBusy;
  savesControl.hidden = installation?.status !== 'ready';
  const savesDisabled = selecting || savingPreference || choosingSource || askingRecovery || savesBusy || modsBusy || steamBusy || settingsBusy ||
    (playing && !gameWindowReady) || installation?.job_marker_present === true;
  savesButton.disabled = chooseSaveExport.disabled = includeSaveReport.disabled = exportSaveButton.disabled = savesDisabled;
  openSavesDir.disabled = savesDisabled;
  importSaveButton.disabled = importSaveFolder.disabled = savesDisabled || playing || installation?.game_running === true;
  savesPanel.hidden = !saveOptions;
  savesButton.setAttribute('aria-expanded', String(!savesPanel.hidden));
  reportSettingsButton.disabled = busy || installation?.job_marker_present === true;
  if (saveOptions) {
    saveDestination.textContent = saveOptions.destination;
    includeSaveReport.checked = saveOptions.with_log;
    pendingSaveImport.hidden = !saveOptions.pending;
    importSaveButton.textContent = saveOptions.waiting ? 'Importar save da pasta' : 'Importar save';
  }
  modsControl.hidden = installation?.status !== 'ready';
  modsPanel.hidden = !modPage;
  modsButton.setAttribute('aria-expanded', String(!modsPanel.hidden));
  const modsDisabled = savesDisabled || modsControl.hidden;
  modsButton.disabled = openModsDir.disabled = addModFolder.disabled = addModArchive.disabled = modsDisabled;
  previousModPage.disabled = modsDisabled || !modPage || modPage.page === 0;
  nextModPage.disabled = modsDisabled || !modPage || modPage.page + 1 === modPage.pages;
  const orderDisabled = modsDisabled || playing || installation?.game_running === true;
  for (const control of modsList.querySelectorAll<HTMLInputElement | HTMLButtonElement>('input, button')) {
    control.disabled = orderDisabled || control.hasAttribute('data-at-boundary');
  }
  steamControl.hidden = installation?.status !== 'ready';
  steamPanel.hidden = !steamView;
  steamButton.setAttribute('aria-expanded', String(!steamPanel.hidden));
  steamButton.disabled = savesDisabled || steamControl.hidden;
  steamAdd.disabled = steamButton.disabled || (steamView?.status !== 'available' && steamView?.status !== 'added');
  steamAdd.textContent = steamView?.status === 'added' ? 'Atualizar atalho da Steam' : 'Adicionar à Steam';
  if (steamView) {
    steamStatus.textContent = steamLabels[steamView.status] || steamLabels.unavailable;
    steamRequirement.textContent = steamView.status === 'available' || steamView.status === 'added'
      ? (steamView.linux ? 'Feche a Steam para alterar o atalho. Você pode reabri-la ao terminar.' :
        'A Steam será reiniciada se estiver aberta.') : '';
  }
  settingsControl.hidden = installation?.status !== 'ready';
  settingsPanel.hidden = !launcherSettings;
  settingsButton.setAttribute('aria-expanded', String(!settingsPanel.hidden));
  const settingsOpeningDisabled = selecting || settingsBusy || savingPreference || choosingSource || savesBusy || modsBusy || steamBusy;
  settingsButton.disabled = settingsOpeningDisabled;
  settingsOpenLogs.disabled = settingsOpeningDisabled;
  settingsOpenGame.disabled = settingsOpeningDisabled || !launcherSettings?.game_dir;
  settingsFields.disabled = settingsDump.disabled = busy || !launcherSettings?.writable || installation?.job_marker_present === true;
  settingsCommitOption.hidden = launcherSettings?.warn_low_commit == null;
  if (launcherSettings && !settingsBusy) {
    settingsChannel.value = launcherSettings.channel;
    settingsGameDir.textContent = launcherSettings.game_dir || 'Não escolhida';
    settingsDumpName.textContent = launcherSettings.dump || 'Nenhum dump registrado';
    for (const key of Object.keys(settingsChecks) as (keyof typeof settingsChecks)[]) {
      settingsChecks[key].checked = launcherSettings[key] === true;
    }
  }
  updateControl.hidden = installation?.status !== 'ready';
  checkUpdatesButton.disabled = checkingUpdates || busy || installation?.job_marker_present === true;
  const previousBuild = installation?.recovery?.state;
  recoveryControls.hidden = previousBuild !== 'alive' && previousBuild !== 'unverified';
  stopBuild.hidden = previousBuild !== 'alive';
  confirmRecovery.hidden = previousBuild !== 'unverified';
  confirmRecovery.disabled = busy || askingRecovery;
  waitBuild.disabled = busy || askingRecovery || recoveryAction !== null;
  stopBuild.disabled = busy || askingRecovery || recoveryAction === 'stop';
  playButton.disabled = busy || playFailed || installation?.status !== 'ready' ||
    installation.job_marker_present === true || installation.game_running === true;
  rebuildButton.hidden = installation?.status !== 'ready';
  rebuildButton.disabled = busy || rebuildButton.hidden ||
    installation?.job_marker_present === true || installation?.game_running === true;
  hideWhilePlaying.disabled = busy || typeof installation?.hide_while_playing !== 'boolean';
  pickGameFolder.disabled = pickGameDump.disabled = busy || firstRun.hidden;
  installButton.disabled = busy || firstRun.hidden || !proposedFolder || !proposedDump || proposedFolder.pathTooLong ||
    installation?.job_marker_present === true || installation?.game_running === true;
  const requirements = installation?.first_run_requirements;
  let notice = '';
  if (requirements?.status === 'runtime_zstd_missing') {
    notice = 'O runtime do launcher está sem suporte a Zstandard. Repare a instalação pelo launcher atual.';
  } else if (requirements?.status === 'linux_tools_missing') {
    notice = requirements.message ?? '';
  } else if (requirements?.status === 'linux_check_failed') {
    notice = 'Não foi possível verificar as ferramentas de compilação no Linux.';
  }
  firstRunRequirements.textContent = notice;
  firstRunRequirements.hidden = !notice;
  const preferences = installation?.first_run_preferences;
  firstRunOptions.hidden = !preferences;
  firstRunOptions.disabled = busy;
  firstRunDesktopOption.hidden = preferences?.desktop_shortcut == null;
  if (!savingPreference) {
    hideWhilePlaying.checked = installation?.hide_while_playing === true;
    firstRunChannel.value = preferences?.channel ?? 'stable';
    firstRunAutoCheck.checked = preferences?.auto_check === true;
    firstRunDesktop.checked = preferences?.desktop_shortcut === true;
  }
}

function showResult(result: Result | null) {
  if (!result) { statusElement.textContent = 'Nenhuma instalação selecionada'; detail.textContent = 'Use o instalador atual do NX2Saria para uma primeira instalação ou selecione a pasta de uma instalação existente.'; return; }
  statusElement.textContent = messages[result.status] || 'Estado desconhecido';
  if (result.reason === 'worker_busy') { detail.textContent = 'Outra instância está usando a instalação. Se o jogo ainda estiver aberto, aguarde ele terminar.'; return; }
  if (result.reason === 'installation_required') { detail.textContent = 'Use o instalador atual do NX2Saria e selecione a pasta da instalação.'; return; }
  if (result.reason === 'runtime_incompatible') { detail.textContent = 'Atualize ou repare esta instalação pelo launcher atual do NX2Saria e tente novamente.'; return; }
  if (result.recovery?.state === 'alive') {
    statusElement.textContent = 'Uma compilação anterior ainda está rodando';
    detail.textContent = 'Aguarde a compilação terminar antes de jogar, instalar ou recompilar.';
    return;
  }
  if (result.recovery?.state === 'unverified') {
    statusElement.textContent = 'Não foi possível verificar a compilação anterior';
    detail.textContent = 'Há um registro de um trabalho interrompido. A instalação fica bloqueada enquanto esse trabalho puder estar em andamento.';
    return;
  }
  if (result.recovery?.state === 'interrupted') {
    detail.textContent = result.status === 'ready'
      ? 'A compilação anterior não terminou. Jogar abre a última versão pronta. Recompilar retoma o trabalho.'
      : 'A instalação anterior não terminou. Iniciar instalação retoma o trabalho das etapas concluídas.';
    return;
  }
  detail.textContent = (result.game_dir || result.root || 'Confira a instalação e tente novamente.') + (result.job_marker_present ? ' Há um registro de trabalho: ele pode estar em andamento ou ter sido interrompido.' : '') + (result.game_running === true ? ' O jogo já está em execução.' : '');
}
async function showInstallDestination(pick: boolean) {
  choosingSource = true;
  updateControls();
  gameFolderError.textContent = '';
  try {
    const proposal = await window.launcher.installDestination(pick);
    if (proposal) {
      proposedFolder = proposal;
      gameFolderProposal.textContent = proposal.folder;
      const note = proposal.completeImage ? 'A imagem do jogo já está pronta.' : `A instalação precisa de cerca de ${proposal.required} GB.`;
      gameFolderSpace.textContent = `${proposal.available} GB disponíveis. ${note}`;
    }
  } catch {
    gameFolderError.textContent = 'Não foi possível verificar a pasta do jogo.';
  } finally {
    if (proposedFolder?.pathTooLong && !gameFolderError.textContent) {
      gameFolderError.textContent = 'O caminho é longo demais para as ferramentas do Windows. Escolha uma pasta com um caminho mais curto.';
    }
    choosingSource = false;
    updateControls();
  }
}
pickGameFolder.onclick = () => { void showInstallDestination(true); };

async function showInstallDump(pick: boolean) {
  choosingSource = true;
  updateControls();
  gameDumpError.textContent = '';
  try {
    const file = await window.launcher.installDump(pick);
    if (file || !pick) {
      proposedDump = file;
      gameDumpProposal.textContent = file ?? 'Nenhum dump escolhido';
    }
  } catch {
    gameDumpError.textContent = 'O arquivo selecionado não é um dump válido do jogo.';
  } finally {
    choosingSource = false;
    updateControls();
  }
}
pickGameDump.onclick = () => { void showInstallDump(true); };

async function searchForDump(selected: Result) {
  gameDumpProposal.textContent = 'Procurando o dump do jogo…';
  while (installation === selected && !proposedDump && !firstRun.hidden) {
    try {
      const result = await window.launcher.searchInstallDump();
      if (installation !== selected || proposedDump || firstRun.hidden) return;
      if (result.status === 'complete') {
        proposedDump = result.file;
        gameDumpProposal.textContent = proposedDump ?? 'Nenhum dump encontrado';
        updateControls();
        return;
      }
    } catch {
      if (installation === selected && !proposedDump && !firstRun.hidden) gameDumpProposal.textContent = 'Escolha o dump do jogo';
      return;
    }
    await new Promise(resolve => setTimeout(resolve, 300));
  }
}

async function loadSelection(action: () => Promise<Result | null>) {
  selecting = true;
  installation = null;
  playFailed = false;
  saveOptions = null;
  saveResult.textContent = '';
  modPage = null;
  modsList.replaceChildren();
  modsResult.textContent = '';
  steamView = null;
  steamResult.textContent = '';
  launcherSettings = null;
  settingsResult.textContent = '';
  resetReportScreen();
  checkingUpdates = false;
  updateResult.textContent = updateNotes.textContent = '';
  updateNotes.hidden = true;
  recoveryAction = null;
  settingError.textContent = firstRunSettingsError.textContent = '';
  proposedFolder = proposedDump = null;
  gameFolderProposal.textContent = gameFolderSpace.textContent = '';
  gameDumpProposal.textContent = '';
  firstRun.hidden = true;
  updateControls();
  try {
    installation = await action();
    showResult(installation);
    if (installation?.status === 'ready' && !installation.job_marker_present) await loadReportsHome('');
    if (installation?.status === 'not_configured' || installation?.status === 'not_ready') {
      firstRun.hidden = false;
      await showInstallDestination(false);
      await showInstallDump(false);
      if (!proposedDump) void searchForDump(installation);
    }
  }
  catch { statusElement.textContent = 'Não foi possível verificar'; detail.textContent = 'Tente novamente.'; }
  finally { selecting = false; updateControls(); }
}
button.onclick = () => { void loadSelection(() => window.launcher.selectInstallation()); };
void loadSelection(() => window.launcher.discoverInstallation());

async function recoverBuild(action: RecoveryAction) {
  const previous = installation;
  askingRecovery = true;
  updateControls();
  try {
    const result = await window.launcher.recoverBuild(action);
    if (installation !== previous) return;
    if (result === 'waiting' || result === 'stopping') {
      recoveryAction = result === 'waiting' ? 'wait' : 'stop';
      detail.textContent = result === 'waiting' ? 'Aguardando a compilação anterior terminar…' : 'Parando a compilação anterior…';
    } else if (result === 'unavailable') {
      detail.textContent = 'Não foi possível executar essa ação. O registro da compilação foi mantido.';
    }
  } catch {
    if (installation === previous) detail.textContent = 'Não foi possível verificar a compilação anterior.';
  } finally {
    askingRecovery = false;
    updateControls();
  }
}
waitBuild.onclick = () => { void recoverBuild('wait'); };
stopBuild.onclick = () => { void recoverBuild('stop'); };
confirmRecovery.onclick = () => { void recoverBuild('confirm'); };
window.launcher.onRecoveryChanged(state => {
  installation = state;
  if (state?.recovery?.state !== 'alive' && state?.recovery?.state !== 'unverified') recoveryAction = null;
  firstRun.hidden = state?.status !== 'not_ready' && state?.status !== 'not_configured';
  showResult(state);
  if (state?.status === 'ready' && !state.job_marker_present) void loadReportsHome('');
  updateControls();
});

hideWhilePlaying.onchange = async () => {
  const selected = installation;
  if (!selected || hideWhilePlaying.disabled) return;
  const value = hideWhilePlaying.checked;
  savingPreference = true;
  settingError.textContent = '';
  updateControls();
  try {
    selected.hide_while_playing = await window.launcher.setHideWhilePlaying(value);
  } catch {
    settingError.textContent = 'Não foi possível salvar a opção de ocultar. Tente novamente.';
  } finally {
    savingPreference = false;
    updateControls();
  }
};

async function saveFirstRunPreference(key: keyof FirstRunPreferences, value: string | boolean) {
  const selected = installation;
  const preferences = selected?.first_run_preferences;
  if (!selected || !preferences || firstRunOptions.disabled) return;
  savingPreference = true;
  firstRunSettingsError.textContent = '';
  updateControls();
  try {
    const saved = await window.launcher.setFirstRunPreference(key, value);
    selected.first_run_preferences = { ...preferences, [key]: saved };
  } catch {
    firstRunSettingsError.textContent = 'Não foi possível salvar a preferência. Tente novamente.';
  } finally {
    savingPreference = false;
    updateControls();
  }
}
firstRunChannel.onchange = () => { void saveFirstRunPreference('channel', firstRunChannel.value); };
firstRunAutoCheck.onchange = () => { void saveFirstRunPreference('auto_check', firstRunAutoCheck.checked); };
firstRunDesktop.onchange = () => { void saveFirstRunPreference('desktop_shortcut', firstRunDesktop.checked); };

async function runSaveAction(action: () => Promise<void>, error: string) {
  savesBusy = true;
  saveResult.textContent = '';
  updateControls();
  try { await action(); } catch { saveResult.textContent = error; }
  finally { savesBusy = false; updateControls(); }
}

savesButton.onclick = () => {
  if (saveOptions) {
    saveOptions = null;
    saveResult.textContent = '';
    updateControls();
    return;
  }
  void runSaveAction(async () => { saveOptions = await window.launcher.savesInfo(); }, 'Não foi possível abrir os saves.');
};
chooseSaveExport.onclick = () => {
  const options = saveOptions;
  if (!options) return;
  void runSaveAction(async () => {
    const destination = await window.launcher.chooseSaveExport();
    if (destination) options.destination = destination;
  }, 'Não foi possível salvar a pasta de exportação.');
};
includeSaveReport.onchange = () => {
  const options = saveOptions;
  if (!options) return;
  const checked = includeSaveReport.checked;
  void runSaveAction(async () => { options.with_log = await window.launcher.setSaveReport(checked); },
    'Não foi possível salvar a opção de diagnóstico.');
};
exportSaveButton.onclick = () => {
  void runSaveAction(async () => {
    const result = await window.launcher.exportSave();
    if (result.status === 'no_save') {
      saveResult.textContent = 'O jogo ainda não tem um save. Jogue até salvar e tente novamente.';
    } else if (result.status === 'ok') {
      saveResult.textContent = result.files
        ? `Save exportado em ${result.name} (${result.files} arquivo(s)).`
        : `O jogo ainda não tem save. O diagnóstico foi exportado em ${result.name}.`;
    } else {
      saveResult.textContent = 'Não foi possível exportar o save. Confira o log do launcher.';
    }
  }, 'Não foi possível exportar o save.');
};

function importSave(source: 'archive' | 'folder') {
  void runSaveAction(async () => {
    const result = await window.launcher.importSave(source);
    if (!result) return;
    if (result.status === 'ok') {
      const imported = result.slot ? `no slot ${result.slot.slice(-2)}` : `(${result.files} arquivo(s))`;
      const backup = result.backup ? ` O save anterior está em ${result.backup}, na pasta dos saves.` : '';
      saveResult.textContent = `Save importado ${imported}.${backup}`;
      saveOptions = await window.launcher.savesInfo();
    } else if (result.status === 'game_running') {
      saveResult.textContent = 'Feche o jogo antes de importar um save.';
    } else if (result.detail) {
      saveResult.textContent = `Não foi possível importar o save. ${result.detail}`;
    } else {
      saveResult.textContent = 'Não foi possível importar o save. Confira o log do launcher.';
    }
  }, 'Não foi possível importar o save.');
}
importSaveButton.onclick = () => importSave('archive');
importSaveFolder.onclick = () => importSave('folder');
openSavesDir.onclick = () => {
  void runSaveAction(async () => {
    await window.launcher.openSavesDir();
    saveResult.textContent = 'Pasta dos saves aberta.';
  }, 'Não foi possível abrir a pasta dos saves.');
};

function renderMods(page: ModPage) {
  modPage = page;
  modsEmpty.hidden = page.count !== 0;
  modsPagination.hidden = page.pages < 2;
  modsPageNumber.textContent = `Página ${page.page + 1} de ${page.pages}`;
  modsList.start = page.page * 6 + 1;
  modsList.replaceChildren();
  page.items.forEach((mod, index) => {
    const row = document.createElement('li');
    const label = document.createElement('label');
    if (mod.usable) {
      const toggle = document.createElement('input');
      toggle.type = 'checkbox';
      toggle.checked = mod.enabled;
      toggle.setAttribute('aria-label', mod.name);
      toggle.onchange = () => {
        const checked = toggle.checked;
        toggle.checked = mod.enabled;
        void runModAction(() => window.launcher.modEnabled(mod.name, checked));
      };
      label.append(toggle, ' ');
    }
    label.append(mod.name);
    row.append(label);
    for (const direction of [-1, 1] as const) {
      const move = document.createElement('button');
      move.textContent = direction === -1 ? 'Subir' : 'Descer';
      move.setAttribute('aria-label', `${move.textContent} ${mod.name}`);
      const position = page.page * 6 + index;
      if (position + direction < 0 || position + direction >= page.count) move.setAttribute('data-at-boundary', '');
      move.onclick = () => { void runModAction(() => window.launcher.modMove(mod.name, direction)); };
      row.append(' ', move);
    }
    const details = [
      `${mod.files} arquivo(s), ${mod.patches} patch(es)${mod.cheats ? ', códigos de trapaça' : ''}.`,
      mod.other_version ? `${mod.other_version} patch(es) de outra versão.` : '',
      mod.waiting ? 'Jogar recompila o jogo com este mod.' : '',
      mod.problem,
      ...mod.overlaps.map(conflict =>
        `${conflict.files} arquivo(s) em conflito com ${conflict.name}. ${conflict.wins ? mod.name : conflict.name} vence.`),
    ];
    const info = document.createElement('p');
    info.textContent = details.filter(Boolean).join(' ');
    row.append(info);
    modsList.append(row);
  });
}

async function loadMods(page: number) {
  modsBusy = true;
  modsResult.textContent = '';
  updateControls();
  try { renderMods(await window.launcher.modsList(page)); }
  catch { modsResult.textContent = 'Não foi possível listar os mods.'; }
  finally { modsBusy = false; updateControls(); }
}

async function runModAction(action: () => Promise<ModAction | null>) {
  modsBusy = true;
  modsResult.textContent = '';
  updateControls();
  try {
    const result = await action();
    if (!result) return;
    if (result.status === 'ok') {
      renderMods(await window.launcher.modsList(result.page ?? modPage?.page ?? 0));
      if (result.name) modsResult.textContent = `Mod adicionado: ${result.name}.${result.problem ? ' ' + result.problem : ''}`;
    } else if (result.status === 'game_running') {
      modsResult.textContent = 'Feche o jogo antes de alterar a ordem ou ativar um mod.';
    } else {
      modsResult.textContent = result.detail || 'Não foi possível alterar os mods. Confira o log do launcher.';
    }
  } catch { modsResult.textContent = 'Não foi possível atualizar os mods. Confira o log do launcher.'; }
  finally { modsBusy = false; updateControls(); }
}

modsButton.onclick = () => {
  if (!modPage) { void loadMods(0); return; }
  modPage = null;
  modsResult.textContent = '';
  updateControls();
};
previousModPage.onclick = () => { if (modPage) void loadMods(modPage.page - 1); };
nextModPage.onclick = () => { if (modPage) void loadMods(modPage.page + 1); };
openModsDir.onclick = () => { void runModAction(() => window.launcher.openModsDir()); };
addModFolder.onclick = () => { void runModAction(() => window.launcher.modAdd('folder')); };
addModArchive.onclick = () => { void runModAction(() => window.launcher.modAdd('archive')); };

async function runSteamAction(action: () => Promise<void>, progress: string) {
  steamBusy = true;
  steamResult.textContent = progress;
  updateControls();
  try { await action(); }
  catch { steamResult.textContent = 'Não foi possível concluir. Confira o log do launcher.'; }
  finally { steamBusy = false; updateControls(); }
}
steamButton.onclick = () => {
  if (steamView) {
    steamView = null;
    steamResult.textContent = '';
    updateControls();
    return;
  }
  void runSteamAction(async () => {
    steamView = await window.launcher.steamInfo();
    steamResult.textContent = '';
  }, 'Verificando a Steam…');
};
steamAdd.onclick = () => {
  void runSteamAction(async () => {
    const result = await window.launcher.steamAdd();
    steamView = result.view;
    steamResult.textContent = result.status === 'ok'
      ? 'Atalho salvo na Steam.' : result.detail || steamLabels[result.status] || steamLabels.unavailable;
  }, 'Salvando o atalho na Steam…');
};

async function runSettingsAction(action: () => Promise<void>, failure: string) {
  settingsBusy = true;
  settingsResult.textContent = '';
  updateControls();
  try { await action(); } catch { settingsResult.textContent = failure; }
  finally { settingsBusy = false; updateControls(); }
}

settingsButton.onclick = () => {
  if (launcherSettings) {
    launcherSettings = null;
    settingsResult.textContent = '';
    updateControls();
    return;
  }
  void runSettingsAction(async () => { launcherSettings = await window.launcher.settingsInfo(); },
    'Não foi possível abrir as configurações.');
};

function changeInstalledSetting(key: InstalledKey, value: boolean | string) {
  const options = launcherSettings;
  if (!options) return;
  void runSettingsAction(async () => {
    const saved = await window.launcher.settingsPreference(key, value);
    if (key === 'channel') {
      if (saved !== 'stable' && saved !== 'preview') throw new Error('Canal inválido');
      options.channel = saved;
      updateResult.textContent = updateNotes.textContent = '';
      updateNotes.hidden = true;
    } else {
      if (typeof saved !== 'boolean') throw new Error('Preferência inválida');
      options[key] = saved;
    }
  }, 'Não foi possível salvar a configuração.');
}
settingsChannel.onchange = () => changeInstalledSetting('channel', settingsChannel.value);
for (const key of Object.keys(settingsChecks) as (keyof typeof settingsChecks)[]) {
  settingsChecks[key].onchange = () => changeInstalledSetting(key, settingsChecks[key].checked);
}
settingsDump.onclick = () => {
  const options = launcherSettings;
  if (!options) return;
  void runSettingsAction(async () => {
    const result = await window.launcher.settingsDump();
    if (!result) return;
    if (result.status === 'ok' && result.name) {
      options.dump = result.name;
      settingsResult.textContent = 'Dump salvo. Ele será usado na próxima recompilação.';
    } else {
      settingsResult.textContent = result.status === 'invalid_dump'
        ? 'O arquivo escolhido não é um dump válido de Ocarina of Time.'
        : 'Não foi possível salvar o dump.';
    }
  }, 'Não foi possível salvar o dump.');
};
function openSettingsFolder(target: 'game' | 'logs') {
  void runSettingsAction(async () => {
    await window.launcher.settingsOpen(target);
    settingsResult.textContent = target === 'game' ? 'Pasta do jogo aberta.' : 'Pasta dos logs aberta.';
  }, 'Não foi possível abrir a pasta.');
}
settingsOpenGame.onclick = () => openSettingsFolder('game');
settingsOpenLogs.onclick = () => openSettingsFolder('logs');

const updateMessages: Record<string, string> = {
  up_to_date: 'O launcher está atualizado.',
  no_release: 'Nenhuma versão encontrada para este canal.',
  failed: 'Não foi possível buscar atualizações. Confira o log do launcher.',
  superseded: 'O canal mudou. Busque atualizações novamente.',
  recent_check: 'O launcher já buscou atualizações há menos de um minuto.',
  unavailable: 'A busca por atualizações está indisponível agora.',
};

checkUpdatesButton.onclick = async () => {
  const selected = installation;
  checkingUpdates = true;
  updateResult.textContent = 'Buscando atualizações…';
  updateNotes.hidden = true;
  updateControls();
  let status = 'unavailable';
  try { status = await window.launcher.checkUpdates(); } catch {}
  if (selected !== installation || status === 'checking') return;
  checkingUpdates = false;
  updateResult.textContent = updateMessages[status] || updateMessages.unavailable;
  updateControls();
};
window.launcher.onUpdateChecked(update => {
  checkingUpdates = false;
  updateResult.textContent = update.status === 'available'
    ? `Versão ${update.version} disponível` : updateMessages[update.status] || updateMessages.unavailable;
  updateNotes.textContent = update.notes?.join('\n') ?? '';
  updateNotes.hidden = !updateNotes.textContent;
  updateControls();
});

function showPlayFeedback(result: PlayFeedback) {
  let kind = 'could_not_start';
  let title = 'Não foi possível iniciar';
  let description = 'Confira o estado da instalação e se há outro trabalho em andamento.';
  if (result.outcome === 'game_ended') {
    const failed = Boolean(result.alarm || (typeof result.code === 'number' && result.code !== 0));
    kind = failed ? 'ended_with_error' : 'game_ended';
    title = failed ? 'O jogo terminou com erro' : 'Fluxo de jogo encerrado';
    description = failed ? result.message || 'Confira os logs do launcher para identificar o problema.'
      : 'Confira os logs para o resultado. Você pode jogar novamente quando a instalação estiver pronta.';
  } else if (result.outcome === 'monitor_lost') {
    kind = 'monitor_lost';
    title = 'Não foi possível acompanhar o jogo';
    description = 'Confira os logs do launcher para identificar o problema.';
  }
  statusElement.textContent = title;
  detail.textContent = description;
  if (result.messageId !== undefined) window.launcher.playMessageShown(result.messageId, kind);
}

function showOpeningGame() {
  statusElement.textContent = 'Abrindo o jogo…';
  detail.textContent = 'Aguarde a janela do jogo aparecer.';
}

playButton.onclick = async () => {
  playing = true;
  updateControls();
  showOpeningGame();
  try {
    const result = await window.launcher.play();
    if (result.started) return;
    showPlayFeedback(result);
  } catch { showPlayFeedback({}); }
  playing = false;
  playFailed = true;
  updateControls();
};
window.launcher.onGameWindowReady(() => {
  gameWindowReady = true;
  updateControls();
  statusElement.textContent = 'Jogo em execução';
  detail.textContent = '';
});
window.launcher.onPlayEnded(result => {
  gameWindowReady = false;
  cancelJob.hidden = true;
  playing = false;
  installation = result.state ?? null;
  firstRun.hidden = true;
  updateControls();
  showPlayFeedback(result);
  if (result.outcome === 'game_ended' && installation?.status === 'ready') void loadCrashOffer();
  if (modPage && !modsBusy && installation?.status === 'ready' && !installation.job_marker_present) {
    void loadMods(modPage.page);
  }
});

const installMessages: Record<string, string> = {
  choose_source: 'Escolha a pasta do jogo e o dump antes de instalar.',
  invalid_dump: 'O dump escolhido não está mais disponível ou é inválido. Escolha o arquivo novamente.',
  invalid_destination: 'A pasta escolhida está indisponível. Selecione outra pasta.',
  path_too_long: 'Escolha uma pasta com um caminho mais curto.',
  already_installed: 'Já existe um jogo pronto nesta instalação.',
  not_built: 'Instale o jogo antes de recompilar.',
  game_running: 'Feche o jogo antes de instalar.',
  need_space: 'Libere cerca de 34 GB na unidade escolhida ou selecione outra pasta.',
};

const buildLabels = {
  install: { active: 'Instalando o jogo…', done: 'Instalação concluída', cancelled: 'Instalação cancelada',
    refused: 'Não foi possível iniciar a instalação', failed: 'Não foi possível concluir a instalação' },
  rebuild: { active: 'Recompilando o jogo…', done: 'Recompilação concluída', cancelled: 'Recompilação cancelada',
    refused: 'Não foi possível iniciar a recompilação', failed: 'Não foi possível concluir a recompilação' },
};

async function startBuild(action: BuildAction) {
  playing = true;
  updateControls();
  const labels = buildLabels[action];
  statusElement.textContent = labels.active;
  detail.textContent = 'Preparando as ferramentas.';
  let result = 'unavailable';
  try { result = await window.launcher[action](); } catch {}
  if (result === 'building') return;
  playing = false;
  statusElement.textContent = labels.refused;
  detail.textContent = installMessages[result] || 'Confira o estado da instalação e tente novamente.';
  updateControls();
}
installButton.onclick = () => { void startBuild('install'); };
rebuildButton.onclick = () => { void startBuild('rebuild'); };
window.launcher.onBuildEnded(({ action, outcome, state }) => {
  playing = false;
  cancelJob.hidden = true;
  installation = state;
  firstRun.hidden = state?.status !== 'not_configured' && state?.status !== 'not_ready';
  const labels = buildLabels[action];
  if (outcome === 'complete' && state?.status === 'ready') {
    showResult(state);
    statusElement.textContent = labels.done;
  } else {
    statusElement.textContent = outcome === 'cancelled' ? labels.cancelled : labels.failed;
    detail.textContent = outcome === 'cancelled' ? 'Você pode tentar novamente.' : 'Confira o log do launcher e tente novamente.';
  }
  if (outcome === 'complete' && state?.status === 'ready') {
    void loadReportsHome(action === 'install' ? 'installation' : '');
  }
  updateControls();
});

cancelJob.onclick = () => { cancelJob.disabled = true; void window.launcher.cancelJob(); };
window.launcher.onJobStatus(result => {
  cancelJob.hidden = result.status !== 'running';
  cancelJob.disabled = false;
  if (result.status === 'running') {
    statusElement.textContent = result.task && result.task !== 'mods'
      ? buildLabels[result.task].active : 'Preparando os mods para jogar…';
    detail.textContent = typeof result.fraction === 'number' ? `Progresso: ${Math.round(result.fraction * 100)}%.` : 'Aguarde a compilação ou cancele.';
  } else if (result.status === 'resuming') {
    showOpeningGame();
  }
});
