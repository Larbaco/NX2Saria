type LanguageChoice = 'automatic' | 'en-US' | 'pt-BR' | 'ru' | 'zh-Hans';
type LanguageView = { choice: LanguageChoice; locale: Exclude<LanguageChoice, 'automatic'>;
  writable: boolean; messages: Record<string, string> };

const staticMessages = Array.from(document.querySelectorAll<HTMLElement>('[data-message]'), element =>
  [element, element.textContent || ''] as const);
const languageSelect = document.querySelector<HTMLSelectElement>('#launcher-language')!;
let languageView: LanguageView | null = null;

function tr(source: string, values: Record<string, string | number | undefined> = {}) {
  const messages = languageView?.messages;
  const message = messages && Object.hasOwn(messages, source) ? messages[source] : source;
  return message.replace(/\{([a-zA-Z]\w*)\}/g, (token, key) =>
    Object.hasOwn(values, key) ? String(values[key] ?? '') : token);
}

function applyLanguage(view: LanguageView) {
  languageView = view;
  document.documentElement.lang = view.locale;
  languageSelect.value = view.choice;
  for (const [element, message] of staticMessages) {
    if (element.isConnected) element.textContent = tr(message);
  }
}

async function loadLanguage() {
  try { applyLanguage(await window.launcher.languageInfo()); }
  catch {}
}
