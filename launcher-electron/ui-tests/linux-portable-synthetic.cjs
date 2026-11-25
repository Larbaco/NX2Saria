const assert = require('node:assert/strict');
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
    await fs.cp(publishedApp, path.join(root, 'app', version), { recursive: true, verbatimSymlinks: true });
    await fs.symlink(path.resolve(publishedRuntime), path.join(root, 'python'), 'dir');
    await fs.writeFile(path.join(root, 'app', 'current.txt'), version);
    await fs.writeFile(path.join(root, 'data', 'launcher.json'), JSON.stringify({ game_dir: game, hide_while_playing: true }));
    await fs.writeFile(path.join(game, 'build-cache', 'state.json'), JSON.stringify({ kit: version, steps: { compile: {} } }));
    await fs.writeFile(path.join(game, 'ocarina'), "#!/bin/sh\nprintf '%s\\n' 'FATAL: portable synthetic failure' > logs/nx2saria.log\nsleep 1\nexit 71\n", { mode: 0o755 });

    app = await _electron.launch({
      executablePath: path.resolve(executable),
      args: ['--user-data-dir=' + path.join(base, 'electron-profile')],
      env: { ...process.env, XDG_DATA_HOME: base },
    });
    const page = await app.firstWindow();
    await page.locator('#status').getByText('Instalação encontrada').waitFor({ timeout: 15000 });
    await page.getByRole('button', { name: 'Jogar' }).click();
    await page.locator('#status').getByText('O jogo terminou com erro').waitFor({ timeout: 15000 });
    assert.match(await page.locator('#detail').textContent(), /code 71\): portable synthetic failure/);
    assert.equal(await app.evaluate(({ BrowserWindow }) => BrowserWindow.getAllWindows()[0].isVisible()), true);
    const marker = JSON.parse(await fs.readFile(path.join(game, 'logs', 'crash-offer.json'), 'utf8'));
    assert.equal(marker.kind, 'crash');
    const workerLog = await fs.readFile(path.join(logs, 'launcher.log'), 'utf8');
    assert.match(workerLog, /worker: runtime=.*python\/bin\/python3 python=3\.14\./);
    await app.close();
    app = null;
    const electronLog = await fs.readFile(path.join(logs, 'launcher-electron.log'), 'utf8');
    assert.match(electronLog, /electron: window hidden_for_game/);
    assert.match(electronLog, /electron: window restored/);
    assert.match(electronLog, /electron: window message_shown kind=ended_with_error/);
    assert.match(electronLog, /electron: exit code=0 play_active=false/);
    assert.doesNotMatch(electronLog, /portable synthetic failure|nx2-portable-linux-ui-|ocarina/);
    console.log('Packaged Linux Electron: installation runtime, Play, error screen and crash-offer marker passed');
  } finally {
    if (app) await app.close().catch(() => {});
    await fs.rm(base, { recursive: true, force: true });
  }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
