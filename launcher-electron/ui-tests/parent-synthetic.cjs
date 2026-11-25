const assert = require('node:assert/strict');
const { execFileSync } = require('node:child_process');
const fs = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');
const { _electron } = require('playwright-core');

const windows = process.platform === 'win32';
const delay = () => new Promise(resolve => setTimeout(resolve, 100));

async function waitFor(probe, label) {
  const deadline = Date.now() + 15000;
  do {
    const value = await probe();
    if (value) return value;
    await delay();
  } while (Date.now() < deadline);
  assert.fail(label);
}

async function workerPids(root) {
  if (windows) {
    const script = `$found = Get-CimInstance Win32_Process -Filter "Name='python.exe'" | Where-Object { $_.CommandLine -like '*launcher.worker*' -and $_.CommandLine.Contains($env:NX2_TEST_ROOT) }; $found.ProcessId`;
    const pids = execFileSync('powershell.exe', ['-NoProfile', '-Command', script], { encoding: 'utf8', env: { ...process.env, NX2_TEST_ROOT: root } }).trim();
    return pids.split(/\s+/).filter(Boolean).map(Number);
  }
  const found = [];
  for (const id of await fs.readdir('/proc')) {
    if (!/^\d+$/.test(id)) continue;
    const argv = (await fs.readFile(`/proc/${id}/cmdline`, 'utf8').catch(() => '')).split('\0');
    if (argv.includes('launcher.worker') && argv.includes(root)) found.push(Number(id));
  }
  return found;
}

async function main() {
  const base = await fs.mkdtemp(path.join(os.tmpdir(), 'nx2-parent-ui-'));
  const root = path.join(base, ...(windows ? ['Programs', 'NX2Saria'] : ['nx2saria']));
  const game = path.join(root, 'game');
  const logPath = path.join(root, 'data', 'logs', 'launcher.log');
  const readLog = () => fs.readFile(logPath, 'utf8').catch(() => '');
  const options = windows
    ? { executablePath: process.env.NX2_TEST_PORTABLE, env: { ...process.env, LOCALAPPDATA: base } }
    : { args: ['--no-sandbox', '.'], cwd: path.resolve(__dirname, '..'), env: { ...process.env, XDG_DATA_HOME: base } };
  const apps = [];
  const launch = async () => {
    const app = await _electron.launch(options);
    apps.push(app);
    return [app, await app.firstWindow()];
  };
  const ready = page => page.locator('#status').getByText('Instalação encontrada').waitFor({ timeout: 15000 });
  const forceExit = async (app, workerPid) => {
    const electron = app.process();
    if (windows) {
      const script = `$worker = Get-CimInstance Win32_Process -Filter "ProcessId=${workerPid}"; $parentProcess = Get-CimInstance Win32_Process -Filter "ProcessId=$($worker.ParentProcessId)"; if ($parentProcess.ExecutablePath -eq $env:NX2_TEST_PORTABLE) { $parentProcess.ProcessId }`;
      const pid = Number(execFileSync('powershell.exe', ['-NoProfile', '-Command', script], { encoding: 'utf8' }).trim());
      assert.ok(pid > 0, 'the worker must be a child of the packaged Electron executable');
      execFileSync('taskkill.exe', ['/F', '/PID', String(pid)]);
      await waitFor(() => {
        try {
          process.kill(pid, 0);
          return false;
        } catch (error) {
          if (error.code === 'ESRCH') return true;
          throw error;
        }
      }, 'Electron main PID did not exit');
    } else {
      electron.kill('SIGKILL');
      await waitFor(() => electron.exitCode !== null || electron.signalCode !== null, 'Electron did not exit');
    }
  };
  try {
    await fs.mkdir(path.join(root, 'data'), { recursive: true });
    await fs.mkdir(path.join(root, 'app'), { recursive: true });
    await fs.mkdir(path.join(game, 'build-cache'), { recursive: true });
    await fs.writeFile(path.join(root, 'data', 'launcher.json'), JSON.stringify({ game_dir: game, hide_while_playing: true, warn_low_commit: false }));
    await fs.writeFile(path.join(root, 'app', 'current.txt'), '0.25.0');
    await fs.writeFile(path.join(game, 'build-cache', 'state.json'), JSON.stringify({ kit: 'test', steps: { compile: {} } }));
    if (windows) {
      assert.ok(process.env.NX2_TEST_PORTABLE && process.env.NX2_TEST_RUNTIME && process.env.NX2_TEST_ACTIVE_APP && process.env.NX2_TEST_HELD_GAME);
      await fs.cp(process.env.NX2_TEST_RUNTIME, path.join(root, 'python'), { recursive: true });
      await fs.cp(process.env.NX2_TEST_ACTIVE_APP, path.join(root, 'app', '0.25.0'), { recursive: true });
      await fs.copyFile(process.env.NX2_TEST_HELD_GAME, path.join(game, 'ocarina.exe'));
    } else {
      await fs.writeFile(path.join(game, 'ocarina'), '#!/bin/sh\necho $$ > game-pid\nfor i in $(seq 1 600); do\n  if test -f finish-game; then echo done > game-finished; exit; fi\n  sleep 0.1\ndone\nexit 2\n', { mode: 0o755 });
    }

    const [idle, idlePage] = await launch();
    await ready(idlePage);
    const idleWorkers = await workerPids(root);
    assert.equal(idleWorkers.length, 1);
    const [idlePid] = idleWorkers;
    const idleLog = await readLog();
    assert.match(idleLog, /worker: started .*lock=held/);
    assert.doesNotMatch(idleLog, /worker: (spawn|job) started/);
    await forceExit(idle, idlePid);
    await waitFor(async () => !(await workerPids(root)).includes(idlePid), 'idle worker remained alive');
    assert.match(await readLog(), windows ? /worker: (parent exit|eof) idle/ : /worker: eof idle/);

    const [playing, page] = await launch();
    await ready(page);
    const playingWorkers = await workerPids(root);
    assert.equal(playingWorkers.length, 1);
    const [playingPid] = playingWorkers;
    const gamePidFile = path.join(game, 'game-pid');
    await page.getByRole('button', { name: 'Jogar' }).click();
    const gamePid = await waitFor(async () => Number((await fs.readFile(gamePidFile, 'utf8').catch(() => '')).trim()), 'no synthetic game PID');
    await waitFor(async () => (await readLog()).includes('worker: spawn started'), 'worker did not record spawn');
    await forceExit(playing, playingPid);
    await waitFor(async () => /worker: (parent exit|eof) game running/.test(await readLog()), 'worker did not record disconnect during play');
    assert.deepEqual(await workerPids(root), [playingPid]);
    const [busy, busyPage] = await launch();
    process.kill(gamePid, 0);
    await busyPage.getByText('Outra instância está usando a instalação.', { exact: false }).waitFor({ timeout: 10000 });
    process.kill(gamePid, 0);
    assert.equal(await busyPage.getByRole('button', { name: 'Jogar' }).isDisabled(), true);
    await busy.close();
    const electronLog = await fs.readFile(path.join(root, 'data', 'logs', 'launcher-electron.log'), 'utf8');
    const refusal = electronLog.match(/electron: window second_instance_refused instance=(\d+)/);
    assert.ok(refusal);
    assert.ok(electronLog.includes(`electron: inspection result=worker_busy instance=${refusal[1]}`));
    assert.ok(electronLog.includes(`electron: exit code=0 play_active=false instance=${refusal[1]}`));
    await fs.writeFile(path.join(game, 'finish-game'), '');
    await waitFor(async () => (await readLog()).includes('worker: game ended'), 'worker did not record game end');
    await waitFor(async () => !(await workerPids(root)).includes(playingPid), 'worker did not exit after the game');
    const [, reopened] = await launch();
    await ready(reopened);
    assert.equal((await readLog()).match(/worker: spawn started/g).length, 1);
    console.log(`Parent-exit synthetic regressions passed on ${process.platform}`);
  } catch (error) {
    console.error(await readLog());
    throw error;
  } finally {
    await fs.writeFile(path.join(game, 'finish-game'), '').catch(() => {});
    for (const app of apps.reverse()) await app.close().catch(() => {});
    await fs.rm(base, { recursive: true, force: true });
  }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
