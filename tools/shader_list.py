import subprocess
import sys

TAIL = '/assets/shader_list.tsv'


def list_path(repo, rev):
    for p in git(repo, 'ls-tree', '-r', '--name-only', rev, '--', 'games').splitlines():
        if p.endswith(TAIL) and p.count('/') == 3:
            return p
    raise SystemExit(f'shader_list.py: no games/*{TAIL} at {rev}')


def git(repo, *args):
    return subprocess.run(['git', '-C', str(repo), *args], check=True, capture_output=True, encoding='utf-8').stdout


def history(repo, rev='origin/HEAD'):
    if git(repo, 'rev-parse', '--is-shallow-repository').strip() == 'true':
        sys.exit(f'{repo} is a shallow clone without the shader list\'s history; run tools/fetch_refs.sh')
    PATH = list_path(repo, rev)
    lines = git(repo, 'show', f'{rev}:{PATH}').splitlines()
    seen = set(lines)
    for h in git(repo, 'log', '--format=%H', rev, '--', PATH).split()[1:]:
        for line in git(repo, 'show', f'{h}:{PATH}').splitlines():
            if line.startswith('V\t') and line not in seen:
                seen.add(line)
                lines.append(line)
    return '\n'.join(lines) + '\n'
