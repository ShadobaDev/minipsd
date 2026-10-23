"""Writes documents with minipsd and reads them back with psd-tools, an independent reader.

The documents and the expected results are built here, in Python, not taken from minipsd: the
project's own test reader shares the writer's reading of the format, and once let a composite
that was not matted against white pass. psd-tools is how other programs will see the files.

It also reads files from other programs with minipsd_read and writes them back with
minipsd_write: every block, resource and global block psd-tools finds must come back the same.
Photoshop's 16-bit files go through as stored, their ZIP channels copied byte for byte, and
decoded through Python's zlib (minipsd_read's inflate), every layer's pixels as psd-tools gives them.
Files minipsd wrote are also re-encoded by psd-tools as ZIP, with and without prediction, and must
decode to the same pixels: the ZIP path without psd-tools' files.  The other way, documents minipsd
writes as ZIP through Python's zlib (minipsd_write_opts.deflate) are decoded by psd-tools, and the
files read decoded are written back as ZIP with prediction, as Photoshop writes its 16-bit layers.

Each of those also goes into the other format (PSD as PSB, PSB as PSD), its kept content
translated, and must read back the same.

And it reads every file of psd-tools' set through minipsd's reader, every colour mode and depth,
PSD and PSB: each layer and the composite decode to what psd-tools decompresses from the same file.

Usage: roundtrip.py <minipsd shared library> <output directory> [<psd-tools' psd_files directory>]

SPDX-License-Identifier: MIT
SPDX-FileCopyrightText: Copyright (c) 2026 Bartlomiej Mucha <shadobadev@gmail.com>
"""

import ctypes
import pathlib
import struct
import sys
import zlib

from psd_tools import PSDImage
from psd_tools.constants import Compression, Resource as ResourceId

MINIPSD_OK, MINIPSD_RLE, MINIPSD_RAW, MINIPSD_ZIP, MINIPSD_ZIP_PREDICTED = 0, 0, 1, 2, 3
MINIPSD_ERR_UNSUPPORTED = -7
WRITE_FN = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t)
INFLATE_FN = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p,
                              ctypes.c_size_t)


@INFLATE_FN
def inflate(_user, src, size, dst, dst_size):
    """Python's zlib as minipsd_inflate_fn: 0 when the stream fills dst exactly."""
    try:
        out = zlib.decompress(ctypes.string_at(src, size))
    except zlib.error:
        return 1
    if len(out) != dst_size:
        return 1
    ctypes.memmove(dst, out, dst_size)
    return 0


class Bytes(ctypes.Structure):
    _fields_ = [("data", ctypes.c_void_p), ("size", ctypes.c_size_t)]


class Mask(ctypes.Structure):
    _fields_ = [("top", ctypes.c_int32), ("left", ctypes.c_int32),
                ("bottom", ctypes.c_int32), ("right", ctypes.c_int32),
                ("default_color", ctypes.c_uint8), ("flags", ctypes.c_uint8), ("pixels", ctypes.c_void_p),
                ("stored", Bytes), ("real", Bytes), ("rest", Bytes)]


class Resource(ctypes.Structure):
    _fields_ = [("id", ctypes.c_uint16), ("data", Bytes), ("name", Bytes)]


class ResolutionInfo(ctypes.Structure):
    _fields_ = [("horizontal_resolution", ctypes.c_double), ("horizontal_resolution_unit", ctypes.c_uint16),
                ("width_unit", ctypes.c_uint16), ("vertical_resolution", ctypes.c_double),
                ("vertical_resolution_unit", ctypes.c_uint16), ("height_unit", ctypes.c_uint16)]


class VersionInfo(ctypes.Structure):
    _fields_ = [("version", ctypes.c_uint32), ("has_real_merged_data", ctypes.c_uint8),
                ("writer_name", Bytes), ("reader_name", Bytes), ("file_version", ctypes.c_uint32)]


class GridAndGuides(ctypes.Structure):
    _fields_ = [("version", ctypes.c_uint32), ("horizontal_grid", ctypes.c_uint32),
                ("vertical_grid", ctypes.c_uint32), ("guide_count", ctypes.c_uint32)]


class Guide(ctypes.Structure):
    _fields_ = [("location", ctypes.c_int32), ("direction", ctypes.c_uint8)]


class BlockSettings(ctypes.Structure):
    _fields_ = [("layer_id", ctypes.c_uint32), ("sheet_color", ctypes.c_uint16), ("fill_opacity", ctypes.c_uint8),
                ("protection_flags", ctypes.c_uint32), ("knockout", ctypes.c_uint8),
                ("blend_clipped_elements", ctypes.c_uint8), ("blend_interior_elements", ctypes.c_uint8),
                ("present", ctypes.c_uint8)]


class BlendRange(ctypes.Structure):
    _fields_ = [("black", ctypes.c_uint8 * 2), ("white", ctypes.c_uint8 * 2)]


class BlendingRange(ctypes.Structure):
    _fields_ = [("source", BlendRange), ("destination", BlendRange)]


class Layer(ctypes.Structure):
    _fields_ = [("name", ctypes.c_char_p), ("pixels", ctypes.c_void_p),
                ("opacity", ctypes.c_uint8), ("hidden", ctypes.c_uint8),
                ("top", ctypes.c_int32), ("left", ctypes.c_int32),
                ("bottom", ctypes.c_int32), ("right", ctypes.c_int32),
                ("blend_mode", ctypes.c_char * 4), ("clipping", ctypes.c_uint8), ("flags", ctypes.c_uint8),
                ("additional_info", Bytes), ("stored", ctypes.c_void_p),
                ("section_divider_type", ctypes.c_uint8), ("mask", ctypes.POINTER(Mask)),
                ("blending_ranges", ctypes.POINTER(BlendingRange)), ("blending_range_count", ctypes.c_uint32),
                ("name_pascal", Bytes)]


class Doc(ctypes.Structure):
    _fields_ = [("width", ctypes.c_uint32), ("height", ctypes.c_uint32), ("dpi", ctypes.c_double),
                ("layers", ctypes.POINTER(Layer)), ("layer_count", ctypes.c_size_t),
                ("icc", ctypes.c_void_p), ("icc_size", ctypes.c_size_t), ("compression", ctypes.c_int),
                ("composite", ctypes.c_void_p), ("composite_matted", ctypes.c_int),
                ("resources", ctypes.c_void_p), ("resource_count", ctypes.c_size_t), ("layer_tail", Bytes),
                ("depth", ctypes.c_uint16), ("format", ctypes.c_uint16),
                ("composite_stored", Bytes), ("composite_channels", ctypes.c_uint16),
                ("composite_alpha", ctypes.c_uint8), ("color_mode", ctypes.c_uint16), ("color_mode_data", Bytes)]


DEFLATE_FN = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p,
                              ctypes.POINTER(ctypes.c_size_t))


@DEFLATE_FN
def deflate(_user, src, size, dst, dst_size):
    """Python's zlib as minipsd_deflate_fn: the stream, when it fits the room given."""
    out = zlib.compress(ctypes.string_at(src, size))
    if len(out) > dst_size[0]:
        return 1
    ctypes.memmove(dst, out, len(out))
    dst_size[0] = len(out)
    return 0


class ReadOpts(ctypes.Structure):
    _fields_ = [("stored", ctypes.c_int), ("inflate", INFLATE_FN), ("inflate_user", ctypes.c_void_p)]


class WriteOpts(ctypes.Structure):
    _fields_ = [("format", ctypes.c_uint16), ("deflate", DEFLATE_FN), ("deflate_user", ctypes.c_void_p)]


class Plane(ctypes.Structure):
    _fields_ = [("data", ctypes.c_void_p), ("pixel_step", ctypes.c_ssize_t), ("row_step", ctypes.c_ssize_t)]


class Target(ctypes.Structure):
    _fields_ = [("channel", Plane * 5)]


PLANES = 5   # MINIPSD_PLANES


class DecodeOpts(ctypes.Structure):
    _fields_ = [("y_first", ctypes.c_uint32), ("y_count", ctypes.c_uint32), ("keep_matte", ctypes.c_int),
                ("depth", ctypes.c_int), ("inflate", INFLATE_FN), ("inflate_user", ctypes.c_void_p),
                ("scratch", ctypes.c_void_p), ("scratch_size", ctypes.c_size_t), ("scratch_kept", ctypes.c_int)]


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
    ("stack-zip",    33,    9,      72.0,  MINIPSD_ZIP, stack(33, 9)),
    ("rects-zip-predicted", 24, 16, 72.0,  MINIPSD_ZIP_PREDICTED, rects(24, 16)),
    ("blend-modes",  9 * 32, 3 * 32, 72.0, MINIPSD_RLE, blend_modes(32)),
]

# The same written as PSB.
MINIPSD_PSB = 2
PSB_DOCUMENTS = [(f"{name}-psb", w, h, dpi, compression, layers)
                 for name, w, h, dpi, compression, layers in DOCUMENTS if name in ("template", "rects-raw")]

# A PSB wider than a PSD may be.  psd-tools' images stop at 30 000 px, PSB or not, so its layer and
# composite are checked as the channels psd-tools decompresses.
WIDE = ("wide-psb", 30001, 2, 72.0, MINIPSD_RLE, [("Wide", image(30001, 2, lambda x, y: (x % 256, 200, y * 90, 255)), 255, False)])


def check_wide(psd, layers):
    name, w, h = WIDE[0], WIDE[1], WIDE[2]
    pixels, errors = layers[0][1], []
    if (psd.version, psd.size) != (2, (w, h)):
        errors.append(f"{name}: version {psd.version}, size {psd.size}")
    planes = [bytes(px[k] for row in pixels for px in row) for k in (0, 1, 2, 3)]
    info = psd._record.layer_and_mask_information.layer_info
    rec, data = info.layer_records[0], info.channel_image_data[0]
    got = {c.id: d.get_data(w, h, 8, 2) for c, d in zip(rec.channel_info, data)}
    if [got.get(k) for k in (0, 1, 2, -1)] != planes:
        errors.append(f"{name}: the layer's channels differ")
    composite = psd._record.image_data.get_data(psd._record.header)
    if composite[:3] != planes[:3] or composite[3] != planes[3]:   # opaque: the matte changes nothing
        errors.append(f"{name}: the composite differs")
    return errors


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

def write(lib, path, w, h, dpi, compression, layers, fmt=0):
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
    doc.format = fmt
    out = bytearray()

    def collect(_user, data, size):
        out.extend(ctypes.string_at(data, size))
        return 1

    rc = lib.minipsd_write(ctypes.byref(doc), ctypes.byref(WriteOpts(fmt, deflate)), WRITE_FN(collect), None)
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

SECTION_LAYER, SECTION_GROUP, SECTION_GROUP_CLOSED, SECTION_GROUP_END = 0, 1, 2, 3


# "Blend If" of "Masked": composite grey, then R, G, B, each (source, destination) as
# ((black low, high), (white low, high)); split points on red.
BLEND_IF = [(((0, 0), (255, 255)), ((59, 59), (210, 210))), (((10, 30), (200, 240)), ((0, 0), (255, 255))),
            (((0, 0), (255, 255)), ((0, 0), (255, 255))), (((0, 0), (255, 255)), ((5, 5), (250, 250)))]


def psd_tools_range(r):
    """A range as psd-tools reads it: two big-endian 16-bit numbers, black then white."""
    (b0, b1), (w0, w1) = r
    return (b0 << 8 | b1, w0 << 8 | w1)


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
    mask = Mask(top=10, left=8, bottom=30, right=40, default_color=0, flags=0, pixels=ctypes.cast(mask_buf, ctypes.c_void_p))
    ranges = (BlendingRange * len(BLEND_IF))(*[BlendingRange(BlendRange(*s), BlendRange(*d)) for s, d in BLEND_IF])
    records = (Layer * 7)()
    spec = [  # name, rgba, kind, blend, mask
        ("Base", pixels(w, h, (240, 240, 240, 255)), SECTION_LAYER, b"\0\0\0\0", None),
        ("</Layer group>", None, SECTION_GROUP_END, b"\0\0\0\0", None),
        ("Masked", pixels(w, h, (200, 40, 40, 255)), SECTION_LAYER, b"\0\0\0\0", ctypes.pointer(mask)),
        ("</Layer group>", None, SECTION_GROUP_END, b"\0\0\0\0", None),
        ("Deep", pixels(w, h, (40, 40, 200, 255)), SECTION_LAYER, b"\0\0\0\0", None),
        ("Inner", None, SECTION_GROUP_CLOSED, b"mul ", None),
        ("Outer", None, SECTION_GROUP, b"\0\0\0\0", None),
    ]
    for i, (name, rgba, kind, blend, m) in enumerate(spec):
        records[i] = Layer(name=name.encode(), pixels=rgba, opacity=255, blend_mode=blend, section_divider_type=kind, mask=m)
    records[2].blending_ranges, records[2].blending_range_count = ranges, len(BLEND_IF)
    doc = Doc(w, h, 72.0, records, len(spec), None, 0, MINIPSD_RLE)
    out = bytearray()

    def collect(_user, data, size):
        out.extend(ctypes.string_at(data, size))
        return 1

    rc = lib.minipsd_write(ctypes.byref(doc), None, WRITE_FN(collect), None)
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
    expect("Inner closed", inner._record.tagged_blocks.get_data(b"lsct").kind.value, SECTION_GROUP_CLOSED)
    expect("Inner's layers", [l.name for l in inner], ["Deep"])
    masked = list(outer)[0]
    expect("mask", masked.has_mask(), True)
    if masked.has_mask():
        expect("mask rectangle", tuple(masked.mask.bbox), (8, 10, 40, 30))
        expect("mask colour", masked.mask.background_color, 0)
        expect("mask disabled", masked.mask.disabled, False)
        expect("mask pixels", masked.mask.topil().tobytes(), mask_px)
    br = masked._record.blending_ranges
    expect("Masked's blending ranges", [list(map(tuple, br.composite_ranges))] + [list(map(tuple, c)) for c in br.channel_ranges],
           [[psd_tools_range(s), psd_tools_range(d)] for s, d in BLEND_IF])
    return errors


# --- 16 bits, written from scratch ---------------------------------------------------------------

def deep(lib, path, compression=MINIPSD_RLE):
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
    records[0] = Layer(name=b"Gradient", pixels=samples(base), opacity=255)
    records[1] = Layer(name=b"Masked", pixels=samples(top), opacity=255, top=5, left=10, bottom=5 + lh, right=10 + lw,
                       mask=ctypes.pointer(mask))
    doc = Doc(w, h, 72.0, records, 2, None, 0, compression)
    doc.depth = 16
    out = bytearray()

    def collect(_user, data, size):
        out.extend(ctypes.string_at(data, size))
        return 1

    rc = lib.minipsd_write(ctypes.byref(doc), ctypes.byref(WriteOpts(0, deflate)), WRITE_FN(collect), None)
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
# PSB files, written back as PSB: lengths and row counts stay wide, long keys' blocks too.
KEPT_PSB = ["text.psb", "group.psb", "hidden-groups.psb", "layer_mask_data.psb", "unicode_pathname.psb",
            "placedLayer.psb", "layer_comps.psb", "masks.psb"]
KEPT_PSB_16 = ["16bit5x5.psb"]


def resave(lib, source, path, stored=False, fmt=0, zip_=False):
    """source through minipsd_read (stored, or with zip_ decoding ZIP channels through zlib) and
    minipsd_write (as fmt, or as the document's format; with zip_, what is encoded as ZIP with
    prediction, through zlib)."""
    data = source.read_bytes()
    doc = ctypes.POINTER(Doc)()
    lost = ctypes.c_uint32(0)
    opts = ReadOpts(int(stored), inflate if zip_ else INFLATE_FN()) if stored or zip_ else None
    rc = lib.minipsd_read(data, len(data), None, ctypes.byref(opts) if opts else None, ctypes.byref(doc),
                          ctypes.byref(lost))
    if rc != MINIPSD_OK:
        return rc, 0
    out = bytearray()

    def collect(_user, chunk, size):
        out.extend(ctypes.string_at(chunk, size))
        return 1

    if zip_:
        doc.contents.compression = MINIPSD_ZIP_PREDICTED
    rc = lib.minipsd_write(doc, ctypes.byref(WriteOpts(fmt, deflate)), WRITE_FN(collect), None)
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


def channel_size(rec, cid):
    """A channel's width and height: the layer's rectangle, or its mask's (-2), or its real mask's."""
    m = rec.mask_data
    if cid >= -1:
        return rec.right - rec.left, rec.bottom - rec.top
    if cid == -2:
        return m.right - m.left, m.bottom - m.top
    return m.real_right - m.real_left, m.real_bottom - m.real_top


def pixels(psd):
    """Every layer channel with an area, as psd-tools decodes it: by layer and channel id."""
    (recs, data), out = records(psd._record.layer_and_mask_information), {}
    for i, (rec, channels) in enumerate(zip(recs, data)):
        for c, d in zip(rec.channel_info, channels):
            w, h = channel_size(rec, c.id.value)
            if w > 0 and h > 0:
                out[i, c.id.value] = d.get_data(w, h, psd.depth, psd.version)
    return out


def compressions(psd):
    """How the layer channels with an area are compressed, all of them."""
    (recs, data), out = records(psd._record.layer_and_mask_information), set()
    for rec, channels in zip(recs, data):
        for c, d in zip(rec.channel_info, channels):
            w, h = channel_size(rec, c.id.value)
            if w > 0 and h > 0 and c.id.value >= -2:
                out.add(d.compression)
    return out


def same_pixels(before, after):
    """The pixels of before's channels in after: decoded, a layer without transparency gets one."""
    b = pixels(before)
    a = pixels(after)
    return b == {k: a.get(k) for k in b}


def check_kept(name, before, after, stored=False):
    """The content, as the spec's round trip rule has it: the header; every layer's blocks, every
    resource and every global block ('Lr16' aside: its layers are the file's layers), byte for
    byte as psd-tools writes them, in any order; every record's rectangle (an empty one in any
    form), names, opacity, blend key, clipping, flags, mask data, blending ranges and channels, in
    any order.  Stored: every channel's data as well, and the composite, every channel of it."""
    errors = []

    def same(what, a, b):
        if a != b:
            errors.append(f"{name}: {what} differ")

    def blocks(tagged):
        return {key: block.tobytes() for key, block in tagged.items() if getattr(key, "value", key) not in (b"Lr16", b"Lr32")}

    def resources(psd):
        return {key: resource.tobytes() for key, resource in psd.image_resources.items()}

    def header(psd):   # decoded, the composite is written R, G, B and transparency
        h = psd._record.header
        return h.channels if stored else None, h.height, h.width, h.depth, h.color_mode

    def rect(rec):
        empty = rec.bottom <= rec.top or rec.right <= rec.left
        return None if empty else (rec.top, rec.left, rec.bottom, rec.right)

    def mask(lmi):
        info = lmi.global_layer_mask_info
        return info.tobytes() if info else b"\0\0\0\0"   # none and an empty one (its length 0) alike

    same("header", header(before), header(after))
    same("resources", resources(before), resources(after))
    gb, ga = before._record.layer_and_mask_information, after._record.layer_and_mask_information
    same("global blocks", blocks(gb.tagged_blocks) if gb.tagged_blocks else {}, blocks(ga.tagged_blocks) if ga.tagged_blocks else {})
    same("global layer mask", mask(gb), mask(ga))
    (rb, cb), (ra, ca) = records(gb), records(ga)
    same("layer count", len(rb), len(ra))
    if stored:
        same("composite", before._record.image_data.get_data(before._record.header),
             after._record.image_data.get_data(after._record.header))
        vb, va, depth = before.version, after.version, before.depth

        def data(rec, c, d, version):
            if c.id.value >= -1 and (rec.right - rec.left) * (rec.bottom - rec.top) == 0:
                return d.data   # no area: nothing stored, the marker is the document's compression
            if vb == va:
                return d.compression, d.data   # byte for byte
            w, h = channel_size(rec, c.id.value)
            if w <= 0 or h <= 0:
                return d.compression, d.data   # no rows: no count table to differ
            return d.compression, d.get_data(w, h, depth, version)

        for i, (b, a, db, da) in enumerate(zip(rb, ra, cb, ca)):
            got = {c.id.value: data(a, c, d, va) for c, d in zip(a.channel_info, da)}
            same(f"layer {i} channel data", {c.id.value: data(b, c, d, vb) for c, d in zip(b.channel_info, db)}, got)
    for i, (b, a) in enumerate(zip(rb, ra)):
        same(f"layer {i} blocks", blocks(b.tagged_blocks), blocks(a.tagged_blocks))
        same(f"layer {i} mask data", b.mask_data.tobytes() if b.mask_data else None, a.mask_data.tobytes() if a.mask_data else None)
        same(f"layer {i} blending ranges", b.blending_ranges.tobytes(), a.blending_ranges.tobytes())
        ids = [c.id.value for c in b.channel_info]
        added = [] if -1 in ids or stored else [-1]   # decoded, a layer without transparency gets an opaque one
        same(f"layer {i} channels", sorted(added + ids), sorted(c.id.value for c in a.channel_info))
        same(f"layer {i} rectangle", rect(b), rect(a))
        same(f"layer {i} names", (b.name, blocks(b.tagged_blocks).get(b"luni")), (a.name, blocks(a.tagged_blocks).get(b"luni")))
        same(f"layer {i} opacity", b.opacity, a.opacity)
        same(f"layer {i} blend, clipping, flags", (b.blend_mode, b.clipping, b.flags), (a.blend_mode, a.clipping, a.flags))
    return errors


# --- every file: its content, and a second pass ------------------------------------------------------

def check_every_file(lib, files, out):
    """Every file minipsd_read takes (with stored), through minipsd_write: its content kept
    (check_kept), and a second pass changes no byte (the writer's own layout, the same each time).
    The bytes themselves may differ: the spec's round trip rule."""
    errors, count = [], 0
    for source in sorted(files.rglob("*.ps[db]")):
        name = source.relative_to(files).as_posix()
        path = out / ("every-" + name.replace("/", "__"))
        rc, _ = resave(lib, source, path, stored=True)
        if rc != MINIPSD_OK:
            continue                                         # refused: not a document minipsd takes
        errors += check_kept(name, PSDImage.open(source), PSDImage.open(path), stored=True)
        again = path.with_name("again-" + path.name)
        rc, _ = resave(lib, path, again, stored=True)
        if rc != MINIPSD_OK or again.read_bytes() != path.read_bytes():
            errors.append(f"{name}: a second pass changes the bytes (rc {rc})")
        count += 1
    print(f"every file: {count} read and written back")
    return errors


# --- ZIP channels through the caller's inflate --------------------------------------------------------

def check_zip(lib, source, out):
    """source, a file minipsd wrote, its layer and mask channels re-encoded by psd-tools as ZIP, with
    and without prediction: refused without an inflate, and through zlib the same pixels."""
    errors = []
    for compression in (Compression.ZIP, Compression.ZIP_WITH_PREDICTION):
        name = f"{source.stem}-zip{compression.value}"
        path, back = out / f"{name}.psd", out / f"{name}-decoded.psd"
        psd, zipped = PSDImage.open(source), 0
        recs, data = records(psd._record.layer_and_mask_information)
        for rec, channels in zip(recs, data):
            for c, d in zip(rec.channel_info, channels):
                w, h = channel_size(rec, c.id.value)
                if w > 0 and h > 0 and c.id.value >= -2:
                    raw = d.get_data(w, h, psd.depth, psd.version)
                    d.compression = compression
                    d.set_data(raw, w, h, psd.depth, psd.version)
                    c.length = 2 + len(d.data)
                    zipped += 1
        psd.save(path)
        rc, _ = resave(lib, path, back)
        if not zipped or rc != MINIPSD_ERR_UNSUPPORTED:
            errors.append(f"{name}: {zipped} ZIP channels; without an inflate, minipsd_read returned {rc}")
        rc, _ = resave(lib, path, back, zip_=True)
        if rc != MINIPSD_OK:
            errors.append(f"{name}: through zlib, minipsd_read or minipsd_write returned {rc}")
        elif not same_pixels(PSDImage.open(source), PSDImage.open(back)):
            errors.append(f"{name}: through zlib, the layers decode to other pixels")
    return errors


# --- PSB through the reader --------------------------------------------------------------------------

MINIPSD_MORE, ZIP = 1, (2, 3)
VIEW_BYTES = 1024   # room for a minipsd_layer_view, opaque here but for its rectangle, which comes first


def target(buffer, width, sample, planes):
    """planes samples a pixel, interleaved in buffer, `sample` bytes each; the other planes NULL."""
    t = Target()
    for k in range(planes):
        t.channel[k] = Plane(ctypes.addressof(buffer) + k * sample, planes * sample, planes * sample * width)
    return t


def interleaved(planes, width, height, depth):
    """psd-tools' planes as the file stores them (None for an absent transparency) as minipsd gives
    them: host-order samples, a float at 32 bits, a byte of 0 or 1 (1 black) at 1 bit."""
    import numpy as np
    if depth == 1:
        row = (width + 7) // 8
        arrays = [np.unpackbits(np.frombuffer(p, np.uint8)[:row * height].reshape(height, row), axis=1, count=width).ravel()
                  if p is not None else np.ones(width * height, np.uint8) for p in planes]
        return np.stack(arrays, axis=-1).tobytes()
    big = np.dtype({8: "u1", 16: ">u2", 32: ">f4"}[depth])
    full = {8: 255, 16: 65535, 32: 1.0}[depth]
    arrays = [np.frombuffer(p, big)[:width * height] if p is not None else np.full(width * height, full, big)
              for p in planes]
    return np.stack(arrays, axis=-1).astype(big.newbyteorder("=")).tobytes()


def color_channels(header, alpha):
    """The mode's colour channels, as minipsd_info.color_channels: multichannel's are all but the
    composite's transparency."""
    return {3: 3, 9: 3, 4: 4, 7: header.channels - alpha}.get(int(header.color_mode), 1)


RES_RESOLUTION_INFO, RES_VERSION_INFO, ERR_NOT_FOUND = 1005, 1057, -10
RES_GRID_AND_GUIDES, RES_ALPHA_NAMES, RES_UNICODE_ALPHA_NAMES = 1032, 1006, 1045
RES_ALPHA_IDENTIFIERS, RES_TRANSPARENCY_INDEX = 1053, 1047
SETTINGS = [  # a layer's fixed-layout blocks: key, the field minipsd parses it into, its present bit
    (b"lyid", "layer_id", 0x01), (b"lclr", "sheet_color", 0x02), (b"iOpa", "fill_opacity", 0x04),
    (b"lspf", "protection_flags", 0x08), (b"knko", "knockout", 0x10), (b"clbl", "blend_clipped_elements", 0x20),
    (b"infx", "blend_interior_elements", 0x40)]


def items(lib, fn, res, item):
    """Every item a minipsd_resource_next_* walk gives, or None when it fails."""
    pos, out = ctypes.c_size_t(0), []
    while (rc := fn(ctypes.byref(res), ctypes.byref(pos), ctypes.byref(item))) == MINIPSD_MORE:
        out.append(type(item).from_buffer_copy(item) if isinstance(item, ctypes.Structure) else item.value)
    return out if rc == MINIPSD_OK else None


def as_int(value):
    """psd-tools' value of a small element, an int or an enum, as an int."""
    return int(getattr(value, "value", value))


def check_settings(lib, view, rec, name):
    """A layer's fixed-layout blocks: present where psd-tools has the block, the same value."""
    lib.minipsd_layer_block_settings.argtypes = [ctypes.c_void_p, ctypes.POINTER(BlockSettings)]
    s = BlockSettings()
    rc = lib.minipsd_layer_block_settings(view, ctypes.byref(s))
    if rc != MINIPSD_OK:
        return [f"{name}: minipsd_layer_block_settings returned {rc}"]
    blocks = rec.tagged_blocks
    want = {key: as_int(blocks.get_data(key)) for key, _, _ in SETTINGS if blocks and blocks.get_data(key) is not None}
    got = {key: getattr(s, field) for key, field, bit in SETTINGS if s.present & bit}
    return [] if got == want else [f"{name}: block settings {got}, psd-tools says {want}"]


def utf8(lib, text):
    """A UTF-16BE view as minipsd converts it: measured, then converted."""
    need = ctypes.c_size_t()
    lib.minipsd_utf16be_to_utf8(ctypes.byref(text), None, 0, ctypes.byref(need))
    out = ctypes.create_string_buffer(need.value)
    lib.minipsd_utf16be_to_utf8(ctypes.byref(text), out, need.value, None)
    return out.value.decode()


def check_resources(lib, reader, psd, name):
    """The typed views of resources: what minipsd finds and parses is what psd-tools reads, a name up
    to its first U+0000 as minipsd ends it."""
    lib.minipsd_reader_find_resource.argtypes = [ctypes.c_void_p, ctypes.c_uint16, ctypes.POINTER(Resource)]
    lib.minipsd_resource_resolution_info.argtypes = [ctypes.POINTER(Resource), ctypes.POINTER(ResolutionInfo)]
    lib.minipsd_resource_version_info.argtypes = [ctypes.POINTER(Resource), ctypes.POINTER(VersionInfo)]
    lib.minipsd_utf16be_to_utf8.argtypes = [ctypes.POINTER(Bytes), ctypes.c_char_p, ctypes.c_size_t,
                                            ctypes.POINTER(ctypes.c_size_t)]
    lib.minipsd_resource_grid_and_guides.argtypes = [ctypes.POINTER(Resource), ctypes.POINTER(GridAndGuides)]
    lib.minipsd_resource_transparency_index.argtypes = [ctypes.POINTER(Resource), ctypes.POINTER(ctypes.c_uint16)]
    for fn in ("next_guide", "next_alpha_name", "next_alpha_identifier"):
        getattr(lib, f"minipsd_resource_{fn}").argtypes = [ctypes.POINTER(Resource), ctypes.POINTER(ctypes.c_size_t),
                                                         ctypes.c_void_p]
    lib.minipsd_resource_id_name.argtypes = [ctypes.c_uint16]
    lib.minipsd_resource_id_name.restype = ctypes.c_char_p
    known = {int(m) for m in ResourceId}   # psd-tools' table, Adobe's, without the middle of its ranges
    errors = [f"{name}: resource {int(rid)} has no name" for rid in psd.image_resources.keys()
              if int(rid) in known and lib.minipsd_resource_id_name(int(rid)) is None]
    for rid in (RES_RESOLUTION_INFO, RES_VERSION_INFO, RES_GRID_AND_GUIDES, RES_ALPHA_NAMES, RES_UNICODE_ALPHA_NAMES,
                RES_ALPHA_IDENTIFIERS, RES_TRANSPARENCY_INDEX):
        res = Resource()
        rc = lib.minipsd_reader_find_resource(reader, rid, ctypes.byref(res))
        theirs = psd.image_resources.get_data(rid)
        if rc != (MINIPSD_OK if theirs is not None else ERR_NOT_FOUND):
            errors.append(f"{name}: resource {rid}: minipsd_reader_find_resource returned {rc}")
            continue
        if theirs is None:
            continue
        if rid == RES_RESOLUTION_INFO:
            ri = ResolutionInfo()
            rc = lib.minipsd_resource_resolution_info(ctypes.byref(res), ctypes.byref(ri))
            got = (round(ri.horizontal_resolution * 65536), ri.horizontal_resolution_unit, ri.width_unit,
                   round(ri.vertical_resolution * 65536), ri.vertical_resolution_unit, ri.height_unit)
            want = (theirs.horizontal, int(theirs.horizontal_unit), int(theirs.width_unit),
                    theirs.vertical, int(theirs.vertical_unit), int(theirs.height_unit))
        elif rid == RES_GRID_AND_GUIDES:
            g = GridAndGuides()
            rc = lib.minipsd_resource_grid_and_guides(ctypes.byref(res), ctypes.byref(g))
            guides = items(lib, lib.minipsd_resource_next_guide, res, Guide())
            got = (g.version, g.horizontal_grid, g.vertical_grid,
                   guides and [(x.location & 0xFFFFFFFF, x.direction) for x in guides])
            want = (theirs.version, theirs.horizontal, theirs.vertical, [tuple(x) for x in theirs.data])
        elif rid in (RES_ALPHA_NAMES, RES_UNICODE_ALPHA_NAMES):
            names = items(lib, lib.minipsd_resource_next_alpha_name, res, Bytes())
            rc = MINIPSD_OK if names is not None else -1
            got = [utf8(lib, n) if rid == RES_UNICODE_ALPHA_NAMES else ctypes.string_at(n.data, n.size).decode("mac_roman")
                   for n in names or []]
            want = [n.split("\0")[0] if rid == RES_UNICODE_ALPHA_NAMES else n for n in theirs]
        elif rid == RES_ALPHA_IDENTIFIERS:
            got = items(lib, lib.minipsd_resource_next_alpha_identifier, res, ctypes.c_uint32())
            rc = MINIPSD_OK if got is not None else -1
            want = list(theirs)
        elif rid == RES_TRANSPARENCY_INDEX:
            index = ctypes.c_uint16()
            rc = lib.minipsd_resource_transparency_index(ctypes.byref(res), ctypes.byref(index))
            got, want = index.value, as_int(theirs)
        else:
            vi = VersionInfo()
            rc = lib.minipsd_resource_version_info(ctypes.byref(res), ctypes.byref(vi))
            got = (vi.version, bool(vi.has_real_merged_data), utf8(lib, vi.writer_name), utf8(lib, vi.reader_name),
                   vi.file_version)
            want = (theirs.version, bool(theirs.has_composite), theirs.writer.split("\0")[0],
                    theirs.reader.split("\0")[0], theirs.file_version)
        if rc != MINIPSD_OK or got != want:
            errors.append(f"{name}: resource {rid} is {got} (rc {rc}), psd-tools says {want}")
    return errors


def check_decoded(lib, path):
    """The reader over a file of any mode and depth: each layer and the composite (as stored, with
    its matte) decode to what psd-tools decompresses from the same file, the mode's colour channels
    and transparency, as many as a target holds."""
    lib.minipsd_reader_init.argtypes = [ctypes.POINTER(ctypes.c_void_p), ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p,
                                        ctypes.c_size_t, ctypes.c_void_p, ctypes.c_void_p]
    lib.minipsd_reader_next_layer.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    lib.minipsd_decode_layer.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(Target), ctypes.POINTER(DecodeOpts)]
    lib.minipsd_decode_composite.argtypes = [ctypes.c_void_p, ctypes.POINTER(Target), ctypes.POINTER(DecodeOpts)]
    data = path.read_bytes()
    buf = ctypes.create_string_buffer(data, len(data))
    need = ctypes.c_size_t()
    lib.minipsd_reader_workspace_size(ctypes.byref(need))
    workspace = ctypes.create_string_buffer(need.value)
    reader, info = ctypes.c_void_p(), ctypes.create_string_buffer(512)
    rc = lib.minipsd_reader_init(ctypes.byref(reader), buf, len(data), workspace, need.value, None, info)
    if rc != MINIPSD_OK:
        return [f"{path.name}: minipsd_reader_init returned {rc}"]
    psd = PSDImage.open(path)
    record = psd._record
    header, errors = record.header, check_resources(lib, reader, psd, path.name)
    depth, version = header.depth, header.version
    sample = max(depth // 8, 1)
    opts = DecodeOpts(0, 0, 1, depth, inflate)
    layer_info = record._get_layer_info()
    records = layer_info.layer_records if layer_info else []
    merged = any("SAVING_MERGED_TRANSPARENCY" in str(k) for k in (psd.tagged_blocks.keys() if psd.tagged_blocks else []))
    alpha = int(bool(layer_info is not None and layer_info.layer_count < 0) or merged)
    colors = color_channels(header, alpha)
    planes = min(colors + 1, PLANES)
    ids = [c for c in range(min(colors, PLANES))] + ([-1] if colors < PLANES else [])
    for i, rec in enumerate(records):
        view = ctypes.create_string_buffer(VIEW_BYTES)
        rc = lib.minipsd_reader_next_layer(reader, view)
        if rc != MINIPSD_MORE:
            errors.append(f"{path.name}: layer {i}: minipsd_reader_next_layer returned {rc}")
            break
        errors += check_settings(lib, view, rec, f"{path.name}: layer {i}")
        top, left, bottom, right = struct.unpack_from("=4i", view.raw)
        if rec.bottom >= rec.top and rec.right >= rec.left and (top, left, bottom, right) != (rec.top, rec.left, rec.bottom, rec.right):
            errors.append(f"{path.name}: layer {i}: rectangle {(top, left, bottom, right)}")
        w, h = right - left, bottom - top
        channels = {c.id: d for c, d in zip(rec.channel_info, layer_info.channel_image_data[i])}
        if w * h == 0:
            continue
        scratch = ctypes.create_string_buffer(((w * depth + 7) // 8) * h)   # one channel, for ZIP
        opts.scratch, opts.scratch_size = ctypes.addressof(scratch), len(scratch)
        out = ctypes.create_string_buffer(w * h * planes * sample)
        t = target(out, w, sample, planes)
        rc = lib.minipsd_decode_layer(reader, view, ctypes.byref(t), ctypes.byref(opts))
        want = [channels[c].get_data(w, h, depth, version) if c in channels else None for c in ids]
        if rc != MINIPSD_OK or out.raw != interleaved(want, w, h, depth):
            errors.append(f"{path.name}: layer {i}: decodes differently from psd-tools (rc {rc})")
    if record.image_data.compression not in ZIP:
        w, h = header.width, header.height
        out = ctypes.create_string_buffer(w * h * planes * sample)
        t = target(out, w, sample, planes)
        rc = lib.minipsd_decode_composite(reader, ctypes.byref(t), ctypes.byref(opts))
        try:
            stored = record.image_data.get_data(header)
        except ValueError:   # a malformed file (the reader test's knownBad): minipsd must refuse it too
            return errors + ([f"{path.name}: psd-tools refuses the composite, minipsd decodes it"] if rc == MINIPSD_OK else [])
        want = [stored[k] if k < colors else stored[colors] if alpha and colors < len(stored) else None for k in range(planes)]
        if rc != MINIPSD_OK or out.raw != interleaved(want, w, h, depth):
            errors.append(f"{path.name}: the composite decodes differently from psd-tools (rc {rc})")
    return errors


def main():
    lib = ctypes.CDLL(sys.argv[1])
    lib.minipsd_write.argtypes = [ctypes.POINTER(Doc), ctypes.c_void_p, WRITE_FN, ctypes.c_void_p]
    lib.minipsd_write.restype = ctypes.c_int
    lib.minipsd_read.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_void_p,
                                 ctypes.POINTER(ctypes.POINTER(Doc)), ctypes.POINTER(ctypes.c_uint32)]
    lib.minipsd_read.restype = ctypes.c_int
    lib.minipsd_doc_free.argtypes = [ctypes.POINTER(Doc)]
    lib.minipsd_doc_free.restype = None
    out = pathlib.Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)

    lib.minipsd_resource_id_name.argtypes = [ctypes.c_uint16]
    lib.minipsd_resource_id_name.restype = ctypes.c_char_p
    errors = [f"resource {int(m)} ({m.name}) has no name in minipsd" for m in ResourceId
              if lib.minipsd_resource_id_name(int(m)) is None]   # every id psd-tools knows, Adobe's table
    for (name, w, h, dpi, compression, layers), fmt in [(d, 0) for d in DOCUMENTS] + [(d, MINIPSD_PSB) for d in PSB_DOCUMENTS]:
        path = out / (f"{name}.psb" if fmt else f"{name}.psd")
        rc = write(lib, path, w, h, dpi, compression, layers, fmt)
        if rc != MINIPSD_OK:
            errors.append(f"{name}: minipsd_write returned {rc}")
            continue
        psd = PSDImage.open(path)
        found = check(name, psd, w, h, dpi, layers)
        if psd.version != (2 if fmt else 1):
            found.append(f"{name}: version {psd.version}")
        if compression >= MINIPSD_ZIP and compressions(psd) != {Compression(compression)}:
            found.append(f"{name}: layer channels compressed as {compressions(psd)}")
        errors += found
        print(f"{name}: {'FAILED' if found else 'ok'}")

    name, w, h, dpi, compression, layers = WIDE
    path = out / f"{name}.psb"
    rc = write(lib, path, w, h, dpi, compression, layers, MINIPSD_PSB)
    found = [f"{name}: minipsd_write returned {rc}"] if rc != MINIPSD_OK else check_wide(PSDImage.open(path), layers)
    errors += found
    print(f"{name}: {'FAILED' if found else 'ok'}")

    path = out / "groups-masks.psd"
    rc, mask_px = groups_and_mask(lib, path)
    found = [f"groups-masks: minipsd_write returned {rc}"] if rc != MINIPSD_OK else check_groups_and_mask(PSDImage.open(path), mask_px)
    errors += found
    print(f"groups-masks: {'FAILED' if found else 'ok'}")

    for name, compression in (("deep-16", MINIPSD_RLE), ("deep-16-zip-predicted", MINIPSD_ZIP_PREDICTED)):
        path = out / f"{name}.psd"
        rc, expected = deep(lib, path, compression)
        found = [f"{name}: minipsd_write returned {rc}"] if rc != MINIPSD_OK else check_deep(PSDImage.open(path), expected)
        if rc == MINIPSD_OK and compression and compressions(PSDImage.open(path)) != {Compression(compression)}:
            found.append(f"{name}: layer channels compressed as {compressions(PSDImage.open(path))}")
        errors += found
        print(f"{name}: {'FAILED' if found else 'ok'}")

    for source in (out / "groups-masks.psd", out / "deep-16.psd"):
        found = check_zip(lib, source, out)
        errors += found
        print(f"zip {source.stem}: {'FAILED' if found else 'ok'}")

    files = pathlib.Path(sys.argv[3]) if len(sys.argv) > 3 and sys.argv[3] else None
    if files is None:
        print("kept content: skipped, no psd-tools files (MINIPSD_PSD_TOOLS_FILES)")
    # Decoded (ZIP through zlib), and 16-bit ones also as stored.
    kept = [(n, False) for n in KEPT_FILES + KEPT_PSB + KEPT_16 + KEPT_PSB_16] + [(n, True) for n in KEPT_16 + KEPT_PSB_16]
    # As it is, then in the other format: PSD as PSB, PSB as PSD, every channel stored, so that its
    # RLE count table is what changes width.
    converted = [((n, True), True) for n in dict.fromkeys(n for n, _ in kept)]
    for (name, stored), convert in [(k, False) for k in kept] + converted if files else []:
        source = files / name
        version = 1 if source.suffix == ".psd" else 2
        target = 3 - version if convert else version
        kind = "converted" if convert else "resaved" if stored else "decoded"
        path = out / f"{kind}-{source.stem}{'.psd' if target == 1 else '.psb'}"
        rc, lost = resave(lib, source, path, stored, target if convert else 0, zip_=True)
        if rc != MINIPSD_OK:
            errors.append(f"{name}: minipsd_read or minipsd_write returned {rc}")
            continue
        before, after = PSDImage.open(source), PSDImage.open(path)
        found = check_kept(name, before, after, stored)
        if after.version != target:
            found.append(f"{name}: written as version {after.version}, not {target}")
        for lb, la in zip(before.descendants(), after.descendants()):   # mask pixels, as psd-tools decodes them
            if lb.has_mask() and lb.mask.topil() is not None and lb.mask.topil().tobytes() != la.mask.topil().tobytes():
                found.append(f"{name}: '{lb.name}' mask pixels differ")
        if kind == "decoded" and not same_pixels(before, after):
            found.append(f"{name}: decoded and written back, the layers' pixels differ")
        errors += found
        print(f"{kind} {name} (lost {lost}): {'FAILED' if found else 'ok'}")
    if files:
        errors += check_every_file(lib, files, out)
    for path in sorted(list(files.rglob("*.psd")) + list(files.rglob("*.psb"))) if files else []:
        found = check_decoded(lib, path)
        errors += found
        print(f"decoded {path.relative_to(files)}: {'FAILED' if found else 'ok'}")
    for e in errors:
        print(e)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
