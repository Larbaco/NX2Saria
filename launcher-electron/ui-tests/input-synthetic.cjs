const assert = require('node:assert/strict');
const fs = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');
const { _electron } = require('playwright-core');

async function main() {
  const base = await fs.mkdtemp(path.join(os.tmpdir(), 'nx2-input-'));
  const root = path.join(base, 'nx2saria');
  const game = path.join(root, 'game');
  let app;
  try {
    await fs.mkdir(path.join(root, 'data'), { recursive: true });
    await fs.mkdir(path.join(root, 'app'));
    await fs.writeFile(path.join(root, 'app', 'current.txt'), '0.0.1');
    await fs.writeFile(path.join(root, 'data', 'launcher.json'), JSON.stringify({ game_dir: game }));
    await fs.mkdir(path.join(game, 'build-cache'), { recursive: true });
    await fs.writeFile(path.join(game, 'build-cache', 'state.json'), '{"kit":"0.0.1","steps":{"compile":{}}}');
    await fs.writeFile(path.join(game, 'ocarina'), 'synthetic executable marker');

    app = await _electron.launch({ args: ['--no-sandbox', '.'], cwd: path.resolve(__dirname, '..'),
      env: { ...process.env, HOME: base, XDG_DATA_HOME: base, NX2SARIA_TELEMETRY_URL: 'none',
        NX2SARIA_ELECTRON_TEST_SEND_REPORTS: '0', GSETTINGS_BACKEND: 'memory' } });
    const page = await app.firstWindow();
    await app.evaluate(({ BrowserWindow }) => {
      const window = BrowserWindow.getAllWindows()[0];
      window.setSize(720, 500);
      window.focus();
    });
    const customize = page.getByRole('button', { name: 'Personalizar', exact: true });
    await customize.waitFor();
    await page.locator('#steam-button').focus();
    await page.keyboard.press('Tab');
    assert.equal(await page.evaluate(() => document.activeElement.id), 'appearance-button');
    await page.evaluate(() => {
      window.testPad = { mapping: 'standard', connected: true,
        buttons: Array.from({ length: 17 }, () => ({ pressed: false, value: 0 })), axes: [0, 0, 0, 0] };
      Object.defineProperty(navigator, 'getGamepads', { value: () => [window.testPad, null, null, null] });
    });
    const press = async index => {
      await page.evaluate(async index => {
        const button = window.testPad.buttons[index];
        const frame = () => new Promise(resolve => requestAnimationFrame(resolve));
        button.pressed = true;
        button.value = 1;
        await frame();
        await frame();
        button.pressed = false;
        button.value = 0;
        await frame();
        await frame();
      }, index);
    };
    await press(0);
    await page.locator('#appearance-panel').waitFor();
    await press(13);
    await page.waitForFunction(() => document.activeElement.id === 'appearance-theme');
    await press(15);
    await page.waitForFunction(() => document.querySelector('#appearance-theme').value === 'volcano');
    await page.waitForFunction(() => !document.querySelector('#appearance-button').disabled);
    await press(1);
    await page.locator('#appearance-panel').waitFor({ state: 'hidden' });
    assert.equal(await page.evaluate(() => document.activeElement.id), 'appearance-button');

    await page.evaluate(() => Object.defineProperty(document, 'hasFocus', { value: () => false, configurable: true }));
    await press(0);
    assert.equal(await page.locator('#appearance-panel').isHidden(), true);
    await page.evaluate(async () => {
      delete document.hasFocus;
      const frame = () => new Promise(resolve => requestAnimationFrame(resolve));
      await frame();
      await frame();
    });
    await page.keyboard.press('Enter');
    await page.locator('#appearance-panel').waitFor();
    await page.keyboard.press('Escape');
    await page.locator('#appearance-panel').waitFor({ state: 'hidden' });
    await page.evaluate(() => {
      window.scrollTo(0, 0);
      window.testPad.axes[3] = 0.9;
    });
    await page.waitForFunction(() => window.scrollY > 0, undefined, { timeout: 5000 });
    await page.evaluate(() => { window.testPad.axes[3] = 0; });
    assert.equal(await page.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth), true);
    await app.evaluate(({ BrowserWindow }) => BrowserWindow.getAllWindows()[0].webContents.setZoomFactor(1.4));
    await page.waitForFunction(() => window.innerWidth < 600);
    assert.equal(await page.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth), true);
    const log = await fs.readFile(path.join(root, 'data', 'logs', 'launcher-electron.log'), 'utf8');
    assert.match(log, /electron: input controller=standard/);
  } finally {
    if (app) await app.close().catch(() => {});
    await fs.rm(base, { recursive: true, force: true, maxRetries: 3 });
  }
}

main().catch(error => { console.error(error); process.exitCode = 1; });
