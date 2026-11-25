# Copyright (c) 2026 MacNativeGames contributors (MIT; see THIRD_PARTY_NOTICES.md).
import argparse
import hashlib
import json
import struct
import sys
import time
from pathlib import Path

from check_content import GENERATED_MARKER

DESCRIPTION = 'Build the RomFS index the runtime binary-searches (runtime/hle/romfs.c) and its romfs_config.h.'
MAGIC = b'NX2NRFS1'
HEADER = struct.Struct('<8sQQ')
ROW = struct.Struct('<QQII')


def build_index(manifest):
    files = sorted(manifest['files'], key=lambda e: e['path'].encode())
    size = manifest['romfs_size']
    strings = bytearray()
    rows = bytearray()
    previous = None
    for item in files:
        path = item['path']
        name = path.encode()
        if not name or name.startswith(b'/') or b'\\' in name or b'\0' in name:
            sys.exit(f'romfs_index: malformed path in index.json (entry {len(rows) // ROW.size})')
        if name == previous:
            sys.exit('romfs_index: duplicate path in index.json')
        previous = name
        if item['offset'] < 0 or item['size'] < 0 or item['offset'] + item['size'] > size:
            sys.exit(f'romfs_index: entry {len(rows) // ROW.size} lies outside the image')
        rows += ROW.pack(item['offset'], item['size'], len(strings), len(name) + 1)
        strings += name + b'\0'
    return HEADER.pack(MAGIC, len(files), HEADER.size + len(rows)) + bytes(rows) + bytes(strings)


def write_if_changed(path, data):
    if path.exists() and path.read_bytes() == data:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def write(romfs, out):
    t0 = time.monotonic()
    manifest = json.loads((romfs / 'index.json').read_text(encoding='utf-8'))
    if manifest.get('status') != 'pass':
        sys.exit(f'romfs_index: {romfs / "index.json"} does not record a verified extraction')
    image = romfs / 'romfs.bin'
    if not image.is_file() or image.stat().st_size != manifest['romfs_size']:
        sys.exit(f'romfs_index: {image} is missing or its size differs from index.json')
    index = build_index(manifest)
    count = len(manifest['files'])
    write_if_changed(out / 'romfs-index.bin', index)
    config = (f'// RomFS image and index identity, written by tools/romfs_index.py; do not edit. {GENERATED_MARKER}\n'
              '#pragma once\n'
              f'#define ROMFS_IMAGE_SIZE {manifest["romfs_size"]}ull\n'
              f'#define ROMFS_IMAGE_SHA256 "{manifest["romfs_sha256"].lower()}"\n'
              f'#define ROMFS_INDEX_SHA256 "{hashlib.sha256(index).hexdigest()}"\n'
              f'#define ROMFS_FILE_COUNT {count}ull\n')
    write_if_changed(out / 'romfs_config.h', config.encode())
    return count, len(index), time.monotonic() - t0


def write_placeholder(out):
    (out / 'romfs-index.bin').unlink(missing_ok=True)
    config = (f'// No RomFS found by tools/romfs_index.py; do not edit. {GENERATED_MARKER}\n'
              '#pragma once\n'
              '#define ROMFS_MISSING 1\n'
              '#define ROMFS_IMAGE_SIZE 0ull\n'
              '#define ROMFS_IMAGE_SHA256 ""\n'
              '#define ROMFS_INDEX_SHA256 ""\n'
              '#define ROMFS_FILE_COUNT 0ull\n')
    write_if_changed(out / 'romfs_config.h', config.encode())


def main():
    ap = argparse.ArgumentParser(description=DESCRIPTION)
    ap.add_argument('--romfs', type=Path, required=True, help='directory with romfs.bin and index.json')
    ap.add_argument('--out', type=Path, required=True, help='build directory')
    args = ap.parse_args()
    count, size, dt = write(args.romfs.resolve(), args.out.resolve())
    print(f'romfs index: {count} files, {size:,} bytes, {dt:.1f} s')


if __name__ == '__main__':
    main()
