type ReportsState = {
  available: boolean; send_enabled: boolean; question: boolean; seconds: number | null;
  consent: 'on' | 'off'; notice: boolean; closed: boolean; text_limit: number; crash_ask: boolean | null;
};
type CrashOffer = { status: 'shown'; kind: string; message: string; head_lines: number; tail_lines: number;
  bytes: number; summary: boolean; whole: boolean; guard_ms: number; can_send: boolean };
type ReportsScreen = 'question' | 'options' | 'preview' | 'comment' | 'crash';

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
const reportCrashSetting = document.querySelector<HTMLInputElement>('#reports-crash-enabled')!;
const reportCrashOption = document.querySelector<HTMLElement>('#reports-crash-option')!;
const reportCrashError = document.querySelector<HTMLElement>('#reports-crash-error')!;
const reportCrashDetails = document.querySelector<HTMLElement>('#reports-crash-details')!;
const reportCrashSend = document.querySelector<HTMLButtonElement>('#reports-crash-send')!;
const reportCrashDecline = document.querySelector<HTMLButtonElement>('#reports-crash-decline')!;
const reportCrashPreview = document.querySelector<HTMLButtonElement>('#reports-crash-preview')!;
const reportCrashComment = document.querySelector<HTMLButtonElement>('#reports-crash-comment')!;
const reportPanels = {
  question: document.querySelector<HTMLElement>('#reports-consent-view')!,
  options: document.querySelector<HTMLElement>('#reports-options-view')!,
  preview: document.querySelector<HTMLElement>('#reports-preview-view')!,
  comment: document.querySelector<HTMLElement>('#reports-comment-view')!,
  crash: document.querySelector<HTMLElement>('#reports-crash-view')!,
};
const reportTitles = { question: 'Ajudar a melhorar o NX2Saria?', options: 'Relatórios de sessão',
  preview: 'O que é enviado', comment: 'Escrever comentário', crash: 'Enviar o log deste erro?' };
let reportState: ReportsState | null = null;
let reportScreen: ReportsScreen | null = null;
let reportPreviewBack: 'question' | 'options' | 'crash' = 'options';
let reportOffer: CrashOffer | null = null;
let crashGuardUntil = 0;
let reportBusy = false;
let reportRefresh = false;
let reportGeneration = 0;

function showReportScreen(screen: ReportsScreen | null) {
  reportScreen = screen;
  if (screen === null) { reportDialog.close(); return; }
  reportTitle.textContent = tr(reportTitles[screen]);
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
    ? tr("Os relatórios de sessão estão ligados. Você pode desligá-los em Configurações.") : '';
  reportTimer.textContent = reportState?.seconds == null
    ? tr("Os relatórios de sessão são ativados ao escolher Enviar ou fechar esta janela. Escolha Não enviar para mantê-los desligados.")
    : tr("Os relatórios de sessão serão ativados em {value1} s ou se você fechar esta janela. Escolha Não enviar para mantê-los desligados.", { value1: reportState.seconds });
  if (!reportBusy) {
    reportConsent.checked = reportState?.consent === 'on';
    reportCrashSetting.checked = reportState?.crash_ask === true;
  }
  reportCrashOption.hidden = reportState?.crash_ask == null;
  reportServiceClosed.hidden = !reportState?.closed;
  reportSendDisabled.hidden = reportState?.send_enabled !== false;
  reportCommentSend.disabled = reportBusy || reportState?.send_enabled !== true || reportState.closed;
  reportConsent.disabled = reportCrashSetting.disabled = reportBusy;
  const guard = reportBusy || performance.now() < crashGuardUntil;
  reportCrashSend.disabled = guard || reportOffer?.can_send !== true;
  reportCrashDecline.disabled = reportCrashPreview.disabled = reportCrashComment.disabled = guard;
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
  reportState = reportOffer = null;
  crashGuardUntil = 0;
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
    if (generation !== reportGeneration) return;
    applyReportState(state);
    if (state?.available && !state.question) await loadCrashOffer();
  } catch {}
}

async function refreshReportState(touch = false) {
  if (reportRefresh || reportBusy || reportScreen !== 'question' || reportState?.seconds == null) return;
  reportRefresh = true;
  try {
    const state = await (touch ? window.launcher.reportsTouch() : window.launcher.reportsStatus());
    if (!reportBusy) {
      applyReportState(state);
      if (state?.available && !state.question) await loadCrashOffer();
    }
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
  catch { reportMessage.textContent = tr("Não foi possível concluir. Confira o log do launcher."); }
  finally { reportBusy = false; updateReportScreen(); }
}

function answerReports(answer: 'send' | "don't send" | 'closed') {
  void runReportAction(async () => {
    const state = await window.launcher.reportsAnswer(answer);
    if (!state) throw new Error('Reports unavailable');
    applyReportState(state);
    if (!state.question) await loadCrashOffer();
  });
}

async function loadCrashOffer() {
  if (!reportState?.available || reportState.crash_ask === null || reportState.question || reportScreen !== null) return;
  const generation = reportGeneration;
  try {
    const offer = await window.launcher.reportsCrashOpen();
    if (!offer || generation !== reportGeneration || reportScreen !== null) return;
    reportOffer = offer;
    crashGuardUntil = performance.now() + offer.guard_ms;
    reportCrashError.textContent = offer.message || tr("O jogo terminou com erro.");
    const total = offer.head_lines + offer.tail_lines;
    const lines = offer.whole ? tr("O log inteiro desta sessão ({value1} linhas)", { value1: total }) :
      offer.head_lines ? tr("As primeiras {value1} e as últimas {value2} linhas do log", { value1: offer.head_lines, value2: offer.tail_lines }) :
      tr("As últimas {value1} linhas do log", { value1: offer.tail_lines });
    const summary = offer.summary ? tr(" e o relatório desta sessão") : '';
    reportCrashDetails.textContent = tr("{value1} ({value2} KB){value3}. O launcher encurta dados pessoais no log. Nada é enviado sem sua confirmação.", { value1: lines, value2: Math.max(1, Math.round(offer.bytes / 1024)), value3: summary });
    showReportScreen('crash');
    window.setTimeout(updateReportScreen, offer.guard_ms + 20);
  } catch {}
}

function chooseCrashOffer(answer: 'send' | 'decline' | 'comment' | 'closed') {
  if (!reportOffer || performance.now() < crashGuardUntil) return;
  void runReportAction(async () => {
    if (answer === 'send') reportMessage.textContent = tr("Enviando log…");
    const result = await window.launcher.reportsCrashAnswer(answer);
    if (result.status === 'guarded' || result.status === 'disabled') return;
    reportOffer = null;
    if (result.status === 'comment') {
      reportCommentAttach.checked = true;
      showReportScreen('comment');
      updateReportCommentSize();
      reportCommentText.focus();
    } else if (answer === 'send') {
      showReportScreen('options');
      reportMessage.textContent = result.status === 'ok'
        ? tr("Log enviado. Obrigado.") : tr("Não foi possível enviar o log.");
      const state = await window.launcher.reportsStatus();
      if (state) applyReportState(state);
    } else {
      showReportScreen(null);
    }
  });
}

function openReportsPreview() {
  const previous = reportScreen === 'question' || reportScreen === 'crash' ? reportScreen : 'options';
  void runReportAction(async () => {
    const preview = await (previous === 'crash' ? window.launcher.reportsCrashPreview() : window.launcher.reportsPreview());
    if (!preview) throw new Error('Preview unavailable');
    reportPreviewBack = previous;
    reportPreviewLabel.textContent = previous === 'crash' ? tr("Conteúdo exato da oferta de log.") :
      preview.example ? tr("Exemplo de um relatório.") : tr("Relatório da sessão pendente.");
    reportPreviewText.textContent = preview.text;
    if (previous === 'question') applyReportState(await window.launcher.reportsStatus());
    showReportScreen('preview');
    reportPreviewText.focus();
  });
}

function closeReportScreen() {
  if (reportBusy) return;
  if (reportScreen === 'question') answerReports('closed');
  else if (reportScreen === 'crash') chooseCrashOffer('closed');
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
reportCrashSend.onclick = () => chooseCrashOffer('send');
reportCrashDecline.onclick = () => chooseCrashOffer('decline');
reportCrashComment.onclick = () => chooseCrashOffer('comment');
reportCrashPreview.onclick = openReportsPreview;
reportCrashSetting.onchange = () => {
  const enabled = reportCrashSetting.checked;
  void runReportAction(async () => {
    const state = await window.launcher.reportsCrashPreference(enabled);
    if (!state) throw new Error('Crash preference unavailable');
    applyReportState(state);
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
  reportCommentSize.textContent = tr("{value1} de {value2} caracteres", { value1: Math.min(characters.length, limit), value2: limit });
}
document.querySelector<HTMLButtonElement>('#reports-comment')!.onclick = () => {
  showReportScreen('comment');
  updateReportCommentSize();
  reportCommentText.focus();
};
reportCommentText.oninput = updateReportCommentSize;
reportCommentSend.onclick = () => {
  void runReportAction(async () => {
    reportMessage.textContent = tr("Enviando comentário…");
    const result = await window.launcher.reportsComment(reportCommentText.value, reportCommentAttach.checked);
    if (result.status === 'ok') {
      reportCommentText.value = '';
      updateReportCommentSize();
      reportMessage.textContent = tr("Comentário enviado. Obrigado.");
    } else {
      reportMessage.textContent = result.reason
        ? tr("Não foi possível enviar o comentário. {value1}", { value1: result.reason }) : tr("Não foi possível enviar o comentário.");
    }
    const state = await window.launcher.reportsStatus();
    if (state) applyReportState(state);
  });
};
