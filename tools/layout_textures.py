import ctypes
import hashlib
import importlib
import json
import math
import os
import re
import shutil
import subprocess
import sys
import tempfile
from dataclasses import replace
from pathlib import Path
from types import ModuleType

import numpy as np

import layout_dump as ld

try:
    from PIL import Image, ImageDraw
except ImportError:
    Image = ImageDraw = None

CACHE = ld.ROOT / 'build' / 'wide-menus' / 'textures'
ARCHIVE_DIR = 'UI/Layout2Archive/'


def decoder():
    try:
        return importlib.import_module('ui_textures')
    except ModuleNotFoundError as e:
        if e.name != 'compression':
            raise
    compression = ModuleType('compression')
    compression.zstd = None
    sys.modules['compression'] = compression
    try:
        return importlib.import_module('ui_textures')
    finally:
        del sys.modules['compression']


def astc_library(cache):
    src = ld.ROOT / 'runtime' / 'gfx' / 'tex_astc.c'
    library = cache / ('astc.dll' if os.name == 'nt' else 'astc.so')
    if not library.is_file() or library.stat().st_mtime < src.stat().st_mtime:
        cc = shutil.which('clang') or shutil.which('cc')
        if not cc:
            raise ld.FormatError('decoding ASTC textures needs clang or a C compiler')
        import build_game
        includes = [*build_game.runtime_include_dirs(ld.ROOT / 'runtime'), ld.ROOT / 'toolchain' / 'vma',
                    Path(cc).resolve().parents[1] / 'include' / 'vma']
        if path := os.environ.get('NX2SARIA_VMA_INCLUDE'):
            includes.insert(0, Path(path))
        with tempfile.TemporaryDirectory(prefix='astc-', dir=cache) as work:
            output = Path(work) / library.name
            args = [cc, '-shared', '-O2']
            if os.name != 'nt':
                args.append('-fPIC')
            for include in includes:
                args.extend(('-I', str(include)))
            result = subprocess.run([*args, str(src), '-o', str(output)], capture_output=True, text=True)
            if result.returncode:
                raise ld.FormatError('could not build the standalone ASTC decoder (set NX2SARIA_VMA_INCLUDE '
                                     'to the directory containing vk_mem_alloc.h):\n' + result.stderr.strip())
            output.replace(library)
    lib = ctypes.CDLL(str(library))
    lib.astc_decode_ldr.restype = None
    lib.astc_decode_ldr.argtypes = [ctypes.c_char_p, *([ctypes.c_uint32] * 4), ctypes.c_bool, ctypes.c_void_p]
    return lib


class TextureStore:
    def __init__(self, romfs):
        self.romfs = romfs
        self.index = json.loads((romfs / 'index.json').read_text())
        self.paths = {}
        for entry in self.index['files']:
            path = entry['path']
            if path.startswith(ARCHIVE_DIR) and path.endswith('.bntx.zs'):
                self.paths.setdefault(Path(path).name.split('.')[0], path)
        if not self.paths:
            raise ld.FormatError('no UI texture archives in the RomFS index')
        self.ui = decoder()
        self.archives = {}
        self.images = {}
        self.library = None
        CACHE.mkdir(parents=True, exist_ok=True)

    def archive(self, label):
        if label not in self.archives:
            data = ld.romfs_file(self.romfs, self.index, self.paths[label])
            digest = hashlib.sha256(data).hexdigest()[:16]
            folder = CACHE / f'{label}-{digest}'
            with tempfile.TemporaryDirectory(prefix='layout-bntx-', dir=CACHE) as work:
                dictionaries = ld.romfs_file(self.romfs, self.index, ld.DICTIONARIES) if ld.frame_dict_id(data) else None
                raw = ld.unpack_zs(data, work, dictionaries)
            if raw[:4] != b'BNTX':
                raise ld.FormatError(f'{self.paths[label]} did not decompress to BNTX')
            self.archives[label] = (folder, raw, {t['name']: t for t in self.ui.bntx_textures(raw)})
        return self.archives[label]

    def get(self, archive, name):
        if not name or name.startswith('_CapturePane_'):
            return None
        key = archive, name
        if key not in self.images:
            self.images[key] = self.decode(archive, name)
        return self.images[key]

    def decode(self, archive, name):
        for label in dict.fromkeys((archive, 'Common')):
            if label not in self.paths:
                continue
            folder, raw, textures = self.archive(label)
            info = textures.get(name)
            if info is None:
                continue
            cache_name = hashlib.sha256(name.encode()).hexdigest()[:16] + '.npy'
            path = folder / cache_name
            if path.is_file():
                return np.load(path, allow_pickle=False)
            if info['fmt'] >> 8 == 0x2d and self.library is None:
                self.library = astc_library(CACHE)
            rgba = self.ui.decode(info, raw, self.library)
            if rgba is None:
                raise ld.FormatError(f'{name}: unsupported BNTX format {info["fmt"]:#x}')
            folder.mkdir(exist_ok=True)
            with tempfile.TemporaryDirectory(prefix='texture-', dir=folder) as work:
                temporary = Path(work) / cache_name
                np.save(temporary, rgba, allow_pickle=False)
                temporary.replace(path)
            return rgba
        return None


def png(path, rgba):
    if Image is not None:
        Image.fromarray(rgba).save(path)
    else:
        decoder().png(path, rgba)


def save_panels(path, panels, gap):
    if len(panels) == 1:
        png(path, panels[0])
        return
    divider = np.full((panels[0].shape[0], gap, 4), (24, 24, 24, 255), dtype=np.uint8)
    png(path, np.concatenate((panels[0], divider, panels[1]), axis=1))


def uv_of(binding, u, v):
    sx, sy = binding.scale
    tx, ty = binding.translate
    c, s = math.cos(binding.rotation), math.sin(binding.rotation)
    x, y = (u - 0.5) * sx, (v - 0.5) * sy
    return c * x - s * y + 0.5 + tx, s * x + c * y + 0.5 + ty


def texel_indices(i, length, wrap):
    if wrap == 0:
        return i % length
    if wrap == 1:
        folded = i % (2 * length)
        return np.minimum(folded, 2 * length - 1 - folded)
    return np.clip(i, 0, length - 1)


def sample(texture, binding, u, v):
    h, w = texture.shape[:2]
    u, v = uv_of(binding, u, v)
    x, y = u * w - 0.5, v * h - 0.5
    i, j = np.floor(x).astype(np.intp), np.floor(y).astype(np.intp)
    fx, fy = (x - i).astype(np.float32)[..., None], (y - j).astype(np.float32)[..., None]
    x0, x1 = (texel_indices(n, w, binding.wrap[0]) for n in (i, i + 1))
    y0, y1 = (texel_indices(n, h, binding.wrap[1]) for n in (j, j + 1))
    top = texture[y0, x0] * (1 - fx) + texture[y0, x1] * fx
    bottom = texture[y1, x0] * (1 - fx) + texture[y1, x1] * fx
    return top * (1 - fy) + bottom * fy


def intersect(a, b):
    left, top, right, bottom = max(a[0], b[0]), max(a[1], b[1]), min(a[2], b[2]), min(a[3], b[3])
    return (left, top, right, bottom) if left < right and top < bottom else None


def bounds(quad):
    corners = quad @ np.array([[0, 1, 0, 1], [0, 0, 1, 1], [1, 1, 1, 1]])
    return (math.floor(corners[0].min()), math.floor(corners[1].min()),
            math.ceil(corners[0].max()), math.ceil(corners[1].max()))


def blend(dest, source):
    if Image is not None:
        dest[:] = np.asarray(Image.alpha_composite(Image.fromarray(dest), Image.fromarray(source)))
        return
    src = source.astype(np.float32)
    sa = src[..., 3:4] / 255
    da = dest[..., 3:4] / 255 * (1 - sa)
    total = sa + da
    colour = src[..., :3] * sa + dest[..., :3] * da
    dest[..., :3] = np.rint(colour / np.where(total > 0, total, 1)).astype(np.uint8)
    dest[..., 3:4] = np.rint(total * 255).astype(np.uint8)


def paint(canvas, quad, region, tint, texture, binding, alpha):
    l, t, r, b = region
    inverse = np.linalg.inv(quad)
    x = np.arange(l, r, dtype=np.float32)[None, :] + 0.5
    y = np.arange(t, b, dtype=np.float32)[:, None] + 0.5
    u, v = (row[0] * x + row[1] * y + row[2] for row in inverse[:2])
    inside = (0 <= u) & (u < 1) & (0 <= v) & (v < 1)
    if texture is None:
        pixels = np.full((b - t, r - l, 4), 255, dtype=np.float32)
    else:
        pixels = sample(texture, binding, u, v)
    colour = np.clip(tint, 0, 1).astype(np.float32)
    colour[:3] = np.where(colour[:3] < 0.0031308, colour[:3] * 12.92, 1.055 * colour[:3] ** (1 / 2.4) - 0.055)
    pixels *= colour
    pixels[..., 3] *= inside * alpha
    blend(canvas[t:b, l:r], np.rint(pixels).astype(np.uint8))


class Renderer:
    def __init__(self, visible, scale, textures, colours):
        self.width, self.height = round(visible * scale), round(2160 * scale)
        if self.width < 1 or self.height < 1:
            raise ld.SpecError('the output scale is too small for this picture')
        self.clip = (0, 0, self.width, self.height)
        self.grid = np.array([[self.width / visible, 0, self.width / 2],
                              [0, -self.height / 2160, self.height / 2], [0, 0, 1]])
        self.image = np.zeros((self.height, self.width, 4), dtype=np.uint8)
        self.textures = textures
        self.colours = colours
        self.annotations = []

    def draw(self, layout, layouts, state_for, stretch, depth, label_depth, boxes=False, marks=frozenset(), hud=False):
        self.draw_part(layout, layouts, state_for, self.grid @ np.diag([stretch, 1, 1]), self.clip, 1.0, depth,
                       label_depth, boxes, marks, hud, layout.name)

    def draw_part(self, layout, layouts, state_for, transform, clip, opacity, depth, label_depth, boxes, marks, hud,
                  instance, initial=()):
        state = state_for(layout, instance, initial)
        material_state = state_for(layout, instance, initial, material=True)
        pictures = layout.pictures()

        def visit(pane, matrix, parent, parent_size, region, alpha):
            pstate = state.get(pane.index, {})
            visible = pstate.get((ld.VISIBILITY, 0), pane.flags & 1) >= 0.5
            alpha *= max(0, min(1, pstate.get((ld.ALPHA, 0), layout.pane_alpha(pane))))
            if not visible or alpha == 0:
                return
            x, y, sx, sy, w, h, angle = ld.pane_geometry(pane, pstate)
            if parent is not None:
                ax, ay = ld.anchor_offset(pane, parent, *parent_size)
                x, y = x + ax, y + ay
            c, s = math.cos(angle), math.sin(angle)
            matrix = matrix @ np.array([[c * sx, -s * sy, x], [s * sx, c * sy, y], [0, 0, 1]])
            if w and h and sx and sy:
                oh, ov = pane.origin
                fx, fy = ld.H_CODES.get(oh, ('', .5))[1], ld.V_CODES.get(ov, ('', .5))[1]
                quad = matrix @ np.array([[w, 0, -w * fx], [0, -h, h * (1 - fy)], [0, 0, 1]])
                extent = intersect(bounds(quad), region)
                if pane.kind == 4:
                    if extent is None:
                        return
                    region = extent
                if extent is not None:
                    marked = boxes and (layout.name, pane.name) in marks
                    if boxes or hud or pane.kind == 2:
                        colour = '#e67e22' if hud else '#e0004d' if marked else self.colours.get(pane.kind_name, '#888888')
                        label = pane.name if boxes and (marked or (label_depth >= 0 and pane.kind != 0)) else ''
                        opacity = 170 if boxes or hud else round(50 * alpha)
                        self.annotations.append((extent, colour, opacity, label))
                    if pane.kind == 1 and not hud and pane.name in pictures:
                        self.draw_picture(layout, pictures[pane.name], quad, extent, alpha, pstate,
                                          material_state.get(pane.index, {}))
            elif sx == 0 or sy == 0 or pane.kind == 4:
                return
            if pane.kind == 6 and depth:
                target = layouts.get(layout.parts.get(pane.name))
                if target is not None:
                    self.draw_part(target, layouts, state_for, matrix, region, alpha, depth - 1, label_depth - 1,
                                   boxes, marks, hud, f'{instance}/{pane.name}', layout.part_animations(pane.name))
            for index in pane.children:
                visit(layout.panes[index], matrix, pane, (w, h), region, alpha)

        for pane in layout.panes:
            if pane.parent < 0:
                visit(pane, transform, None, (0, 0), clip, opacity)

    def draw_picture(self, layout, picture, quad, region, alpha, state, maps):
        material = picture.material
        if picture.window and len(material.textures) > 1:
            return
        colour = tuple(state.get((ld.COLOUR, 4 * i), value) for i, value in enumerate(material.colour))
        if colour[3] == 0:
            return
        for i, binding in enumerate(material.textures):
            if maps:
                index = round(maps.get((i, ld.TEXTURE_PATTERN, 0), -1))
                if 0 <= index < len(binding.alternatives):
                    binding = replace(binding, name=binding.alternatives[index])
                scale = tuple(maps.get((i, ld.UV_SCALE, 4 * j), value) for j, value in enumerate(binding.scale))
                translate = tuple(maps.get((i, ld.UV_TRANSLATE, 4 * j), value) for j, value in enumerate(binding.translate))
                binding = replace(binding, scale=scale, translate=translate)
            texture = self.textures.get(layout.archive, binding.name)
            if texture is not None:
                paint(self.image, quad, region, colour, texture, binding, alpha)
                return
        if not material.textures:
            paint(self.image, quad, region, colour, None, None, alpha)

    def finish(self):
        if not self.annotations:
            return self.image
        overlay = np.zeros_like(self.image)
        for (l, t, r, b), colour, opacity, _ in self.annotations:
            rgb = bytes.fromhex(colour[1:]) if len(colour) == 7 else bytes.fromhex(''.join(c * 2 for c in colour[1:]))
            rgba = (*rgb, opacity)
            overlay[t, l:r] = overlay[b - 1, l:r] = rgba
            overlay[t:b, l] = overlay[t:b, r - 1] = rgba
        if Image is not None and any(a[3] for a in self.annotations):
            labels = Image.fromarray(overlay).copy()
            draw = ImageDraw.Draw(labels)
            for (l, t, _, _), _, opacity, label in self.annotations:
                if label:
                    draw.text((l + 2, t + 1), label, fill=(255, 255, 255, opacity), stroke_width=1,
                              stroke_fill=(0, 0, 0, opacity))
            overlay = np.asarray(labels)
        blend(self.image, overlay)
        return self.image


def dump_texture(layout, pane, store, out):
    picture = layout.pictures().get(pane)
    if picture is None:
        raise ld.SpecError(f'{layout.name} has no picture pane {pane}')
    for binding in picture.material.textures:
        texture = store.get(layout.archive, binding.name)
        if texture is not None:
            if not re.fullmatch(r'[\w.-]+', pane):
                raise ld.SpecError(f'cannot use {pane!r} as a texture dump filename')
            path = out / f'{pane}.png'
            png(path, texture)
            return path, binding.name
    raise ld.SpecError(f'{layout.name}/{pane} has no static texture')
