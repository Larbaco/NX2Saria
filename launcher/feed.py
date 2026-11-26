import json
import re
import sys
import time
import urllib.parse
from pathlib import Path

from . import net, versions

MANIFEST = 'NX2Saria-update.json'
LEGACY_MANIFEST = 'NX2navi-update.json'
LINUX_MANIFEST = 'NX2Saria-update-linux.json'
DEFAULT = {'type': 'github', 'repo': 'Larbaco/NX2Saria'}
GITHUB_API = 'https://api.github.com'


class FeedError(Exception):
    pass


class Release:
    def __init__(self, manifest, notes='', page='', assets=None, headers=None, prerelease=False):
        self.manifest = manifest
        self.version = manifest['version']
        self.build = manifest.get('build', self.version)
        self.notes = (manifest.get('notes') or notes or '').strip()
        self.page = page
        self.assets = assets or {}
        self.headers = headers or {}
        self.prerelease = prerelease

    def asset(self, key):
        entry = self.manifest.get(key)
        if not entry:
            raise FeedError(f'the {self.version} manifest has no {key} file')
        name, size, sha = entry.get('name'), entry.get('size'), entry.get('sha256')
        if (not isinstance(name, str) or not name or name in ('.', '..') or name != Path(name).name
                or '/' in name or '\\' in name or ':' in name):
            raise FeedError(f'the {self.version} manifest names its {key} file {name!r}, not a plain file name')
        if (not isinstance(size, int) or isinstance(size, bool) or size <= 0 or not isinstance(sha, str)
                or not re.fullmatch(r'[0-9a-fA-F]{64}', sha)):
            raise FeedError(f'the {self.version} manifest gives no size and sha256 for {name}')
        url = self.assets.get(name)
        if not url:
            raise FeedError(f'the {self.version} release lacks {name}')
        return url, self.headers, size, sha


def load(layout, settings):
    feed = dict(DEFAULT)
    try:
        if layout.feed_file.is_file():
            feed.update(json.loads(layout.feed_file.read_text(encoding='utf-8')))
    except (OSError, ValueError):
        pass
    if settings.get('feed_repo'):
        feed = {'type': 'github', 'repo': settings['feed_repo']}
    return feed


def _check_manifest(m, where):
    if not isinstance(m, dict) or m.get('format') != 1 or 'version' not in m or 'app' not in m:
        raise FeedError(f'{where}: not an NX2Saria update manifest')
    versions.parse(m['version'])
    return m


def retry_text(headers, now=None):
    now = time.time() if now is None else now
    try:
        if headers.get('X-RateLimit-Remaining') == '0' and headers.get('X-RateLimit-Reset'):
            wait = int(headers.get('X-RateLimit-Reset')) - now
        elif headers.get('Retry-After'):
            wait = int(headers.get('Retry-After'))
        else:
            return ''
    except (AttributeError, TypeError, ValueError):
        return ''
    if wait > 86400:
        return ''
    if wait <= 0:
        return 'in a minute'
    minutes = int(-(-wait // 60))
    return (f'after {time.strftime("%H:%M", time.localtime(now + wait))} '
            f'(in {minutes} minute{"" if minutes == 1 else "s"})')


def _manifest_names(platform=None):
    platform = sys.platform if platform is None else platform
    return (LINUX_MANIFEST,) if platform.startswith('linux') else (MANIFEST, LEGACY_MANIFEST)


def _tag_rank(tag):
    try:
        return 1, versions.parse(tag)
    except ValueError:
        return 0, ()


def _newest(candidates):
    best = None
    for c in candidates:
        if best is None or _tag_rank(c[0].get('tag_name')) > _tag_rank(best[0].get('tag_name')):
            best = c
    return best


def _github(feed, channel, token, version, platform=None, log=None):
    api = feed.get('api', GITHUB_API).rstrip('/')
    headers = {'Accept': 'application/vnd.github+json', 'X-GitHub-Api-Version': '2022-11-28'}
    if token:
        headers['Authorization'] = f'Bearer {token}'
    try:
        releases = net.get_json(f'{api}/repos/{feed["repo"]}/releases?per_page=20', headers, version=version)
    except Exception as e:
        code = getattr(e, 'code', None)
        if code == 404:
            raise FeedError(f'no releases found for {feed["repo"]} (a private repository needs a token)') from e
        if code in (403, 429):
            when = retry_text(getattr(e, 'headers', None))
            raise FeedError(f'GitHub refused the request (rate limit); try again {when or "in an hour"}') from e
        raise FeedError(f'GitHub could not be reached: {e}') from e
    candidates = []
    for r in releases:
        if r.get('draft') or (r.get('prerelease') and channel != 'preview'):
            continue
        assets = {a['name']: a for a in r.get('assets', [])}
        manifest_name = next((n for n in _manifest_names(platform) if n in assets), None)
        if manifest_name is not None:
            candidates.append((r, assets, manifest_name))
    if not candidates:
        return None
    r, assets, manifest_name = _newest(candidates)
    if log and r is not candidates[0][0]:
        log(f'update check: the feed lists {candidates[0][0].get("tag_name")} first; {r.get("tag_name")} is the newest '
            'and is the one offered')
    if token:
        urls = {n: a['url'] for n, a in assets.items()}
        asset_headers = {'Accept': 'application/octet-stream', 'Authorization': f'Bearer {token}'}
    else:
        urls = {n: a['browser_download_url'] for n, a in assets.items()}
        asset_headers = {}
    try:
        m = _check_manifest(json.loads(net.get_bytes(urls[manifest_name], asset_headers, version=version)),
                            f'{r.get("tag_name")}/{manifest_name}')
    except (FeedError, ValueError, OSError) as e:
        raise FeedError(f'{r.get("tag_name")}: {e}') from e
    return Release(m, r.get('body') or '', r.get('html_url', ''), urls, asset_headers, bool(r.get('prerelease')))


def _url(feed, version, platform=None):
    url = feed['url']
    if ((platform or sys.platform).startswith('linux')
            and Path(urllib.parse.urlparse(url).path).name in (MANIFEST, LEGACY_MANIFEST)):
        parts = urllib.parse.urlsplit(url)
        url = urllib.parse.urlunsplit((parts.scheme, parts.netloc,
                                       str(Path(parts.path).with_name(LINUX_MANIFEST)), parts.query, parts.fragment))
    try:
        m = _check_manifest(net.get_json(url, version=version), url)
    except FeedError:
        raise
    except Exception as e:
        raise FeedError(f'{url}: {e}') from e
    base = url.rsplit('/', 1)[0] + '/'
    assets = {e['name']: urllib.parse.urljoin(base, e['name']) for e in (m.get('app'), m.get('runtime')) if e}
    return Release(m, assets=assets)


def latest(feed, channel='stable', token='', version='dev', platform=None, log=None):
    if feed.get('type') == 'url':
        return _url(feed, version, platform)
    if feed.get('type') == 'github':
        return _github(feed, channel, token, version, platform, log)
    raise FeedError(f'unknown feed type {feed.get("type")!r}')


def plain_notes(text, limit=None):
    import re

    out = []
    for line in (text or '').splitlines():
        s = line.strip()
        if not s or s.startswith(('<!--', '```', '---')):
            continue
        links_only = re.sub(r'\[[^]]+\]\(https?://[^)]+\)', '', s)
        links_only = re.sub(r'https?://\S+', '', links_only)
        if not re.sub(r'[·|\s]', '', links_only):
            continue
        s = s.lstrip('#').strip()
        for mark in ('- ', '* ', '+ '):
            if s.startswith(mark):
                s = s[2:]
        s = re.sub(r'\[([^]]+)\]\(https?://[^)]+\)', r'\1', s)
        s = re.sub(r'\[([^]]+)\]\((?!https?://)[^)]+\)', r'\1', s)
        s = s.replace('**', '').replace('__', '')
        s = re.sub(r'(?<!\w)[*_]|[*_](?!\w)', '', s).replace('`', '')
        s = re.sub(r'https?://\S+', '', s).strip()
        if not s or not re.search(r'[\w\u0080-\uffff]', s):
            continue
        if s:
            out.append(s)
        if limit is not None and len(out) >= limit:
            break
    return out
