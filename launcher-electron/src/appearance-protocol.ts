const themes = ['forest', 'volcano', 'lake', 'desert', 'temple', 'shadow', 'custom'];
const soundLevels = ['off', 'low', 'medium', 'high'];
const dimming = [0, 20, 40, 60, 80];
const colorKeys = ['custom_background', 'custom_panel', 'custom_buttons'];

function hex(value: unknown): value is string {
  return typeof value === 'string' && /^#[a-f0-9]{6}$/i.test(value);
}

export function validAppearancePreference(key: unknown, value: unknown): boolean {
  if (key === 'theme') return typeof value === 'string' && themes.includes(value);
  if (key === 'sounds') return typeof value === 'string' && soundLevels.includes(value);
  if (key === 'animations') return typeof value === 'boolean';
  if (key === 'background_dim') return typeof value === 'number' && dimming.includes(value);
  return typeof key === 'string' && colorKeys.includes(key) && (value === '' || hex(value));
}

export function parseAppearance(value: unknown) {
  if (!value || typeof value !== 'object' || Array.isArray(value)) throw new Error('Invalid appearance');
  const view = value as Record<string, unknown>;
  if (typeof view.theme !== 'string' || !themes.includes(view.theme) || typeof view.sounds !== 'string' ||
      (!soundLevels.includes(view.sounds) || typeof view.sound_gain !== 'number' ||
        !Number.isFinite(view.sound_gain) || view.sound_gain < 0 || view.sound_gain > 1) || typeof view.animations !== 'boolean' ||
      typeof view.background_dim !== 'number' || !dimming.includes(view.background_dim) ||
      typeof view.background !== 'string' || view.background.length > 255 || view.background.includes('\0') ||
      !view.colors || typeof view.colors !== 'object' || !view.palette || typeof view.palette !== 'object') {
    throw new Error('Invalid appearance');
  }
  const colors = view.colors as Record<string, unknown>;
  const palette = view.palette as Record<string, unknown>;
  function swatch(source: Record<string, unknown>, key: string) {
    const color = source[key];
    if (!hex(color)) throw new Error('Invalid palette');
    return color;
  }
  return { theme: view.theme, sounds: view.sounds, sound_gain: view.sound_gain, animations: view.animations, background_dim: view.background_dim,
    background: view.background, colors: { background: swatch(colors, 'background'), panel: swatch(colors, 'panel'),
      buttons: swatch(colors, 'buttons') },
    palette: { background: swatch(palette, 'background'), panel: swatch(palette, 'panel'),
      text: swatch(palette, 'text'), muted: swatch(palette, 'muted'), button: swatch(palette, 'button'),
      border: swatch(palette, 'border'), button_text: swatch(palette, 'button_text') } };
}
