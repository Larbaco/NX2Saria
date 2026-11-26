type ReportsState = {
  available: boolean; send_enabled: boolean; question: boolean; seconds: number | null;
  consent: 'on' | 'off'; notice: boolean; closed: boolean; text_limit: number;
};
type ReportsScreen = 'question' | 'options' | 'preview' | 'comment';

const reportDialog = document.querySelector<HTMLDialogElement>('#reports-dialog')!;
const reportTitle = document.querySelector<HTMLElement>('#reports-heading')!;
const reportSettingsButton = document.querySelector<HTMLButtonElement>('#reports-button')!;
const reportTimer = document.querySelector<HTMLElement>('#reports-question-timer')!;
const reportNotice = document.querySelector<HTMLElement>('#reports-notice')!;
const reportMessage = document.querySelector<HTMLElement>('#reports-message')!;
const reportConsent = document.querySelector<HTMLInputElement>('#reports-enabled')!;
const reportServiceClosed = document.querySelector<HTMLElement>('#reports-closed')!;
const reportSendDisabled = document.querySelector<HTMLElement>('#reports-sending-disabled')!;
const reportPreviewText = document.querySelector<HTMLElement>('#reports-preview-text')!;
const reportPreviewLabel = document.querySelector<HTMLElement>('#reports-preview-label')!;
const reportCommentText = document.querySelector<HTMLTextAreaElement>('#reports-comment-text')!;
const reportCommentSize = document.querySelector<HTMLElement>('#reports-comment-size')!;
const reportCommentAttach = document.querySelector<HTMLInputElement>('#reports-comment-attach')!;
const reportCommentSend = document.querySelector<HTMLButtonElement>('#reports-comment-send')!;
const reportPanels = {
  question: document.querySelector<HTMLElement>('#reports-consent-view')!,
  options: document.querySelector<HTMLElement>('#reports-options-view')!,
  preview: document.querySelector<HTMLElement>('#reports-preview-view')!,
  comment: document.querySelector<HTMLElement>('#reports-comment-view')!,
};
const reportTitles = { question: 'Ajudar a melhorar o NX2Saria?', options: 'Relatórios de sessão',
  preview: 'O que é enviado', comment: 'Escrever comentário' };
let reportState: ReportsState | null = null;
let reportScreen: ReportsScreen | null = null;
let reportPreviewBack: 'question' | 'options' = 'options';
let reportBusy = false;
let reportRefresh = false;
let reportGeneration = 0;

function showReportScreen(screen: ReportsScreen | null) {
  reportScreen = screen;
  if (screen === null) { reportDialog.close(); return; }
  reportTitle.textContent = reportTitles[screen];
  for (const section of Object.keys(reportPanels) as ReportsScreen[]) {
    reportPanels[section].hidden = section !== screen;
  }
  reportMessage.textContent = '';
  updateReportScreen();
  if (!reportDialog.open) reportDialog.showModal();
}

function updateReportScreen() {
  reportSettingsButton.hidden = !reportState?.available;
  reportNotice.hidden = !reportState?.notice;
  reportNotice.textContent = reportState?.notice
    ? 'Os relatórios de sessão estão ligados. Você pode desligá-los em Configurações.' : '';
  reportTimer.textContent = reportState?.seconds == null
    ? 'Os relatórios de sessão são ativados ao escolher Enviar ou fechar esta janela. Escolha Não enviar para mantê-los desligados.'
    : `Os relatórios de sessão serão ativados em ${reportState.seconds} s ou se você fechar esta janela. Escolha Não enviar para mantê-los desligados.`;
  if (!reportBusy) reportConsent.checked = reportState?.consent === 'on';
  reportServiceClosed.hidden = !reportState?.closed;
  reportSendDisabled.hidden = reportState?.send_enabled !== false;
  reportCommentSend.disabled = reportBusy || reportState?.send_enabled !== true || reportState.closed;
  reportConsent.disabled = reportBusy;
}

function applyReportState(state: ReportsState | null) {
  reportState = state;
  updateReportScreen();
  if (state?.question && reportScreen === null) {
    showReportScreen('question');
  } else if (!state?.question && (reportScreen === 'question' ||
      (reportScreen === 'preview' && reportPreviewBack === 'question'))) {
    showReportScreen(null);
  }
}

function resetReportScreen() {
  reportGeneration++;
  reportState = null;
  reportCommentText.value = '';
  reportCommentAttach.checked = false;
  reportPreviewText.textContent = '';
  showReportScreen(null);
  updateReportScreen();
}

async function loadReportsHome(after: '' | 'installation' | 'update') {
  const generation = reportGeneration;
  try {
    const state = await window.launcher.reportsHome(after);
    if (generation === reportGeneration) applyReportState(state);
  } catch {}
}

async function refreshReportState(touch = false) {
  if (reportRefresh || reportBusy || reportScreen !== 'question' || reportState?.seconds == null) return;
  reportRefresh = true;
  try {
    const state = await (touch ? window.launcher.reportsTouch() : window.launcher.reportsStatus());
    if (!reportBusy) applyReportState(state);
  } catch {}
  finally { reportRefresh = false; }
}

document.addEventListener('pointerdown', () => { void refreshReportState(true); });
document.addEventListener('keydown', () => { void refreshReportState(true); });
window.setInterval(() => { void refreshReportState(); }, 500);

async function runReportAction(action: () => Promise<void>) {
  if (reportBusy) return;
  reportBusy = true;
  updateReportScreen();
  try { await action(); }
  catch { reportMessage.textContent = 'Não foi possível concluir. Confira o log do launcher.'; }
  finally { reportBusy = false; updateReportScreen(); }
}

function answerReports(answer: 'send' | "don't send" | 'closed') {
  void runReportAction(async () => {
    const state = await window.launcher.reportsAnswer(answer);
    if (!state) throw new Error('Reports unavailable');
    applyReportState(state);
  });
}

function openReportsPreview() {
  const previous = reportScreen === 'question' ? 'question' : 'options';
  void runReportAction(async () => {
    const preview = await window.launcher.reportsPreview();
    if (!preview) throw new Error('Preview unavailable');
    reportPreviewBack = previous;
    reportPreviewLabel.textContent = preview.example ? 'Exemplo de um relatório.' : 'Relatório da sessão pendente.';
    reportPreviewText.textContent = preview.text;
    if (previous === 'question') applyReportState(await window.launcher.reportsStatus());
    showReportScreen('preview');
    reportPreviewText.focus();
  });
}

function closeReportScreen() {
  if (reportBusy) return;
  if (reportScreen === 'question') answerReports('closed');
  else if (reportScreen === 'comment') showReportScreen('options');
  else if (reportScreen === 'preview') showReportScreen(reportPreviewBack);
  else showReportScreen(null);
}

reportSettingsButton.onclick = () => {
  void runReportAction(async () => {
    const state = await window.launcher.reportsStatus();
    if (!state?.available) return;
    applyReportState(state);
    if (!state.question) showReportScreen('options');
  });
};
reportDialog.oncancel = event => { event.preventDefault(); closeReportScreen(); };
document.querySelector<HTMLButtonElement>('#reports-accept')!.onclick = () => answerReports('send');
document.querySelector<HTMLButtonElement>('#reports-decline')!.onclick = () => answerReports("don't send");
document.querySelector<HTMLButtonElement>('#reports-question-preview')!.onclick = openReportsPreview;
document.querySelector<HTMLButtonElement>('#reports-options-preview')!.onclick = openReportsPreview;
document.querySelector<HTMLButtonElement>('#reports-preview-back')!.onclick = closeReportScreen;
document.querySelector<HTMLButtonElement>('#reports-options-close')!.onclick = closeReportScreen;
document.querySelector<HTMLButtonElement>('#reports-comment-back')!.onclick = closeReportScreen;
reportConsent.onchange = () => {
  const enabled = reportConsent.checked;
  void runReportAction(async () => {
    const state = await window.launcher.reportsConsent(enabled);
    if (!state) throw new Error('Reports unavailable');
    applyReportState(state);
  });
};

function updateReportCommentSize() {
  const limit = reportState?.text_limit ?? 4000;
  const characters = [...reportCommentText.value];
  if (characters.length > limit) reportCommentText.value = characters.slice(0, limit).join('');
  reportCommentSize.textContent = `${Math.min(characters.length, limit)} de ${limit} caracteres`;
}
document.querySelector<HTMLButtonElement>('#reports-comment')!.onclick = () => {
  showReportScreen('comment');
  updateReportCommentSize();
  reportCommentText.focus();
};
reportCommentText.oninput = updateReportCommentSize;
reportCommentSend.onclick = () => {
  void runReportAction(async () => {
    reportMessage.textContent = 'Enviando comentário…';
    const result = await window.launcher.reportsComment(reportCommentText.value, reportCommentAttach.checked);
    if (result.status === 'ok') {
      reportCommentText.value = '';
      updateReportCommentSize();
      reportMessage.textContent = 'Comentário enviado. Obrigado.';
    } else {
      reportMessage.textContent = result.reason
        ? `Não foi possível enviar o comentário. ${result.reason}` : 'Não foi possível enviar o comentário.';
    }
    const state = await window.launcher.reportsStatus();
    if (state) applyReportState(state);
  });
};
