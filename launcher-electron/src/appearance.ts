type AppearanceState = {
  theme: string; sounds: string; sound_gain: number; animations: boolean; background_dim: number; background: string; writable: boolean;
  colors: { background: string; panel: string; buttons: string };
  palette: { background: string; panel: string; text: string; muted: string; button: string; border: string; button_text: string };
};

const appearanceControl = document.querySelector<HTMLElement>('#appearance-control')!;
const appearanceButton = document.querySelector<HTMLButtonElement>('#appearance-button')!;
const appearancePanel = document.querySelector<HTMLElement>('#appearance-panel')!;
const appearanceTheme = document.querySelector<HTMLSelectElement>('#appearance-theme')!;
const appearanceColors = document.querySelector<HTMLElement>('#appearance-colors')!;
const appearanceDim = document.querySelector<HTMLSelectElement>('#appearance-dim')!;
const appearanceAnimations = document.querySelector<HTMLInputElement>('#appearance-animations')!;
const appearanceSounds = document.querySelector<HTMLSelectElement>('#appearance-sounds')!;
const appearanceResult = document.querySelector<HTMLElement>('#appearance-result')!;
const appearancePicture = document.querySelector<HTMLImageElement>('#shell-background')!;
const appearancePictureName = document.querySelector<HTMLElement>('#appearance-picture-name')!;
const appearancePictureChoose = document.querySelector<HTMLButtonElement>('#appearance-picture-choose')!;
const appearancePictureClear = document.querySelector<HTMLButtonElement>('#appearance-picture-clear')!;
const appearanceSlots = ['background', 'panel', 'buttons'] as const;
let appearanceState: AppearanceState | null = null;
let appearanceBusy = false;
let appearanceOpen = false;
let assetRequest = 0;
let titleFont: FontFace | null = null;
const appearanceEffects: Partial<Record<'select' | 'back', HTMLAudioElement>> = {};

function applyAppearance(state: AppearanceState | null) {
  appearanceState = state;
  const root = document.documentElement;
  for (const key of ['background', 'panel', 'text', 'muted', 'button', 'border', 'button_text'] as const) {
    const property = '--shell-' + key.replace('_', '-');
    if (state) root.style.setProperty(property, state.palette[key]);
    else root.style.removeProperty(property);
  }
  root.style.setProperty('--shell-picture-opacity', String(1 - (state?.background_dim ?? 40) / 100));
  root.dataset.animations = state?.animations ? 'on' : 'off';
}

function resetAppearance() {
  assetRequest++;
  setAppearanceFont(null);
  delete appearanceEffects.select;
  delete appearanceEffects.back;
  applyAppearance(null);
  showAppearanceImage(null);
  appearanceOpen = false;
  appearanceResult.textContent = '';
}

async function loadAppearance() {
  try {
    applyAppearance(await window.launcher.appearanceInfo());
    if (appearanceState?.background) await refreshAppearanceImage();
    if (appearanceState) void loadAppearanceAssets();
  }
  catch { applyAppearance(null); }
}

function setAppearanceFont(font: FontFace | null) {
  if (titleFont) document.fonts.delete(titleFont);
  titleFont = font;
  if (font) document.fonts.add(font);
  document.documentElement.classList.toggle('game-display-font', Boolean(font));
}

async function loadAppearanceAssets() {
  const request = ++assetRequest;
  try {
    const assets = await window.launcher.appearanceAssets();
    if (request !== assetRequest || !assets) return;
    for (const cue of ['select', 'back'] as const) {
      if (assets[cue]) appearanceEffects[cue] = new Audio('data:audio/wav;base64,' + assets[cue]);
    }
    if (!assets.title) return;
    const bytes = Uint8Array.from(atob(assets.title), character => character.charCodeAt(0));
    const font = await new FontFace('NX2Saria Game Display', bytes).load();
    if (request === assetRequest) setAppearanceFont(font);
  } catch {}
}

function playAppearanceEffect(cue: 'select' | 'back') {
  const effect = appearanceEffects[cue];
  const gain = appearanceState?.sound_gain ?? 0;
  if (!effect || gain === 0) return;
  effect.volume = gain;
  effect.currentTime = 0;
  void effect.play().catch(() => {});
}

document.addEventListener('click', event => {
  if (!(event.target instanceof Element)) return;
  const control = event.target.closest('button, input, select');
  if (control) playAppearanceEffect(/(?:-back|-close|-cancel)$/.test(control.id) ? 'back' : 'select');
}, true);
document.addEventListener('keydown', event => {
  if (event.key === 'Escape' && !event.repeat) playAppearanceEffect('back');
});

function updateAppearanceControls(disabled: boolean) {
  appearanceControl.hidden = !appearanceState;
  appearancePanel.hidden = !appearanceOpen;
  appearanceButton.setAttribute('aria-expanded', String(appearanceOpen));
  appearanceButton.disabled = disabled || appearanceBusy;
  const readOnly = disabled || appearanceBusy || !appearanceState?.writable;
  appearanceTheme.disabled = appearanceAnimations.disabled = appearanceSounds.disabled = readOnly;
  appearancePictureChoose.disabled = readOnly;
  appearancePictureClear.disabled = readOnly || !appearanceState?.background;
  appearancePictureName.hidden = !appearanceState?.background;
  appearancePictureName.textContent = appearanceState?.background ?? '';
  appearanceDim.disabled = readOnly || !appearanceState?.background;
  for (const button of appearanceColors.querySelectorAll<HTMLButtonElement>('button')) button.disabled = readOnly;
  for (const input of appearanceColors.querySelectorAll<HTMLInputElement>('input')) input.disabled = readOnly;
  if (!appearanceBusy && appearanceState) {
    appearanceTheme.value = appearanceState.theme;
    appearanceDim.value = String(appearanceState.background_dim);
    appearanceSounds.value = appearanceState.sounds;
    appearanceAnimations.checked = appearanceState.animations;
    appearanceColors.hidden = appearanceState.theme !== 'custom';
    for (const slot of appearanceSlots) {
      document.querySelector<HTMLInputElement>('#appearance-color-' + slot)!.value = appearanceState.colors[slot];
    }
  }
}

async function writeAppearance(key: string, value: string | boolean | number) {
  const previousFocus = document.activeElement;
  appearanceBusy = true;
  appearanceResult.textContent = '';
  updateControls();
  try {
    const saved = await window.launcher.appearancePreference(key, value);
    if (!saved) throw new Error('Appearance unavailable');
    applyAppearance(saved);
  } catch {
    appearanceResult.textContent = 'Não foi possível salvar a preferência de aparência.';
  } finally {
    appearanceBusy = false;
    updateControls();
    if (document.activeElement === document.body && previousFocus instanceof HTMLElement &&
        appearancePanel.contains(previousFocus) && previousFocus.getClientRects().length) {
      focusLauncherControl(previousFocus);
    }
  }
}

function showAppearanceImage(image: string | null) {
  appearancePicture.hidden = !image;
  if (image) appearancePicture.src = image;
  else appearancePicture.removeAttribute('src');
}

async function refreshAppearanceImage() {
  try { showAppearanceImage(await window.launcher.appearanceImage()); }
  catch { showAppearanceImage(null); }
}

async function changeAppearanceImage(choose: boolean) {
  appearanceBusy = true;
  appearanceResult.textContent = '';
  updateControls();
  try {
    const result = await window.launcher.appearanceBackground(choose);
    if (!result) return;
    if (result.status === 'ok' && result.appearance) {
      applyAppearance(result.appearance);
      await refreshAppearanceImage();
    } else {
      appearanceResult.textContent = result.status === 'invalid_picture'
        ? 'Não foi possível ler a imagem escolhida.' : 'Não foi possível salvar a imagem de fundo.';
    }
  } catch { appearanceResult.textContent = 'Não foi possível salvar a imagem de fundo.'; }
  finally {
    appearanceBusy = false;
    updateControls();
  }
}

appearancePictureChoose.onclick = () => { void changeAppearanceImage(true); };
appearancePictureClear.onclick = () => { void changeAppearanceImage(false); };
appearanceButton.onclick = () => { appearanceOpen = !appearanceOpen; updateControls(); };
appearanceTheme.onchange = () => { void writeAppearance('theme', appearanceTheme.value); };
appearanceDim.onchange = () => { void writeAppearance('background_dim', Number(appearanceDim.value)); };
appearanceAnimations.onchange = () => { void writeAppearance('animations', appearanceAnimations.checked); };
appearanceSounds.onchange = () => { void writeAppearance('sounds', appearanceSounds.value); };
for (const slot of appearanceSlots) {
  const color = document.querySelector<HTMLInputElement>('#appearance-color-' + slot)!;
  color.onchange = () => { void writeAppearance('custom_' + slot, color.value); };
  document.querySelector<HTMLButtonElement>('#appearance-reset-' + slot)!.onclick = () => {
    void writeAppearance('custom_' + slot, '');
  };
}
