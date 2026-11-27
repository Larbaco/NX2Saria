const assert = require('node:assert/strict');
const fs = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');
const { _electron } = require('playwright-core');

async function main() {
  const [executable, publishedApp, publishedRuntime, version = '0.0.1'] = process.argv.slice(2);
  const base = await fs.mkdtemp(path.join(os.tmpdir(), 'nx2-uninstall-'));
  const root = path.join(base, 'nx2saria');
  const active = path.join(root, 'app', version);
  const game = path.join(root, 'game');
  const data = path.join(root, 'data');
  const settings = path.join(data, 'launcher.json');
  const python = path.join(base, 'worker-python');
  const dump = path.join(base, 'original.synthetic');
  const save = path.join(game, 'progress.sav');
  const entry = path.join(root, 'nx2saria-launcher');
  const initial = JSON.stringify({ auto_check: false, game_dir: game, dump, telemetry: 'off' });
  let app;
  try {
    await fs.mkdir(path.join(game, 'build-cache'), { recursive: true });
    await fs.mkdir(data);
    await fs.writeFile(entry, '');
    if (executable) {
      await fs.cp(publishedApp, active, { recursive: true, verbatimSymlinks: true });
      await fs.symlink(path.resolve(publishedRuntime), path.join(root, 'python'), 'dir');
    } else {
      await fs.mkdir(path.join(active, 'tools'), { recursive: true });
      await fs.writeFile(path.join(active, 'VERSION'), version);
      await fs.writeFile(path.join(active, 'tools', 'make_game.py'), '');
      await fs.writeFile(python, '#!/bin/sh\nexec "$NX2_TEST_UNINSTALL_PYTHON" "$@" --app-version 0.0.1\n', { mode: 0o755 });
    }
    await fs.writeFile(path.join(root, 'app', 'current.txt'), version);
    await fs.writeFile(path.join(data, 'electron-preferences.json'), '{"locale":"pt-BR"}');
    await fs.writeFile(settings, initial);
    await fs.writeFile(dump, 'dump fixture');
    await fs.writeFile(save, 'save fixture');
    const receipt = path.join(game, 'build-cache', 'state.json');
    await fs.writeFile(receipt, JSON.stringify({ kit: '0.0.0', steps: { compile: {} } }));
    await fs.writeFile(path.join(game, 'ocarina'), 'not an executable');
    const options = executable
      ? { executablePath: path.resolve(executable), args: ['--user-data-dir=' + path.join(base, 'electron-profile')] }
      : { args: ['--no-sandbox', '.'], cwd: path.resolve(__dirname, '..') };
    app = await _electron.launch({ ...options,
      env: { ...process.env, HOME: base, XDG_DATA_HOME: base, GSETTINGS_BACKEND: 'memory',
        NX2SARIA_ELECTRON_TEST_SEND_REPORTS: '0', NX2SARIA_TELEMETRY_URL: 'none',
        NX2SARIA_PYTHON: python, NX2_TEST_UNINSTALL_PYTHON: process.env.NX2SARIA_PYTHON || 'python3' } });
    const page = await app.firstWindow();
    const status = page.locator('#status');
    const play = page.getByRole('button', { name: 'Jogar', exact: true });
    await page.getByText(`O jogo foi compilado com 0.0.0. Recompilar o jogo atualiza para ${version}.`, { exact: true }).waitFor();
    await page.waitForFunction(() => !document.querySelector('#play').disabled);
    assert.equal(await page.locator('#rebuild[data-rebuild-required]').isEnabled(), true);
    await app.evaluate(({ dialog }, selected) => {
      dialog.showOpenDialog = async () => ({ canceled: false, filePaths: [selected] });
    }, root);
    await fs.writeFile(receipt, JSON.stringify({ version: 2, kit: version, steps: { exefs: {}, romfs: {} } }));
    await page.locator('#select').click();
    await status.getByText('Recompilação necessária', { exact: true }).waitFor();
    await page.waitForFunction(() => !document.querySelector('#rebuild').disabled);
    assert.equal(await play.isDisabled(), true);
    assert.equal((await page.evaluate(() => window.launcher.play())).error, 'rebuild_required');
    await fs.writeFile(receipt, JSON.stringify({ kit: version, steps: { compile: {} } }));
    await page.locator('#select').click();
    await status.getByText('Instalação encontrada', { exact: true }).waitFor();
    await page.waitForFunction(() => !document.querySelector('#play').disabled);
    const workerLog = await fs.readFile(path.join(data, 'logs', 'launcher.log'), 'utf8');
    assert.doesNotMatch(workerLog, /worker: spawn started/);
    await page.getByRole('button', { name: 'Configurações', exact: true }).click();

    const open = page.getByRole('button', { name: 'Desinstalar NX2Saria', exact: true });
    const modal = page.getByRole('dialog', { name: 'Desinstalar NX2Saria' });
    const remove = modal.getByRole('checkbox', { name: 'Remover o jogo compilado' });
    const electronLog = path.join(data, 'logs', 'launcher-electron.log');
    await open.click();
    assert.equal(await remove.isChecked(), false);
    assert.equal(await modal.getByRole('button', { name: 'Cancelar' }).evaluate(button => button === document.activeElement), true);
    await remove.check();
    await modal.getByText('A pasta do jogo será removida, incluindo saves e mods.').waitFor();
    await page.keyboard.press('Escape');
    await modal.waitFor({ state: 'hidden' });
    assert.equal(await fs.readFile(settings, 'utf8'), initial);
    assert.equal(await fs.readFile(save, 'utf8'), 'save fixture');
    assert.match(await fs.readFile(electronLog, 'utf8'), /electron: uninstall result=cancelled/);

    await open.click();
    assert.equal(await remove.isChecked(), false);
    await modal.getByText('A pasta do jogo, os saves e os mods serão preservados.').waitFor();
    await Promise.all([page.waitForEvent('close', { timeout: 10000 }),
      modal.getByRole('button', { name: 'Desinstalar', exact: true }).click()]);
    const deadline = Date.now() + 10000;
    while (await fs.access(active).then(() => true, () => false)) {
      assert.ok(Date.now() < deadline, 'Uninstall did not remove the synthetic launcher');
      await new Promise(resolve => setTimeout(resolve, 100));
    }
    assert.equal(await fs.readFile(save, 'utf8'), 'save fixture');
    assert.equal(await fs.readFile(dump, 'utf8'), 'dump fixture');
    assert.match(await fs.readFile(electronLog, 'utf8'), /electron: uninstall remove_game=false result=scheduled/);
  } finally {
    if (app) await app.close().catch(() => {});
    await fs.rm(base, { recursive: true, force: true, maxRetries: 3 });
  }
}

main().catch(error => { console.error(error); process.exitCode = 1; });
