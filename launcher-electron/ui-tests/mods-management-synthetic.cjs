const assert = require('node:assert/strict');
const { execFileSync } = require('node:child_process');
const fs = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');
const { _electron } = require('playwright-core');

async function main() {
  const base = await fs.mkdtemp(path.join(os.tmpdir(), 'nx2-mod-management-'));
  const root = path.join(base, 'nx2saria');
  const game = path.join(root, 'game');
  let app;
  try {
    await fs.mkdir(path.join(root, 'app'), { recursive: true });
    await fs.mkdir(path.join(root, 'data'));
    await fs.writeFile(path.join(root, 'app', 'current.txt'), '0.0.1');
    await fs.writeFile(path.join(root, 'data', 'launcher.json'), JSON.stringify({ game_dir: game }));
    await fs.mkdir(path.join(game, 'build-cache'), { recursive: true });
    await fs.writeFile(path.join(game, 'build-cache', 'state.json'), '{"kit":"0.0.1","steps":{"compile":{}}}');
    await fs.writeFile(path.join(game, 'ocarina'), 'synthetic executable marker');
    const alpha = path.join(base, 'Alpha');
    await fs.mkdir(path.join(alpha, 'romfs'), { recursive: true });
    await fs.writeFile(path.join(alpha, 'romfs', 'shared.txt'), 'first asset');
    const beta = path.join(base, 'Beta.zip');
    execFileSync('python3', ['-c', "import sys,zipfile; z=zipfile.ZipFile(sys.argv[1],'w'); z.writestr('romfs/shared.txt','second asset'); z.close()", beta]);
    const invalid = path.join(base, 'invalid.txt');
    await fs.writeFile(invalid, 'stand-in unsupported archive');

    app = await _electron.launch({ args: ['--no-sandbox', '.'], cwd: path.resolve(__dirname, '..'),
      env: { ...process.env, HOME: base, XDG_DATA_HOME: base } });
    const page = await app.firstWindow();
    await page.getByRole('button', { name: 'Mods', exact: true }).click({ timeout: 6000 });
    await page.getByText('Nenhum mod instalado.', { exact: true }).waitFor();
    const select = selected => app.evaluate(({ dialog }, chosen) => {
      dialog.showOpenDialog = async () => ({ canceled: !chosen, filePaths: chosen ? [chosen] : [] });
    }, selected);
    await select(alpha);
    await page.getByRole('button', { name: 'Adicionar pasta de mod' }).click();
    const alphaToggle = page.getByRole('checkbox', { name: 'Alpha', exact: true });
    await alphaToggle.waitFor();
    assert.equal(await alphaToggle.isChecked(), true);
    await select(beta);
    await page.getByRole('button', { name: 'Adicionar arquivo de mod' }).click();
    const betaToggle = page.getByRole('checkbox', { name: 'Beta', exact: true });
    await betaToggle.waitFor();
    await page.getByRole('button', { name: 'Subir Beta', exact: true }).click();
    await page.waitForFunction(() => document.querySelector('#mods-list li input')?.getAttribute('aria-label') === 'Beta');
    await alphaToggle.click();
    await page.waitForFunction(() => document.querySelector('#mods-list input[aria-label="Alpha"]:not(:checked):not(:disabled)'));
    assert.equal(await alphaToggle.isChecked(), false);
    await alphaToggle.click();
    await page.getByText(/conflito com Alpha.*Beta vence/).waitFor();
    await select('');
    await page.getByRole('button', { name: 'Adicionar arquivo de mod' }).click();
    await page.getByRole('button', { name: 'Adicionar arquivo de mod' }).waitFor();
    await select(invalid);
    await page.getByRole('button', { name: 'Adicionar arquivo de mod' }).click();
    await page.getByText(/no archive/).waitFor({ timeout: 5000 });
    assert.equal(await page.locator('#mods-list li').count(), 2);
    const log = await fs.readFile(path.join(root, 'data', 'logs', 'launcher-electron.log'), 'utf8');
    assert.match(log, /electron: mods mod_add result=ok/);
    assert.match(log, /electron: mods mod_move result=ok/);
    assert.match(log, /electron: mods mod_enabled result=ok/);
    assert.match(log, /electron: mods mod_add result=invalid_mod/);
    assert.doesNotMatch(log, /Alpha|Beta|nx2-mod-management-/);
  } finally {
    if (app) await app.close();
    await fs.rm(base, { recursive: true, force: true });
  }
}

main().catch(error => { console.error(error); process.exitCode = 1; });
