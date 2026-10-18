"""Writes documents with minipsd and reads them back with psd-tools, an independent reader.

The documents and the expected results are built here, in Python, not taken from minipsd: the
project's own test reader shares the writer's reading of the format, and once let a composite
that was not matted against white pass. psd-tools is how other programs will see the files.

It also reads files from other programs with minipsd_read and writes them back with
minipsd_write: every block, resource and global block psd-tools finds must come back the same.
Photoshop's 16-bit files go through as stored: their ZIP channels are copied, byte for byte.

Usage: roundtrip.py <minipsd shared library> <output directory> [<psd-tools' psd_files directory>]

SPDX-License-Identifier: MIT
SPDX-FileCopyrightText: Copyright (c) 2026 Bartlomiej Mucha <shadobadev@gmail.com>
"""

import ctypes
import pathlib
import struct
import sys

from psd_tools import PSDImage

MINIPSD_OK, MINIPSD_RLE, MINIPSD_RAW = 0, 0, 1
WRITE_FN = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t)


class Bytes(ctypes.Structure):
    _fields_ = [("data", ctypes.c_void_p), ("size", ctypes.c_size_t)]


class Mask(ctypes.Structure):
    _fields_ = [("top", ctypes.c_int32), ("left", ctypes.c_int32),
                ("bottom", ctypes.c_int32), ("right", ctypes.c_int32),
                ("color", ctypes.c_uint8), ("flags", ctypes.c_uint8), ("pixels", ctypes.c_void_p),
                ("stored", Bytes), ("real", Bytes), ("rest", Bytes)]


class Layer(ctypes.Structure):
    _fields_ = [("name", ctypes.c_char_p), ("rgba", ctypes.c_void_p),
                ("opacity", ctypes.c_uint8), ("hidden", ctypes.c_uint8),
                ("top", ctypes.c_int32), ("left", ctypes.c_int32),
                ("bottom", ctypes.c_int32), ("right", ctypes.c_int32),
                ("blend", ctypes.c_char * 4), ("clipping", ctypes.c_uint8), ("flags", ctypes.c_uint8),
                ("extra", Bytes), ("stored", ctypes.c_void_p),
                ("kind", ctypes.c_uint8), ("mask", ctypes.POINTER(Mask))]


class Doc(ctypes.Structure):
    _fields_ = [("width", ctypes.c_uint32), ("height", ctypes.c_uint32), ("dpi", ctypes.c_double),
                ("layers", ctypes.POINTER(Layer)), ("layer_count", ctypes.c_size_t),
                ("icc", ctypes.c_void_p), ("icc_size", ctypes.c_size_t), ("compression", ctypes.c_int),
                ("composite", ctypes.c_void_p), ("composite_matted", ctypes.c_int),
                ("resources", ctypes.c_void_p), ("resource_count", ctypes.c_size_t), ("layer_tail", Bytes),
                ("depth", ctypes.c_uint16)]


class ReadOpts(ctypes.Structure):
    _fields_ = [("stored", ctypes.c_int)]


# --- the documents --------------------------------------------------------------------------------

# A layer is (name, pixels, opacity, hidden), or with a fifth item (top, left) for a rectangle the size
# of its pixels at that offset; without it the layer covers the full canvas.  A sixth item is the
# blend mode's key; without it, "norm".

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


# Photoshop's blend modes, in its menu order: (name, key).  Pass Through is for groups only.
BLEND_MODES = [
    ("Normal", b"norm"), ("Dissolve", b"diss"),
    ("Darken", b"dark"), ("Multiply", b"mul "), ("Color Burn", b"idiv"), ("Linear Burn", b"lbrn"),
    ("Darker Color", b"dkCl"),
    ("Lighten", b"lite"), ("Screen", b"scrn"), ("Color Dodge", b"div "), ("Linear Dodge (Add)", b"lddg"),
    ("Lighter Color", b"lgCl"),
    ("Overlay", b"over"), ("Soft Light", b"sLit"), ("Hard Light", b"hLit"), ("Vivid Light", b"vLit"),
    ("Linear Light", b"lLit"), ("Pin Light", b"pLit"), ("Hard Mix", b"hMix"),
    ("Difference", b"diff"), ("Exclusion", b"smud"), ("Subtract", b"fsub"), ("Divide", b"fdiv"),
    ("Hue", b"hue "), ("Saturation", b"sat "), ("Color", b"colr"), ("Luminosity", b"lum "),
]


def blend_modes(cell):
    # A gradient below; above it one square per mode, each in a cell of its own, nine to a row,
    # named after its mode: a viewer shows every mode side by side.
    w, h = 9 * cell, 3 * cell
    base = image(w, h, lambda x, y: (x * 255 // (w - 1), 128, y * 255 // (h - 1), 255))
    side = cell - cell // 4
    square = image(side, side, lambda x, y: (230, 60 + x * 150 // side, 40, 160 + y * 95 // side))
    layers = [("Base", base, 255, False)]
    for i, (name, key) in enumerate(BLEND_MODES):
        at = (i // 9 * cell + cell // 8, i % 9 * cell + cell // 8)
        layers.append((name, square, 255, False, at, key))
    return layers


DOCUMENTS = [
    # name,          width, height, dpi,   compression, layers
    ("template",     40,    30,     300.0, MINIPSD_RLE, template(40, 30)),
    ("stack-rle",    33,    9,      72.5,  MINIPSD_RLE, stack(33, 9)),
    ("stack-raw",    33,    9,      150.0, MINIPSD_RAW, stack(33, 9)),
    ("rects-rle",    24,    16,     72.0,  MINIPSD_RLE, rects(24, 16)),
    ("rects-raw",    24,    16,     72.0,  MINIPSD_RAW, rects(24, 16)),
    ("blend-modes",  9 * 32, 3 * 32, 72.0, MINIPSD_RLE, blend_modes(32)),
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
        records[i] = Layer(name.encode("utf-8"), rgba, opacity, int(hidden), top, left, bottom, right,
                           layer[5] if len(layer) > 5 else b"\0\0\0\0")
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
        expect(f"'{lname}' blend mode", got.blend_mode.value, layer[5] if len(layer) > 5 else b"norm")
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


# --- groups and a mask, written from scratch ------------------------------------------------------

KIND_LAYER, KIND_GROUP, KIND_GROUP_CLOSED, KIND_GROUP_END = 0, 1, 2, 3


def groups_and_mask(lib, path):
    """Base; group "Outer" (open, pass through) holding "Masked" and group "Inner" (closed, multiply)
    holding "Deep".  Bottom first, as the file stores them; returns what to expect."""
    w, h = 64, 48
    keep = []

    def pixels(lw, lh, colour):
        buf = ctypes.create_string_buffer(bytes(c for _ in range(lw * lh) for c in colour))
        keep.append(buf)
        return ctypes.cast(buf, ctypes.c_void_p)

    mask_px = bytes((x * 255 // 31) for y in range(20) for x in range(32))   # a ramp, left to right
    mask_buf = ctypes.create_string_buffer(mask_px)
    mask = Mask(top=10, left=8, bottom=30, right=40, color=0, flags=0, pixels=ctypes.cast(mask_buf, ctypes.c_void_p))
    records = (Layer * 7)()
    spec = [  # name, rgba, kind, blend, mask
        ("Base", pixels(w, h, (240, 240, 240, 255)), KIND_LAYER, b"\0\0\0\0", None),
        ("</Layer group>", None, KIND_GROUP_END, b"\0\0\0\0", None),
        ("Masked", pixels(w, h, (200, 40, 40, 255)), KIND_LAYER, b"\0\0\0\0", ctypes.pointer(mask)),
        ("</Layer group>", None, KIND_GROUP_END, b"\0\0\0\0", None),
        ("Deep", pixels(w, h, (40, 40, 200, 255)), KIND_LAYER, b"\0\0\0\0", None),
        ("Inner", None, KIND_GROUP_CLOSED, b"mul ", None),
        ("Outer", None, KIND_GROUP, b"\0\0\0\0", None),
    ]
    for i, (name, rgba, kind, blend, m) in enumerate(spec):
        records[i] = Layer(name=name.encode(), rgba=rgba, opacity=255, blend=blend, kind=kind, mask=m)
    doc = Doc(w, h, 72.0, records, len(spec), None, 0, MINIPSD_RLE)
    out = bytearray()

    def collect(_user, data, size):
        out.extend(ctypes.string_at(data, size))
        return 1

    rc = lib.minipsd_write(ctypes.byref(doc), WRITE_FN(collect), None)
    if rc == MINIPSD_OK:
        path.write_bytes(out)
    return rc, mask_px


def check_groups_and_mask(psd, mask_px):
    errors = []

    def expect(what, got, want):
        if got != want:
            errors.append(f"groups-masks: {what} is {got!r}, expected {want!r}")

    top = list(psd)
    expect("top level", [(l.name, l.is_group()) for l in top], [("Base", False), ("Outer", True)])
    outer = top[1]
    expect("Outer's mode", outer.blend_mode.value, b"pass")
    expect("Outer's layers", [(l.name, l.is_group()) for l in outer], [("Masked", False), ("Inner", True)])
    inner = list(outer)[1]
    expect("Inner's mode", inner.blend_mode.value, b"mul ")
    expect("Inner closed", inner._record.tagged_blocks.get_data(b"lsct").kind.value, KIND_GROUP_CLOSED)
    expect("Inner's layers", [l.name for l in inner], ["Deep"])
    masked = list(outer)[0]
    expect("mask", masked.has_mask(), True)
    if masked.has_mask():
        expect("mask rectangle", tuple(masked.mask.bbox), (8, 10, 40, 30))
        expect("mask colour", masked.mask.background_color, 0)
        expect("mask disabled", masked.mask.disabled, False)
        expect("mask pixels", masked.mask.topil().tobytes(), mask_px)
    return errors


# --- 16 bits, written from scratch ---------------------------------------------------------------

def deep(lib, path):
    """A 16-bit document: an opaque gradient, and above it a half-transparent layer at an offset, with
    a mask.  Samples are uint16 in host order; returns the layers' samples and the mask's."""
    w, h, lw, lh = 48, 32, 20, 12
    keep = []

    def samples(values):
        buf = (ctypes.c_uint16 * len(values))(*values)
        keep.append(buf)
        return ctypes.cast(buf, ctypes.c_void_p)

    base = [c for y in range(h) for x in range(w) for c in (x * 65535 // (w - 1), y * 65535 // (h - 1), 4097, 65535)]
    top = [c for y in range(lh) for x in range(lw) for c in (65535, x * 3000, 1234, 20000 + y * 3000)]
    mask_px = [x * 65535 // (lw - 1) for y in range(lh) for x in range(lw)]
    mask = Mask(top=5, left=10, bottom=5 + lh, right=10 + lw, pixels=samples(mask_px))
    records = (Layer * 2)()
    records[0] = Layer(name=b"Gradient", rgba=samples(base), opacity=255)
    records[1] = Layer(name=b"Masked", rgba=samples(top), opacity=255, top=5, left=10, bottom=5 + lh, right=10 + lw,
                       mask=ctypes.pointer(mask))
    doc = Doc(w, h, 72.0, records, 2, None, 0, MINIPSD_RLE)
    doc.depth = 16
    out = bytearray()

    def collect(_user, data, size):
        out.extend(ctypes.string_at(data, size))
        return 1

    rc = lib.minipsd_write(ctypes.byref(doc), WRITE_FN(collect), None)
    if rc == MINIPSD_OK:
        path.write_bytes(out)
    return rc, (w, h, lw, lh, base, top, mask_px)


def check_deep(psd, expected):
    """Every channel's samples as psd-tools decodes them, big-endian; the composite: the layer over
    the gradient, matted against 65535 (opaque here, so the matte is the identity)."""
    w, h, lw, lh, base, top, mask_px = expected
    errors = []

    def expect(what, got, want):
        if got != want:
            errors.append(f"deep-16: {what} is {got!r}, expected {want!r}")

    def plane(values, k, step):
        return b"".join(struct.pack(">H", values[i]) for i in range(k, len(values), step))

    expect("depth", psd.depth, 16)
    expect("channels", psd.channels, 4)
    li = psd._record.layer_and_mask_information.layer_info
    expect("layer names", [r.name for r in li.layer_records], ["Gradient", "Masked"])
    for record, channels, values, (rw, rh) in zip(li.layer_records, li.channel_image_data, (base, top), ((w, h), (lw, lh))):
        for info, data in zip(record.channel_info, channels):
            if info.id.value == -2:
                got, want = data.get_data(lw, lh, 16), plane(mask_px, 0, 1)
            else:
                got, want = data.get_data(rw, rh, 16), plane(values, {-1: 3}.get(info.id.value, info.id.value), 4)
            expect(f"'{record.name}' channel {info.id.value}", got == want, True)
    merged = psd._record.image_data.get_data(psd._record.header)
    want = [[0.0] * (w * h) for _ in range(4)]
    for y in range(h):
        for x in range(w):
            r, g, b, a = (base[(y * w + x) * 4 + k] / 65535 for k in range(4))
            if 5 <= y < 5 + lh and 10 <= x < 10 + lw:
                i = ((y - 5) * lw + x - 10) * 4
                tr, tg, tb, ta = (top[i + k] / 65535 for k in range(4))
                r, g, b = (t * ta + d * (1 - ta) for t, d in ((tr, r), (tg, g), (tb, b)))   # over an opaque base
            for k, v in enumerate((r, g, b, a)):
                want[k][y * w + x] = v * 65535
    worst = max(abs(struct.unpack_from(">H", merged[k], 2 * i)[0] - want[k][i]) for k in range(4) for i in range(w * h))
    expect("composite within 2 levels", worst <= 2, True)
    return errors


# --- files from other programs: read, written back, what is kept compared --------------------------

# Files with text, effects, groups and resources of every kind; read whole, written back as they are.
KEPT_FILES = ["text.psd", "layer_effects.psd", "group.psd", "blend-and-clipping.psd", "unicode_pathname.psd",
              "hidden-groups.psd", "layer_mask_data.psd", "mask-density-layervectormask.psd", "mask-disabled.psd",
              "mask-parameters-no-real-channel.psd"]
# Photoshop's 16-bit RGB files: the layers in 'Lr16', their channels ZIP-compressed; resaved as stored.
KEPT_16 = ["16bit5x5.psd", "colormodes/4x4_16bit_rgb.psd", "adjustments/posterize_16bits_rgb.psd",
           "adjustments/threshold_16bits_rgb.psd"]


def resave(lib, source, path, stored=False):
    data = source.read_bytes()
    doc = ctypes.POINTER(Doc)()
    lost = ctypes.c_uint32(0)
    opts = ReadOpts(1) if stored else None
    rc = lib.minipsd_read(data, len(data), None, ctypes.byref(opts) if opts else None, ctypes.byref(doc),
                          ctypes.byref(lost))
    if rc != MINIPSD_OK:
        return rc, 0
    out = bytearray()

    def collect(_user, chunk, size):
        out.extend(ctypes.string_at(chunk, size))
        return 1

    rc = lib.minipsd_write(doc, WRITE_FN(collect), None)
    lib.minipsd_doc_free(doc)
    if rc == MINIPSD_OK:
        path.write_bytes(out)
    return rc, lost.value


def records(lmi):
    """The layer records and their channel data: in the layer info, or in Photoshop's 'Lr16'."""
    if lmi.layer_info and lmi.layer_info.layer_records:
        return lmi.layer_info.layer_records, lmi.layer_info.channel_image_data
    deep = lmi.tagged_blocks.get_data(b"Lr16") if lmi.tagged_blocks else None
    return (deep.layer_records, deep.channel_image_data) if deep else ([], [])


def check_kept(name, before, after, stored=False):
    """Every layer's blocks, every resource and every global block ('Lr16' aside: its layers are the
    file's layers), byte for byte as psd-tools writes them, in any order; blend key, clipping and
    flags of every record.  Stored: every channel's data as well, and the composite."""
    errors = []

    def same(what, a, b):
        if a != b:
            errors.append(f"{name}: {what} differ")

    def blocks(tagged):
        return {key: block.tobytes() for key, block in tagged.items() if getattr(key, "value", key) not in (b"Lr16", b"Lr32")}

    def resources(psd):
        return {key: resource.tobytes() for key, resource in psd.image_resources.items()}

    same("resources", resources(before), resources(after))
    gb, ga = before._record.layer_and_mask_information, after._record.layer_and_mask_information
    same("global blocks", blocks(gb.tagged_blocks) if gb.tagged_blocks else {}, blocks(ga.tagged_blocks) if ga.tagged_blocks else {})
    same("global layer mask", gb.global_layer_mask_info, ga.global_layer_mask_info)
    (rb, cb), (ra, ca) = records(gb), records(ga)
    same("layer count", len(rb), len(ra))
    if stored:
        # R, G, B as decoded: minipsd writes four channels where the original may have three.
        same("composite", before._record.image_data.get_data(before._record.header)[:3],
             after._record.image_data.get_data(after._record.header)[:3])
        for i, (b, a, db, da) in enumerate(zip(rb, ra, cb, ca)):
            got = {c.id.value: (d.compression, d.data) for c, d in zip(a.channel_info, da)}
            same(f"layer {i} channel data", {c.id.value: (d.compression, d.data) for c, d in zip(b.channel_info, db)},
                 {k: v for k, v in got.items() if k != -1 or -1 in [c.id.value for c in b.channel_info]})
    for i, (b, a) in enumerate(zip(rb, ra)):
        same(f"layer {i} blocks", blocks(b.tagged_blocks), blocks(a.tagged_blocks))
        same(f"layer {i} mask data", b.mask_data.tobytes() if b.mask_data else None, a.mask_data.tobytes() if a.mask_data else None)
        ids = [c.id.value for c in b.channel_info]
        added = [] if -1 in ids else [-1]   # a layer without transparency gets an opaque one
        same(f"layer {i} channels", added + ids if added else ids, [c.id.value for c in a.channel_info])
        same(f"layer {i} blend, clipping, flags", (b.blend_mode, b.clipping, b.flags), (a.blend_mode, a.clipping, a.flags))
    return errors


def main():
    lib = ctypes.CDLL(sys.argv[1])
    lib.minipsd_write.argtypes = [ctypes.POINTER(Doc), WRITE_FN, ctypes.c_void_p]
    lib.minipsd_write.restype = ctypes.c_int
    lib.minipsd_read.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_void_p,
                                 ctypes.POINTER(ctypes.POINTER(Doc)), ctypes.POINTER(ctypes.c_uint32)]
    lib.minipsd_read.restype = ctypes.c_int
    lib.minipsd_doc_free.argtypes = [ctypes.POINTER(Doc)]
    lib.minipsd_doc_free.restype = None
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

    path = out / "groups-masks.psd"
    rc, mask_px = groups_and_mask(lib, path)
    found = [f"groups-masks: minipsd_write returned {rc}"] if rc != MINIPSD_OK else check_groups_and_mask(PSDImage.open(path), mask_px)
    errors += found
    print(f"groups-masks: {'FAILED' if found else 'ok'}")

    path = out / "deep-16.psd"
    rc, expected = deep(lib, path)
    found = [f"deep-16: minipsd_write returned {rc}"] if rc != MINIPSD_OK else check_deep(PSDImage.open(path), expected)
    errors += found
    print(f"deep-16: {'FAILED' if found else 'ok'}")

    files = pathlib.Path(sys.argv[3]) if len(sys.argv) > 3 and sys.argv[3] else None
    if files is None:
        print("kept content: skipped, no psd-tools files (MINIPSD_PSD_TOOLS_FILES)")
    for name, stored in [(n, False) for n in KEPT_FILES] + [(n, True) for n in KEPT_16] if files else []:
        path = out / f"resaved-{pathlib.Path(name).name}"
        rc, lost = resave(lib, files / name, path, stored)
        if rc != MINIPSD_OK:
            errors.append(f"{name}: minipsd_read or minipsd_write returned {rc}")
            continue
        before, after = PSDImage.open(files / name), PSDImage.open(path)
        found = check_kept(name, before, after, stored)
        for lb, la in zip(before.descendants(), after.descendants()):   # mask pixels, as psd-tools decodes them
            if lb.has_mask() and lb.mask.topil() is not None and lb.mask.topil().tobytes() != la.mask.topil().tobytes():
                found.append(f"{name}: '{lb.name}' mask pixels differ")
        errors += found
        print(f"resaved {name} (lost {lost}): {'FAILED' if found else 'ok'}")
    for e in errors:
        print(e)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
