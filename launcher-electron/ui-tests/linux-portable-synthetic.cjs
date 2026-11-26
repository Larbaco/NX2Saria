const assert = require('node:assert/strict');
const { execFileSync } = require('node:child_process');
const fs = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');
const { _electron } = require('playwright-core');

async function main() {
  assert.equal(process.platform, 'linux');
  const [executable, publishedApp, publishedRuntime, version] = process.argv.slice(2);
  assert.ok(executable && publishedApp && publishedRuntime && version);
  const base = await fs.mkdtemp(path.join(os.tmpdir(), 'nx2-portable-linux-ui-'));
  const root = path.join(base, 'nx2saria');
  const game = path.join(root, 'game');
  const logs = path.join(root, 'data', 'logs');
  let app;
  try {
    await fs.mkdir(path.join(game, 'build-cache'), { recursive: true });
    await fs.mkdir(path.join(game, 'logs'));
    await fs.mkdir(path.join(root, 'data'));
    await fs.mkdir(path.join(root, 'app'));
    await fs.writeFile(path.join(root, 'nx2saria-launcher'), '');
    await fs.cp(publishedApp, path.join(root, 'app', version), { recursive: true, verbatimSymlinks: true });
    await fs.symlink(path.resolve(publishedRuntime), path.join(root, 'python'), 'dir');
    await fs.writeFile(path.join(root, 'app', 'current.txt'), version);
    const settingsFile = path.join(root, 'data', 'launcher.json');
    const settings = { game_dir: game, hide_while_playing: true, telemetry: 'off', telemetry_asked: version };
    await fs.writeFile(settingsFile, JSON.stringify(settings));
    await fs.writeFile(path.join(game, 'build-cache', 'state.json'), JSON.stringify({ kit: version, steps: { compile: {} } }));
    await fs.writeFile(path.join(game, 'ocarina'), "#!/bin/sh\nprintf '%s\\n' '[host] gfx: window gained the focus at synthetic time' > logs/nx2saria.log\nsleep 2\nprintf '%s\\n' 'FATAL: portable synthetic failure' >> logs/nx2saria.log\nexit 71\n", { mode: 0o755 });

    const launch = () => _electron.launch({
      executablePath: path.resolve(executable),
      args: ['--user-data-dir=' + path.join(base, 'electron-profile')],
      env: { ...process.env, HOME: base, XDG_DATA_HOME: base, GSETTINGS_BACKEND: 'memory',
        NX2SARIA_TELEMETRY_URL: 'http://127.0.0.1:1', NX2SARIA_ELECTRON_TEST_SEND_REPORTS: '0' },
    });
    app = await launch();
    const page = await app.firstWindow();
    await page.locator('#status').getByText('Instalação encontrada').waitFor({ timeout: 15000 });
    const hide = page.getByRole('checkbox', { name: 'Ocultar o launcher ao jogar' });
    await hide.uncheck();
    await page.waitForFunction(() => !document.querySelector('#hide-while-playing').disabled);
    assert.deepEqual(JSON.parse(await fs.readFile(settingsFile, 'utf8')), { ...settings, hide_while_playing: false });
    await hide.check();
    await page.waitForFunction(() => !document.querySelector('#hide-while-playing').disabled);
    assert.deepEqual(JSON.parse(await fs.readFile(settingsFile, 'utf8')), settings);
    const slot = path.join(game, 'profile', 'save', '01'.repeat(16), 'slot_00');
    await fs.mkdir(slot, { recursive: true });
    await fs.writeFile(path.join(game, 'profile', 'account.uid'), Buffer.alloc(16, 1));
    const progress = path.join(slot, 'progress.sav');
    await fs.writeFile(progress, 'portable synthetic save');
    const exportsDir = path.join(base, 'exports');
    await fs.mkdir(exportsDir);
    const pick = selected => app.evaluate(({ dialog }, chosen) => {
      dialog.showOpenDialog = async () => ({ canceled: false, filePaths: [chosen] });
    }, selected);
    await page.getByRole('button', { name: 'Saves', exact: true }).click();
    await pick(exportsDir);
    await page.getByRole('button', { name: 'Escolher pasta de exportação' }).click();
    await page.locator('#save-export-destination').getByText(exportsDir, { exact: true }).waitFor();
    await page.getByRole('button', { name: 'Exportar save' }).click();
    await page.getByText(/Save exportado em/).waitFor();
    const [archive] = await fs.readdir(exportsDir);
    await fs.writeFile(progress, 'portable newer save');
    await pick(path.join(exportsDir, archive));
    await page.getByRole('button', { name: 'Importar save', exact: true }).click();
    await page.getByText(/Save importado/).waitFor();
    assert.equal(await fs.readFile(progress, 'utf8'), 'portable synthetic save');
    assert.equal((await fs.readdir(path.join(game, 'saves'))).some(name => name.startsWith('backup ')), true);
    await page.getByRole('button', { name: 'Saves', exact: true }).click();
    const mod = path.join(base, 'Portable Fixture');
    await fs.mkdir(path.join(mod, 'romfs'), { recursive: true });
    await fs.writeFile(path.join(mod, 'romfs', 'stand-in.txt'), 'portable synthetic mod');
    await page.getByRole('button', { name: 'Mods', exact: true }).click();
    await pick(mod);
    await page.getByRole('button', { name: 'Adicionar pasta de mod' }).click();
    await page.getByText('Mod adicionado: Portable Fixture.', { exact: true }).waitFor();
    assert.equal(await fs.readFile(path.join(game, 'mods', 'Portable Fixture', 'romfs', 'stand-in.txt'), 'utf8'),
      'portable synthetic mod');
    await page.getByRole('button', { name: 'Mods', exact: true }).click();
    await page.getByRole('button', { name: 'Jogar' }).click();
    await page.locator('#status').getByText('O jogo terminou com erro').waitFor({ timeout: 15000 });
    assert.match(await page.locator('#detail').textContent(), /code 71\): portable synthetic failure/);
    assert.equal(await app.evaluate(({ BrowserWindow }) => BrowserWindow.getAllWindows()[0].isVisible()), true);
    const marker = JSON.parse(await fs.readFile(path.join(game, 'logs', 'crash-offer.json'), 'utf8'));
    assert.equal(marker.kind, 'crash');
    const offer = page.locator('#reports-dialog');
    await offer.getByText('Enviar o log deste erro?', { exact: true }).waitFor({ timeout: 10000 });
    assert.equal(await offer.getByRole('button', { name: 'Enviar log' }).isDisabled(), true);
    await offer.getByRole('button', { name: 'Ver o que é enviado' }).click();
    await offer.locator('#reports-preview-text').waitFor();
    const prepared = JSON.parse(await offer.locator('#reports-preview-text').textContent());
    assert.equal(prepared.kind, 'crash');
    assert.match(prepared.log_tail, /FATAL: portable synthetic failure/);
    await offer.getByRole('button', { name: 'Voltar' }).click();
    await offer.getByRole('button', { name: 'Não enviar' }).click();
    await offer.waitFor({ state: 'hidden' });
    await assert.rejects(fs.access(path.join(game, 'logs', 'crash-offer.json')), { code: 'ENOENT' });
    const workerLog = await fs.readFile(path.join(logs, 'launcher.log'), 'utf8');
    assert.match(workerLog, /\(AppImage install, started by Electron\)/);
    assert.match(workerLog, /worker: runtime=.*python\/bin\/python3 python=3\.14\./);
    assert.match(workerLog, /setting: hide_while_playing on -> off/);
    assert.match(workerLog, /setting: hide_while_playing off -> on/);
    await app.close();
    app = null;
    const electronLog = await fs.readFile(path.join(logs, 'launcher-electron.log'), 'utf8');
    assert.match(electronLog, /electron: window hidden_for_game/);
    assert.match(electronLog, /electron: window restored/);
    assert.match(electronLog, /electron: window message_shown kind=ended_with_error/);
    assert.match(electronLog, /electron: exit code=0 play_active=false/);
    assert.doesNotMatch(electronLog, /portable synthetic failure|nx2-portable-linux-ui-|ocarina/);
    const oldMarker = path.join(root, 'data', 'job-running.json');
    await fs.writeFile(oldMarker, '{broken published test marker');
    app = await launch();
    let reopened = await app.firstWindow();
    await reopened.getByText('Não foi possível verificar a compilação anterior', { exact: true }).waitFor({ timeout: 10000 });
    assert.equal(await reopened.getByRole('button', { name: 'Jogar' }).isDisabled(), true);
    await app.evaluate(({ dialog }) => {
      dialog.showMessageBox = async () => ({ response: 1, checkboxChecked: false });
    });
    await reopened.getByRole('button', { name: 'Recuperar instalação' }).click();
    await reopened.getByText(/A compilação anterior não terminou/).waitFor({ timeout: 10000 });
    await assert.rejects(fs.access(oldMarker), { code: 'ENOENT' });
    await app.close();
    app = null;

    const dump = path.join(base, 'valid-synthetic.nup');
    const repository = path.resolve(__dirname, '../..');
    execFileSync('python3', [path.join(repository, 'tests', 'fixtures', 'worker_install_dump.py'), dump],
      { cwd: repository, env: { ...process.env, PYTHONPATH: repository } });
    const firstRun = { telemetry: 'off', dump };
    await fs.writeFile(settingsFile, JSON.stringify(firstRun));
    app = await launch();
    reopened = await app.firstWindow();
    await reopened.locator('#game-dump-proposal').getByText(dump, { exact: true }).waitFor({ timeout: 15000 });
    assert.equal(await reopened.locator('#first-run').getByLabel('Canal de atualizações').inputValue(), 'stable');
    assert.ok(path.isAbsolute(await reopened.locator('#game-folder-proposal').textContent()));
    assert.equal(await reopened.getByRole('button', { name: 'Iniciar instalação' }).isEnabled(), true);
    assert.deepEqual(JSON.parse(await fs.readFile(settingsFile, 'utf8')), firstRun);
    console.log('Packaged Linux Electron: published runtime, Saves, Mods, Play, crash offer and preview, marker recovery and first run passed');
  } finally {
    if (app) await app.close().catch(() => {});
    await fs.rm(base, { recursive: true, force: true, maxRetries: 3 });
  }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
