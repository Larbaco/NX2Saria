const { _electron } = require('playwright-core');
const fs = require('node:fs/promises');
const path = require('node:path');
const os = require('node:os');
const assert = require('node:assert/strict');

async function expectVisibility(app, expected) {
  for (let i = 0; i < 60; i++) {
    if (await app.evaluate(({ BrowserWindow }) => BrowserWindow.getAllWindows()[0].isVisible()) === expected) return;
    await new Promise(resolve => setTimeout(resolve, 100));
  }
  throw new Error(`Window visibility did not become ${expected}`);
}

(async () => {
  if (process.platform !== 'linux') throw new Error('This synthetic UI test currently requires Linux');
  const base = await fs.mkdtemp(path.join(os.tmpdir(), 'nx2saria-electron-ui-'));
  const root = path.join(base, 'nx2saria');
  let app;
  try {
    const game = path.join(root, 'game');
    const script = path.join(root, 'app', 'v1', 'launcher', 'start.pyw');
    const fakePython = path.join(root, 'python', 'bin', 'python3');
    const sentinel = path.join(root, 'called.json');
    await fs.mkdir(path.join(root, 'data', 'logs'), { recursive: true });
    await fs.mkdir(path.join(game, 'build-cache'), { recursive: true });
    await fs.mkdir(path.dirname(script), { recursive: true });
    await fs.mkdir(path.dirname(fakePython), { recursive: true });
    await fs.writeFile(path.join(root, 'data', 'launcher.json'), JSON.stringify({ game_dir: game }));
    await fs.copyFile('/usr/bin/sleep', path.join(game, 'ocarina'));
    await fs.chmod(path.join(game, 'ocarina'), 0o755);
    await fs.writeFile(path.join(game, 'build-cache', 'state.json'), JSON.stringify({ kit: 'test', steps: { compile: {} } }));
    await fs.writeFile(path.join(root, 'app', 'current.txt'), 'v1');
    await fs.writeFile(script, 'fixture');
    await fs.writeFile(fakePython, `#!${process.execPath}\nrequire('node:fs').writeFileSync(${JSON.stringify(sentinel)}, JSON.stringify(process.argv.slice(2)));\n`, { mode: 0o755 });
    app = await _electron.launch({ args: ['--no-sandbox', '.'], cwd: path.resolve(__dirname, '..'), env: { ...process.env, XDG_DATA_HOME: base } });
    await app.evaluate(({ dialog }, selected) => { dialog.showOpenDialog = async () => ({ canceled: false, filePaths: [selected] }); }, root);
    const page = await app.firstWindow();
    page.on('console', message => console.log('console:', message.type(), message.text()));
    page.on('pageerror', error => console.log('pageerror:', error.message));
    await page.locator('#status').getByText('Instalação encontrada').waitFor({ timeout: 5000 });
    assert.equal(await page.getByRole('button', { name: 'Jogar' }).isEnabled(), true);
    await page.getByRole('button', { name: 'Selecionar instalação' }).click();
    await page.locator('#status').getByText('Instalação encontrada').waitFor({ timeout: 5000 });
    await app.evaluate(({ dialog }) => { dialog.showOpenDialog = async () => ({ canceled: true, filePaths: [] }); });
    await page.getByRole('button', { name: 'Selecionar instalação' }).click();
    await page.waitForFunction(() => !document.querySelector('#select').disabled);
    assert.equal(await page.getByRole('button', { name: 'Jogar' }).isDisabled(), true);
    await app.evaluate(({ dialog }, selected) => { dialog.showOpenDialog = async () => ({ canceled: false, filePaths: [selected] }); }, root);
    await page.getByRole('button', { name: 'Selecionar instalação' }).click();
    await page.waitForFunction(() => !document.querySelector('#play').disabled);
    await page.getByRole('button', { name: 'Jogar' }).click();
    await page.locator('#status').getByText('Fluxo de jogo encerrado').waitFor({ timeout: 5000 });
    assert.deepEqual(JSON.parse(await fs.readFile(sentinel, 'utf8')), ['-I', script, '--root', root, '--play']);
    const log = await fs.readFile(path.join(root, 'data', 'logs', 'launcher-electron.log'), 'utf8');
    assert.match(log, /electron: root default \(NX2Saria\)/);
    assert.match(log, /electron: inspection ready/);
    assert.match(log, /electron: play requested/);
    assert.match(log, /electron: play started/);
    assert.match(log, /electron: play ended code=0/);
    assert.equal(log.includes(root), false);
    await fs.writeFile(fakePython, `#!${process.execPath}\nsetTimeout(() => process.exit(0), 500);\n`, { mode: 0o755 });
    await page.getByRole('button', { name: 'Selecionar instalação' }).click();
    await page.waitForFunction(() => !document.querySelector('#play').disabled);
    const concurrent = await page.evaluate(() => Promise.all([window.launcher.play(), window.launcher.play()]));
    assert.equal(concurrent.filter(result => result.started).length, 1);
    await page.locator('#status').getByText('Fluxo de jogo encerrado').waitFor({ timeout: 5000 });
    await fs.writeFile(path.join(root, 'data', 'job-running.json'), '{}');
    await page.getByRole('button', { name: 'Selecionar instalação' }).click();
    await page.getByText('Há um registro de trabalho:', { exact: false }).waitFor({ timeout: 5000 });
    assert.equal(await page.getByRole('button', { name: 'Jogar' }).isDisabled(), true);
    await fs.rm(path.join(root, 'data', 'job-running.json'));
    await fs.writeFile(fakePython, `#!${process.execPath}\nconst child = require('node:child_process').spawn(${JSON.stringify(path.join(game, 'ocarina'))}, ['3']); child.on('exit', () => process.exit(0));\n`, { mode: 0o755 });
    await page.getByRole('button', { name: 'Selecionar instalação' }).click();
    await page.waitForFunction(() => !document.querySelector('#play').disabled);
    await page.getByRole('button', { name: 'Jogar' }).click();
    await expectVisibility(app, false);
    await expectVisibility(app, true);
    await page.locator('#status').getByText('Fluxo de jogo encerrado').waitFor({ timeout: 5000 });
    await fs.writeFile(path.join(root, 'data', 'launcher.json'), JSON.stringify({ game_dir: game, hide_while_playing: false }));
    await page.getByRole('button', { name: 'Selecionar instalação' }).click();
    await page.waitForFunction(() => !document.querySelector('#play').disabled);
    await page.getByRole('button', { name: 'Jogar' }).click();
    await new Promise(resolve => setTimeout(resolve, 1200));
    assert.equal(await app.evaluate(({ BrowserWindow }) => BrowserWindow.getAllWindows()[0].isVisible()), true);
    await page.locator('#status').getByText('Fluxo de jogo encerrado').waitFor({ timeout: 5000 });
    await fs.writeFile(path.join(root, 'data', 'launcher.json'), JSON.stringify({ game_dir: game }));
    await fs.writeFile(fakePython, `#!${process.execPath}\nprocess.exit(42);\n`, { mode: 0o755 });
    await page.getByRole('button', { name: 'Selecionar instalação' }).click();
    await page.waitForFunction(() => !document.querySelector('#play').disabled);
    await page.getByRole('button', { name: 'Jogar' }).click();
    await page.locator('#status').getByText('Fluxo de jogo encerrado com erro').waitFor({ timeout: 5000 });
    await fs.chmod(fakePython, 0o600);
    await page.getByRole('button', { name: 'Selecionar instalação' }).click();
    await page.getByRole('button', { name: 'Jogar' }).waitFor({ state: 'visible' });
    await page.waitForFunction(() => !document.querySelector('#play').disabled);
    await page.getByRole('button', { name: 'Jogar' }).click();
    await page.locator('#status').getByText('Não foi possível iniciar').waitFor({ timeout: 5000 });
    assert.equal(await page.getByRole('button', { name: 'Selecionar instalação' }).isEnabled(), true);
    console.log('Synthetic selection, Play, visibility, marker refusal and errors passed');
  } finally {
    if (app) await app.close();
    await fs.rm(base, { recursive: true, force: true });
  }
})().catch(error => { console.error(error); process.exitCode = 1; });
