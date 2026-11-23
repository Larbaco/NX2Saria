type Result = { status: string; root?: string; game_dir?: string | null; reason?: string; job_marker_present?: boolean; game_running?: true | null };
interface Window { launcher: { discoverInstallation(): Promise<Result | null>; selectInstallation(): Promise<Result | null>; play(): Promise<{ started?: boolean; error?: string }>; onPlayEnded(callback: (result: { code?: number | null; signal?: string | null; error?: string }) => void): void } }
const statusElement = document.querySelector<HTMLElement>('#status')!;
const detail = document.querySelector<HTMLElement>('#detail')!;
const button = document.querySelector<HTMLButtonElement>('#select')!;
const playButton = document.querySelector<HTMLButtonElement>('#play')!;
const messages: Record<string, string> = { ready: 'Instalação encontrada', not_ready: 'Jogo não está pronto', not_configured: 'Jogo não configurado', inspection_error: 'Não foi possível verificar' };
function showResult(result: Result | null) {
  if (!result) { statusElement.textContent = 'Nenhuma instalação selecionada'; detail.textContent = 'Selecione a pasta para verificar o estado.'; return; }
  playButton.disabled = result.status !== 'ready' || result.job_marker_present === true || result.game_running === true;
  statusElement.textContent = messages[result.status] || 'Estado desconhecido';
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

playButton.onclick = async () => {
  playButton.disabled = true;
  button.disabled = true;
  statusElement.textContent = 'Abrindo o fluxo de jogo existente…';
  try {
    const result = await window.launcher.play();
    if (!result.started) { button.disabled = false; statusElement.textContent = 'Não foi possível iniciar'; detail.textContent = 'Confira o estado da instalação e se há outro trabalho em andamento.'; }
  } catch { button.disabled = false; statusElement.textContent = 'Não foi possível iniciar'; }
};
window.launcher.onPlayEnded(result => {
  button.disabled = false;
  const failed = Boolean(result.error || result.signal || (typeof result.code === 'number' && result.code !== 0));
  statusElement.textContent = result.error ? 'Não foi possível iniciar' : failed ? 'Fluxo de jogo encerrado com erro' : 'Fluxo de jogo encerrado';
  detail.textContent = failed ? 'Confira os logs do launcher para identificar o problema.' : 'Confira os logs para o resultado; selecione a instalação novamente para atualizar o estado.';
});
