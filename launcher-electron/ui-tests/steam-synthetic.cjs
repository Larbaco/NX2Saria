const assert = require('node:assert/strict');
const fs = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');
const { _electron } = require('playwright-core');

const worker = `
import runpy,sys
from pathlib import Path
from unittest import mock
from launcher import gameart,steam
from launcher.ui import art
sys.argv=sys.argv[2:]
base=Path(sys.argv[-1]).parent
running=base/'steam-running'
pics=dict.fromkeys(('portrait','wide','hero','logo'),None)
with (mock.patch.object(steam,'steam_running',side_effect=running.exists),
      mock.patch.object(gameart,'make',return_value=pics),
      mock.patch.object(art,'sprig_icon'),
      mock.patch.object(art,'png_bytes',return_value=b'Synthetic PNG')):
    runpy.run_module('launcher.worker',run_name='__main__')
`;

async function main() {
  const base = await fs.mkdtemp(path.join(os.tmpdir(), 'nx2-steam-ui-'));
  const root = path.join(base, 'nx2saria');
  const game = path.join(root, 'game');
  const native = path.join(base, '.local', 'share', 'Steam');
  const cfg = path.join(native, 'userdata', '729018', 'config');
  const running = path.join(base, 'steam-running');
  let app;
  try {
    await fs.mkdir(path.join(root, 'data'), { recursive: true });
    await fs.mkdir(path.join(root, 'app'));
    await fs.writeFile(path.join(root, 'app', 'current.txt'), '0.0.1');
    await fs.writeFile(path.join(root, 'nx2saria-launcher'), 'synthetic wrapper');
    await fs.writeFile(path.join(root, 'data', 'launcher.json'), JSON.stringify({ game_dir: game }));
    await fs.mkdir(path.join(game, 'build-cache'), { recursive: true });
    await fs.writeFile(path.join(game, 'build-cache', 'state.json'), '{"kit":"0.0.1","steps":{"compile":{}}}');
    await fs.writeFile(path.join(game, 'ocarina'), 'synthetic executable marker');
    await fs.mkdir(cfg, { recursive: true });
    await fs.writeFile(running, '');
    const python = path.join(base, 'test-python');
    await fs.writeFile(python, '#!/bin/sh\nexec "$NX2_TEST_PYTHON" -c "$NX2_TEST_STEAM" "$@"\n', { mode: 0o755 });
    app = await _electron.launch({ args: ['--no-sandbox', '.'], cwd: path.resolve(__dirname, '..'),
      env: { ...process.env, HOME: base, XDG_DATA_HOME: base, NX2SARIA_PYTHON: python,
        NX2SARIA_TELEMETRY_URL: 'none', NX2SARIA_ELECTRON_TEST_SEND_REPORTS: '0',
        NX2_TEST_PYTHON: process.env.NX2SARIA_PYTHON || 'python3', NX2_TEST_STEAM: worker } });
    const page = await app.firstWindow();
    const open = page.getByRole('button', { name: 'Steam', exact: true });
    const add = page.getByRole('button', { name: 'Adicionar à Steam', exact: true });
    await open.click({ timeout: 6000 });
    await page.getByText('Steam encontrada', { exact: true }).waitFor();
    await add.click();
    await page.getByText(/Steam is open/).waitFor();
    await assert.rejects(fs.access(path.join(cfg, 'shortcuts.vdf')), { code: 'ENOENT' });
    await fs.unlink(running);
    await add.click();
    await page.getByText('Atalho disponível na Steam', { exact: true }).waitFor();
    await fs.access(path.join(cfg, 'shortcuts.vdf'));
    assert.equal((await fs.readdir(path.join(cfg, 'grid'))).length, 5);

    const flatpak = path.join(base, '.var', 'app', 'com.valvesoftware.Steam', '.local', 'share', 'Steam');
    await fs.mkdir(path.dirname(flatpak), { recursive: true });
    await fs.rename(native, flatpak);
    await open.click();
    await open.click();
    await page.getByText(/sandbox padrão do Flatpak/).waitFor();
    assert.equal(await page.getByRole('button', { name: 'Adicionar à Steam', exact: true }).isDisabled(), true);
    const log = await fs.readFile(path.join(root, 'data', 'logs', 'launcher-electron.log'), 'utf8');
    assert.match(log, /electron: steam add result=ok/);
    assert.doesNotMatch(log, /729018|nx2-steam-ui-/);
  } finally {
    if (app) await app.close().catch(() => {});
    await fs.rm(base, { recursive: true, force: true, maxRetries: 3 });
  }
}

main().catch(error => { console.error(error); process.exitCode = 1; });
