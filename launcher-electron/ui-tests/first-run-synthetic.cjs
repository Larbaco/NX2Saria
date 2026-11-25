const assert = require('node:assert/strict');
const fs = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');
const { execFileSync } = require('node:child_process');
const { _electron } = require('playwright-core');

async function main() {
  const base = await fs.mkdtemp(path.join(os.tmpdir(), 'nx2-first-run-ui-'));
  const root = path.join(base, 'nx2saria');
  const settings = path.join(root, 'data', 'launcher.json');
  const folder = path.join(base, 'games');
  let app;
  try {
    await fs.mkdir(path.join(root, 'app'), { recursive: true });
    await fs.mkdir(path.join(root, 'data'));
    await fs.writeFile(path.join(root, 'app', 'current.txt'), '0.0.1');
    await fs.writeFile(settings, '{}');
    await fs.mkdir(folder);
    await fs.writeFile(path.join(folder, 'unrelated.txt'), 'keep');
    app = await _electron.launch({
      args: ['--no-sandbox', '.'],
      cwd: path.resolve(__dirname, '..'),
      env: { ...process.env, XDG_DATA_HOME: base }
    });
    const page = await app.firstWindow();
    const location = page.locator('#game-folder-proposal');
    const pick = page.getByRole('button', { name: 'Escolher pasta do jogo' });
    await pick.waitFor({ timeout: 10000 });
    await location.getByText(path.join(root, 'game'), { exact: true }).waitFor({ timeout: 10000 });

    await app.evaluate(({ dialog }, selected) => {
      dialog.showOpenDialog = async () => ({ canceled: false, filePaths: [selected] });
    }, folder);
    await pick.click();
    await location.getByText(path.join(folder, 'NX2Saria'), { exact: true }).waitFor({ timeout: 10000 });

    await app.evaluate(({ dialog }) => {
      dialog.showOpenDialog = async () => ({ canceled: true, filePaths: [] });
    });
    await pick.click();
    await page.waitForFunction(() => !document.querySelector('#pick-game-folder').disabled);
    assert.equal(await location.textContent(), path.join(folder, 'NX2Saria'));
    const valid = path.join(base, 'synthetic-game.nup');
    const invalid = path.join(base, 'invalid.nup');
    const repository = path.resolve(__dirname, '../..');
    execFileSync('python3', [path.join(repository, 'tests', 'fixtures', 'worker_install_dump.py'), valid],
      { cwd: repository, env: { ...process.env, PYTHONPATH: repository } });
    await fs.writeFile(invalid, 'synthetic invalid metadata');
    const dump = page.locator('#game-dump-proposal');
    const pickDump = page.getByRole('button', { name: 'Escolher dump do jogo' });
    await app.evaluate(({ dialog }, file) => {
      dialog.showOpenDialog = async () => ({ canceled: false, filePaths: [file] });
    }, valid);
    await pickDump.click({ timeout: 5000 });
    await dump.getByText(valid, { exact: true }).waitFor({ timeout: 5000 });
    await app.evaluate(({ dialog }, file) => {
      dialog.showOpenDialog = async () => ({ canceled: false, filePaths: [file] });
    }, invalid);
    await pickDump.click();
    await page.getByText('O arquivo selecionado não é um dump válido do jogo.').waitFor({ timeout: 5000 });
    assert.equal(await dump.textContent(), valid);
    await app.evaluate(({ dialog }) => {
      dialog.showOpenDialog = async () => ({ canceled: true, filePaths: [] });
    });
    await pickDump.click();
    await page.waitForFunction(() => !document.querySelector('#pick-game-dump').disabled);
    assert.equal(await dump.textContent(), valid);
    assert.deepEqual(await fs.readdir(folder), ['unrelated.txt']);
    assert.equal(await fs.readFile(settings, 'utf8'), '{}');
  } finally {
    if (app) await app.close();
    await fs.rm(base, { recursive: true, force: true });
  }
}

main().catch(error => { console.error(error); process.exitCode = 1; });
