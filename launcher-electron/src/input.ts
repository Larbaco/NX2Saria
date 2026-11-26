type Direction = 'up' | 'down' | 'left' | 'right';

function activeDialog(): HTMLDialogElement | null {
  return document.querySelector<HTMLDialogElement>('dialog[open]');
}

function launcherControls() {
  const root = activeDialog() ?? document;
  return Array.from(root.querySelectorAll<HTMLElement>(
    'button, input:not([type=hidden]), select, textarea, a[href], [tabindex="0"]',
  )).filter(control => !control.matches(':disabled') && control.tabIndex >= 0 &&
    control.getClientRects().length > 0 && getComputedStyle(control).visibility === 'visible');
}

function focusLauncherControl(control: HTMLElement) {
  control.focus({ preventScroll: true });
  control.scrollIntoView({ block: 'nearest', inline: 'nearest' });
}

function moveLauncherFocus(direction: Direction) {
  const focus = document.activeElement;
  if (focus instanceof HTMLSelectElement && !focus.disabled && (direction === 'left' || direction === 'right')) {
    const step = direction === 'left' ? -1 : 1;
    for (let i = focus.selectedIndex + step; i >= 0 && i < focus.options.length; i += step) {
      if (!focus.options[i].disabled) {
        focus.selectedIndex = i;
        focus.dispatchEvent(new Event('change', { bubbles: true }));
        break;
      }
    }
    return;
  }
  const controls = launcherControls();
  if (!controls.length) return;
  const current = controls.indexOf(focus as HTMLElement);
  const backwards = direction === 'up' || direction === 'left';
  const index = current < 0 ? (backwards ? controls.length - 1 : 0) :
    (current + (backwards ? -1 : 1) + controls.length) % controls.length;
  focusLauncherControl(controls[index]);
}

function activateLauncherControl() {
  const controls = launcherControls();
  const current = document.activeElement;
  if (!(current instanceof HTMLElement) || !controls.includes(current)) {
    if (controls.length) focusLauncherControl(controls[0]);
    return;
  }
  current.click();
}

let returningFromLauncherPanel = false;

function returnFromLauncherPanel() {
  const dialog = activeDialog();
  if (dialog) {
    dialog.dispatchEvent(new Event('cancel', { cancelable: true }));
    return true;
  }
  const focused = document.activeElement?.closest('section');
  const expanded = focused?.querySelector<HTMLButtonElement>('button[aria-expanded="true"]');
  if (!expanded || expanded.disabled) return false;
  focusLauncherControl(expanded);
  returningFromLauncherPanel = true;
  try { expanded.click(); } finally { returningFromLauncherPanel = false; }
  return true;
}

document.addEventListener('keydown', event => {
  if (event.key !== 'Escape' || event.defaultPrevented || activeDialog()) return;
  if (returnFromLauncherPanel()) event.preventDefault();
});

function padInput(pad: Gamepad) {
  const button = (index: number) => pad.buttons[index]?.pressed === true;
  let direction: Direction | null = null;
  if (button(12)) direction = 'up';
  else if (button(13)) direction = 'down';
  else if (button(14)) direction = 'left';
  else if (button(15)) direction = 'right';
  else {
    const [x = 0, y = 0] = pad.axes;
    if (Math.abs(x) > Math.abs(y) && Math.abs(x) > 0.65) direction = x < 0 ? 'left' : 'right';
    else if (Math.abs(y) > 0.65) direction = y < 0 ? 'up' : 'down';
  }
  const y = pad.axes[3] ?? 0;
  const scroll = Math.abs(y) > 0.65 ? Math.sign(y) : 0;
  return { direction, scroll, accept: button(0), back: button(1) };
}

function scrollLauncher(delta: number) {
  const focus = document.activeElement;
  if (focus instanceof HTMLElement && focus.scrollHeight > focus.clientHeight &&
      ['auto', 'scroll'].includes(getComputedStyle(focus).overflowY)) {
    focus.scrollBy(0, delta);
  } else {
    (activeDialog() ?? window).scrollBy(0, delta);
  }
}

let lastPadDirection: Direction | null = null;
let padRepeatAt = 0;
let padScrollAt = 0;
let padAcceptHeld = false;
let padBackHeld = false;
let padNeedsNeutral = false;
let padLogged = false;

function pollLauncherPad(now: number) {
  requestAnimationFrame(pollLauncherPad);
  if (!document.hasFocus() || document.visibilityState !== 'visible') {
    padNeedsNeutral = true;
    lastPadDirection = null;
    padAcceptHeld = padBackHeld = false;
    return;
  }
  let pad: Gamepad | null | undefined;
  try { pad = Array.from(navigator.getGamepads()).find(pad => pad?.connected && pad.mapping === 'standard'); }
  catch { return; }
  if (!pad) {
    padNeedsNeutral = false;
    lastPadDirection = null;
    padAcceptHeld = padBackHeld = false;
    return;
  }
  const input = padInput(pad);
  const any = Boolean(input.direction || input.scroll || input.accept || input.back);
  if (!any) padNeedsNeutral = false;
  if (padNeedsNeutral) return;
  if (any && !padLogged) {
    window.launcher.controllerUsed();
    padLogged = true;
  }
  if (input.back && !padBackHeld) returnFromLauncherPanel();
  else if (input.accept && !padAcceptHeld) activateLauncherControl();
  else if (input.direction && (input.direction !== lastPadDirection || now >= padRepeatAt)) {
    padRepeatAt = now + (input.direction === lastPadDirection ? 120 : 340);
    moveLauncherFocus(input.direction);
  }
  if (input.scroll && now >= padScrollAt) {
    scrollLauncher(input.scroll * 30);
    padScrollAt = now + 40;
  }
  lastPadDirection = input.direction;
  padAcceptHeld = input.accept;
  padBackHeld = input.back;
}

requestAnimationFrame(pollLauncherPad);
