# The function seeds and the .eh_frame_hdr reader: Copyright (c) 2026 Peiikai (MIT; see THIRD_PARTY_NOTICES.md).
import struct
from collections import OrderedDict

import numpy as np

from lifter import Insn, Lifter, Unsupported, hx
from nso import Nso, R_AARCH64_RELATIVE

LIFT_CACHE = 1 << 16


def _read_encoded(img, off, enc, pc_base):
    fmt, app = enc & 0x0F, enc & 0x70
    if fmt == 0x0B:
        v, size = struct.unpack_from('<i', img, off)[0], 4
    elif fmt == 0x03:
        v, size = struct.unpack_from('<I', img, off)[0], 4
    elif fmt in (0x0C, 0x04):
        v, size = struct.unpack_from('<q' if fmt == 0x0C else '<Q', img, off)[0], 8
    else:
        raise ValueError(f'unsupported pointer encoding {enc:#x}')
    if app == 0x10:
        v += off
    elif app == 0x30:
        v += pc_base
    return v, size


class Module:
    def __init__(self, name, path, base, simd='c'):
        self.name = name
        self.nso = Nso(str(path))
        self.base = base
        n = self.nso
        self.image = bytes(n.image)
        self.lo = base + n.text[0]
        self.hi = self.lo + n.text[1]
        self.words = np.frombuffer(self.image, dtype='<u4', count=n.text[1] // 4, offset=n.text[0])
        self.lifter = Lifter(base, simd)
        self._cache = OrderedDict()


    def in_text(self, a):
        return self.lo <= a < self.hi and a % 4 == 0

    def in_image(self, a, n=1):
        return self.base <= a and a + n <= self.base + len(self.image)

    def word(self, a):
        return int(self.words[(a - self.lo) >> 2])

    def read(self, a, n, signed=False):
        o = a - self.base
        return int.from_bytes(self.image[o:o + n], 'little', signed=signed)


    def lift(self, a):
        ins = self._cache.get(a)
        if ins is not None:
            self._cache.move_to_end(a)
            return ins
        try:
            ins = self.lifter.lift(self.word(a), a)
        except Unsupported as e:
            ins = Insn('trap', f'rt_udf(c, {hx(a)}); /* {e} */')
        self._cache[a] = ins
        if len(self._cache) > LIFT_CACHE:
            self._cache.popitem(last=False)
        return ins


    def eh_frame_functions(self):
        img, hdr = self.image, self.nso.eh_start
        if not hdr or hdr >= len(img):
            return []
        _, ptr_enc, cnt_enc, tbl_enc = img[hdr:hdr + 4]
        off = hdr + 4
        _, sz = _read_encoded(img, off, ptr_enc, hdr)
        off += sz
        count, sz = _read_encoded(img, off, cnt_enc, hdr)
        off += sz
        starts = []
        for _ in range(count):
            loc, sz = _read_encoded(img, off, tbl_enc, hdr)
            off += sz
            _, sz = _read_encoded(img, off, tbl_enc, hdr)
            off += sz
            starts.append(self.base + loc)
        return starts

    def seeds(self):
        starts = set(self.eh_frame_functions())
        for _, info, shndx, value, _ in self.nso.syms:
            if shndx and (info & 0xF) == 2:
                starts.add(self.base + value)
        for _, typ, _, add in self.nso.relocs:
            if typ == R_AARCH64_RELATIVE:
                starts.add(self.base + add)
        return sorted(a for a in starts if self.in_text(a))
