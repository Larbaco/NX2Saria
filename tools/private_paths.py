"""Personal folders written short in the text players share: the launcher's log and crash.log, the "Copy Report" text,
the AppImage's build.log and the game log a bug-report zip carries.

The same rules as the game's own log (runtime/log_privacy.c, runtime/log_privacy_core.h):

- each of these folders, where it starts a path, is written as its marker and the rest of the path is kept:
  Windows %LOCALAPPDATA%, %APPDATA%, %USERPROFILE% (also HOMEDRIVE + HOMEPATH and the 8.3 short form), %HOMESHARE%,
  %OneDrive%, %TEMP%; Linux (and the Unix home a Windows build sees under Wine) ~;
- a folder matches only as a whole: the character before it and the one after it are not part of a name (a letter,
  digit, '_', '.' or '-'), so /home/ana leaves /home/anabel and /var/home/ana alone; the longest folder wins;
- the computer's name (COMPUTERNAME, USERDOMAIN, HOSTNAME, the host name and its first label), three characters or
  longer, as a word of its own anywhere and without case: %COMPUTERNAME% (%USERDOMAIN% for a domain's name). Names a
  system gives every machine (COMMON_HOSTS: localhost, steamdeck, ...) are not looked for;
- the account's name, three characters or longer, as a word of its own inside a path (the run of non-blank characters
  around it holds '/' or '\\'): %USERNAME% on Windows, $USER elsewhere. "Steam Deck" stays as it is for the account
  deck;
- Windows paths match without case, with either separator, and with a run of separators for one (an escaped path in
  an error's text: 'C:\\\\Users\\\\ana');
- a '.' or '-' right after a folder ends it when no name character follows (the end of a sentence);
- names too common to be the account's alone (COMMON: user, admin, deck, ...) are not looked for alone; a path under
  their home still matches as a folder.

NX2SARIA_LOG_SHORT_PATHS=0 in the environment writes everything in full (for debugging on one's own machine), as
log_short_paths = 0 in nx2saria.ini does for the game.
"""
import os
import re
import sys

SWITCH = 'NX2SARIA_LOG_SHORT_PATHS'
NAME_MIN = 3
COMMON = {'user', 'users', 'admin', 'administrator', 'root', 'guest', 'deck', 'steamuser', 'owner', 'default', 'public',
          'home', 'local', 'data', 'game', 'games'}   # runtime/log_privacy_core.h priv_common
COMMON_HOSTS = {'localhost', 'steamdeck', 'ubuntu', 'debian', 'fedora', 'archlinux', 'linux', 'desktop', 'laptop',
                'computer', 'workgroup', 'mint', 'pop-os', 'manjaro', 'nobara', 'bazzite', 'cachyos', 'endeavouros',
                'opensuse', 'localdomain'}   # runtime/log_privacy_core.h priv_common_host


def _hostname():
    try:
        import socket
        return socket.gethostname()
    except OSError:
        return ''

_WORD = r'[^\W]'           # a letter (any script), a digit or '_'
_PATH = r'[^\W]|[.\-]'     # ... or '.' and '-': what continues a folder's name


def _strip(p):
    p = p.rstrip('/\\')
    if not p or p.endswith(':') or not re.search(r'[/\\]', p):
        return ''   # a root or a drive's root would write every path short
    return p


def _short_path(p):
    """The Windows 8.3 form of a folder (C:\\Users\\ANASOU~1), or ''."""
    if os.name != 'nt':
        return ''
    try:
        import ctypes
        buf = ctypes.create_unicode_buffer(1024)
        n = ctypes.windll.kernel32.GetShortPathNameW(p, buf, len(buf))
        return buf.value if 0 < n < len(buf) else ''
    except (OSError, AttributeError, ValueError):
        return ''


def rules(env=None, windows=None, real=os.path.realpath, short=_short_path, host=_hostname):
    """(prefixes, name, hosts, windows): the folders to write short as (folder, marker), longest first, the account's
    name with its marker, and the computer's names as (name, marker). env and windows default to this process's
    environment and system."""
    env = os.environ if env is None else env
    windows = os.name == 'nt' if windows is None else windows
    pre = []

    def add(p, mark):
        p = _strip(p or '')
        if p and all(p.lower() != q.lower() if windows else p != q for q, _ in pre):
            pre.append((p, mark))

    if windows:
        for var, mark in (('LOCALAPPDATA', '%LOCALAPPDATA%'), ('APPDATA', '%APPDATA%'), ('USERPROFILE', '%USERPROFILE%'),
                          ('HOMESHARE', '%HOMESHARE%'), ('OneDrive', '%OneDrive%'), ('TEMP', '%TEMP%'),
                          ('TMP', '%TEMP%')):
            add(env.get(var), mark)
        hd, hp = env.get('HOMEDRIVE', ''), env.get('HOMEPATH', '')
        if hd and hp:
            add(hd + hp, '%USERPROFILE%')
        for var, mark in (('USERPROFILE', '%USERPROFILE%'), ('LOCALAPPDATA', '%LOCALAPPDATA%')):
            if env.get(var):
                add(short(env[var]), mark)
        home = env.get('HOME', '')   # Wine passes the Unix environment on: /home/<name>, seen as Z:\home\<name>
        if home.startswith('/'):
            add(home, '~')
            add('Z:' + home.replace('/', '\\'), '~')
        wh = env.get('WINEHOMEDIR', '')
        add(wh[4:] if wh.startswith('\\??\\') else wh, '~')
        name, name_mark = env.get('USERNAME', ''), '%USERNAME%'
    else:
        home = env.get('HOME', '')
        add(home, '~')
        if home:
            try:
                add(real(home), '~')
            except OSError:
                pass
        name, name_mark = env.get('USER') or env.get('LOGNAME') or '', '$USER'
    pre.sort(key=lambda r: -len(r[0]))
    if len(name) < NAME_MIN or re.search(r'[/\\]', name) or name.lower() in COMMON:
        name = ''
    hosts = []

    def add_host(full, mark='%COMPUTERNAME%'):
        for h in (full, full.split('.')[0]) if full else ():
            if (len(h) >= NAME_MIN and h.lower() not in COMMON_HOSTS and not re.search(r'[/\\\s]', h)
                    and h.lower() not in (x.lower() for x, _ in hosts)):
                hosts.append((h, mark))

    if windows:
        cn, dom = env.get('COMPUTERNAME', ''), env.get('USERDOMAIN', '')
        add_host(cn)
        add_host(dom, '%COMPUTERNAME%' if dom.lower() == cn.lower() else '%USERDOMAIN%')
    else:
        add_host(host())
    add_host(env.get('HOSTNAME', ''))
    hosts.sort(key=lambda r: -len(r[0]))
    return pre, (name, name_mark), hosts, windows


class Shortener:
    def __init__(self, env=None, windows=None, **kw):
        pre, (name, name_mark), hosts, self.windows = rules(env, windows, **kw)
        self.prefixes = pre
        self.name = name
        flags = re.IGNORECASE if self.windows else 0
        parts = []
        self._marks = {}
        for i, (p, mark) in enumerate(pre):
            if self.windows:
                body = ''.join((r'[/\\]' if k == 0 else r'[/\\]+') if c in '/\\' else re.escape(c)
                               for k, c in enumerate(p))
            else:
                body = re.escape(p)
            parts.append(f'(?P<p{i}>(?<!{_PATH}){body}(?:(?!{_PATH})|(?=[.\\-](?!{_PATH}))))')
            self._marks[f'p{i}'] = mark
        for i, (h, mark) in enumerate(hosts):   # a word of its own anywhere, without case
            parts.append(f'(?P<h{i}>(?i:(?<!{_WORD}){re.escape(h)}(?!{_WORD})))')
            self._marks[f'h{i}'] = mark
        if name:
            parts.append(f'(?P<name>(?<!{_WORD}){re.escape(name)}(?!{_WORD}))')
            self._marks['name'] = name_mark
        self._re = re.compile('|'.join(parts), flags) if parts else None
        self.hits = 0

    def _sub(self, m):
        key = m.lastgroup
        if key == 'name' and not self._in_path(m.string, m.start(), m.end()):
            return m.group()
        self.hits += 1
        return self._marks[key]

    @staticmethod
    def _in_path(s, a, b):
        """A '/' or '\\' in the run of non-blank characters around s[a:b]; reads that run alone, not the text before
        it (a bug report's log can be 64 MiB)."""
        i = a
        while i > 0 and not s[i - 1].isspace():
            i -= 1
            if s[i] in '/\\':
                return True
        j, n = b, len(s)
        while j < n and not s[j].isspace():
            if s[j] in '/\\':
                return True
            j += 1
        return False

    def __call__(self, text):
        if self._re is None or not text:
            return text
        return self._re.sub(self._sub, text)


_default = None


def on(env=None):
    env = os.environ if env is None else env
    return env.get(SWITCH, '1').strip().lower() not in ('0', 'off', 'false', 'no')


def shorten(text):
    """text with the personal folders written short, or text as it is when NX2SARIA_LOG_SHORT_PATHS=0."""
    global _default
    if not isinstance(text, str) or not on():
        return text
    if _default is None:
        _default = Shortener()
    return _default(text)


def describe(env=None):
    """For the start of a log: whether paths are written short."""
    if not on(env):
        return f'paths are written in full, the account and computer names included ({SWITCH}=0)'
    return (f'personal folders are written short ({"%USERPROFILE%, %LOCALAPPDATA%" if os.name == "nt" else "~"}, '
            f'the account name and the computer name; {SWITCH}=0 writes them in full)')


if __name__ == '__main__':   # filter: python tools/private_paths.py < in > out
    sys.stdout.write(shorten(sys.stdin.read()))
