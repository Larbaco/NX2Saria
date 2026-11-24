"""Xbox, PlayStation, Steam Deck and keyboard-and-mouse button prompts for Ocarina of Time (Switch 2), drawn from button"""
import argparse
import hashlib
import io
import json
import struct
import sys
from pathlib import Path

import zstandard
from PIL import Image, ImageDraw, ImageFont
from fontTools.pens.boundsPen import BoundsPen
from fontTools.ttLib import TTFont

sys.path.insert(0, str(Path(__file__).resolve().parent))
import button_icons  # noqa: E402  (bfotf_decode, bfotf_encode, redraw_font, grow_restbl)
from prompt_art import BLANKS, KM, OPTIONS, SHARE, STYLES, overlay_folder  # noqa: E402  (the art, the folders)

FONT = 'Font2/Scalable/External.Nin_Ounce_NVN2.bfotf.zs'
TEXTURES = 'UI/Layout2Archive/Common.Product.100.Nin_Ounce_NVN2.bntx.zs'
DICTIONARIES = 'Pack/ZsDic.pack.zs'

GLYPHS = {
    0xE040: 'A', 0xE041: 'B', 0xE042: 'X', 0xE043: 'Y', 0xE044: 'L', 0xE045: 'R', 0xE046: 'ZL', 0xE047: 'ZR',
    0xE049: 'DPAD', 0xE04B: 'PLUS', 0xE04C: 'MINUS',
    0xE050: 'LSTICK_DOWN', 0xE051: 'RSTICK_DOWN', 0xE052: 'LSTICK_UP', 0xE053: 'RSTICK_UP',
    0xE054: 'LSTICK_LEFTRIGHT', 0xE055: 'RSTICK_LEFTRIGHT', 0xE056: 'LSTICK_PRESS', 0xE057: 'RSTICK_PRESS',
    0xE058: 'LSTICK_RIGHT', 0xE059: 'RSTICK_RIGHT', 0xE05A: 'LSTICK_LEFT', 0xE05B: 'RSTICK_LEFT',
    0xE05C: 'LSTICK_UPDOWN', 0xE05D: 'RSTICK_UPDOWN', 0xE05E: 'LSTICK', 0xE05F: 'RSTICK',
    0xE060: 'DPAD_UP', 0xE061: 'DPAD_DOWN', 0xE062: 'DPAD_LEFT', 0xE063: 'DPAD_RIGHT',
    0xE064: 'DPAD_UPDOWN', 0xE065: 'DPAD_LEFTRIGHT', 0xE068: 'RSTICK_ROTATE', 0xE06A: 'DPAD',
}
FACE = {'position': {'A': 'east', 'B': 'south', 'X': 'north', 'Y': 'west'},
        'label': {'A': 'south', 'B': 'east', 'X': 'west', 'Y': 'north'}}


MASKS = {'KeyTexFontA_00': 'A', 'KeyTexFontB_00': 'B', 'KeyTexFontX_00': 'X', 'KeyTexFontY_00': 'Y'}
NOTES = {'OcarinaButtonA_00': 'A', 'OcarinaButtonB_00': 'B', 'OcarinaButtonX_00': 'X', 'OcarinaButtonY_00': 'Y',
         'OcarinaButtonL_00': 'L', 'OcarinaButtonR_00': 'R'}


def pad_pictures(style, layout):
    s = STYLES[style]
    glyph = {b: s['glyph'][FACE[layout].get(b, b)] for b in set(GLYPHS.values())}
    note = {b: s['note'][FACE[layout].get(b, b)] for b in NOTES.values()}
    return glyph, note


PC_KEYS = {'A': 'e', 'B': 'lctrl', 'X': 'space', 'Y': 'mouse_l', 'L': 'q', 'R': 'r', 'ZL': 'mouse_r', 'ZR': 'f',
           'PLUS': 'tab', 'MINUS': 'm', 'LEFT': '1', 'UP': '2', 'RIGHT': '3', 'DOWN': '4', 'LCLICK': 'v', 'RCLICK': 'c',
           'LSTICK_UP': 'w', 'LSTICK_LEFT': 'a', 'LSTICK_DOWN': 's', 'LSTICK_RIGHT': 'd'}
KEY_ART = {'space': 'Space', 'tab': 'Tab', 'lshift': 'Shift', 'rshift': 'Shift', 'lctrl': 'Crtl', 'rctrl': 'Crtl',
           'lalt': 'Alt', 'up': 'Up', 'down': 'Down', 'left': 'Left', 'right': 'Right', 'enter': 'Enter',
           'backspace': 'BackSpace', 'capslock': 'CapsLock', 'insert': 'Ins', 'delete': 'Del', 'home': 'Home',
           'end': 'End', 'pageup': 'PageUp', 'pagedown': 'PageDown', 'minus': 'Minus', 'num_sub': 'Minus',
           'num_add': 'Plus', 'num_mul': 'Asterisk', 'num_div': 'Slash', 'lbracket': 'Brackets_L',
           'rbracket': 'Brackets_R', 'semicolon': 'Semicolon', 'apostrophe': 'Quotation', 'slash': 'Slash',
           'grave': 'Tilde', 'mouse_l': 'Mouse_Left', 'mouse_r': 'Mouse_Right', 'mouse_m': 'Mouse_Middle'}
KEY_LABELS = {'comma': ',', 'period': '.', 'equals': '=', 'num_decimal': '.', 'backslash': '\\', 'mouse_x1': 'M4',
              'mouse_x2': 'M5'}


def pc_keys(ini_text=None):
    keys = dict(PC_KEYS)
    for line in (ini_text or '').splitlines():
        line = line.split('#')[0].split(';')[0].strip()
        if not line.lower().startswith('bind_') or '=' not in line:
            continue
        button, value = (s.strip() for s in line.split('=', 1))
        entries = [e.strip()[4:].strip().lower() for e in value.split(',') if e.strip().lower().startswith('key:')]
        if entries and entries[0] != 'default' and button[5:].upper() in keys:
            keys[button[5:].upper()] = entries[0]
    return keys


def key_picture(art, key, shade):
    stem = KEY_ART.get(key)
    if not stem:
        digit = key[3:] if key.startswith('num') and key[3:].isdigit() else key
        stem = digit.upper() if len(digit) == 1 or (digit[0] == 'f' and digit[1:].isdigit()) else None
    for name in ([f'T_{stem}_Key_{shade}.png'] + ([f'T_3_Key_{shade}-1.png'] if stem == '4' else [])) if stem else []:
        if (art / KM / shade / name).is_file():
            return f'{KM}{shade}/{name}'
    return ('label', key, shade)


def pc_pictures(art, keys):
    def k(button, shade='Dark'):
        return key_picture(art, keys[button], shade)

    def grid(*cells):
        return ('grid', tuple(cells))

    def mouse(axes):
        return f'{KM}Dark/T_Mouse_{axes}_Key_Dark.png'
    glyph = {b: k(b) for b in ('A', 'B', 'X', 'Y', 'L', 'R', 'ZL', 'ZR', 'PLUS', 'MINUS')}
    glyph.update({
        'LSTICK_PRESS': k('LCLICK'), 'RSTICK_PRESS': k('RCLICK'),
        'DPAD': grid((k('UP'), 1, 0), (k('LEFT'), 0, 1), (k('DOWN'), 1, 1), (k('RIGHT'), 2, 1)),
        'DPAD_UP': k('UP'), 'DPAD_DOWN': k('DOWN'), 'DPAD_LEFT': k('LEFT'), 'DPAD_RIGHT': k('RIGHT'),
        'DPAD_UPDOWN': grid((k('UP'), 0, 0), (k('DOWN'), 0, 1)), 'DPAD_LEFTRIGHT': grid((k('LEFT'), 0, 0), (k('RIGHT'), 1, 0)),
        'LSTICK': grid((k('LSTICK_UP'), 1, 0), (k('LSTICK_LEFT'), 0, 1), (k('LSTICK_DOWN'), 1, 1),
                       (k('LSTICK_RIGHT'), 2, 1)),
        'LSTICK_UP': k('LSTICK_UP'), 'LSTICK_DOWN': k('LSTICK_DOWN'), 'LSTICK_LEFT': k('LSTICK_LEFT'),
        'LSTICK_RIGHT': k('LSTICK_RIGHT'), 'LSTICK_UPDOWN': grid((k('LSTICK_UP'), 0, 0), (k('LSTICK_DOWN'), 0, 1)),
        'LSTICK_LEFTRIGHT': grid((k('LSTICK_LEFT'), 0, 0), (k('LSTICK_RIGHT'), 1, 0)),
        'RSTICK': mouse('XY'), 'RSTICK_ROTATE': mouse('XY'), 'RSTICK_UP': mouse('Y'), 'RSTICK_DOWN': mouse('Y'),
        'RSTICK_UPDOWN': mouse('Y'), 'RSTICK_LEFT': mouse('X'), 'RSTICK_RIGHT': mouse('X'), 'RSTICK_LEFTRIGHT': mouse('X'),
    })
    note = {b: k(b, 'White') for b in NOTES.values()}
    return glyph, note


class Romfs:
    def __init__(self, root):
        self.root = Path(root)
        index = self.root / 'index.json'
        self.files = None
        if index.is_file():
            data = json.loads(index.read_text())
            assert data.get('status') == 'pass', f'{index} is not a verified RomFS index'
            self.files = {f['path']: f for f in data['files']}
        self.dicts = None

    def raw(self, path):
        if self.files is None:
            return (self.root / path).read_bytes()
        f = self.files[path]
        with (self.root / 'romfs.bin').open('rb') as image:
            image.seek(f['offset'])
            data = image.read(f['size'])
        assert len(data) == f['size'], f'{path}: short read'
        return data

    def paths(self):
        if self.files is not None:
            return list(self.files)
        return [p.relative_to(self.root).as_posix() for p in self.root.rglob('*') if p.is_file()]

    def dictionary(self, dict_id):
        if self.dicts is None:
            self.dicts = {}
            pack = unpack(self.raw(DICTIONARIES))
            for lo, hi in sarc_members(pack).values():
                d = zstandard.ZstdCompressionDict(pack[lo:hi])
                self.dicts[d.dict_id()] = d
        return self.dicts[dict_id]

    def load(self, path):
        raw = self.raw(path)
        if not path.endswith('.zs'):
            return raw, None
        dict_id = zstandard.get_frame_parameters(raw).dict_id
        d = self.dictionary(dict_id) if dict_id else None
        return unpack(raw, d), d


PIECE = 1 << 16


def unpack(raw, dictionary=None):
    out, parts = zstandard.ZstdDecompressor(dict_data=dictionary).decompressobj(), []
    for at in range(0, len(raw), PIECE):
        parts.append(out.decompress(raw[at:at + PIECE]))
        if out.eof:
            break
    return b''.join(parts)


def sarc_members(data):
    assert data[:4] == b'SARC'
    header, start = struct.unpack_from('<H', data, 4)[0], struct.unpack_from('<I', data, 12)[0]
    count = struct.unpack_from('<H', data, header + 6)[0]
    names, out = header + 12 + count * 16 + 8, {}
    for i in range(count):
        _, attribute, lo, hi = struct.unpack_from('<4I', data, header + 12 + 16 * i)
        at = names + (attribute & 0xffffff) * 4
        out[data[at:data.index(0, at)].decode()] = (start + lo, start + hi)
    return out


def compress(contents, dictionary, level):
    return zstandard.ZstdCompressor(level=level, dict_data=dictionary, threads=-1).compress(contents)


def load_art(art, rel):
    if rel == OPTIONS:
        return options_art(art)
    if isinstance(rel, tuple) and rel[0] == 'grid':
        return grid_art(art, rel[1])
    if isinstance(rel, tuple) and rel[0] == 'label':
        return label_art(art, *rel[1:])
    path = art / rel
    if not path.is_file():
        sys.exit(f'{path} missing: --art is the art pack\'s Source folder')
    return Image.open(path).convert('RGBA')


def opaque(img):
    return img.crop(img.getchannel('A').point(lambda a: 255 if a > 8 else 0).getbbox())


def grid_art(art, cells):
    pics = [(opaque(load_art(art, rel)), c, r) for rel, c, r in cells]
    cw, ch, gap, pad = max(p.width for p, _, _ in pics), max(p.height for p, _, _ in pics), 6, 4
    cols, rows = max(c for _, c, _ in pics) + 1, max(r for _, _, r in pics) + 1
    canvas = Image.new('RGBA', (cols * cw + (cols - 1) * gap + 2 * pad, rows * ch + (rows - 1) * gap + 2 * pad))
    for p, c, r in pics:
        canvas.alpha_composite(p, (pad + c * (cw + gap) + (cw - p.width) // 2, pad + r * (ch + gap) + (ch - p.height) // 2))
    return canvas


def label_art(art, key, shade):
    blank = load_art(art, BLANKS[shade])
    name = KEY_LABELS.get(key, key.upper())
    d = ImageDraw.Draw(blank)
    x0, y0, x1, y1 = blank.getchannel('A').point(lambda a: 255 if a > 127 else 0).getbbox()
    size = 48
    while size > 10:
        font = ImageFont.load_default(size)
        l, t, r, b = d.textbbox((0, 0), name, font=font)
        if r - l <= (x1 - x0) * 0.7 and b - t <= (y1 - y0) * 0.5:
            break
        size -= 2
    d.text(((x0 + x1 - (r - l)) / 2 - l, (y0 + y1 - (b - t)) / 2 - t), name, font=font,
           fill=(255, 255, 255, 255) if shade == 'Dark' else (40, 40, 40, 255))
    return blank


def options_art(art):
    share = load_art(art, SHARE)
    x0, y0, x1, y1 = share.getchannel('A').point(lambda a: 255 if a > 127 else 0).getbbox()
    k = 4
    big = Image.new('RGBA', (share.width * k, share.height * k), (0, 0, 0, 0))
    d = ImageDraw.Draw(big)
    d.rounded_rectangle((x0 * k, y0 * k, x1 * k - 1, y1 * k - 1), radius=(y1 - y0) * k // 2, fill=(42, 42, 42, 255))
    cx, cy, half, bar = (x0 + x1) * k / 2, (y0 + y1) * k / 2, (y1 - y0) * k * 0.32, (y1 - y0) * k * 0.05
    for dy in (-0.22, 0, 0.22):
        y = cy + dy * (y1 - y0) * k
        d.rounded_rectangle((cx - half, y - bar, cx + half, y + bar), radius=bar, fill=(255, 255, 255, 255))
    return big.convert('RGBa').resize(share.size, Image.LANCZOS).convert('RGBA')


LIT_AREA, LIT_RING = 200, 6   # pixels of the art


def ink(img, lit=False):
    a, lum, near, parts = _parts(img)
    w, h = img.size
    field = [[a[y][x] * (1 - lum[y][x]) + (a[y][x] * lum[y][x] if near[y][x] else 0) for x in range(w)] for y in range(h)]
    for part in parts if lit else ():
        if len(part) < LIT_AREA:
            continue
        inside = set(part)
        dist = {p: 1 for p in part if any((p[0] + dx, p[1] + dy) not in inside
                                          for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)))}
        front = list(dist)
        while front:
            nxt = []
            for x, y in front:
                for q in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
                    if q in inside and q not in dist:
                        dist[q] = dist[x, y] + 1
                        nxt.append(q)
            front = nxt
        for x, y in part:
            field[y][x] = a[y][x] if dist[x, y] <= LIT_RING else 0.0
    return field


def symbol(img):
    a, lum, near, _ = _parts(img)
    out = Image.new('L', img.size)
    for y in range(img.height):
        for x in range(img.width):
            white = min(1.0, max(0.0, (lum[y][x] - 0.25) / 0.6))
            out.putpixel((x, y), 0 if near[y][x] else round(255 * a[y][x] * white))
    return out


def _parts(img):
    w, h = img.size
    px = img.load()
    a = [[px[x, y][3] / 255 for x in range(w)] for y in range(h)]
    lum = [[(0.299 * px[x, y][0] + 0.587 * px[x, y][1] + 0.114 * px[x, y][2]) / 255 for x in range(w)] for y in range(h)]
    white = [[a[y][x] > 0.5 and lum[y][x] > 0.5 for x in range(w)] for y in range(h)]
    bg = [[False] * w for _ in range(h)]
    stack = [(x, y) for x in range(w) for y in (0, h - 1)] + [(x, y) for y in range(h) for x in (0, w - 1)]
    while stack:
        x, y = stack.pop()
        if 0 <= x < w and 0 <= y < h and not bg[y][x] and a[y][x] <= 0.5:
            bg[y][x] = True
            stack += [(x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)]
    outside = [[False] * w for _ in range(h)]
    seen = [[False] * w for _ in range(h)]
    parts = []
    for sy in range(h):
        for sx in range(w):
            if not white[sy][sx] or seen[sy][sx]:
                continue
            part, stack, edge, touching = [], [(sx, sy)], 0, 0
            seen[sy][sx] = True
            while stack:
                x, y = stack.pop()
                part.append((x, y))
                if not all(0 <= nx < w and 0 <= ny < h and white[ny][nx]
                           for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1))):
                    edge += 1
                    touching += any(bg[yy][xx] for yy in range(max(0, y - 1), min(h, y + 2))
                                    for xx in range(max(0, x - 1), min(w, x + 2)))
                for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
                    if 0 <= nx < w and 0 <= ny < h and white[ny][nx] and not seen[ny][nx]:
                        seen[ny][nx] = True
                        stack.append((nx, ny))
            parts.append(part)
            if touching * 4 >= edge:
                for x, y in part:
                    outside[y][x] = True
    near = [[outside[y][x] or any(outside[yy][xx] for yy in range(max(0, y - 1), min(h, y + 2))
                                  for xx in range(max(0, x - 1), min(w, x + 2))) for x in range(w)] for y in range(h)]
    return a, lum, near, parts


def trace(field, level=0.5):
    h, w = len(field), len(field[0])

    def v(x, y):   # one pixel of zero around the field, so every outline closes
        return field[y - 1][x - 1] if 1 <= x <= w and 1 <= y <= h else 0.0

    def cross(p, q):
        (x0, y0), (x1, y1) = p, q
        a, b = v(x0, y0), v(x1, y1)
        t = (level - a) / (b - a)
        return (x0 + (x1 - x0) * t - 1, y0 + (y1 - y0) * t - 1)   # back to field coordinates

    starts = {}   # edge -> (start point, end edge, end point) of the segment leaving the cell through it
    for y in range(h + 1):
        for x in range(w + 1):
            corners = [(x, y), (x + 1, y), (x + 1, y + 1), (x, y + 1)]   # clockwise with y down
            inside = [v(*c) >= level for c in corners]
            if all(inside) or not any(inside):
                continue
            hits = []
            for i in range(4):
                p, q = corners[i], corners[(i + 1) % 4]
                if inside[i] != inside[(i + 1) % 4]:
                    hits.append((tuple(sorted((p, q))), cross(p, q), inside[(i + 1) % 4]))
            if len(hits) == 2:
                enter, leave = (hits[0], hits[1]) if hits[0][2] else (hits[1], hits[0])
                pairs = [(enter, leave)]
            else:   # a saddle: the middle decides whether the covered corners join
                order = hits if hits[0][2] else hits[1:] + hits[:1]   # enter, leave, enter, leave
                e1, l1, e2, l2 = order
                joined = (v(x, y) + v(x + 1, y) + v(x + 1, y + 1) + v(x, y + 1)) / 4 >= level
                pairs = [(e2, l1), (e1, l2)] if joined else [(e1, l1), (e2, l2)]
            for (sk, sp, _), (ek, ep, _) in pairs:
                starts[sk] = (sp, ek, ep)
    outlines = []
    while starts:
        key, (point, end, _) = next(iter(starts.items()))
        loop = []
        while key in starts:
            point, end, _ = starts.pop(key)
            loop.append(point)
            key = end
        if len(loop) >= 3:
            outlines.append(loop)
    return outlines


def simplify(loop, tolerance):
    def dp(points):
        if len(points) < 3:
            return points
        (ax, ay), (bx, by) = points[0], points[-1]
        dx, dy = bx - ax, by - ay
        norm = (dx * dx + dy * dy) ** 0.5
        far, at = -1.0, 0
        for i in range(1, len(points) - 1):
            px, py = points[i]
            d = abs(dy * (px - ax) - dx * (py - ay)) / norm if norm else ((px - ax) ** 2 + (py - ay) ** 2) ** 0.5
            if d > far:
                far, at = d, i
        if far <= tolerance:
            return [points[0], points[-1]]
        return dp(points[:at + 1])[:-1] + dp(points[at:])
    x0, y0 = loop[0]
    k = max(range(len(loop)), key=lambda i: (loop[i][0] - x0) ** 2 + (loop[i][1] - y0) ** 2)
    out = dp(loop[:k + 1])[:-1] + dp(loop[k:] + loop[:1])[:-1]
    return out if len(out) >= 3 else loop


def area(loop):
    return sum(x0 * y1 - x1 * y0 for (x0, y0), (x1, y1) in zip(loop, loop[1:] + loop[:1])) / 2


def outlines_of(img, lit=False):
    return [s for s in (simplify(o, 0.3) for o in trace(ink(img, lit))) if abs(area(s)) >= 1.5]


def bounds(outlines):
    xs = [x for o in outlines for x, _ in o]
    ys = [y for o in outlines for _, y in o]
    return min(xs), min(ys), max(xs), max(ys)


def draw_font(otf, glyph, traced, lit_dpad):
    font = TTFont(io.BytesIO(otf))
    glyphs, cmap, hmtx = font.getGlyphSet(), font.getBestCmap(), font['hmtx']
    box = BoundsPen(glyphs)
    glyphs[cmap[0xE040]].draw(box)
    gx0, gy0, gx1, gy1 = box.bounds   # the game's A: a round button
    rx0, ry0, rx1, ry1 = bounds(traced(glyph['A']))
    scale = (gy1 - gy0) / (ry1 - ry0)
    mid_y_px, mid_y = (ry0 + ry1) / 2, (gy0 + gy1) / 2
    side = gx0   # the game's A has this much room on either side
    drawn = {}
    for cp, button in GLYPHS.items():
        pic = glyph[button]
        outlines = traced(pic, lit_dpad and button.startswith('DPAD_'))
        x0, y0, x1, y1 = bounds(outlines)
        s, cy = scale, mid_y_px
        if isinstance(pic, tuple) and pic[0] == 'grid':
            s, cy = min(scale, (gy1 - gy0) / (y1 - y0)), (y0 + y1) / 2
        advance = max(hmtx[cmap[cp]][0], round((x1 - x0) * s + 2 * side))
        ox = advance / 2 - (x0 + x1) / 2 * s

        def draw(pen, outlines=outlines, ox=ox, s=s, cy=cy):
            for o in outlines:
                pen.moveTo((ox + o[0][0] * s, mid_y - (o[0][1] - cy) * s))
                for x, y in o[1:]:
                    pen.lineTo((ox + x * s, mid_y - (y - cy) * s))
                pen.closePath()
        drawn[cp] = (advance, draw)
    return button_icons.redraw_font(otf, drawn)


def bntx_textures(data):
    assert data[:4] == b'BNTX'
    count, table = struct.unpack_from('<IQ', data, 0x24)
    out = {}
    for k in range(count):
        info = struct.unpack_from('<Q', data, table + 8 * k)[0]
        assert data[info:info + 4] == b'BRTI'
        at = struct.unpack_from('<Q', data, info + 0x60)[0]
        name = data[at + 2:at + 2 + struct.unpack_from('<H', data, at)[0]].decode()
        mips = struct.unpack_from('<H', data, info + 0x16)[0]
        fmt, (w, h), th = struct.unpack_from('<I', data, info + 0x1c)[0], struct.unpack_from('<II', data, info + 0x24), \
            struct.unpack_from('<I', data, info + 0x34)[0]
        start = struct.unpack_from('<Q', data, struct.unpack_from('<Q', data, info + 0x70)[0])[0]
        out[name] = dict(fmt=fmt, w=w, h=h, th=th, mips=mips, start=start, size=struct.unpack_from('<I', data, info + 0x50)[0])
    return out


def level_tiles(th, rows):
    x = 8 << (th - 1) if th else 0
    while th and x >= rows:
        th -= 1
        x >>= 1
    return th


def block_linear(row_bytes, rows, th):
    th = level_tiles(th, rows)
    unit, block, gobs = 8 << th, 512 << th, (row_bytes + 63) // 64
    return lambda x, y: ((y // unit) * gobs * block + (x // 64) * block + ((y % unit) // 8) * 512 + (x % 64 // 32) * 256
                         + (y % 8 // 4) * 128 + (x % 32 // 16) * 64 + (y % 4) * 16)


def read_level(data, t, row_bytes, rows):
    at = block_linear(row_bytes, rows, t['th'])
    out = bytearray(row_bytes * rows)
    for y in range(rows):
        for x in range(0, row_bytes, 16):
            o = t['start'] + at(x, y)
            out[y * row_bytes + x:y * row_bytes + x + 16] = data[o:o + 16]
    return out


def write_level(data, t, lin, row_bytes, rows):
    at = block_linear(row_bytes, rows, t['th'])
    for y in range(rows):
        for x in range(0, row_bytes, 16):
            o = at(x, y)
            assert o + 16 <= t['size'], 'past the end of the texture'
            data[t['start'] + o:t['start'] + o + 16] = lin[y * row_bytes + x:y * row_bytes + x + 16]


BC4_UNORM, ASTC_4X4_SRGB = 0x1d01, 0x2d06


def bc4_decode(lin, w, h):
    out = Image.new('L', (w, h))
    wb = (w + 3) // 4
    for by in range((h + 3) // 4):
        for bx in range(wb):
            b = lin[(by * wb + bx) * 8:(by * wb + bx) * 8 + 8]
            pal = bc4_palette(b[0], b[1])
            bits = int.from_bytes(b[2:8], 'little')
            for i in range(16):
                x, y = bx * 4 + i % 4, by * 4 + i // 4
                if x < w and y < h:
                    out.putpixel((x, y), pal[bits >> (3 * i) & 7])
    return out


def bc4_palette(a, b):
    if a > b:
        return [a, b] + [((6 - i) * a + (i + 1) * b) // 7 for i in range(6)]
    return [a, b] + [((4 - i) * a + (i + 1) * b) // 5 for i in range(4)] + [0, 255]


def bc4_encode(img):
    w, h = img.size
    px = img.load()
    out = bytearray()
    for by in range((h + 3) // 4):
        for bx in range((w + 3) // 4):
            vals = [px[min(w - 1, bx * 4 + i % 4), min(h - 1, by * 4 + i // 4)] for i in range(16)]
            hi, lo = max(vals), min(vals)
            if hi == lo:
                out += bytes([hi, lo]) + bytes(6)
                continue
            pal = bc4_palette(hi, lo)
            bits = 0
            for i, v in enumerate(vals):
                bits |= min(range(8), key=lambda k: abs(pal[k] - v)) << (3 * i)
            out += bytes([hi, lo]) + bits.to_bytes(6, 'little')
    return bytes(out)


ASTC_WEIGHTS = (0, 21, 43, 64)


def astc_block(texels):
    n = len(texels)
    mean = [sum(t[c] for t in texels) / n for c in range(4)]
    cov = [[sum((t[i] - mean[i]) * (t[j] - mean[j]) for t in texels) for j in range(4)] for i in range(4)]
    axis = [1.0, 1.0, 1.0, 1.0]
    for _ in range(12):
        nxt = [sum(cov[i][j] * axis[j] for j in range(4)) for i in range(4)]
        norm = sum(v * v for v in nxt) ** 0.5
        if norm < 1e-9:
            break
        axis = [v / norm for v in nxt]
    proj = [sum((t[c] - mean[c]) * axis[c] for c in range(4)) for t in texels]
    lo, hi = min(proj), max(proj)
    e0 = [max(0, min(255, round(mean[c] + lo * axis[c]))) for c in range(4)]
    e1 = [max(0, min(255, round(mean[c] + hi * axis[c]))) for c in range(4)]
    if sum(e1[:3]) < sum(e0[:3]):   # the decoder would read the ends the other way (blue contraction)
        e0, e1 = e1, e0
    span = [e1[c] - e0[c] for c in range(4)]
    length = sum(v * v for v in span)
    weights = []
    for t in texels:
        f = sum((t[c] - e0[c]) * span[c] for c in range(4)) / length if length else 0.0
        weights.append(min(range(4), key=lambda k: abs(ASTC_WEIGHTS[k] / 64 - f)))
    bits = 0x042 | (12 << 13)
    for i, v in enumerate((e0[0], e1[0], e0[1], e1[1], e0[2], e1[2], e0[3], e1[3])):
        bits |= v << (17 + 8 * i)
    for i, wt in enumerate(weights):
        for b in range(2):
            if wt >> b & 1:
                bits |= 1 << (127 - (2 * i + b))
    return bits.to_bytes(16, 'little')


def astc_encode(img):
    w, h = img.size
    px = img.load()
    out = bytearray()
    for by in range((h + 3) // 4):
        for bx in range((w + 3) // 4):
            out += astc_block([px[min(w - 1, bx * 4 + i % 4), min(h - 1, by * 4 + i // 4)] for i in range(16)])
    return bytes(out)


def fit(img, box, size):
    crop = img.crop(img.getchannel('A').point(lambda a: 255 if a > 8 else 0).getbbox())
    bw, bh = box[2] - box[0], box[3] - box[1]
    k = min(bw / crop.width, bh / crop.height)
    scaled = crop.convert('RGBa').resize((max(1, round(crop.width * k)), max(1, round(crop.height * k))), Image.LANCZOS)
    canvas = Image.new('RGBa', size, (0, 0, 0, 0))
    canvas.paste(scaled, (round(box[0] + (bw - scaled.width) / 2), round(box[1] + (bh - scaled.height) / 2)))
    return canvas.convert('RGBA')


def bleed(img):
    w, h = img.size
    px = img.load()
    known = {(x, y) for y in range(h) for x in range(w) if px[x, y][3] > 8}
    for _ in range(6):
        grown = {}
        for y in range(h):
            for x in range(w):
                if (x, y) in known:
                    continue
                near = [px[x + dx, y + dy] for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1))
                        if 0 <= x + dx < w and 0 <= y + dy < h and (x + dx, y + dy) in known]
                if near:
                    grown[x, y] = tuple(sum(p[c] for p in near) // len(near) for c in range(3)) + (px[x, y][3],)
        for xy, p in grown.items():
            px[xy] = p
        known |= set(grown)
    return img


def mask_of(img, rel):
    if '/T_Mouse_' not in str(rel):
        return symbol(img)
    field = ink(img)
    out = Image.new('L', img.size)
    out.putdata([round(255 * min(1.0, v)) for row in field for v in row])
    return out


def note_texture(art, rel, box):
    pic = fit(load_art(art, rel), box, (93, 93))
    return astc_encode(bleed(pic))


def draw_textures(data, glyph, note, art, cache):
    textures = bntx_textures(data)
    for name, button in {**MASKS, **NOTES}.items():
        assert name in textures, f'no {name} in {TEXTURES}'
        t = textures[name]
        assert t['mips'] == 1, f'{name}: {t["mips"]} levels'
        wb, hb = (t['w'] + 3) // 4, (t['h'] + 3) // 4
        if name in MASKS:
            assert t['fmt'] == BC4_UNORM, f'{name}: format {t["fmt"]:#x}'
            old = bc4_decode(read_level(data, t, wb * 8, hb), t['w'], t['h'])
            box = old.point(lambda v: 255 if v > 64 else 0).getbbox()
            cx, cy, half = (box[0] + box[2]) / 2, (box[1] + box[3]) / 2, (box[3] - box[1]) / 2
            rel = glyph[button]
            sym = cache(('mask', rel), lambda: mask_of(load_art(art, rel), rel))
            rgba = Image.merge('RGBA', (sym.point(lambda v: 255),) * 3 + (sym,))
            mask = fit(rgba, (cx - half * 1.15, cy - half, cx + half * 1.15, cy + half), (t['w'], t['h']))
            write_level(data, t, bc4_encode(mask.getchannel('A')), wb * 8, hb)
        else:
            assert t['fmt'] == ASTC_4X4_SRGB and (t['w'], t['h']) == (93, 93), f'{name}: format {t["fmt"]:#x}, ' \
                f'{t["w"]}x{t["h"]}'
            box = NOTE_BOXES['shoulder' if button in 'LR' else 'disc']
            rel = note[button]
            encoded = cache(('note', rel, box), lambda: note_texture(art, rel, box))
            write_level(data, t, encoded, wb * 16, hb)


NOTE_BOXES = {'disc': (13, 13, 80, 80), 'shoulder': (8, 15, 84, 76)}


def write_overlay(out, outputs, originals):
    lines = []
    for path, data in outputs.items():
        target = out / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        lines.append('\t'.join([path, str(len(data)), hashlib.sha256(data).hexdigest(),
                                hashlib.sha256(originals[path]).hexdigest()]))
    (out / 'manifest.tsv').write_text('# path\tsize\tsha256\toriginal_sha256 (tools/pad_prompts.py)\n'
                                      + ''.join(line + '\n' for line in lines))



def default_binds(game):
    if not game:
        return None
    current, legacy = Path(game) / 'nx2saria.ini', Path(game) / 'nx2navi.ini'
    return current if current.is_file() else legacy if legacy.is_file() else None

def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--art', type=Path, required=True,
                    help='the art pack\'s Source folder (XGamepad, P5Gamepad, Keyboard_Mouse)')
    ap.add_argument('--game', type=Path, help='the folder of the exe and romfs-index.bin; the overlays go there')
    ap.add_argument('--romfs', type=Path, help='default <game>/romfs')
    ap.add_argument('--style', choices=sorted(STYLES) + ['pc'], help='default: every one')
    ap.add_argument('--layout', choices=sorted(FACE), help='default: both (pc has one)')
    ap.add_argument('--binds', type=Path, help='pc: a settings file whose bind_ lines the keys follow '
                                               '(default <game>/nx2saria.ini, else an existing nx2navi.ini)')
    ap.add_argument('--out', type=Path, help='the overlay folder, for one --style and --layout')
    ap.add_argument('--level', type=int, default=9, help='zstd level of the written files')
    args = ap.parse_args()
    if not args.game and not (args.out and args.romfs):
        ap.error('--game, or --out with --romfs')
    if args.out and not (args.style and (args.layout or args.style == 'pc')):
        ap.error('--out writes one folder: give --style and --layout')
    binds = args.binds or default_binds(args.game)
    keys = pc_keys(binds.read_text(encoding='utf-8', errors='replace') if binds else None)
    romfs = Romfs(args.romfs or args.game / 'romfs')
    restbl = max((p for p in romfs.paths() if p.startswith('System/Resource/ResourceSizeTable.') and p.endswith('.zs')),
                 key=lambda p: int(p.split('.')[2]) if p.split('.')[2].isdigit() else 0, default=None)
    assert restbl, 'no resource size table in the RomFS'
    originals = {p: romfs.raw(p) for p in (FONT, TEXTURES, restbl)}
    font_blob, font_dict = romfs.load(FONT)
    otf, key = button_icons.bfotf_decode(font_blob)
    table, table_dict = romfs.load(restbl)
    textures, textures_dict = romfs.load(TEXTURES)
    memo = {}

    def cache(k, make):
        if k not in memo:
            memo[k] = make()
        return memo[k]

    def traced(rel, lit=False):
        return cache(('outlines', rel, lit), lambda: outlines_of(load_art(args.art, rel), lit))

    report = []
    for style in [args.style] if args.style else sorted(STYLES) + ['pc']:
        for layout in [None] if style == 'pc' else [args.layout] if args.layout else sorted(FACE):
            out = args.out or args.game / overlay_folder(style, layout)
            glyph, note = pc_pictures(args.art, keys) if style == 'pc' else pad_pictures(style, layout)
            new_otf = draw_font(otf, glyph, traced, style != 'pc')
            blob = button_icons.bfotf_encode(new_otf, key, font_blob)
            data = bytearray(textures)
            draw_textures(data, glyph, note, args.art, cache)
            assert len(data) == len(textures)
            outputs = {FONT: compress(blob, font_dict, 19), TEXTURES: compress(bytes(data), textures_dict, args.level)}
            if len(blob) > len(font_blob):
                outputs[restbl] = compress(button_icons.grow_restbl(table, {FONT: len(blob) - len(font_blob)}),
                                           table_dict, 19)
            write_overlay(out, outputs, originals)
            report.append(dict(folder=str(out), style=style, layout=layout, font_bytes=len(blob),
                               font_grew=len(blob) - len(font_blob), files=sorted(outputs)))
            print(f'{out}: {len(outputs)} files, the font grew by {len(blob) - len(font_blob)} bytes', flush=True)
    print(json.dumps(dict(status='pass', folders=report)))


if __name__ == '__main__':
    main()
