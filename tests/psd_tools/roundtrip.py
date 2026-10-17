"""Writes documents with minipsd and reads them back with psd-tools, an independent reader.

The documents and the expected results are built here, in Python, not taken from minipsd: the
project's own test reader shares the writer's reading of the format, and once let a composite
that was not matted against white pass. psd-tools is how other programs will see the files.

Usage: roundtrip.py <minipsd shared library> <output directory>

SPDX-License-Identifier: MIT
SPDX-FileCopyrightText: Copyright (c) 2026 Bartlomiej Mucha <shadobadev@gmail.com>
"""

import ctypes
import pathlib
import sys

from psd_tools import PSDImage

MINIPSD_OK, MINIPSD_RLE, MINIPSD_RAW = 0, 0, 1
WRITE_FN = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t)


class Layer(ctypes.Structure):
    _fields_ = [("name", ctypes.c_char_p), ("rgba", ctypes.c_void_p),
                ("opacity", ctypes.c_uint8), ("hidden", ctypes.c_uint8),
                ("top", ctypes.c_int32), ("left", ctypes.c_int32),
                ("bottom", ctypes.c_int32), ("right", ctypes.c_int32)]


class Doc(ctypes.Structure):
    _fields_ = [("width", ctypes.c_uint32), ("height", ctypes.c_uint32), ("dpi", ctypes.c_double),
                ("layers", ctypes.POINTER(Layer)), ("layer_count", ctypes.c_size_t),
                ("icc", ctypes.c_void_p), ("icc_size", ctypes.c_size_t), ("compression", ctypes.c_int)]


# --- the documents --------------------------------------------------------------------------------

# A layer is (name, pixels, opacity, hidden), or with a fifth item (top, left) for a rectangle the size
# of its pixels at that offset; without it the layer covers the full canvas.

def image(w, h, pixel):
    """Straight-alpha RGBA rows, top down, from pixel(x, y) -> (r, g, b, a)."""
    return [[pixel(x, y) for x in range(w)] for y in range(h)]


def template(w, h):
    # Platemaker's layout: a canvas with a half-transparent pink margin, an empty layer above.
    def pixel(x, y):
        margin = x < 4 or x >= w - 4 or y < 3 or y >= h - 3
        return (255, 105, 180, 128) if margin else (255, 255, 255, 255)
    return [("Template", image(w, h, pixel), 255, False), ("Layer", None, 255, False)]


def stack(w, h):
    bottom = image(w, h, lambda x, y: (x * 255 // (w - 1), y * 255 // (h - 1), 64, 255))
    middle = image(w, h, lambda x, y: (0, 200, 255, 96 + (x * 159) // (w - 1)))
    top = image(w, h, lambda x, y: (255, 0, 0, 255))
    return [("Gradient", bottom, 255, False),
            ("Żółw \U0001F422", middle, 128, False),   # UTF-8 name, surrogate pair in UTF-16
            ("Hidden", top, 255, True)]


def rects(w, h):
    # Rectangles: smaller than the canvas, reaching past its edge, wholly outside it, and empty.
    full = image(w, h, lambda x, y: (40, 40, 40, 255))
    small = image(8, 6, lambda x, y: (255, x * 30, y * 40, 255))
    edge = image(10, 10, lambda x, y: (0, 255, 0, 160))
    outside = image(5, 5, lambda x, y: (0, 0, 255, 255))
    return [("Full", full, 255, False),
            ("Small", small, 255, False, (3, 5)),
            ("Edge", edge, 200, False, (-4, w - 6)),
            ("Outside", outside, 255, False, (h + 2, w + 3)),
            ("Empty", None, 255, False)]


DOCUMENTS = [
    # name,          width, height, dpi,   compression, layers
    ("template",     40,    30,     300.0, MINIPSD_RLE, template(40, 30)),
    ("stack-rle",    33,    9,      72.5,  MINIPSD_RLE, stack(33, 9)),
    ("stack-raw",    33,    9,      150.0, MINIPSD_RAW, stack(33, 9)),
    ("rects-rle",    24,    16,     72.0,  MINIPSD_RLE, rects(24, 16)),
    ("rects-raw",    24,    16,     72.0,  MINIPSD_RAW, rects(24, 16)),
]


def rect(layer, w, h):
    """The layer's (top, left, bottom, right) on the canvas; None for an empty layer."""
    pixels = layer[1]
    if pixels is None:
        return None
    top, left = layer[4] if len(layer) > 4 else (0, 0)
    return top, left, top + len(pixels), left + len(pixels[0])


# --- the expectation: straight-alpha "over", computed independently -----------------------------

def composite(w, h, layers):
    out = [[(0.0, 0.0, 0.0, 0.0)] * w for _ in range(h)]
    for layer in layers:
        _, pixels, opacity, hidden = layer[:4]
        if pixels is None or hidden or opacity == 0:
            continue
        top, left, bottom, right = rect(layer, w, h)
        for y in range(max(top, 0), min(bottom, h)):
            for x in range(max(left, 0), min(right, w)):
                r, g, b, a = pixels[y - top][x - left]
                dr, dg, db, da = out[y][x]
                sa = a / 255 * opacity / 255
                oa = sa + da * (1 - sa)
                if oa > 0:
                    mix = lambda s, d: (s * sa + d * da * (1 - sa)) / oa
                    out[y][x] = (mix(r, dr), mix(g, dg), mix(b, db), oa)
    return [[(r, g, b, a * 255) for r, g, b, a in row] for row in out]


# --- writing and checking -----------------------------------------------------------------------

def write(lib, path, w, h, dpi, compression, layers):
    keep = []   # the pixel buffers must outlive the call
    records = (Layer * len(layers))()
    for i, layer in enumerate(layers):
        name, pixels, opacity, hidden = layer[:4]
        rgba = None
        if pixels is not None:
            buffer = ctypes.create_string_buffer(bytes(c for row in pixels for px in row for c in px))
            keep.append(buffer)
            rgba = ctypes.cast(buffer, ctypes.c_void_p)
        top, left, bottom, right = rect(layer, w, h) if len(layer) > 4 else (0, 0, 0, 0)
        records[i] = Layer(name.encode("utf-8"), rgba, opacity, int(hidden), top, left, bottom, right)
    doc = Doc(w, h, dpi, records, len(layers), None, 0, compression)
    out = bytearray()

    def collect(_user, data, size):
        out.extend(ctypes.string_at(data, size))
        return 1

    rc = lib.minipsd_write(ctypes.byref(doc), WRITE_FN(collect), None)
    if rc == MINIPSD_OK:
        path.write_bytes(out)
    return rc


def check(name, psd, w, h, dpi, layers):
    errors = []

    def expect(what, got, want):
        if got != want:
            errors.append(f"{name}: {what} is {got!r}, expected {want!r}")

    expect("size", psd.size, (w, h))
    expect("depth", psd.depth, 8)
    expect("channels", psd.channels, 4)
    expect("colour mode", psd.color_mode.name, "RGB")
    resolution = psd.image_resources.get_data(1005)   # psd-tools leaves it as 16.16 fixed point
    expect("dpi", resolution.horizontal / 65536, dpi)
    expect("vertical dpi", resolution.vertical / 65536, dpi)

    read = list(psd)   # bottom first, as written
    expect("layer count", len(read), len(layers))
    for got, layer in zip(read, layers):
        lname, pixels, opacity, hidden = layer[:4]
        expect("layer name", got.name, lname)
        expect(f"'{lname}' opacity", got.opacity, opacity)
        expect(f"'{lname}' visible", got.visible, not hidden)
        if pixels is None:
            expect(f"'{lname}' bbox", tuple(got.bbox), (0, 0, 0, 0))
            continue
        top, left, bottom, right = rect(layer, w, h)
        expect(f"'{lname}' bbox", tuple(got.bbox), (left, top, right, bottom))
        got_px = got.topil().convert("RGBA")
        lw, lh = right - left, bottom - top
        expect(f"'{lname}' image size", got_px.size, (lw, lh))
        if got_px.size != (lw, lh):
            continue
        bad = [(x, y) for y in range(lh) for x in range(lw) if got_px.getpixel((x, y)) != tuple(pixels[y][x])]
        if bad:
            x, y = bad[0]
            errors.append(f"{name}: '{lname}' has {len(bad)} wrong pixels, first at {(x, y)}: "
                          f"{got_px.getpixel((x, y))} instead of {tuple(pixels[y][x])}")

    # The merged image as a reader that ignores layers sees it. 8-bit matting and un-matting may move
    # a colour by about 255 / (2 * alpha) levels, so the tolerance follows the alpha.
    merged = psd.topil().convert("RGBA")
    want = composite(w, h, layers)
    bad = []
    for y in range(h):
        for x in range(w):
            got_px, want_px = merged.getpixel((x, y)), want[y][x]
            if abs(got_px[3] - want_px[3]) > 1:
                bad.append((x, y, got_px, want_px))
            elif want_px[3] >= 1:
                tolerance = 1 + 255 / (2 * want_px[3])
                if any(abs(got_px[k] - want_px[k]) > tolerance for k in range(3)):
                    bad.append((x, y, got_px, want_px))
    if bad:
        x, y, got_px, want_px = bad[0]
        errors.append(f"{name}: composite has {len(bad)} wrong pixels, first at {(x, y)}: "
                      f"{got_px} instead of {tuple(round(c) for c in want_px)}")
    return errors


def main():
    lib = ctypes.CDLL(sys.argv[1])
    lib.minipsd_write.argtypes = [ctypes.POINTER(Doc), WRITE_FN, ctypes.c_void_p]
    lib.minipsd_write.restype = ctypes.c_int
    out = pathlib.Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)

    errors = []
    for name, w, h, dpi, compression, layers in DOCUMENTS:
        path = out / f"{name}.psd"
        rc = write(lib, path, w, h, dpi, compression, layers)
        if rc != MINIPSD_OK:
            errors.append(f"{name}: minipsd_write returned {rc}")
            continue
        found = check(name, PSDImage.open(path), w, h, dpi, layers)
        errors += found
        print(f"{name}: {'FAILED' if found else 'ok'}")
    for e in errors:
        print(e)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
