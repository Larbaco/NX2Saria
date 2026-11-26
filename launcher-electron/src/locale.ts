export const locales = ['en-US', 'pt-BR', 'ru', 'zh-Hans'] as const;
export type Locale = typeof locales[number];
export type LanguageChoice = 'automatic' | Locale;
export type Messages = Record<string, string>;

export function localeChoice(value: unknown): LanguageChoice {
  return locales.find(locale => locale === value) ?? 'automatic';
}

export function systemLocale(languages: readonly string[]): Locale {
  for (const value of languages) {
    const language = value.toLowerCase();
    if (/^en(?:-|$)/.test(language)) return 'en-US';
    if (/^pt(?:-|$)/.test(language)) return 'pt-BR';
    if (/^ru(?:-|$)/.test(language)) return 'ru';
    if (language === 'zh' || /^zh-(?:hans|cn|sg)(?:-|$)/.test(language)) return 'zh-Hans';
  }
  return 'en-US';
}

export function translateMessage(messages: Messages, source: string, values: Record<string, string | number | undefined> = {}) {
  const message = Object.hasOwn(messages, source) ? messages[source] : source;
  return message.replace(/\{([a-zA-Z]\w*)\}/g, (token, key) =>
    Object.hasOwn(values, key) ? String(values[key] ?? '') : token);
}
