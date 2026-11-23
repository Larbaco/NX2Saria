import path from 'node:path';
import { existsSync } from 'node:fs';

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
    if (existsSync(path.join(root, 'app', 'current.txt'))) return root;
  }
  return null;
}
