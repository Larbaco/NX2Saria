import hashlib
import struct
import threading

SUPER = struct.Struct('<4sHHQQ32sQQII16s32s')
PACKAGE = struct.Struct('<QIIIIBBHIQI20s')
DIRECTORY = struct.Struct('<IIIIQ')
FILE = struct.Struct('<IIQQQ32sBBH12s')
CHUNK = 8 << 20


class NupError(Exception):
    pass


class Nup:
    def __init__(self, path):
        self.path = str(path)
        self._local = threading.local()
        with open(self.path, 'rb') as f:
            head = f.read(SUPER.size)
            if len(head) < SUPER.size:
                raise NupError('not a .nup file (too short)')
            magic, _a, _b, toc_off, toc_size, toc_sha, _blob, _size, _count, _z, _uid, _zeros = SUPER.unpack(head)
            if magic != b'NUP\x01':
                raise NupError(f'not a .nup file (magic {magic!r})')
            end = f.seek(0, 2)
            if toc_off > end or toc_size > end - toc_off:
                raise NupError('the table of contents is damaged (it lies past the end of the file)')
            f.seek(toc_off)
            toc = f.read(toc_size)
        if len(toc) != toc_size or hashlib.sha256(toc).digest() != toc_sha:
            raise NupError('the table of contents is damaged (its sha256 does not match)')
        try:
            self._parse(toc)
        except (struct.error, IndexError, ValueError, OverflowError, MemoryError) as e:
            raise NupError(f'the table of contents is malformed ({type(e).__name__}: {e})') from None

    def _parse(self, toc):
        tmagic, npkg, ndir, nfile, pool_off, pool_len, _ = struct.unpack_from('<IIQQQQQ', toc, 0)
        if tmagic != 0x01434F54:
            raise NupError('unknown table of contents')
        if (npkg * PACKAGE.size + ndir * DIRECTORY.size + nfile * FILE.size + 0x30 > len(toc)
                or pool_off + pool_len > len(toc)):
            raise NupError('the table of contents is malformed (its counts or name pool run past its end)')
        at = 0x30
        self.packages = []
        for i in range(npkg):
            tid, root, exefs, romfs, version, kind, *_ = PACKAGE.unpack_from(toc, at + i * PACKAGE.size)
            self.packages.append(dict(title_id=tid, root=root, exefs=exefs, romfs=romfs, version=version, kind=kind))
        at += npkg * PACKAGE.size
        self.dirs = [DIRECTORY.unpack_from(toc, at + i * DIRECTORY.size) for i in range(ndir)]
        at += ndir * DIRECTORY.size
        self.files = [FILE.unpack_from(toc, at + i * FILE.size) for i in range(nfile)]
        at += nfile * FILE.size
        if pool_off < at:
            raise NupError('the table of contents is malformed (its name pool overlaps its entries)')
        self.children = struct.unpack_from(f'<{(pool_off - at) // 4}I', toc, at)
        self.pool = toc[pool_off:pool_off + pool_len]
        self.ndir = ndir

    def _name(self, off, n):
        try:
            return self.pool[off:off + n].decode('utf-8')
        except UnicodeDecodeError:
            raise NupError(f'a name in the table of contents is not UTF-8 (at {off:#x})') from None

    def tree(self, dir_index):
        stack = [(dir_index, '')]
        seen = set()
        while stack:
            d, prefix = stack.pop()
            if d in seen:
                raise NupError(f'directory {d} of the table of contents is listed below itself')
            seen.add(d)
            try:
                _no, _nl, first, count, _ = self.dirs[d]
                entries = self.children[first:first + count]
                for c in entries:
                    if c < self.ndir:
                        no, nl = self.dirs[c][0], self.dirs[c][1]
                        stack.append((c, prefix + self._name(no, nl) + '/'))
                    else:
                        fi = c - self.ndir
                        yield prefix + self._name(self.files[fi][0], self.files[fi][1]), fi
            except IndexError:
                raise NupError(f'directory {d} of the table of contents names an entry it does not have') from None

    def size(self, fi):
        return self.files[fi][2]

    def _handle(self):
        f = getattr(self._local, 'f', None)
        if f is None:
            f = self._local.f = open(self.path, 'rb')
        return f

    def chunks(self, fi):
        _no, _nl, size, off, stored, sha, codec, *_ = self.files[fi]
        f = self._handle()
        f.seek(off)
        h = hashlib.sha256()
        got = 0
        if codec == 0:
            left = stored
            while left:
                b = f.read(min(CHUNK, left))
                if not b:
                    break
                left -= len(b)
                got += len(b)
                h.update(b)
                yield b
        elif codec == 1:
            import zstandard
            d = zstandard.ZstdDecompressor().decompressobj()
            left = stored
            while left:
                b = f.read(min(CHUNK, left))
                if not b:
                    break
                left -= len(b)
                try:
                    out = d.decompress(b)
                except zstandard.ZstdError as e:
                    raise NupError(f'file {fi}: damaged ({e})') from None
                if out:
                    got += len(out)
                    h.update(out)
                    yield out
        else:
            raise NupError(f'file {fi}: unknown codec {codec}')
        if got != size or h.digest() != sha:
            raise NupError(f'file {fi}: damaged (size or sha256 differs from the table of contents)')

    def read(self, fi):
        return b''.join(self.chunks(fi))
