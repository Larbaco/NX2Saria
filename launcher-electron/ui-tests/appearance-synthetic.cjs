const assert = require('node:assert/strict');
const { execFileSync } = require('node:child_process');
const fs = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');
const { _electron } = require('playwright-core');

async function main() {
  const base = await fs.mkdtemp(path.join(os.tmpdir(), 'nx2-appearance-'));
  const root = path.join(base, 'nx2saria');
  const game = path.join(root, 'game');
  const settings = path.join(root, 'data', 'launcher.json');
  let app;
  try {
    await fs.mkdir(path.join(root, 'data'), { recursive: true });
    await fs.mkdir(path.join(root, 'app'));
    await fs.writeFile(path.join(root, 'app', 'current.txt'), '0.0.1');
    await fs.writeFile(settings, JSON.stringify({ game_dir: game, theme: 'lake', future_option: 'preserved' }));
    await fs.mkdir(path.join(game, 'build-cache'), { recursive: true });
    await fs.writeFile(path.join(game, 'build-cache', 'state.json'), '{"kit":"0.0.1","steps":{"compile":{}}}');
    await fs.writeFile(path.join(game, 'ocarina'), 'synthetic executable marker');

    app = await _electron.launch({ args: ['--no-sandbox', '.'], cwd: path.resolve(__dirname, '..'),
      env: { ...process.env, HOME: base, XDG_DATA_HOME: base, NX2SARIA_TELEMETRY_URL: 'none',
        NX2SARIA_ELECTRON_TEST_SEND_REPORTS: '0', GSETTINGS_BACKEND: 'memory' } });
    const page = await app.firstWindow();
    await page.getByRole('button', { name: 'Personalizar', exact: true }).click({ timeout: 6000 });
    const scheme = page.getByLabel('Tema de cores');
    assert.equal(await scheme.inputValue(), 'lake');
    const original = await page.evaluate(() => document.documentElement.style.getPropertyValue('--shell-panel'));
    await scheme.selectOption('custom');
    const color = page.getByLabel('Cor dos painéis');
    await color.waitFor();
    await color.fill('#6d426d');
    await page.waitForFunction(() => !document.querySelector('#appearance-color-panel').disabled);
    const current = await page.evaluate(() => document.documentElement.style.getPropertyValue('--shell-panel'));
    assert.notEqual(current, original);
    assert.equal(JSON.parse(await fs.readFile(settings, 'utf8')).custom_panel, '#6d426d');
    await page.getByRole('button', { name: 'Restaurar cor dos painéis' }).click();
    await page.waitForFunction(() => !document.querySelector('#appearance-color-panel').disabled);
    assert.equal(JSON.parse(await fs.readFile(settings, 'utf8')).custom_panel, '');
    await page.getByRole('checkbox', { name: 'Animações da interface' }).uncheck();
    await page.waitForFunction(() => document.documentElement.dataset.animations === 'off');
    await page.getByLabel('Sons da interface').selectOption('off');
    await page.waitForFunction(() => !document.querySelector('#appearance-sounds').disabled);
    const saved = JSON.parse(await fs.readFile(settings, 'utf8'));
    assert.deepEqual({ theme: saved.theme, animations: saved.animations, sounds: saved.sounds,
      untouched: saved.future_option }, { theme: 'custom', animations: false, sounds: 'off', untouched: 'preserved' });
    const selected = path.join(base, 'test-background.png');
    const invalid = path.join(base, 'test-invalid.png');
    execFileSync(process.env.NX2SARIA_PYTHON || 'python3',
      ['-c', 'from PIL import Image; import sys; Image.new("RGB", (18, 14), "green").save(sys.argv[1])', selected]);
    await fs.writeFile(invalid, 'invalid image');
    const pick = file => app.evaluate(({ dialog }, selected) => {
      dialog.showOpenDialog = async () => ({ canceled: !selected, filePaths: selected ? [selected] : [] });
    }, file);
    const choose = page.getByRole('button', { name: 'Escolher imagem de fundo' });
    await pick(selected);
    await choose.click();
    await page.waitForFunction(() => document.querySelector('#shell-background').naturalWidth === 1200);
    await page.getByLabel('Escurecimento da imagem').selectOption('80');
    await page.waitForFunction(() => Number(getComputedStyle(document.querySelector('#shell-background')).opacity) === 0.2);
    await pick(invalid);
    await choose.click();
    await page.getByText('Não foi possível ler a imagem escolhida.', { exact: true }).waitFor();
    assert.equal(await page.locator('#shell-background').isVisible(), true);
    await pick(null);
    await choose.click();
    await page.waitForFunction(() => !document.querySelector('#appearance-picture-choose').disabled);
    assert.equal(JSON.parse(await fs.readFile(settings, 'utf8')).background, selected);
    await page.getByRole('button', { name: 'Remover imagem de fundo' }).click();
    await page.locator('#shell-background').waitFor({ state: 'hidden' });
    assert.equal(JSON.parse(await fs.readFile(settings, 'utf8')).background, '');
    await fs.access(selected);
    const log = await fs.readFile(path.join(root, 'data', 'logs', 'launcher-electron.log'), 'utf8');
    assert.match(log, /electron: appearance key=custom_panel result=saved/);
    assert.match(log, /electron: appearance background choose result=invalid_picture/);
    assert.doesNotMatch(log, /test-background\.png|nx2-appearance-/);
  } finally {
    if (app) await app.close().catch(() => {});
    await fs.rm(base, { recursive: true, force: true, maxRetries: 3 });
  }
}

main().catch(error => { console.error(error); process.exitCode = 1; });
