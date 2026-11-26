import ctypes
import os
import tempfile
from pathlib import Path

import numpy as np

KEEP_W, KEEP_H = 1600, 928
DIMS = (0, 20, 40, 60, 80)
MAX_SIDE, MAX_PIXELS = 16384, 120_000_000
try:
    from PIL import Image, ImageOps
except ImportError:
    Image = None
TYPES = ('.png', '.jpg', '.jpeg', '.webp', '.bmp', '.gif', '.tif', '.tiff') if Image else     ('.png', '.jpg', '.jpeg', '.bmp', '.gif', '.tif', '.tiff') if os.name == 'nt' else ('.png', '.gif')


class PictureError(Exception):
    pass
def _too_large(path, w, h):
    return PictureError(f'{Path(path).name} is {w} x {h} pixels; the background takes up to {MAX_SIDE} pixels a side '
                        f'and {MAX_PIXELS // 1_000_000} megapixels.')


def _load_pillow(path):
    try:
        with Image.open(path) as im:
            w, h = im.size
            if not w or not h or w > MAX_SIDE or h > MAX_SIDE or w * h > MAX_PIXELS:
                raise _too_large(path, w, h)
            return np.asarray(ImageOps.exif_transpose(im).convert('RGB')).copy()
    except PictureError:
        raise
    except Exception as e:
        raise PictureError(f'{Path(path).name} is not a picture the launcher can read: {e}') from None


def _load_gdiplus(path):
    from ctypes import wintypes

    class StartupInput(ctypes.Structure):
        _fields_ = [('GdiplusVersion', ctypes.c_uint32), ('DebugEventCallback', ctypes.c_void_p),
                    ('SuppressBackgroundThread', wintypes.BOOL), ('SuppressExternalCodecs', wintypes.BOOL)]

    class BitmapData(ctypes.Structure):
        _fields_ = [('Width', ctypes.c_uint), ('Height', ctypes.c_uint), ('Stride', ctypes.c_int),
                    ('PixelFormat', ctypes.c_int), ('Scan0', ctypes.c_void_p), ('Reserved', ctypes.c_void_p)]

    g = ctypes.WinDLL('gdiplus')
    vp, pvp = ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)
    g.GdiplusStartup.argtypes = [pvp, ctypes.POINTER(StartupInput), vp]
    g.GdiplusShutdown.argtypes = [vp]
    g.GdipCreateBitmapFromFile.argtypes = [ctypes.c_wchar_p, pvp]
    g.GdipGetImageWidth.argtypes = g.GdipGetImageHeight.argtypes = [vp, ctypes.POINTER(ctypes.c_uint)]
    g.GdipBitmapLockBits.argtypes = [vp, ctypes.POINTER(ctypes.c_int * 4), ctypes.c_uint, ctypes.c_int,
                                     ctypes.POINTER(BitmapData)]
    g.GdipBitmapUnlockBits.argtypes = [vp, ctypes.POINTER(BitmapData)]
    g.GdipDisposeImage.argtypes = [vp]
    token = vp()
    if g.GdiplusStartup(ctypes.byref(token), ctypes.byref(StartupInput(1, None, 0, 0)), None):
        raise PictureError('Windows could not start its picture reader (GDI+).')
    bmp = vp()
    try:
        if g.GdipCreateBitmapFromFile(str(path), ctypes.byref(bmp)) or not bmp:
            raise PictureError(f'{Path(path).name} is not a picture Windows can read (PNG, JPEG, BMP, GIF or TIFF).')
        w, h = ctypes.c_uint(), ctypes.c_uint()
        g.GdipGetImageWidth(bmp, ctypes.byref(w))
        g.GdipGetImageHeight(bmp, ctypes.byref(h))
        w, h = w.value, h.value
        if not w or not h or w > MAX_SIDE or h > MAX_SIDE or w * h > MAX_PIXELS:
            raise _too_large(path, w, h)
        data = BitmapData()
        rect = (ctypes.c_int * 4)(0, 0, w, h)
        if g.GdipBitmapLockBits(bmp, ctypes.byref(rect), 1, 0x26200A, ctypes.byref(data)) or not data.Scan0:
            raise PictureError(f'{Path(path).name} could not be decoded.')
        try:
            stride = abs(data.Stride)
            raw = np.frombuffer(ctypes.string_at(data.Scan0, stride * h), dtype=np.uint8).reshape(h, stride)
            bgra = raw[:, :w * 4].reshape(h, w, 4)
            if data.Stride < 0:
                bgra = bgra[::-1]
            return bgra[..., [2, 1, 0]].copy()
        finally:
            g.GdipBitmapUnlockBits(bmp, ctypes.byref(data))
    finally:
        if bmp:
            g.GdipDisposeImage(bmp)
        g.GdiplusShutdown(token)


def read_ppm(data):
    fields, pos = [], 0
    while len(fields) < 4:
        while data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b'#':
            pos = data.index(b'\n', pos) + 1
            continue
        end = pos
        while end < len(data) and not data[end:end + 1].isspace():
            end += 1
        fields.append(data[pos:end])
        pos = end
    if fields[0] != b'P6' or int(fields[3]) != 255:
        raise PictureError('not an 8-bit binary PPM')
    w, h = int(fields[1]), int(fields[2])
    return np.frombuffer(data, dtype=np.uint8, count=w * h * 3, offset=pos + 1).reshape(h, w, 3).copy()


def _load_tk(path, master):
    import tkinter as tk
    try:
        photo = tk.PhotoImage(master=master, file=str(path))
    except tk.TclError:
        raise PictureError(f'{Path(path).name} is not a PNG or GIF picture.') from None
    w, h = photo.width(), photo.height()
    if not w or not h or w > MAX_SIDE or h > MAX_SIDE or w * h > MAX_PIXELS:
        raise _too_large(path, w, h)
    fd, tmp = tempfile.mkstemp(suffix='.ppm')
    os.close(fd)
    try:
        photo.write(tmp, format='ppm')
        return read_ppm(Path(tmp).read_bytes())
    finally:
        os.unlink(tmp)


def load(path, master=None):
    if not Path(path).is_file():
        raise PictureError(f'{path} is not there.')
    if Image:
        return _load_pillow(path)
    if os.name == 'nt':
        try:
            return _load_gdiplus(path)
        except OSError as e:
            if master is None:
                raise PictureError(f'{Path(path).name} could not be read: {e}') from None
    return _load_tk(path, master)


def resize(img, w, h):
    H, W = img.shape[:2]
    k = min(H // h, W // w)
    if k >= 2:
        img = img[:H // k * k, :W // k * k].reshape(H // k, k, W // k, k, 3).mean(axis=(1, 3), dtype=np.float32)
        H, W = img.shape[:2]
    if (H, W) == (h, w):
        return img
    ys = (np.arange(h, dtype=np.float32) + 0.5) * H / h - 0.5
    xs = (np.arange(w, dtype=np.float32) + 0.5) * W / w - 0.5
    y0 = np.clip(np.floor(ys).astype(int), 0, H - 1)
    x0 = np.clip(np.floor(xs).astype(int), 0, W - 1)
    y1, x1 = np.minimum(y0 + 1, H - 1), np.minimum(x0 + 1, W - 1)
    fy = np.clip(ys - y0, 0, 1)[:, None, None]
    fx = np.clip(xs - x0, 0, 1)[None, :, None]
    a = img.astype(np.float32)
    top = a[y0][:, x0] * (1 - fx) + a[y0][:, x1] * fx
    bot = a[y1][:, x0] * (1 - fx) + a[y1][:, x1] * fx
    return top * (1 - fy) + bot * fy


def cover(img, w, h, as_float=False):
    H, W = img.shape[:2]
    if W * h > H * w:
        cw = max(1, round(H * w / h))
        x = (W - cw) // 2
        img = img[:, x:x + cw]
    else:
        ch = max(1, round(W * h / w))
        y = (H - ch) // 2
        img = img[y:y + ch]
    out = resize(img, w, h)
    if as_float:
        return np.clip(out.astype(np.float32), 0, 255)
    return np.clip(np.rint(out), 0, 255).astype(np.uint8)


def keep(path, store, master=None):
    fitted = cover(load(path, master), KEEP_W, KEEP_H)
    store = Path(store)
    tmp = store.with_name(store.name + '.tmp')
    with open(tmp, 'wb') as f:
        np.save(f, fitted, allow_pickle=False)
    os.replace(tmp, store)
    return fitted


def kept_shape(store):
    try:
        a = np.load(store, mmap_mode='r', allow_pickle=False)
        return a.shape[:2] if a.ndim == 3 else None
    except (OSError, ValueError):
        return None


def kept(store, w, h):
    try:
        a = np.load(store, allow_pickle=False)
    except (OSError, ValueError):
        return None
    if a.dtype != np.uint8 or a.ndim != 3 or a.shape[2] != 3 or a.shape[0] < 8 or a.shape[1] < 8:
        return None
    return cover(a, w, h, as_float=True) / 255.0
