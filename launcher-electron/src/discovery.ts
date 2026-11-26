import path from 'node:path';
import { statSync } from 'node:fs';
import { execFile } from 'node:child_process';
import type { Inspection } from './inspection';

export type DiscoverySource = 'default' | 'registry' | 'explicit';

function absoluteRoot(value: string, platform: NodeJS.Platform) {
  if (/[\x00-\x1f]/.test(value)) return false;
  if (platform !== 'win32') return path.posix.isAbsolute(value);
  const { root } = path.win32.parse(value);
  return path.win32.isAbsolute(value) && root !== '\\' && root !== '/';
}

export function explicitLauncherRoot(args: readonly string[], platform: NodeJS.Platform) {
  const index = args.findIndex(arg => arg === '--launcher-root' || arg.startsWith('--launcher-root='));
  if (index < 0) return undefined;
  const argument = args[index];
  const value = argument === '--launcher-root' ? args[index + 1] : argument.slice('--launcher-root='.length);
  return value && absoluteRoot(value, platform) ? value : null;
}

export function defaultRoots(platform: NodeJS.Platform, home: string, env: NodeJS.ProcessEnv): string[] {
  if (platform === 'win32') {
    const base = env.LOCALAPPDATA;
    if (!base || !path.win32.isAbsolute(base)) return [];
    return ['NX2Saria', 'NX2navi'].map(name => path.win32.join(base, 'Programs', name));
  }
  if (platform === 'linux') {
    const value = env.XDG_DATA_HOME;
    const base = value && path.posix.isAbsolute(value) ? value : path.posix.join(home, '.local', 'share');
    return ['nx2saria', 'nx2navi'].map(name => path.posix.join(base, name));
  }
  return [];
}

export function findDefaultRoot(roots: string[]): string | null {
  for (const root of roots) {
    try {
      if (statSync(path.join(root, 'app', 'current.txt'), { throwIfNoEntry: false })?.isFile()) return root;
    } catch {}
  }
  return null;
}

type Discovery = {
  explicitRoot?: string | null;
  defaultRoot(): string | null;
  registryRoots(): Promise<string[]>;
  inspect(root: string, source: DiscoverySource): Promise<Inspection>;
};

export async function discoverRoot({ explicitRoot, defaultRoot, registryRoots, inspect }: Discovery): Promise<Inspection | null> {
  if (explicitRoot !== undefined) {
    return explicitRoot === null ? { status: 'inspection_error', reason: 'invalid_root_argument' } : inspect(explicitRoot, 'explicit');
  }
  const root = defaultRoot();
  if (root) return inspect(root, 'default');
  for (const registered of await registryRoots()) {
    const result = await inspect(registered, 'registry');
    if (result.status !== 'inspection_error' || result.reason === 'worker_busy') return result;
  }
  return null;
}

const setupInstallLocationQuery = String.raw`
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)
$locations = foreach ($name in 'NX2Saria', 'NX2navi') {
  $key = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Software\Microsoft\Windows\CurrentVersion\Uninstall\' + $name)
  if ($null -ne $key) {
    try {
      $value = $key.GetValue('InstallLocation', $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
      if ($value -is [string]) { $value }
    } finally { $key.Dispose() }
  }
}
[Console]::WriteLine((ConvertTo-Json -InputObject @($locations) -Compress))
`;

function registryQuery(file: string, args: string[]): Promise<string> {
  return new Promise((resolve, reject) => {
    execFile(file, args, { windowsHide: true, encoding: 'utf8', timeout: 1800, maxBuffer: 8192 }, (error, stdout) => {
      if (error) reject(error);
      else resolve(stdout);
    });
  });
}

export async function registeredWindowsRoots(systemRoot: string | undefined, query = registryQuery): Promise<string[]> {
  if (!systemRoot || !absoluteRoot(systemRoot, 'win32')) return [];
  const exe = path.win32.join(systemRoot, 'System32', 'WindowsPowerShell', 'v1.0', 'powershell.exe');
  try {
    const data: unknown = JSON.parse((await query(exe, ['-NoProfile', '-NonInteractive', '-Command', setupInstallLocationQuery])).trim());
    if (!Array.isArray(data)) return [];
    return data.slice(0, 2).filter(root => typeof root === 'string' && absoluteRoot(root, 'win32'));
  } catch {
    return [];
  }
}
