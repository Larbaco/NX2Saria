"""Read the game's interface layouts (BFFH files in a SARC layout archive) and print them as text or JSON.

    layout_dump.py <archive.blarc | romfs folder> [--layout NAME ...] [--json]
    layout_dump.py <archive.blarc | romfs folder> --find-animated [--layout NAME ...]
    layout_dump.py <archive.blarc | romfs folder> --codes

The archive is a decompressed layout archive (a SARC whose members are `<Layout>.buil`), or a RomFS folder with
romfs.bin and its index.json, from which UI/Layout2Archive/{Common,Title}.Product.100.Nin_Ounce_NVN2.blarc.zs are read
at the index's offsets and decompressed with the zstd command and the dictionary Pack/ZsDic.pack.zs names, in a
temporary folder under build/ that is removed afterwards. Nothing is written to the RomFS folder.

For each layout: its canvas, its panes as a tree (kind, translate, size, scale, rotation, origin, anchor on the parent
and the pane's extent on the grid, with every ancestor's translate, scale and anchor applied), the layouts its part
panes instantiate, and every animation with each track decoded (pane, property, component, keys as frame:value,
constants). --find-animated lists every pane whose translate x, scale x or size an animation keys, which a menu edit
has to take into account. --codes counts the track target codes the archive uses.

The format (docs/wide-hud.md, "The layout format (BFFH)"): records point at each other by 64-bit offsets from the
file's start. The header has a slot of 0x20 bytes per table (+0x00 the list of name offsets, +0x08 the list of record
offsets, +0x18 the count), and the canvas size as two floats at 0x2f8. The grid's origin is the canvas's centre, y up.

A picture (table 0x158) has a material index (u16) at +0x10 and an optional window record offset at +0x08. The
material (table 0x138) has an array of texture-map offsets at +0x00 with the count at +0x08. Each map starts with an
optional list of texture-name offsets and a count at +0x08. These names are the alternatives for a texture pattern.
Its UV scale, translation and rotation in radians start at +0x10, +0x18 and +0x20. An ordinary picture generates
unit-square UVs (top-left = 0,0), which the map transforms in normalized texture space. The picture's +0x20 list
of four pointers is a separate set of corner parameters (normally all four pairs are 2.0, 16.0).
The material's u32 at +0x14 indexes the render-material offset array (header 0x228, count at 0x230).
Each render material starts with a sampler offset array and count at +0x08. A sampler has U/V wrap bytes at +0/+1
and the default texture-name offset at +0x08. A pane's u32 at +0x24 indexes the parameter offset array (header
0x2e0, count 0x2e8), where +0x10 is its default alpha. The material's u32 at +0x28 indexes the RGBA float
palette (header 0x2c0, count 0x2c8).

A part's initial animation states are an offset array at +0x28 (count at +0x30). Each entry names the child
animation by string offset at +0x00 and gives its initial frame as a float at +0x08. A texture-pattern track
(target 0, property group 1 and code 28) addresses a pane's material. Its first code byte selects the texture map
and component 0 selects a name from that map's alternatives.

layout_preview.py and tests/layout_tools_check.py import this file; it uses the standard library only.
"""
import argparse
import json
import shutil
import struct
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ROMFS_ARCHIVES = ('UI/Layout2Archive/Common.Product.100.Nin_Ounce_NVN2.blarc.zs',
                  'UI/Layout2Archive/Title.Product.100.Nin_Ounce_NVN2.blarc.zs')
DICTIONARIES = 'Pack/ZsDic.pack.zs'
ZSTD_MAGIC = b'\x28\xb5\x2f\xfd'
DICT_MAGIC = 0xEC30A437

TABLES = {'panes': 0x118, 'materials': 0x138, 'pictures': 0x158, 'texts': 0x178, 'alignments': 0x198,
          'parts': 0x1b8, 'captures': 0x1d8, 'animations': 0x238}
CANVAS = 0x2f8

# A pane record: 0x44 bytes, parents before children.
PANE_SCALE_X, PANE_SCALE_Y, PANE_ROTATE = 0x10, 0x14, 0x18
PANE_X, PANE_Y, PANE_PARENT, PANE_W, PANE_H, PANE_END, PANE_FLAGS = 0x1c, 0x20, 0x28, 0x2c, 0x30, 0x34, 0x38
PANE_SIZE = 0x44
# Bits 7-10 of the flags; the names' prefixes agree (N_, P_/W_, T_, A_, S_, C_, Pa_/L_, N_SlideList).
KINDS = {0: 'null', 1: 'picture', 2: 'text', 3: 'alignment', 4: 'scissor', 5: 'capture', 6: 'part', 7: 'slide list'}
# The origin (where on its own box a pane's position is) and the anchor (the point of the parent's box its translate
# counts from) use the same codes: horizontal 1 left, 2 centre, 3 right; vertical 4 top, 5 centre, 6 bottom.
H_CODES = {1: ('left', 0.0), 2: ('centre', 0.5), 3: ('right', 1.0)}
# The fraction of the box below the point (y up): a top origin has the whole box below its position.
V_CODES = {4: ('top', 1.0), 5: ('centre', 0.5), 6: ('bottom', 0.0)}

# Animation body: (list offset, count offset) for tracks, curves, curve components, constants, constant components.
BODY_TRACKS, BODY_CURVES, BODY_CURVE_COMPONENTS, BODY_CONSTANTS, BODY_CONSTANT_COMPONENTS = 0x08, 0x18, 0x28, 0x38, 0x48
ANIM_LENGTH = 0x28   # in the animation record: its length in frames (the name's offset follows at +0x30)
NO_INDEX = 0xffff
TARGET_PANE = 7      # bits 8-10 of a track's code for a pane's property (0 for a material's)
# (bit 22, byte 3) of a pane track's code. Text and material values are reported by number.
PROPERTIES = {(1, 0): 'scale', (1, 1): 'translate', (1, 2): 'visibility', (1, 4): 'alpha', (0, 1): 'rotation',
              (0, 2): 'size', (0, 47): 'colour'}
SCALE, TRANSLATE, VISIBILITY, ALPHA, ROTATION, SIZE = (1, 0), (1, 1), (1, 2), (1, 4), (0, 1), (0, 2)
COLOUR = (0, 47)
TEXTURE_PATTERN, UV_SCALE, UV_TRANSLATE = (1, 28), (0, 31), (0, 32)
COMPONENTS = {0: 'x', 4: 'y', 8: 'z'}
SAME = 1e-3          # the game stores some zeros as -2.8e-14


class FormatError(Exception):
    pass


class SpecError(Exception):
    pass


# ---- bytes, every offset checked
def _get(data, fmt, off):
    size = struct.calcsize(fmt)
    if off < 0 or off + size > len(data):
        raise FormatError(f'read of {size} bytes at 0x{off:x} past the end (0x{len(data):x})')
    return struct.unpack_from(fmt, data, off)[0]


def u8(d, o): return _get(d, '<B', o)
def u16(d, o): return _get(d, '<H', o)
def u32(d, o): return _get(d, '<I', o)
def i32(d, o): return _get(d, '<i', o)
def u64(d, o): return _get(d, '<Q', o)
def f32(d, o): return _get(d, '<f', o)


def cstr(d, o):
    if o < 0 or o >= len(d):
        raise FormatError(f'string at 0x{o:x} past the end')
    end = d.find(b'\0', o)
    if end < 0:
        raise FormatError(f'string at 0x{o:x} has no end')
    return bytes(d[o:end]).decode('utf-8', 'replace')


def same(a, b):
    return abs(a - b) < SAME


# ---- SARC
def sarc_members(data):
    """Every member of a SARC as (name, start, end) in the order of its entries; one name may come several times."""
    if data[:4] != b'SARC':
        raise FormatError('not a SARC archive')
    head, bom = u16(data, 4), u16(data, 6)
    if bom != 0xfeff:
        raise FormatError('SARC archive is not little-endian')
    start = u32(data, 12)
    if data[head:head + 4] != b'SFAT':
        raise FormatError('SARC archive has no SFAT')
    count = u16(data, head + 6)
    names = head + 12 + 16 * count
    if data[names:names + 4] != b'SFNT':
        raise FormatError('SARC archive has no SFNT')
    names += 8
    out = []
    for i in range(count):
        e = head + 12 + 16 * i
        attr, lo, hi = u32(data, e + 4), u32(data, e + 8), u32(data, e + 12)
        name = cstr(data, names + (attr & 0xffffff) * 4) if attr >> 24 else f'#{i}'
        if lo > hi or start + hi > len(data):
            raise FormatError(f'SARC member {name} past the end')
        out.append((name, start + lo, start + hi))
    return out


# ---- zstd, through the zstd command (Python 3.12 has no zstd module)
def frame_dict_id(z):
    """The dictionary id a zstd frame names (0: none)."""
    if z[:4] != ZSTD_MAGIC:
        raise FormatError('not a zstd frame')
    fhd = z[4]
    size = (0, 1, 2, 4)[fhd & 3]
    at = 5 + (0 if fhd & 0x20 else 1)   # single segment frames have no window byte
    return int.from_bytes(z[at:at + size], 'little') if size else 0


def zstd_decompress(z, work, dictionary=None):
    exe = shutil.which('zstd')
    if not exe:
        raise FormatError('the zstd command is needed to read a compressed archive')
    src = Path(work) / 'in.zs'
    src.write_bytes(z)
    cmd = [exe, '-q', '-d', '-c', str(src)]
    if dictionary:
        cmd[2:2] = ['-D', str(dictionary)]
    r = subprocess.run(cmd, capture_output=True)
    if r.returncode:
        raise FormatError(f'zstd failed: {r.stderr.decode(errors="replace").strip()}')
    return r.stdout


def unpack_zs(z, work, dictionaries=None):
    """A .zs file's bytes: decompressed with the dictionary its frame names, taken from Pack/ZsDic.pack.zs's bytes."""
    did = frame_dict_id(z)
    if not did:
        return zstd_decompress(z, work)
    if dictionaries is None:
        raise FormatError(f'the archive needs zstd dictionary {did} (Pack/ZsDic.pack.zs)')
    dics = zstd_decompress(dictionaries, work)
    for name, a, b in sarc_members(dics):
        d = dics[a:b]
        if len(d) >= 8 and u32(d, 0) == DICT_MAGIC and u32(d, 4) == did:
            path = Path(work) / 'zs.zsdic'
            path.write_bytes(d)
            return zstd_decompress(z, work, path)
    raise FormatError(f'no dictionary {did} in Pack/ZsDic.pack.zs')


def romfs_file(folder, index, path):
    for e in index['files']:
        if e['path'] == path:
            with open(Path(folder) / 'romfs.bin', 'rb') as f:
                f.seek(e['offset'])
                data = f.read(e['size'])
            if len(data) != e['size']:
                raise FormatError(f'{path} runs past the end of romfs.bin')
            return data
    raise FormatError(f'{path} is not in index.json')


def read_archives(source):
    """[(archive label, SARC bytes)] for an archive file or a RomFS folder."""
    source = Path(source)
    if source.is_dir():
        index = json.loads((source / 'index.json').read_text())
        build = ROOT / 'build'
        build.mkdir(exist_ok=True)
        out = []
        with tempfile.TemporaryDirectory(prefix='layout-tools-', dir=build) as work:
            dics = romfs_file(source, index, DICTIONARIES)
            for path in ROMFS_ARCHIVES:
                out.append((path.split('/')[-1].split('.')[0], unpack_zs(romfs_file(source, index, path), work, dics)))
        return out
    data = source.read_bytes()
    if data[:4] == ZSTD_MAGIC:
        build = ROOT / 'build'
        build.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='layout-tools-', dir=build) as work:
            data = unpack_zs(data, work)
    return [(source.name.split('.')[0], data)]


# ---- the layout
@dataclass
class Pane:
    index: int
    name: str
    at: int           # the record's offset
    sx: float
    sy: float
    rot: float
    x: float
    y: float
    parent: int
    w: float
    h: float
    end: int
    flags: int
    children: list = field(default_factory=list)

    @property
    def kind(self): return (self.flags >> 7) & 15

    @property
    def kind_name(self): return KINDS.get(self.kind, f'kind{self.kind}')

    @property
    def origin(self): return (self.flags >> 11) & 7, (self.flags >> 14) & 7

    @property
    def anchor(self): return (self.flags >> 17) & 7, (self.flags >> 20) & 7


def place_name(h, v):
    hn = H_CODES.get(h, (f'h{h}',))[0]
    vn = V_CODES.get(v, (f'v{v}',))[0]
    return 'centre' if hn == vn == 'centre' else f'{vn}-{hn}'


@dataclass
class Curve:
    index: int
    component: int
    at: int            # the curve record
    values_at: int     # the values record (+0 span)
    keys_at: int       # the floats: (value, slope) pairs
    frames: list
    values: list       # (value, slope)
    span: float


@dataclass
class Constant:
    index: int
    component: int
    at: int            # the float's offset
    value: float


@dataclass
class Track:
    code: bytes
    target: int        # bits 8-10 of the code
    pane: int          # the index the code names (a pane's when target is TARGET_PANE)
    group: int         # bit 22
    prop: int          # byte 3
    curves: list
    constants: list

    @property
    def key(self):
        return self.group, self.prop

    @property
    def prop_name(self):
        if self.target != TARGET_PANE:
            return f'target{self.target}.{self.group}.{self.prop}'
        return PROPERTIES.get(self.key, f'prop{self.group}.{self.prop}')


@dataclass
class Animation:
    name: str
    at: int
    length: int
    tracks: list


@dataclass(frozen=True)
class TextureMap:
    name: str
    alternatives: tuple = ()
    scale: tuple = (1.0, 1.0)
    translate: tuple = (0.0, 0.0)
    rotation: float = 0.0
    wrap: tuple = (2, 2)


@dataclass(frozen=True)
class Material:
    name: str
    textures: tuple
    colour: tuple


@dataclass(frozen=True)
class Picture:
    material: Material
    window: bool


class Layout:
    def __init__(self, name, data, archive='', copies=1):
        self.name, self.data, self.archive, self.copies = name, data, archive, copies
        d = data
        if d[:4] != b'BFFH':
            raise FormatError(f'{name} is not a BFFH layout')
        self.canvas = (f32(d, CANVAS), f32(d, CANVAS + 4))
        self.tables = {k: self.table(v) for k, v in TABLES.items()}
        self.panes = []
        for i, (pname, at) in enumerate(self.tables['panes']):
            if at + PANE_SIZE > len(d):
                raise FormatError(f'{name} pane {pname} past the end')
            p = Pane(i, pname, at, f32(d, at + PANE_SCALE_X), f32(d, at + PANE_SCALE_Y), f32(d, at + PANE_ROTATE),
                     f32(d, at + PANE_X), f32(d, at + PANE_Y), i32(d, at + PANE_PARENT), f32(d, at + PANE_W),
                     f32(d, at + PANE_H), u32(d, at + PANE_END), u32(d, at + PANE_FLAGS))
            if p.parent >= i:
                raise FormatError(f'{name} pane {pname} comes before its parent')
            if p.parent >= 0:
                self.panes[p.parent].children.append(i)
            self.panes.append(p)
        self.by_name = {p.name: p for p in self.panes}
        # A part record starts with the offset of the name of the layout it instantiates; the table's own names are
        # the part panes' names.
        self.parts = {pname: cstr(d, u64(d, at)) for pname, at in self.tables['parts']}
        self.part_records = dict(self.tables['parts'])
        self.animations = [self.animation(an, at) for an, at in self.tables['animations']]

    def pane_alpha(self, pane):
        d = self.data
        params = u64(d, 0x2e0)
        if not params:
            return 1.0
        index = u32(d, pane.at + 0x24)
        if index >= u32(d, 0x2e8):
            raise FormatError(f'{self.name}: no pane parameters for {pane.name}')
        return f32(d, u64(d, params + 8 * index) + 0x10)

    def part_animations(self, pane_name):
        d, at = self.data, self.part_records[pane_name]
        entries, count = u64(d, at + 0x28), u32(d, at + 0x30)
        if count > (len(d) - entries) // 8:
            raise FormatError(f'{self.name}: animation list for {pane_name} past the end')
        out = []
        for i in range(count):
            entry = u64(d, entries + 8 * i)
            out.append((cstr(d, u64(d, entry)), f32(d, entry + 8)))
        return out

    def pictures(self):
        """Picture records by pane name, with their texture bindings and UV transforms."""
        d = self.data
        materials = self.tables['materials']
        result = {}
        for name, at in self.tables['pictures']:
            index = u16(d, at + 0x10)
            if index >= len(materials):
                raise FormatError(f'{self.name}: no material {index} for {name}')
            mname, mat = materials[index]
            textures = self.texture_maps(mat)
            palette, colour_index = u64(d, 0x2c0), u32(d, mat + 0x28)
            colour = (1.0,) * 4
            if palette:
                if colour_index >= u32(d, 0x2c8):
                    raise FormatError(f'{self.name}: no palette colour {colour_index} for {mname}')
                colour = tuple(f32(d, palette + 16 * colour_index + 4 * i) for i in range(4))
            result[name] = Picture(Material(mname, textures, colour), bool(u64(d, at + 8)))
        return result

    def texture_maps(self, mat):
        d = self.data
        index = u32(d, mat + 0x14)
        if index >= u32(d, 0x230):
            raise FormatError(f'{self.name}: no render material {index}')
        render = u64(d, u64(d, 0x228) + index * 8)
        samplers, count = u64(d, render), u32(d, render + 8)
        maps, map_count = u64(d, mat), u32(d, mat + 8)
        if count > (len(d) - samplers) // 8 or map_count > (len(d) - maps) // 8:
            raise FormatError(f'{self.name}: texture binding list past the end')
        textures = []
        for i in range(count):
            sampler = u64(d, samplers + 8 * i)
            name = u64(d, sampler + 8)
            wrap = (u8(d, sampler), u8(d, sampler + 1))
            params = {}
            if i < map_count:
                mapping = u64(d, maps + 8 * i)
                alternatives, n = u64(d, mapping), u32(d, mapping + 8)
                if n > (len(d) - alternatives) // 8:
                    raise FormatError(f'{self.name}: texture pattern list past the end')
                params = dict(alternatives=tuple(cstr(d, u64(d, alternatives + 8 * j)) for j in range(n)),
                              scale=(f32(d, mapping + 0x10), f32(d, mapping + 0x14)),
                              translate=(f32(d, mapping + 0x18), f32(d, mapping + 0x1c)),
                              rotation=f32(d, mapping + 0x20))
            textures.append(TextureMap(cstr(d, name) if name else '', wrap=wrap, **params))
        return tuple(textures)

    def table(self, slot):
        d = self.data
        names, records, count = u64(d, slot), u64(d, slot + 8), u32(d, slot + 24)
        if count and (names + 8 * count > len(d) or records + 8 * count > len(d)):
            raise FormatError(f'{self.name}: table at 0x{slot:x} past the end')
        return [(cstr(d, u64(d, names + 8 * i)), u64(d, records + 8 * i)) for i in range(count)]

    def animation(self, name, at):
        d = self.data
        body = u64(d, at)
        lists = {}
        for key, off in (('tracks', BODY_TRACKS), ('curves', BODY_CURVES), ('ccomp', BODY_CURVE_COMPONENTS),
                         ('consts', BODY_CONSTANTS), ('kcomp', BODY_CONSTANT_COMPONENTS)):
            lists[key] = (u64(d, body + off), u32(d, body + off + 8))
        tracks = []
        for t in range(lists['tracks'][1]):
            e = u64(d, lists['tracks'][0] + 8 * t)
            code_at = u64(d, e)
            code = bytes(d[code_at:code_at + 6])
            if len(code) != 6:
                raise FormatError(f'{self.name} {name}: track code past the end')
            first, cfirst, nc, nk = u16(d, e + 8), u16(d, e + 10), u8(d, e + 12), u8(d, e + 13)
            v = int.from_bytes(code, 'little')
            track = Track(code, (v >> 8) & 7, ((v >> 11) & 0xf) | (((v >> 16) & 7) << 4), (v >> 22) & 1, code[3],
                          [], [])
            if first != NO_INDEX:
                for k in range(first, first + nc):
                    if k >= lists['curves'][1] or k >= lists['ccomp'][1]:
                        raise FormatError(f'{self.name} {name}: curve {k} past the list')
                    track.curves.append(self.curve(k, u64(d, lists['curves'][0] + 8 * k),
                                                   u8(d, lists['ccomp'][0] + k)))
            if cfirst != NO_INDEX:
                for k in range(cfirst, cfirst + nk):
                    if k >= lists['consts'][1] or k >= lists['kcomp'][1]:
                        raise FormatError(f'{self.name} {name}: constant {k} past the list')
                    off = lists['consts'][0] + 4 * k
                    track.constants.append(Constant(k, u8(d, lists['kcomp'][0] + k), off, f32(d, off)))
            tracks.append(track)
        return Animation(name, at, u32(d, at + ANIM_LENGTH), tracks)

    def curve(self, index, at, component):
        d = self.data
        frames_at, values_at = u64(d, at), u64(d, at + 8)
        fo, fn = u64(d, frames_at + 8), u32(d, frames_at + 16)
        vo, vn = u64(d, values_at + 8), u32(d, values_at + 16)
        frames = [f32(d, fo + 4 * i) for i in range(fn)]
        raw = [f32(d, vo + 4 * i) for i in range(vn)]
        values = list(zip(raw[0::2], raw[1::2])) if vn == 2 * fn else [(v, 0.0) for v in raw]
        return Curve(index, component, at, values_at, vo, frames, values, f32(d, values_at))

    # -- where things are
    def pane_names(self, index):
        return self.panes[index].name if 0 <= index < len(self.panes) else f'#{index}'

    def tracks_of(self, pane_index, prop=None):
        """[(animation, track)] of a pane's properties (prop: a PROPERTIES key, None for all)."""
        return [(a, t) for a in self.animations for t in a.tracks
                if t.target == TARGET_PANE and t.pane == pane_index and (prop is None or t.key == prop)]


def evaluate(curve, frame):
    """A curve's value at a frame: Hermite interpolation of its (value, slope) keys, held before and after them. Two
    keys on one frame make a step; on that frame the later one holds (pattern animations pick a state by frame)."""
    f, v = curve.frames, curve.values
    if not f:
        return None
    if frame < f[0]:
        return v[0][0]
    i = max(k for k in range(len(f)) if f[k] <= frame)
    if f[i] == frame or i == len(f) - 1:
        return v[i][0]
    dt = f[i + 1] - f[i]
    t = (frame - f[i]) / dt
    t2, t3 = t * t, t * t * t
    return ((2 * t3 - 3 * t2 + 1) * v[i][0] + (t3 - 2 * t2 + t) * dt * v[i][1] +
            (-2 * t3 + 3 * t2) * v[i + 1][0] + (t3 - t2) * dt * v[i + 1][1])


def animated_state(layout, anim_name, frame, *, material=False):
    """Pane properties at a frame, or material properties by (texture map, property, component)."""
    for a in layout.animations:
        if a.name != anim_name:
            continue
        out = {}
        for t in a.tracks:
            if t.target != (0 if material else TARGET_PANE):
                continue
            slot = out.setdefault(t.pane, {})
            key = (t.code[0], t.key) if material else (t.key,)
            for c in t.curves:
                value = evaluate(c, frame)
                if value is not None:
                    slot[(*key, c.component)] = value
            for k in t.constants:
                slot[(*key, k.component)] = k.value
        return out
    raise SpecError(f'{layout.name} has no animation {anim_name}')


@dataclass
class Box:
    """A pane on the grid: the point its position names, the scale its children inherit, its box."""
    px: float
    py: float
    gsx: float
    gsy: float
    w: float           # the pane's own size (an animation may change it)
    h: float
    left: float
    right: float
    bottom: float
    top: float
    rotated: bool


def pane_geometry(pane, state):
    """Animated translate, scale, size and Z rotation, in the parent's coordinates."""
    return (state.get((TRANSLATE, 0), pane.x), state.get((TRANSLATE, 4), pane.y),
            state.get((SCALE, 0), pane.sx), state.get((SCALE, 4), pane.sy),
            state.get((SIZE, 0), pane.w), state.get((SIZE, 4), pane.h), state.get((ROTATION, 8), pane.rot))


def anchor_offset(pane, parent, width, height):
    """The child's anchor point on the parent's box, relative to the parent's origin."""
    qh, qv = parent.origin
    ah, av = pane.anchor
    return ((H_CODES.get(ah, ('', .5))[1] - H_CODES.get(qh, ('', .5))[1]) * width,
            (V_CODES.get(av, ('', .5))[1] - V_CODES.get(qv, ('', .5))[1]) * height)


def place(layout, state=None, at=(0.0, 0.0), scale=(1.0, 1.0)):
    """Every pane's Box on the grid. A child's position is its parent's position plus the parent's scale times (the
    anchor's point on the parent's box plus the child's translate); its box spans its size times its scale on the grid
    around its origin. Rotation is not applied (Box.rotated says a pane or an ancestor is turned). state: overrides
    from animated_state()."""
    state = state or {}
    boxes = []
    for p in layout.panes:
        x, y, sx, sy, pw, ph, angle = pane_geometry(p, state.get(p.index, {}))
        rotated = not same(angle, 0)
        if p.parent < 0:
            px, py, psx, psy, rot = at[0], at[1], scale[0], scale[1], False
            ax = ay = 0.0
        else:
            q, qp = boxes[p.parent], layout.panes[p.parent]
            px, py, psx, psy, rot = q.px, q.py, q.gsx, q.gsy, q.rotated
            ax, ay = anchor_offset(p, qp, q.w, q.h)
        gx, gy = px + psx * (ax + x), py + psy * (ay + y)
        gsx, gsy = psx * sx, psy * sy
        oh, ov = p.origin
        fx, fy = H_CODES.get(oh, ('', .5))[1], V_CODES.get(ov, ('', .5))[1]
        w, h = pw * gsx, ph * gsy
        boxes.append(Box(gx, gy, gsx, gsy, pw, ph, gx - fx * w, gx + (1 - fx) * w, gy - fy * h, gy + (1 - fy) * h,
                         rot or rotated))
    return boxes


# ---- loading
def load(source):
    """{layout name: Layout} of an archive or a RomFS folder; the first copy of a name, with the count of copies."""
    out = {}
    for label, data in read_archives(source):
        counts = {}
        for name, a, b in sarc_members(data):
            counts[name] = counts.get(name, 0) + 1
        for name, a, b in sarc_members(data):
            if not name.endswith('.buil'):
                continue
            lname = name[:-len('.buil')]
            if lname in out:
                if out[lname].archive != label:
                    print(f'layout_dump: {lname} is in {out[lname].archive} and {label}; {out[lname].archive}\'s '
                          'is used', file=sys.stderr)
                continue
            out[lname] = Layout(lname, bytearray(data[a:b]), label, counts[name])
    return out


# ---- edits (layout_preview.py --spec)
def out_units(aspect):
    """How far a side of the picture lies beyond the middle 16:9, in grid units: 0 at 16:9, 640 at 21:9."""
    return max(0.0, 1080.0 * aspect - 1920.0)


def parse_aspect(text):
    """A picture's width over its height from 21:9, 3440x1440 or 2.39. 21:9 names the 2560x1080 picture (64:27)."""
    if text == '21:9':
        return 64 / 27
    for sep in (':', 'x'):
        if sep in text:
            a, b = text.split(sep)
            return float(a) / float(b)
    return float(text)


def _put_f32(layout, off, value):
    struct.pack_into('<f', layout.data, off, value)


# ---- the "split" edit: a picture drawn in pieces, each a pane of its own
PICTURE_BLOB = 0x88                  # a picture record without a window, with its colour and corner lists
PICTURE_POINTERS = (0x00, 0x20, 0x48, 0x50, 0x58, 0x60)   # its offsets that point inside it
MATERIAL_RECORD = 0x40
SECTION_POOL = 0xd0                  # the second BFSI section (StringPool): +0x08 its size
FILE_SIZE = 0x08
HASH_SEED = 0x5157_4944_4548_5544    # made-up names' hashes: FNV-1a from this seed (they only need to differ)


def _align(n, a=8):
    return (n + a - 1) // a * a


def name_hash(name):
    h = HASH_SEED
    for c in name.encode():
        h = ((h ^ c) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


def split_picture(layout, op, out):
    """The layout's bytes with picture pane op['pane'] drawn in pieces, and a line per piece. Each piece
    {"src": [a, b], "at": [x0, x1], "f": [f0, f1]} shows the picture's texture from a to b (x in the pane's parent, the
    picture unrotated at scale 1 with its centre origin) between x0 + f0 * out and x1 + f1 * out. The first piece is
    the pane itself; the others are copies inserted right after it in pane order, each with a copy of the picture
    record and of the material, whose texture map gets the piece's UV window. New lists of names, records and hashes
    replace the pane, picture and material tables' (the old ones are left unused); everything new is appended past the
    end of the file, which the string pool section grows to cover. Every pane index past the pane's (parents, subtree
    ends, the animation track codes) moves by the number of copies."""
    d = bytearray(layout.data)
    p = layout.by_name.get(op.get('pane'))
    if not p or p.kind != 1 or p.children:
        raise SpecError(f'{layout.name}: split needs a picture pane without children, not {op.get("pane")!r}')
    if not same(p.rot, 0) or not same(p.sx, 1) or p.origin[0] != 2:
        raise SpecError(f'{layout.name} {p.name}: split needs a picture unrotated at scale 1 with its origin centred')
    pieces = op['pieces']
    if not pieces:
        raise SpecError(f'{layout.name} {p.name}: split with no pieces')
    pics, mats = layout.tables['pictures'], layout.tables['materials']
    pic_index = u16(d, p.at + 0x42)
    if pic_index >= len(pics) or pics[pic_index][0] != p.name:
        raise SpecError(f'{layout.name} {p.name}: its picture record is not the one its pane names')
    pic_at = pics[pic_index][1]
    mat_index = u16(d, pic_at + 0x10)
    if mat_index != u16(d, p.at + 0x3c) or mat_index >= len(mats):
        raise SpecError(f'{layout.name} {p.name}: its material is not the one its picture names')
    mat_at = mats[mat_index][1]
    if u64(d, pic_at + 8) or u32(d, mat_at + 8) != 1 or u64(d, mat_at) != mat_at + MATERIAL_RECORD:
        raise SpecError(f'{layout.name} {p.name}: split needs a plain picture with one texture map')
    for off in PICTURE_POINTERS:
        if not pic_at <= u64(d, pic_at + off) < pic_at + PICTURE_BLOB:
            raise SpecError(f'{layout.name} {p.name}: its picture record is not laid out as expected')
    map_at = u64(d, mat_at + MATERIAL_RECORD)
    if map_at != mat_at + MATERIAL_RECORD + 8 or u64(d, map_at):
        raise SpecError(f'{layout.name} {p.name}: its texture map is not laid out as expected')
    mat_blob = MATERIAL_RECORD + 8 + 0x30
    sx, tx = f32(d, map_at + 0x10), f32(d, map_at + 0x18)
    if not same(f32(d, map_at + 0x20), 0):
        raise SpecError(f'{layout.name} {p.name}: its texture map is rotated')
    left = p.x - p.w / 2
    extra = len(pieces) - 1
    i = p.index
    lines = []

    def windows(piece):
        (a, b), (x0, x1), (f0, f1) = piece['src'], piece['at'], piece['f']
        t0, t1 = x0 + f0 * out, x1 + f1 * out
        if not (left - SAME <= a < b <= left + p.w + SAME) or t1 < t0:
            raise SpecError(f'{layout.name} {p.name}: piece {piece} is outside the picture or reversed')
        su0, su1 = (a - left) / p.w, (b - left) / p.w
        return (t0 + t1) / 2, t1 - t0, sx * (su1 - su0), tx + ((su0 + su1) / 2 - 0.5) * sx

    # renumber what follows the pane, in place, before anything is appended
    for q in layout.panes:
        if q.parent > i:
            struct.pack_into('<i', d, q.at + PANE_PARENT, q.parent + extra)
        if q.end > i and q.index != i:   # its ancestors' and later panes' subtrees end further on
            struct.pack_into('<I', d, q.at + PANE_END, q.end + extra)
    codes = set()
    for an, at in layout.tables['animations']:
        body = u64(d, at)
        tl, tn = u64(d, body + BODY_TRACKS), u32(d, body + BODY_TRACKS + 8)
        for k in range(tn):
            codes.add(u64(d, u64(d, tl + 8 * k)))
    for c in sorted(codes):
        v = int.from_bytes(d[c:c + 6], 'little')
        idx = ((v >> 11) & 0xf) | (((v >> 16) & 7) << 4)
        if idx > i:
            n = idx + extra
            if n > 127:
                raise SpecError(f'{layout.name}: a track would name pane {n}, past the 128 a code can name')
            v = (v & ~((0xf << 11) | (7 << 16))) | ((n & 0xf) << 11) | (((n >> 4) & 7) << 16)
            d[c:c + 6] = v.to_bytes(6, 'little')

    blob = bytearray()
    base = _align(len(d))

    def put(data, align=8):
        nonlocal blob
        pad = _align(base + len(blob), align) - (base + len(blob))
        blob += b'\0' * pad
        at = base + len(blob)
        blob += data
        return at

    # the pane itself shows the first piece
    cx, w, usx, utx = windows(pieces[0])
    struct.pack_into('<f', d, p.at + PANE_X, cx)
    struct.pack_into('<f', d, p.at + PANE_W, w)
    struct.pack_into('<ff', d, map_at + 0x10, usx, f32(d, map_at + 0x14))
    struct.pack_into('<ff', d, map_at + 0x18, utx, f32(d, map_at + 0x1c))
    lines.append(f'{p.name} piece 0: x {p.x:g} -> {cx:g}, width {p.w:g} -> {w:g}, uv scale {sx:g} -> {usx:g}, '
                 f'translate {tx:g} -> {utx:g}')
    new_panes, new_pics, new_mats = [], [], []
    for k, piece in enumerate(pieces[1:], 1):
        name = f'{p.name}_{k}'
        cx, w, usx, utx = windows(piece)
        name_at = put(name.encode() + b'\0', 1)
        # material: record, its one-entry map list and the map, internal pointers moved with them
        m = bytearray(d[mat_at:mat_at + mat_blob])
        mat_new = put(bytes(m))
        struct.pack_into('<Q', blob, mat_new - base, mat_new + MATERIAL_RECORD)
        struct.pack_into('<Q', blob, mat_new - base + MATERIAL_RECORD, mat_new + MATERIAL_RECORD + 8)
        mp = mat_new - base + MATERIAL_RECORD + 8
        struct.pack_into('<ff', blob, mp + 0x10, usx, f32(d, map_at + 0x14))
        struct.pack_into('<ff', blob, mp + 0x18, utx, f32(d, map_at + 0x1c))
        new_mats.append((name_at, mat_new, name))
        # picture record, pointing at the new material
        pr = bytearray(d[pic_at:pic_at + PICTURE_BLOB])
        pic_new = put(bytes(pr))
        for off in PICTURE_POINTERS:
            struct.pack_into('<Q', blob, pic_new - base + off, u64(d, pic_at + off) - pic_at + pic_new)
        struct.pack_into('<H', blob, pic_new - base + 0x10, len(mats) + k - 1)
        new_pics.append((name_at, pic_new, name))
        # pane record
        pn = bytearray(d[p.at:p.at + PANE_SIZE])
        struct.pack_into('<f', pn, PANE_X, cx)
        struct.pack_into('<f', pn, PANE_W, w)
        struct.pack_into('<I', pn, PANE_END, i + k + 1)
        struct.pack_into('<H', pn, 0x3c, len(mats) + k - 1)
        struct.pack_into('<H', pn, 0x42, len(pics) + k - 1)
        pane_new = put(bytes(pn))
        new_panes.append((name_at, pane_new, name))
        lines.append(f'{name} piece {k}: x {cx:g}, width {w:g}, uv scale {usx:g}, translate {utx:g}')

    def new_table(slot, entries, insert_at):
        names, records, hashes, count = u64(d, slot), u64(d, slot + 8), u64(d, slot + 16), u32(d, slot + 24)
        rows = [(u64(d, names + 8 * j), u64(d, records + 8 * j), u64(d, hashes + 8 * j)) for j in range(count)]
        add = [(na, ra, name_hash(nm)) for na, ra, nm in entries]
        rows = rows[:insert_at] + add + rows[insert_at:]
        lists = [put(b''.join(struct.pack('<Q', r[c]) for r in rows)) for c in range(3)]
        return slot, lists, len(rows)

    tables = [new_table(TABLES['panes'], new_panes, i + 1),
              new_table(TABLES['pictures'], new_pics, len(pics)),
              new_table(TABLES['materials'], new_mats, len(mats))]
    d += b'\0' * (base - len(d)) + blob
    for slot, lists, count in tables:
        struct.pack_into('<QQQI', d, slot, *lists, count)
    pool_start = u64(d, SECTION_POOL + 0x10)
    struct.pack_into('<I', d, SECTION_POOL + 8, len(d) - pool_start)
    struct.pack_into('<I', d, FILE_SIZE, len(d))
    return d, lines


def apply_ops(layout, ops, out):
    """Apply a list of edit operations to a layout's bytes (a bytearray) and return the reparsed layout and a line per
    operation. Raises SpecError, leaving `layout` untouched, when an operation names what the layout does not hold or a
    value it expects differs."""
    trial = Layout(layout.name, bytearray(layout.data), layout.archive, layout.copies)
    lines = []

    def pane_of(op):
        p = trial.by_name.get(op.get('pane'))
        if not p:
            raise SpecError(f'{layout.name}: no pane {op.get("pane")!r}')
        for key, have in (('x', p.x), ('y', p.y), ('w', p.w), ('h', p.h), ('sx', p.sx), ('sy', p.sy)):
            if key in op and not same(have, float(op[key])):
                raise SpecError(f'{layout.name} {p.name}: {key} is {have:g}, the spec expects {op[key]:g}')
        return p

    for op in ops:
        kind = op.get('op')
        if kind == 'split':
            data, split_lines = split_picture(trial, op, out)
            trial = Layout(trial.name, data, trial.archive, trial.copies)
            lines += split_lines
            continue
        if kind == 'move':
            p = pane_of(op)
            nx = p.x + float(op['f']) * out
            _put_f32(trial, p.at + PANE_X, nx)
            lines.append(f'{p.name} x {p.x:g} -> {nx:g}')
        elif kind == 'grow':
            p = pane_of(op)
            nw = p.w + 2 * float(op['g']) * out
            _put_f32(trial, p.at + PANE_W, nw)
            lines.append(f'{p.name} width {p.w:g} -> {nw:g}')
        elif kind == 'widen':
            p = pane_of(op)
            ns = p.sx * (1 + float(op['g']) * out / 1920)
            _put_f32(trial, p.at + PANE_SCALE_X, ns)
            lines.append(f'{p.name} scale x {p.sx:g} -> {ns:g}')
        elif kind == 'narrow':
            p = pane_of(op)
            ns = p.sx * 1920 / (1920 + out)
            _put_f32(trial, p.at + PANE_SCALE_X, ns)
            lines.append(f'{p.name} scale x {p.sx:g} -> {ns:g}')
        elif kind == 'keys':
            p = pane_of(op)
            prop = {'scale': SCALE, 'translate': TRANSLATE, 'size': SIZE}.get(op.get('prop'))
            if prop is None:
                raise SpecError(f'{layout.name}: keys op needs prop translate, scale or size, not {op.get("prop")!r}')
            comp = {'x': 0, 'y': 4}[op.get('component', 'x')]
            found = [t for a, t in trial.tracks_of(p.index, prop) if a.name == op.get('anim')]
            if not found:
                raise SpecError(f'{layout.name}: animation {op.get("anim")!r} has no {op["prop"]} track of {p.name}')
            for m in op.get('map', []):
                v, f = float(m['value']), float(m['f'])
                nv, hits = v + f * out, 0
                for t in found:
                    for c in t.curves:
                        if c.component != comp:
                            continue
                        for i, (val, _slope) in enumerate(c.values):
                            if same(val, v):
                                _put_f32(trial, c.keys_at + 8 * i, nv)
                                hits += 1
                    for k in t.constants:
                        if k.component == comp and same(k.value, v):
                            _put_f32(trial, k.at, nv)
                            hits += 1
                if not hits:
                    raise SpecError(f'{layout.name} {op["anim"]} {p.name} {op["prop"]}: no key holds {v:g}')
                lines.append(f'{op["anim"]} {p.name} {op["prop"]} {op.get("component", "x")}: {hits} key(s) '
                             f'{v:g} -> {nv:g}')
            # each curve's span (its last value less its first) follows its keys
            for t in found:
                for c in t.curves:
                    if c.component == comp and len(c.values) > 1:
                        new = Layout.curve(trial, c.index, c.at, c.component)
                        _put_f32(trial, c.values_at, new.values[-1][0] - new.values[0][0])
        else:
            raise SpecError(f'{layout.name}: unknown op {kind!r}')
        trial = Layout(trial.name, trial.data, trial.archive, trial.copies)
    return trial, lines


# ---- text and JSON
def fmt(v):
    return f'{v:.6g}' if abs(v) >= 1e-3 or v == 0 else '0'


def layout_json(lay, layouts):
    boxes = place(lay)
    panes = []
    for p, b in zip(lay.panes, boxes):
        e = {'index': p.index, 'name': p.name, 'kind': p.kind_name, 'parent': p.parent, 'x': p.x, 'y': p.y,
             'w': p.w, 'h': p.h, 'sx': p.sx, 'sy': p.sy, 'rotation': p.rot, 'origin': place_name(*p.origin),
             'anchor': place_name(*p.anchor), 'flags': p.flags, 'grid': [b.left, b.right, b.bottom, b.top]}
        if p.name in lay.parts:
            e['part'] = lay.parts[p.name]
        panes.append(e)
    anims = []
    for a in lay.animations:
        tracks = []
        for t in a.tracks:
            tracks.append({'code': t.code.hex(), 'target': t.target, 'pane': lay.pane_names(t.pane)
                           if t.target == TARGET_PANE else t.pane, 'property': t.prop_name,
                           'curves': [{'component': COMPONENTS.get(c.component, c.component),
                                       'keys': [[f, v, s] for f, (v, s) in zip(c.frames, c.values)],
                                       'span': c.span} for c in t.curves],
                           'constants': [{'component': COMPONENTS.get(k.component, k.component), 'value': k.value}
                                         for k in t.constants]})
        anims.append({'name': a.name, 'frames': a.length, 'tracks': tracks})
    return {'name': lay.name, 'archive': lay.archive, 'copies': lay.copies, 'canvas': list(lay.canvas),
            'panes': panes, 'parts': lay.parts, 'animations': anims}


def track_text(lay, t):
    who = lay.pane_names(t.pane) if t.target == TARGET_PANE else f'target {t.target} #{t.pane}'
    parts = []
    for c in t.curves:
        keys = ' '.join(f'{fmt(f)}:{fmt(v)}' for f, (v, s) in zip(c.frames, c.values))
        parts.append(f'{COMPONENTS.get(c.component, c.component)} [{keys}]')
    for k in t.constants:
        parts.append(f'{COMPONENTS.get(k.component, k.component)} ={fmt(k.value)}')
    return f'{who} {t.prop_name}: ' + '; '.join(parts) + f'   (code {t.code.hex()})'


def layout_text(lay, layouts):
    lines = [f'== {lay.name}  ({lay.archive}, {lay.copies} cop{"y" if lay.copies == 1 else "ies"})  canvas '
             f'{fmt(lay.canvas[0])} x {fmt(lay.canvas[1])}  {len(lay.panes)} panes, {len(lay.animations)} animations']
    boxes = place(lay)
    lines.append('  panes: name  kind  x y (in the parent)  w x h  scale  [rotation]  origin / anchor  '
                 '| x extent on the grid')
    depth = [0] * len(lay.panes)
    for p, b in zip(lay.panes, boxes):
        if p.parent >= 0:
            depth[p.index] = depth[p.parent] + 1
        extra = f' rot {fmt(p.rot)}' if not same(p.rot, 0) else ''
        part = f'  -> {lay.parts[p.name]}' if p.name in lay.parts else ''
        lines.append(f'  {p.index:3d} {"  " * depth[p.index]}{p.name:<{max(1, 34 - 2 * depth[p.index])}} '
                     f'{p.kind_name:<10} {fmt(p.x):>8} {fmt(p.y):>8}  {fmt(p.w):>7} x {fmt(p.h):<7} '
                     f'{fmt(p.sx)}x{fmt(p.sy)}{extra}  {place_name(*p.origin)} / {place_name(*p.anchor)}  '
                     f'| {fmt(b.left)} .. {fmt(b.right)}{" (turned)" if b.rotated else ""}{part}')
    if lay.parts:
        lines.append('  parts:')
        for pane, target in lay.parts.items():
            lines.append(f'    {pane} -> {target}{"" if target in layouts else " (not in this archive)"}')
    for a in lay.animations:
        lines.append(f'  animation {a.name} ({a.length} frames, {len(a.tracks)} tracks)')
        for t in a.tracks:
            lines.append('    ' + track_text(lay, t))
    return '\n'.join(lines)


def find_animated(lay):
    """[(pane, what, [animations])] for panes whose translate x, scale x or size an animation keys."""
    found = {}
    for a in lay.animations:
        for t in a.tracks:
            if t.target != TARGET_PANE:
                continue
            comps = [c.component for c in t.curves] + [k.component for k in t.constants]
            if t.key in (SCALE, TRANSLATE, SIZE) and 0 in comps:
                what = {SCALE: 'scale x', TRANSLATE: 'translate x', SIZE: 'width'}[t.key]
            elif t.key == ROTATION:
                what = 'rotation'
            else:
                continue
            kind = 'curve' if any(c.component == 0 for c in t.curves) else 'constant'
            found.setdefault((lay.pane_names(t.pane), what), []).append(f'{a.name} ({kind})')
    return [(p, w, an) for (p, w), an in sorted(found.items())]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('source', type=Path, help='a decompressed layout archive (.blarc) or a RomFS folder')
    ap.add_argument('--layout', action='append', help='only this layout (repeatable)')
    ap.add_argument('--json', action='store_true', help='print JSON')
    ap.add_argument('--find-animated', action='store_true',
                    help='list the panes whose translate x, scale x or size an animation keys')
    ap.add_argument('--codes', action='store_true', help="count the archive's track target codes")
    args = ap.parse_args(argv)
    try:
        layouts = load(args.source)
    except (FormatError, OSError, ValueError, KeyError) as e:
        sys.exit(f'layout_dump: {e}')
    names = args.layout or sorted(layouts)
    missing = [n for n in names if n not in layouts]
    if missing:
        sys.exit(f'layout_dump: no layout {", ".join(missing)}')
    if args.codes:
        counts = {}
        for n in names:
            for a in layouts[n].animations:
                for t in a.tracks:
                    comps = tuple(sorted({c.component for c in t.curves} | {k.component for k in t.constants}))
                    key = (t.target, t.group, t.prop, comps)
                    counts.setdefault(key, [0, n, a.name, layouts[n].pane_names(t.pane)])[0] += 1
        print('target bit22 byte3 property   components  tracks  first seen')
        for (target, group, prop, comps), (c, n, an, pn) in sorted(counts.items()):
            what = PROPERTIES.get((group, prop), '') if target == TARGET_PANE else ''
            print(f'{target:6} {group:5} {prop:5} {what:<10} {",".join(COMPONENTS.get(x, str(x)) for x in comps):<11} '
                  f'{c:7}  {n} {an} {pn}')
        return
    if args.find_animated:
        for n in names:
            for pane, what, anims in find_animated(layouts[n]):
                print(f'{n} {pane} {what}: {", ".join(anims)}')
        return
    if args.json:
        json.dump([layout_json(layouts[n], layouts) for n in names], sys.stdout, indent=1)
        print()
        return
    for n in names:
        print(layout_text(layouts[n], layouts))


if __name__ == '__main__':
    main()
