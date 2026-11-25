type Result = { status: string; root?: string; game_dir?: string | null; reason?: string; job_marker_present?: boolean; game_running?: true | null };
type PlayFeedback = { messageId?: number; outcome?: 'game_ended' | 'monitor_lost'; code?: number | null; alarm?: boolean; message?: string; state?: Result };
interface Window { launcher: { discoverInstallation(): Promise<Result | null>; selectInstallation(): Promise<Result | null>; play(): Promise<{ started?: boolean; building?: boolean; error?: string; messageId?: number }>; cancelJob(): Promise<unknown>; onJobStatus(callback: (result: { status: string; fraction?: number }) => void): void; onPlayEnded(callback: (result: PlayFeedback) => void): void; playMessageShown(id: number, kind: string): void } }
const statusElement = document.querySelector<HTMLElement>('#status')!;
const detail = document.querySelector<HTMLElement>('#detail')!;
const button = document.querySelector<HTMLButtonElement>('#select')!;
const playButton = document.querySelector<HTMLButtonElement>('#play')!;
const cancelJob = document.querySelector<HTMLButtonElement>('#cancel-job')!;
const messages: Record<string, string> = { ready: 'Instalação encontrada', not_ready: 'Jogo não está pronto', not_configured: 'Jogo não configurado', inspection_error: 'Não foi possível verificar' };
function showResult(result: Result | null) {
  if (!result) { statusElement.textContent = 'Nenhuma instalação selecionada'; detail.textContent = 'Use o instalador atual do NX2Saria para uma primeira instalação ou selecione a pasta de uma instalação existente.'; return; }
  playButton.disabled = result.status !== 'ready' || result.job_marker_present === true || result.game_running === true;
  statusElement.textContent = messages[result.status] || 'Estado desconhecido';
  if (result.reason === 'worker_busy') { detail.textContent = 'Outra instância está usando a instalação. Se o jogo ainda estiver aberto, aguarde ele terminar.'; return; }
  if (result.reason === 'installation_required') { detail.textContent = 'Use o instalador atual do NX2Saria e selecione a pasta da instalação.'; return; }
  if (result.reason === 'runtime_incompatible') { detail.textContent = 'Atualize ou repare esta instalação pelo launcher atual do NX2Saria e tente novamente.'; return; }
  detail.textContent = (result.game_dir || result.root || 'Confira a instalação e tente novamente.') + (result.job_marker_present ? ' Há um registro de trabalho: ele pode estar em andamento ou ter sido interrompido.' : '') + (result.game_running === true ? ' O jogo já está em execução.' : '');
}
async function loadSelection(action: () => Promise<Result | null>) {
  button.disabled = true;
  playButton.disabled = true;
  try { showResult(await action()); }
  catch { statusElement.textContent = 'Não foi possível verificar'; detail.textContent = 'Tente novamente.'; }
  finally { button.disabled = false; }
}
button.onclick = () => { void loadSelection(() => window.launcher.selectInstallation()); };
void loadSelection(() => window.launcher.discoverInstallation());

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

playButton.onclick = async () => {
  playButton.disabled = true;
  button.disabled = true;
  statusElement.textContent = 'Abrindo o fluxo de jogo existente…';
  try {
    const result = await window.launcher.play();
    if (!result.started) { button.disabled = false; showPlayFeedback(result); }
  } catch { button.disabled = false; showPlayFeedback({}); }
};
window.launcher.onPlayEnded(result => {
  cancelJob.hidden = true;
  button.disabled = false;
  playButton.disabled = !result.state || result.state.status !== 'ready' || result.state.job_marker_present === true || result.state.game_running === true;
  showPlayFeedback(result);
});

cancelJob.onclick = () => { cancelJob.disabled = true; void window.launcher.cancelJob(); };
window.launcher.onJobStatus(result => {
  cancelJob.hidden = result.status !== 'running';
  cancelJob.disabled = false;
  if (result.status === 'running') {
    statusElement.textContent = 'Preparando os mods para jogar…';
    detail.textContent = typeof result.fraction === 'number' ? `Progresso: ${Math.round(result.fraction * 100)}%.` : 'Aguarde a compilação ou cancele.';
  } else if (result.status === 'resuming') {
    statusElement.textContent = 'Abrindo o jogo…';
  }
});
