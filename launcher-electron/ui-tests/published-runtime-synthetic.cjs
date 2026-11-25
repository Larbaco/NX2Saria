const assert = require('node:assert/strict');
const { execFileSync } = require('node:child_process');
const fs = require('node:fs/promises');
const path = require('node:path');
const { _electron } = require('playwright-core');

const privacyCheck = `import os, sys
from pathlib import Path
source, app, game = map(Path, sys.argv[1:4])
sys.path[:0] = [str(source), str(app), str(app / 'tools')]
from launcher import context, telemetry, worker
import private_paths
assert Path(worker.__file__).is_relative_to(source)
assert Path(private_paths.__file__).samefile(app / 'tools' / 'private_paths.py')
private = Path(os.environ['USERPROFILE']) / 'private' / 'synthetic.txt'
raw = str(private)
assert context.shorten(raw) == raw
logs = game / 'logs'
logs.mkdir()
(logs / 'nx2saria.log').write_text(f'version: NX2Saria 0.0.0, game 1.1.1\\nFATAL: {raw}\\n', encoding='utf-8')
assert '%USERPROFILE%' in telemetry.log_tail(game)
assert raw not in telemetry.log_tail(game)
summary = telemetry.synthesize_crash(game)
assert summary is not None
assert '%USERPROFILE%' in summary['fatal']
assert raw not in summary['fatal']
assert raw in (logs / 'nx2saria.log').read_text(encoding='utf-8')
`;

async function testRelease(version) {
  const base = path.join(process.env.NX2_TEST_RELEASES, version);
  const root = path.join(base, 'Programs', 'NX2Saria');
  const publishedApp = path.join(root, 'app', version);
  const game = path.join(root, 'game');
  const resources = path.join(path.dirname(process.env.NX2_TEST_PORTABLE), 'resources');
  const python = path.join(root, 'python', 'python.exe');
  const env = { ...process.env, LOCALAPPDATA: base };
  const privacyEnv = { ...env, USERPROFILE: 'C:\\Users\\Synthetic Player', NX2SARIA_LOG_SHORT_PATHS: '0' };
  await fs.mkdir(path.join(root, 'data'), { recursive: true });
  await fs.mkdir(path.join(game, 'build-cache'), { recursive: true });
  assert.equal((await fs.readFile(path.join(root, 'app', 'current.txt'), 'utf8')).trim(), version);
  await fs.writeFile(path.join(root, 'data', 'launcher.json'), JSON.stringify({ game_dir: game, hide_while_playing: false, warn_low_commit: false }));
  await fs.writeFile(path.join(game, 'build-cache', 'state.json'), JSON.stringify({ kit: version, steps: { compile: {} } }));
  await fs.copyFile(process.env.NX2_TEST_FAKE_GAME, path.join(game, 'ocarina.exe'));
  execFileSync(python, ['-I', '-B', '-c', privacyCheck, resources, publishedApp, game], { env: privacyEnv, timeout: 15000 });
  await fs.rename(path.join(game, 'logs'), path.join(base, 'privacy-logs'));

  const app = await _electron.launch({
    executablePath: process.env.NX2_TEST_PORTABLE,
    args: ['--user-data-dir=' + path.join(base, 'electron-profile')],
    env,
  });
  const log = path.join(root, 'data', 'logs', 'launcher.log');
  try {
    const page = await app.firstWindow();
    await page.locator('#status').getByText('Instalação encontrada').waitFor({ timeout: 15000 });
    await page.getByRole('button', { name: 'Jogar' }).click();
    await page.locator('#status').getByText('Fluxo de jogo encerrado', { exact: true }).waitFor({ timeout: 15000 });
    const evidence = await fs.readFile(log, 'utf8');
    assert.match(evidence, /worker: started version=.* protocol=1 lock=held/);
    assert.match(evidence, /worker: runtime=.*app=/);
    assert.match(evidence, /worker: inspect status=ready/);
    assert.match(evidence, /worker: game ended/);
    console.log(`Published ${version}: inspection, synthetic Play and privacy passed`);
  } catch (error) {
    console.error(await fs.readFile(log, 'utf8').catch(() => ''));
    throw error;
  } finally {
    await app.close();
  }
}

(async () => {
  assert.equal(process.platform, 'win32');
  for (const version of ['0.24.0-rc.1', '0.25.0', '0.26.0-rc.1']) await testRelease(version);
})().catch(error => { console.error(error); process.exitCode = 1; });
