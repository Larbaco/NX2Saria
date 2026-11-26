const assert = require('node:assert/strict');
const fs = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');
const { _electron } = require('playwright-core');

async function main() {
  const base = await fs.mkdtemp(path.join(os.tmpdir(), 'nx2-language-'));
  const root = path.join(base, 'nx2saria');
  const game = path.join(root, 'game');
  const settings = path.join(root, 'data', 'launcher.json');
  const preferences = path.join(root, 'data', 'electron-preferences.json');
  let app;
  try {
    await fs.mkdir(path.join(root, 'data'), { recursive: true });
    await fs.mkdir(path.join(root, 'app'));
    await fs.writeFile(path.join(root, 'app', 'current.txt'), '0.0.1');
    const initial = JSON.stringify({ game_dir: game, channel: 'preview' });
    await fs.writeFile(settings, initial);
    await fs.mkdir(path.join(game, 'build-cache'), { recursive: true });
    await fs.writeFile(path.join(game, 'build-cache', 'state.json'), '{"kit":"0.0.1","steps":{"compile":{}}}');
    await fs.writeFile(path.join(game, 'ocarina'), 'synthetic executable marker');

    const launch = () => _electron.launch({ args: ['--no-sandbox', '.'], cwd: path.resolve(__dirname, '..'),
      env: { ...process.env, HOME: base, XDG_DATA_HOME: base, NX2SARIA_TELEMETRY_URL: 'none',
        NX2SARIA_TEST_LOCALE: 'ru-RU', NX2SARIA_ELECTRON_TEST_SEND_REPORTS: '0', GSETTINGS_BACKEND: 'memory' } });
    app = await launch();
    const page = await app.firstWindow();
    const status = page.locator('#status');
    const language = page.locator('#launcher-language');
    const layout = async (locale, ready, settingsLabel) => {
      await status.getByText(ready, { exact: true }).waitFor({ timeout: 10000 });
      assert.equal(await page.locator('html').getAttribute('lang'), locale);
      assert.equal(await page.locator('#detail').textContent(), game);
      await page.getByRole('button', { name: settingsLabel, exact: true }).click();
      await language.waitFor();
      assert.equal(await page.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth), true);
    };
    await app.evaluate(({ BrowserWindow }) => {
      const window = BrowserWindow.getAllWindows()[0];
      window.setSize(720, 500);
      window.webContents.setZoomFactor(1.2);
    });
    await layout('ru', 'Установка найдена', 'Настройки');
    assert.equal(await language.inputValue(), 'automatic');
    await fs.mkdir(preferences);
    await language.selectOption('en-US');
    await page.getByText('Не удалось сохранить язык.', { exact: true }).waitFor();
    assert.equal(await language.inputValue(), 'automatic');
    assert.equal(await page.locator('html').getAttribute('lang'), 'ru');
    await fs.rmdir(preferences);
    await language.selectOption('zh-Hans');
    await layout('zh-Hans', '已找到安装', '设置');
    await page.getByRole('button', { name: '选择游戏转储', exact: true }).waitFor();
    await app.evaluate(({ dialog }) => {
      dialog.showOpenDialog = async (_, options) => {
        globalThis.localizedTitle = options.title;
        return { canceled: true, filePaths: [] };
      };
    });
    await page.getByRole('button', { name: '选择游戏转储', exact: true }).click();
    await page.waitForFunction(() => !document.querySelector('#settings-dump').disabled);
    assert.equal(await app.evaluate(() => globalThis.localizedTitle), '选择游戏转储');
    await language.selectOption('en-US');
    await layout('en-US', 'Installation found', 'Settings');
    await language.selectOption('pt-BR');
    await layout('pt-BR', 'Instalação encontrada', 'Configurações');
    assert.equal(await fs.readFile(settings, 'utf8'), initial);
    assert.equal(JSON.parse(await fs.readFile(preferences, 'utf8')).locale, 'pt-BR');
    await app.close();
    app = await launch();
    const reopened = await app.firstWindow();
    await reopened.locator('#status').getByText('Instalação encontrada', { exact: true }).waitFor({ timeout: 10000 });
    assert.equal(await reopened.locator('html').getAttribute('lang'), 'pt-BR');
  } finally {
    if (app) await app.close().catch(() => {});
    await fs.rm(base, { recursive: true, force: true, maxRetries: 3 });
  }
}

main().catch(error => { console.error(error); process.exitCode = 1; });
