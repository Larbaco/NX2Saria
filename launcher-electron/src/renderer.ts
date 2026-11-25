type Result = { status: string; root?: string; game_dir?: string | null; reason?: string; job_marker_present?: boolean; game_running?: true | null; hide_while_playing?: boolean | null };
type PlayFeedback = { messageId?: number; outcome?: 'game_ended' | 'monitor_lost'; code?: number | null; alarm?: boolean; message?: string; state?: Result };
type InstallFeedback = { outcome: string; state: Result | null };
interface Window { launcher: { discoverInstallation(): Promise<Result | null>; selectInstallation(): Promise<Result | null>; installDestination(pick: boolean): Promise<string | null>; installDump(pick: boolean): Promise<string | null>; searchInstallDump(): Promise<{ status: 'searching' | 'complete'; file: string | null }>; install(): Promise<string>; onInstallEnded(callback: (result: InstallFeedback) => void): void; setHideWhilePlaying(value: boolean): Promise<boolean>; play(): Promise<{ started?: boolean; building?: boolean; error?: string; messageId?: number }>; cancelJob(): Promise<unknown>; onJobStatus(callback: (result: { status: string; task?: 'install' | 'mods'; fraction?: number }) => void): void; onGameWindowReady(callback: () => void): void; onPlayEnded(callback: (result: PlayFeedback) => void): void; playMessageShown(id: number, kind: string): void } }
const statusElement = document.querySelector<HTMLElement>('#status')!;
const detail = document.querySelector<HTMLElement>('#detail')!;
const button = document.querySelector<HTMLButtonElement>('#select')!;
const playButton = document.querySelector<HTMLButtonElement>('#play')!;
const cancelJob = document.querySelector<HTMLButtonElement>('#cancel-job')!;
const hideWhilePlaying = document.querySelector<HTMLInputElement>('#hide-while-playing')!;
const settingError = document.querySelector<HTMLElement>('#setting-error')!;
const installButton = document.querySelector<HTMLButtonElement>('#begin-install')!;
const firstRun = document.querySelector<HTMLElement>('#first-run')!;
const gameFolderProposal = document.querySelector<HTMLElement>('#game-folder-proposal')!;
const pickGameFolder = document.querySelector<HTMLButtonElement>('#pick-game-folder')!;
const gameFolderError = document.querySelector<HTMLElement>('#game-folder-error')!;
const gameDumpProposal = document.querySelector<HTMLElement>('#game-dump-proposal')!;
const pickGameDump = document.querySelector<HTMLButtonElement>('#pick-game-dump')!;
const gameDumpError = document.querySelector<HTMLElement>('#game-dump-error')!;
const messages: Record<string, string> = { ready: 'Instalação encontrada', not_ready: 'Jogo não está pronto', not_configured: 'Jogo não configurado', inspection_error: 'Não foi possível verificar' };
let installation: Result | null = null;
let selecting = false;
let playing = false;
let savingPreference = false;
let playFailed = false;
let choosingSource = false;
let proposedFolder: string | null = null;
let proposedDump: string | null = null;

function updateControls() {
  const busy = selecting || playing || savingPreference || choosingSource;
  button.disabled = busy;
  playButton.disabled = busy || playFailed || installation?.status !== 'ready' ||
    installation.job_marker_present === true || installation.game_running === true;
  hideWhilePlaying.disabled = busy || typeof installation?.hide_while_playing !== 'boolean';
  pickGameFolder.disabled = pickGameDump.disabled = busy || firstRun.hidden;
  installButton.disabled = busy || firstRun.hidden || !proposedFolder || !proposedDump ||
    installation?.job_marker_present === true || installation?.game_running === true;
  if (!savingPreference) hideWhilePlaying.checked = installation?.hide_while_playing === true;
}

function showResult(result: Result | null) {
  if (!result) { statusElement.textContent = 'Nenhuma instalação selecionada'; detail.textContent = 'Use o instalador atual do NX2Saria para uma primeira instalação ou selecione a pasta de uma instalação existente.'; return; }
  statusElement.textContent = messages[result.status] || 'Estado desconhecido';
  if (result.reason === 'worker_busy') { detail.textContent = 'Outra instância está usando a instalação. Se o jogo ainda estiver aberto, aguarde ele terminar.'; return; }
  if (result.reason === 'installation_required') { detail.textContent = 'Use o instalador atual do NX2Saria e selecione a pasta da instalação.'; return; }
  if (result.reason === 'runtime_incompatible') { detail.textContent = 'Atualize ou repare esta instalação pelo launcher atual do NX2Saria e tente novamente.'; return; }
  detail.textContent = (result.game_dir || result.root || 'Confira a instalação e tente novamente.') + (result.job_marker_present ? ' Há um registro de trabalho: ele pode estar em andamento ou ter sido interrompido.' : '') + (result.game_running === true ? ' O jogo já está em execução.' : '');
}
async function showInstallDestination(pick: boolean) {
  choosingSource = true;
  updateControls();
  gameFolderError.textContent = '';
  try {
    const folder = await window.launcher.installDestination(pick);
    if (folder) gameFolderProposal.textContent = proposedFolder = folder;
  } catch {
    gameFolderError.textContent = 'Não foi possível verificar a pasta do jogo.';
  } finally {
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
  settingError.textContent = '';
  proposedFolder = proposedDump = null;
  gameFolderProposal.textContent = '';
  gameDumpProposal.textContent = '';
  firstRun.hidden = true;
  updateControls();
  try {
    installation = await action();
    showResult(installation);
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
  statusElement.textContent = 'Jogo em execução';
  detail.textContent = '';
});
window.launcher.onPlayEnded(result => {
  cancelJob.hidden = true;
  playing = false;
  installation = result.state ?? null;
  firstRun.hidden = true;
  updateControls();
  showPlayFeedback(result);
});

const installMessages: Record<string, string> = {
  choose_source: 'Escolha a pasta do jogo e o dump antes de instalar.',
  invalid_dump: 'O dump escolhido não está mais disponível ou é inválido. Escolha o arquivo novamente.',
  invalid_destination: 'A pasta escolhida está indisponível. Selecione outra pasta.',
  path_too_long: 'Escolha uma pasta com um caminho mais curto.',
  already_installed: 'Já existe um jogo pronto nesta instalação.',
  game_running: 'Feche o jogo antes de instalar.',
  need_space: 'Libere cerca de 34 GB na unidade escolhida ou selecione outra pasta.',
};

installButton.onclick = async () => {
  playing = true;
  updateControls();
  statusElement.textContent = 'Instalando o jogo…';
  detail.textContent = 'Preparando a instalação.';
  let result = 'unavailable';
  try { result = await window.launcher.install(); } catch {}
  if (result === 'building') return;
  playing = false;
  statusElement.textContent = 'Não foi possível iniciar a instalação';
  detail.textContent = installMessages[result] || 'Confira o estado da instalação e tente novamente.';
  updateControls();
};
window.launcher.onInstallEnded(({ outcome, state }) => {
  playing = false;
  cancelJob.hidden = true;
  installation = state;
  firstRun.hidden = state?.status !== 'not_configured' && state?.status !== 'not_ready';
  if (outcome === 'complete' && state?.status === 'ready') {
    showResult(state);
    statusElement.textContent = 'Instalação concluída';
  } else {
    statusElement.textContent = outcome === 'cancelled' ? 'Instalação cancelada' : 'Não foi possível concluir a instalação';
    detail.textContent = outcome === 'cancelled' ? 'Você pode iniciar a instalação novamente.' : 'Confira o log do launcher e tente novamente.';
  }
  updateControls();
});

cancelJob.onclick = () => { cancelJob.disabled = true; void window.launcher.cancelJob(); };
window.launcher.onJobStatus(result => {
  cancelJob.hidden = result.status !== 'running';
  cancelJob.disabled = false;
  if (result.status === 'running') {
    statusElement.textContent = result.task === 'install' ? 'Instalando o jogo…' : 'Preparando os mods para jogar…';
    detail.textContent = typeof result.fraction === 'number' ? `Progresso: ${Math.round(result.fraction * 100)}%.` : 'Aguarde a compilação ou cancele.';
  } else if (result.status === 'resuming') {
    showOpeningGame();
  }
});
