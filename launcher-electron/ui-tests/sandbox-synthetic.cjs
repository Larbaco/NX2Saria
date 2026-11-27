const assert = require('node:assert/strict');
const fs = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');
const { _electron } = require('playwright-core');

async function main() {
  const base = await fs.mkdtemp(path.join(os.tmpdir(), 'nx2-sandbox-ui-'));
  let app;
  try {
    const root = path.join(base, 'nx2saria');
    await fs.mkdir(path.join(root, 'data', 'logs'), { recursive: true });
    await fs.mkdir(path.join(root, 'app'));
    await fs.writeFile(path.join(root, 'app', 'current.txt'), '0.0.1');
    await fs.writeFile(path.join(root, 'data', 'launcher.json'), '{"auto_check":false,"telemetry":"off"}');
    const env = { ...process.env, XDG_DATA_HOME: base, NX2SARIA_TELEMETRY_URL: 'none',
      NX2SARIA_ELECTRON_TEST_SEND_REPORTS: '0' };
    delete env.ELECTRON_DISABLE_SANDBOX;
    app = await _electron.launch({
      chromiumSandbox: true, args: ['.', '--launcher-root', root],
      cwd: path.resolve(__dirname, '..'), env, timeout: 15000
    });
    const page = await app.firstWindow({ timeout: 5000 });
    await page.locator('#pick-game-folder').waitFor({ timeout: 5000 });
    const processes = await app.evaluate(({ app, BrowserWindow }) => ({
      main: process.pid,
      renderer: BrowserWindow.getAllWindows()[0].webContents.getOSProcessId(),
      disabled: app.commandLine.hasSwitch('no-sandbox')
    }));
    assert.equal(processes.disabled, false);
    const main = await fs.readFile(`/proc/${processes.main}/status`, 'utf8');
    const renderer = await fs.readFile(`/proc/${processes.renderer}/status`, 'utf8');
    const filterCount = status => Number(status.match(/^Seccomp_filters:\s+(\d+)$/m)?.[1]);
    assert.match(renderer, /^NoNewPrivs:\s+1$/m);
    assert.match(renderer, /^Seccomp:\s+2$/m);
    assert.ok(filterCount(renderer) > filterCount(main), 'the renderer must add a seccomp filter');
    const log = await fs.readFile(path.join(root, 'data', 'logs', 'launcher-electron.log'), 'utf8');
    assert.match(log, /electron: window shown/);
    assert.match(log, /electron: inspection result=not_configured source=explicit/);
    console.log('Sandboxed Electron window inspected the synthetic installation');
  } finally {
    if (app) await app.close();
    await fs.rm(base, { recursive: true, force: true });
  }
}

main().catch(error => { console.error(error); process.exitCode = 1; });
