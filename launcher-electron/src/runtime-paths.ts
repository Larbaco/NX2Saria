import path from 'node:path';

export function runtimePaths(packaged: boolean, resources: string, dirname: string, env: NodeJS.ProcessEnv, platform: NodeJS.Platform) {
  if (packaged) return { source: resources, python: path.join(resources, 'python', platform === 'win32' ? 'python.exe' : 'bin/python3') };
  return { source: path.resolve(dirname, '../..'), python: env.NX2SARIA_PYTHON || (platform === 'win32' ? 'python' : 'python3') };
}
