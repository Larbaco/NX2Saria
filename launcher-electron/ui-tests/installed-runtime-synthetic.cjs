const assert = require('node:assert/strict');
const { createHash } = require('node:crypto');
const fs = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');
const { _electron } = require('playwright-core');

const makeGame = `import hashlib, json, sys, time
from pathlib import Path
game = Path(sys.argv[sys.argv.index('--out') + 1])
root = game.parent
assert Path(sys.executable).samefile(root / 'python' / 'python.exe')
assert Path(__file__).samefile(root / 'app' / '0.25.0' / 'tools' / 'make_game.py')
(game / 'build-python.txt').write_text(str(sys.version_info[:3]), encoding='utf-8')
print('@@{"ev":"step","step":"compile","state":"start"}', flush=True)
for i in range(600):
    if (game / 'finish-build').is_file():
        key = 'Synthetic/cheats/0000000000000000.txt'
        digest = hashlib.sha256((game / 'mods' / key).read_bytes()).hexdigest()
        (game / 'build-cache' / 'mod-code.json').write_text(json.dumps({key: digest}), encoding='utf-8')
        print('@@{"ev":"step","step":"compile","state":"done"}', flush=True)
        break
    time.sleep(0.1)
else:
    sys.exit(1)
`;

async function waitFor(probe, label) {
  const deadline = Date.now() + 15000;
  do {
    if (await probe()) return;
    await new Promise(resolve => setTimeout(resolve, 100));
  } while (Date.now() < deadline);
  assert.fail(label);
}

async function main() {
  assert.equal(process.platform, 'win32');
  const base = await fs.mkdtemp(path.join(os.tmpdir(), 'nx2-runtime-ui-'));
  const root = path.join(base, 'Programs', 'NX2Saria');
  const active = path.join(root, 'app', '0.25.0');
  const game = path.join(root, 'game');
  const file = (...parts) => path.join(root, ...parts);
  const localRuntime = file('python');
  const marker = file('data', 'job-running.json');
  const buildPython = path.join(game, 'build-python.txt');
  const compileGate = path.join(game, 'finish-build');
  const gameGate = path.join(game, 'finish-game');
  const readLog = () => fs.readFile(file('data', 'logs', 'launcher.log'), 'utf8').catch(() => '');
  const options = { executablePath: process.env.NX2_TEST_PORTABLE, env: { ...process.env, LOCALAPPDATA: base, NX2SARIA_TELEMETRY_URL: 'none', NX2SARIA_ELECTRON_TEST_SEND_REPORTS: '0' } };
  let app;
  try {
    await fs.mkdir(path.join(root, 'app'), { recursive: true });
    app = await _electron.launch(options);
    let page = await app.firstWindow();
    await page.waitForFunction(() => document.querySelector('#detail').textContent.includes('NX2Saria'));
    assert.equal(await page.locator('#play').isDisabled(), true);
    await app.close();
    await fs.cp(process.env.NX2_TEST_ACTIVE_APP, active, { recursive: true });
    await fs.cp(process.env.NX2_TEST_OLD_RUNTIME, localRuntime, { recursive: true });
    await fs.writeFile(file('app', 'current.txt'), '0.25.0');
    await fs.mkdir(file('data'), { recursive: true });
    await fs.writeFile(file('data', 'electron-preferences.json'), '{"locale":"pt-BR"}');
    await fs.writeFile(file('data', 'launcher.json'), JSON.stringify({ auto_check: false, game_dir: game, hide_while_playing: false, warn_low_commit: false }));
    app = await _electron.launch(options);
    page = await app.firstWindow();
    await page.getByText('Atualize ou repare esta instalação', { exact: false }).waitFor({ timeout: 15000 });
    assert.equal(await page.getByRole('button', { name: 'Jogar' }).isDisabled(), true);
    await app.close();
    await fs.rm(localRuntime, { recursive: true, force: true });
    await fs.cp(process.env.NX2_TEST_RUNTIME, localRuntime, { recursive: true });
    await fs.mkdir(path.join(game, 'build-cache'), { recursive: true });
    await fs.mkdir(path.join(game, 'exefs'), { recursive: true });
    const header = Buffer.alloc(0x48);
    header.write('NSO0');
    await fs.writeFile(path.join(game, 'exefs', 'main'), header);
    const cheats = path.join(game, 'mods', 'Synthetic', 'cheats');
    await fs.mkdir(cheats, { recursive: true });
    const cheat = path.join(cheats, '0000000000000000.txt');
    await fs.writeFile(cheat, '[Synthetic]\n');
    await fs.writeFile(path.join(game, 'build-cache', 'state.json'), JSON.stringify({ version: 2, kit: 'test', steps: { compile: {}, exefs: {}, romfs: {} } }));
    await fs.copyFile(process.env.NX2_TEST_HELD_GAME, path.join(game, 'ocarina.exe'));
    await fs.writeFile(path.join(active, 'tools', 'make_game.py'), makeGame);
    await fs.writeFile(path.join(active, 'toolchain.lock.json'), JSON.stringify({ format: 1, packages: [] }));
    const msys = file('toolchain', 'msys64');
    for (const leaf of ['bin/clang.exe', 'bin/clang++.exe', 'bin/SDL3.dll', 'lib/libSDL3.dll.a', 'include/vma']) {
      const dest = path.join(msys, 'ucrt64', leaf);
      await fs.mkdir(path.dirname(dest), { recursive: true });
      await fs.writeFile(dest, '');
    }
    await fs.writeFile(path.join(msys, 'installed.json'), JSON.stringify({ digest: createHash('sha256').update('').digest('hex') }));
    await fs.mkdir(file('pydeps'));
    await fs.writeFile(file('pydeps', 'zbic.py'), 'def decompress(data):\n    return data\n');

    app = await _electron.launch(options);
    page = await app.firstWindow();
    await page.locator('#status').getByText('Instalação encontrada').waitFor({ timeout: 15000 });
    const play = page.getByRole('button', { name: 'Jogar' });
    const cancel = page.getByRole('button', { name: 'Cancelar compilação' });
    await play.click();
    await cancel.waitFor({ timeout: 15000 });
    await waitFor(async () => (await fs.readFile(buildPython, 'utf8').catch(() => '')).startsWith('(3, 14,'), 'active tool did not run under installed CPython');
    assert.equal((await fs.stat(marker)).isFile(), true);
    assert.match(await readLog(), /worker: play status=rebuild/);
    await fs.writeFile(compileGate, '');
    await waitFor(() => fs.stat(path.join(game, 'game-pid')).then(() => true).catch(() => false), 'successful job did not resume Play');
    await fs.writeFile(gameGate, '');
    await page.locator('#status').getByText('Fluxo de jogo encerrado').waitFor({ timeout: 15000 });
    assert.match(await readLog(), /worker: job result=ok/);
    assert.match(await readLog(), /worker: runtime=.*python\.exe python=3\.14\./i);
    assert.equal(await fs.stat(marker).then(() => true).catch(() => false), false);

    await fs.rm(compileGate);
    await fs.rm(buildPython);
    await fs.writeFile(cheat, '[Synthetic updated]\n');
    await play.click();
    await cancel.waitFor({ timeout: 15000 });
    await waitFor(() => fs.stat(buildPython).then(() => true).catch(() => false), 'second synthetic job did not start');
    await cancel.click();
    await page.locator('#status').getByText('Não foi possível iniciar').waitFor({ timeout: 15000 });
    assert.match(await readLog(), /worker: job result=cancelled/);
    assert.equal((await readLog()).match(/worker: spawn started/g).length, 1);
    assert.equal(await fs.stat(marker).then(() => true).catch(() => false), false);
    console.log('Packaged Windows runtime and mod-job regressions passed');
  } catch (error) {
    console.error(await readLog());
    throw error;
  } finally {
    await fs.writeFile(gameGate, '').catch(() => {});
    if (app) await app.close().catch(() => {});
    await fs.rm(base, { recursive: true, force: true });
  }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
