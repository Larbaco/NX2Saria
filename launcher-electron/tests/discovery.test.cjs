const { test } = require('node:test');
const assert = require('node:assert/strict');
const { defaultRoots } = require('../dist/discovery.js');
test('uses XDG data home and falls back for relative XDG paths', () => {
  assert.deepEqual(defaultRoots('linux', '/home/ana', { XDG_DATA_HOME: '/data/ana' }), ['/data/ana/nx2saria', '/data/ana/nx2navi']);
  assert.deepEqual(defaultRoots('linux', '/home/ana', { XDG_DATA_HOME: 'relative' }), ['/home/ana/.local/share/nx2saria', '/home/ana/.local/share/nx2navi']);
});
test('prefers current Windows installation to legacy installation', () => {
  assert.deepEqual(defaultRoots('win32', 'C:\\Users\\Ana', { LOCALAPPDATA: 'C:\\Users\\Ana\\AppData\\Local' }), ['C:\\Users\\Ana\\AppData\\Local\\Programs\\NX2Saria', 'C:\\Users\\Ana\\AppData\\Local\\Programs\\NX2navi']);
});
test('selects the first existing installation and falls back to the legacy root', async () => {
  const fs = require('node:fs/promises');
  const os = require('node:os');
  const path = require('node:path');
  const { findDefaultRoot } = require('../dist/discovery.js');
  const base = await fs.mkdtemp(path.join(os.tmpdir(), 'nx2saria-discovery-'));
  const current = path.join(base, 'nx2saria');
  const legacy = path.join(base, 'nx2navi');
  async function make(root) {
    await fs.mkdir(path.join(root, 'app'), { recursive: true });
    await fs.mkdir(path.join(root, 'data'), { recursive: true });
    await fs.writeFile(path.join(root, 'app', 'current.txt'), 'v1');
    await fs.writeFile(path.join(root, 'data', 'launcher.json'), '{}');
  }
  try {
    assert.equal(findDefaultRoot([current, legacy]), null);
    await make(legacy);
    await fs.rm(path.join(legacy, 'data', 'launcher.json'));
    assert.equal(findDefaultRoot([current, legacy]), legacy);
    const invalidPointer = path.join(current, 'app', 'current.txt');
    await fs.mkdir(invalidPointer, { recursive: true });
    assert.equal(findDefaultRoot([current, legacy]), legacy);
    await fs.rmdir(invalidPointer);
    await make(current);
    assert.equal(findDefaultRoot([current, legacy]), current);
  } finally { await fs.rm(base, { recursive: true, force: true }); }
});

test('an explicit Windows root skips both searches even when its argument is invalid', async () => {
  const { explicitLauncherRoot, discoverRoot } = require('../dist/discovery.js');
  const root = 'D:\\Jogos\\Instalação 星';
  assert.equal(explicitLauncherRoot([], 'win32'), undefined);
  assert.equal(explicitLauncherRoot(['--launcher-root', root], 'win32'), root);
  assert.equal(explicitLauncherRoot(['--launcher-root=' + root], 'win32'), root);
  assert.equal(explicitLauncherRoot(['--launcher-root', '\\NoDrive'], 'win32'), null);
  assert.equal(explicitLauncherRoot(['--launcher-root'], 'win32'), null);
  const inspect = async (candidate, source) => ({ status: 'ready', root: candidate, source });
  const defaultRoot = () => assert.fail('checked default paths for an explicit root');
  const registryRoots = () => assert.fail('started the registry query for an explicit root');
  assert.deepEqual(await discoverRoot({ explicitRoot: root, inspect, defaultRoot, registryRoots }),
    { status: 'ready', root, source: 'explicit' });
  assert.equal((await discoverRoot({ explicitRoot: null, inspect, defaultRoot, registryRoots })).reason,
    'invalid_root_argument');
});

test('default discovery skips the registry and keeps failures visible for that root', async () => {
  const { discoverRoot } = require('../dist/discovery.js');
  const result = await discoverRoot({
    defaultRoot: () => 'default-root',
    registryRoots: () => assert.fail('started the registry query for a default root'),
    inspect: async (root, source) => ({ status: 'inspection_error', reason: 'worker_busy', root, source }),
  });
  assert.deepEqual(result, { status: 'inspection_error', reason: 'worker_busy', root: 'default-root', source: 'default' });
});

test('registry fallback skips failed inspections and preserves a busy installation', async () => {
  const { discoverRoot } = require('../dist/discovery.js');
  const examined = [];
  const inputs = {
    defaultRoot: () => null,
    registryRoots: async () => ['stale', 'occupied'],
    inspect: async (root, source) => {
      examined.push([root, source]);
      return { status: 'inspection_error', reason: root === 'occupied' ? 'worker_busy' : 'runtime_incompatible' };
    },
  };
  assert.deepEqual(await discoverRoot(inputs), { status: 'inspection_error', reason: 'worker_busy' });
  assert.deepEqual(examined, [['stale', 'registry'], ['occupied', 'registry']]);
  assert.equal(await discoverRoot({ ...inputs, registryRoots: async () => ['stale'] }), null);
});

test('registry lookup is an optional read-only system command with a bounded result', async () => {
  const { registeredWindowsRoots } = require('../dist/discovery.js');
  const system = 'C:\\Windows';
  assert.deepEqual(await registeredWindowsRoots('relative', () => assert.fail('started an untrusted executable')), []);
  assert.deepEqual(await registeredWindowsRoots(system, async (file, args) => {
    assert.equal(file, 'C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe');
    assert.deepEqual(args.slice(0, 3), ['-NoProfile', '-NonInteractive', '-Command']);
    assert.equal(args.length, 4);
    assert.match(args[3], /CurrentUser\.OpenSubKey/);
    assert.match(args[3], /GetValue\('InstallLocation'/);
    assert.doesNotMatch(args[3], /SetValue|CreateSubKey|ExecutionPolicy/);
    return JSON.stringify(['\u0000C:\\Invalid', 'D:\\Jogos\\Instalação 星']);
  }), ['D:\\Jogos\\Instalação 星']);
  assert.deepEqual(await registeredWindowsRoots(system, async () => { throw new Error('timeout'); }), []);
  assert.deepEqual(await registeredWindowsRoots(system, async () => 'unreadable'), []);
});

test('Windows registry query round-trips a Unicode folder and ignores a stale entry', {
  skip: process.platform !== 'win32',
}, async () => {
  const { execFileSync } = require('node:child_process');
  const { randomUUID } = require('node:crypto');
  const fs = require('node:fs/promises');
  const os = require('node:os');
  const path = require('node:path');
  const { registeredWindowsRoots, discoverRoot, findDefaultRoot } = require('../dist/discovery.js');
  const base = await fs.mkdtemp(path.join(os.tmpdir(), 'nx2-registry-scratch-'));
  const live = path.join(base, 'Instalação 星');
  const stale = path.join(base, 'removed');
  const key = 'Software\\NX2Saria discovery test ' + randomUUID();
  const powershell = path.join(process.env.SystemRoot, 'System32', 'WindowsPowerShell', 'v1.0', 'powershell.exe');
  const env = { ...process.env, NX2_TEST_DISCOVERY_KEY: key, NX2_TEST_DISCOVERY_LIVE: live, NX2_TEST_DISCOVERY_STALE: stale };
  const execute = (file, args) => execFileSync(file, args, { env, windowsHide: true, timeout: 6000, encoding: 'utf8' });
  const run = script => execute(powershell, ['-NoProfile', '-NonInteractive', '-Command', script]);
  try {
    await fs.mkdir(path.join(live, 'app'), { recursive: true });
    await fs.writeFile(path.join(live, 'app', 'current.txt'), '1.0.0');
    run(String.raw`
      $ErrorActionPreference = 'Stop'
      $root = [Microsoft.Win32.Registry]::CurrentUser
      $current = $root.CreateSubKey($env:NX2_TEST_DISCOVERY_KEY + '\NX2Saria')
      $legacy = $root.CreateSubKey($env:NX2_TEST_DISCOVERY_KEY + '\NX2navi')
      try {
        $current.SetValue('InstallLocation', $env:NX2_TEST_DISCOVERY_STALE)
        $legacy.SetValue('InstallLocation', $env:NX2_TEST_DISCOVERY_LIVE)
      } finally { $current.Dispose(); $legacy.Dispose() }
    `);
    const wanted = String.raw`Software\Microsoft\Windows\CurrentVersion\Uninstall` + '\\';
    const entries = await registeredWindowsRoots(process.env.SystemRoot, async (file, args) => {
      assert.equal(file, powershell);
      assert.ok(args[3].includes(wanted));
      const scratch = args[3].replace(wanted, () => key + '\\');
      return execute(file, [...args.slice(0, 3), scratch]);
    });
    assert.deepEqual(entries, [stale, live]);
    const chosen = await discoverRoot({
      defaultRoot: () => null,
      registryRoots: async () => entries,
      inspect: async root => ({ status: findDefaultRoot([root]) === root ? 'ready' : 'inspection_error', root }),
    });
    assert.deepEqual(chosen, { status: 'ready', root: live });
  } finally {
    try {
      run('[Microsoft.Win32.Registry]::CurrentUser.DeleteSubKeyTree($env:NX2_TEST_DISCOVERY_KEY, $false)');
    } finally {
      await fs.rm(base, { recursive: true, force: true });
    }
  }
});
