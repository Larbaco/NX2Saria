import path from 'node:path';
import { readFileSync, statSync } from 'node:fs';

export class InstallationRuntimeError extends Error {
  constructor(readonly code: 'installation_required' | 'runtime_incompatible') { super(code); }
}

export function runtimePaths(packaged: boolean, resources: string, dirname: string, env: NodeJS.ProcessEnv, platform: NodeJS.Platform, root: string) {
  if (!packaged) {
    return { source: path.resolve(dirname, '../..'), python: env.NX2SARIA_PYTHON || (platform === 'win32' ? 'python' : 'python3') };
  }
  let version: string;
  try {
    version = readFileSync(path.join(root, 'app', 'current.txt'), 'utf8').trim();
  } catch (error) {
    throw new InstallationRuntimeError((error as NodeJS.ErrnoException).code === 'ENOENT' ? 'installation_required' : 'runtime_incompatible');
  }
  if (!version) throw new InstallationRuntimeError('installation_required');
  if (!/^v?\d+(?:\.\d+)*(?:-[0-9A-Za-z.-]+)?(?:\+[0-9A-Za-z.-]+)?$/.test(version)) {
    throw new InstallationRuntimeError('runtime_incompatible');
  }
  const app = path.join(root, 'app', version);
  const python = path.join(root, 'python', platform === 'win32' ? 'python.exe' : 'bin/python3');
  for (const file of [python, path.join(app, 'VERSION'), path.join(app, 'tools', 'make_game.py')]) {
    if (!statSync(file, { throwIfNoEntry: false })?.isFile()) throw new InstallationRuntimeError('runtime_incompatible');
  }
  return { source: resources, python, installed: { version, app } };
}
