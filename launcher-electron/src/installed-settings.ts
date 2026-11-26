import path from 'node:path';

export type SettingKey = 'channel' | 'auto_check' | 'auto_install' | 'keep_cache' | 'warn_low_commit';

export function validSetting(key: unknown, value: unknown, platform: NodeJS.Platform): key is SettingKey {
  if (key === 'channel') return value === 'stable' || value === 'preview';
  return typeof value === 'boolean' && (
    key === 'auto_check' || key === 'auto_install' || key === 'keep_cache' ||
    (key === 'warn_low_commit' && platform === 'win32')
  );
}

export function parseInstalledSettings(value: unknown) {
  if (!value || typeof value !== 'object' || Array.isArray(value)) throw new Error('Invalid launcher settings');
  const settings = value as Record<string, unknown>;
  if (typeof settings.game_dir !== 'string' || (settings.game_dir && !path.isAbsolute(settings.game_dir)) ||
      typeof settings.dump !== 'string' ||
      (settings.channel !== 'stable' && settings.channel !== 'preview') ||
      typeof settings.auto_check !== 'boolean' || typeof settings.auto_install !== 'boolean' ||
      typeof settings.keep_cache !== 'boolean' ||
      (settings.warn_low_commit !== null && typeof settings.warn_low_commit !== 'boolean')) {
    throw new Error('Invalid launcher settings');
  }
  return { game_dir: settings.game_dir, dump: settings.dump, channel: settings.channel,
    auto_check: settings.auto_check, auto_install: settings.auto_install, keep_cache: settings.keep_cache,
    warn_low_commit: settings.warn_low_commit };
}
