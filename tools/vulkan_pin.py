import argparse
import hashlib
import io
import re
import shutil
import sys
import tarfile
import urllib.request
from pathlib import Path

DESCRIPTION = "The Vulkan headers the runtime is compiled against where the system's are older."
ROOT = Path(__file__).resolve().parent.parent
VERSION = '1.4.363'
HEADER_VERSION = 363
SOURCE = (f'https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/v{VERSION}.tar.gz',
          'cbaf687d3c59b9666fe080e7f8396b6b4f4d344a768ee6b337c59852f4526b68')
TOP = f'Vulkan-Headers-{VERSION}/'
FOLDER = Path('toolchain') / 'vulkan-headers'
STAMP = 'pinned.txt'
KEEP = re.compile(r'include/(vulkan|vk_video)/[A-Za-z0-9_]+\.h|LICENSE\.md')


def have(folder):
    try:
        return (folder / STAMP).read_text(encoding='ascii').strip() == VERSION and \
            (folder / 'include' / 'vulkan' / 'vulkan_core.h').is_file()
    except OSError:
        return False


def unpack(archive, folder):
    shutil.rmtree(folder, ignore_errors=True)
    n = 0
    with tarfile.open(fileobj=io.BytesIO(archive), mode='r:gz') as tar:
        for m in tar:
            rel = m.name[len(TOP):] if m.name.startswith(TOP) else ''
            if not m.isfile() or not KEEP.fullmatch(rel):
                continue
            dst = folder / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_bytes(tar.extractfile(m).read())
            n += 1
    if not (folder / 'include' / 'vulkan' / 'vulkan_core.h').is_file() or not (folder / 'LICENSE.md').is_file():
        shutil.rmtree(folder, ignore_errors=True)
        sys.exit(f'vulkan_pin: the archive of Vulkan-Headers {VERSION} lacks vulkan_core.h or LICENSE.md')
    (folder / STAMP).write_text(VERSION + '\n', encoding='ascii')
    return n


def resolve(folder=None, archive=None):
    folder = Path(folder) if folder else ROOT / FOLDER
    if have(folder):
        return folder
    url, want = SOURCE
    if archive:
        data = Path(archive).read_bytes()
    else:
        print(f'vulkan_pin: fetching the Vulkan headers {VERSION} into {folder}', flush=True)
        try:
            data = urllib.request.urlopen(url, timeout=120).read()
        except OSError as e:
            sys.exit(f'vulkan_pin: fetching {url} failed ({e}). Install Vulkan headers {VERSION} or newer, or put the '
                     f'archive on disk and run tools/vulkan_pin.py --archive <file>')
    if hashlib.sha256(data).hexdigest() != want:
        sys.exit(f'vulkan_pin: {archive or url} is not the pinned archive of Vulkan-Headers {VERSION} (SHA-256 differs)')
    unpack(data, folder)
    return folder


def header_version(text):
    m = re.search(r'^[ \t]*#[ \t]*define[ \t]+VK_HEADER_VERSION[ \t]+(\d+)', text, re.M)
    return int(m.group(1)) if m else 0


def main():
    ap = argparse.ArgumentParser(description=DESCRIPTION)
    ap.add_argument('--dest', type=Path, default=ROOT / FOLDER, help='where the headers go (default toolchain/vulkan-headers)')
    ap.add_argument('--archive', type=Path, help=f'the archive of Vulkan-Headers {VERSION} on disk instead of the download')
    a = ap.parse_args()
    folder = resolve(a.dest, a.archive)
    n = sum(1 for _ in (folder / 'include').rglob('*.h'))
    print(f'{folder}: Vulkan headers {VERSION}, {n} files')
    return 0


if __name__ == '__main__':
    sys.exit(main())
