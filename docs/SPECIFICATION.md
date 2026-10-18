# Specification

The current contract of minipsd: what it is, what it writes and reads, and what must not break.
This file describes the **current state** only; history goes to [CHANGELOG.md](CHANGELOG.md),
planned work to [TODO.md](TODO.md).

| Section | Covers |
|---|---|
| [Purpose](#purpose) | what minipsd is and is not |
| [PSD in five minutes](#psd-in-five-minutes) | the parts of the file format minipsd touches |
| [Form](#form) | header layout, language, configuration macros |
| [API](#api) | types, functions, defaults, error codes |
| [Memory and ownership](#memory-and-ownership) | whose each buffer is, its size, lifetime and where it may live |
| [What goes into the file](#what-goes-into-the-file) | the exact PSD subset written |
| [What is read](#what-is-read) | which files are described, which decoded, and what is left as raw data |
| [Working with libvips](#working-with-libvips) | the buffers they share; rendering layers with libvips |
| [Consumer requirements](#consumer-requirements) | guarantees Platemaker depends on |
| [Verification](#verification) | how a format change is proven correct |
| [Distribution and SBOM](#distribution-and-sbom) | how consumers obtain and declare minipsd |
| [Extensibility rules](#extensibility-rules) | how to grow without breaking callers |

---

## Purpose

minipsd is a **header-only, dependency-free C99 library that writes and reads layered
Photoshop (PSD) files**. It exists because no header-only PSD writer was available; it was
extracted from [Platemaker](https://github.com/ShadobaDev/PlateMaker), which uses it to save
canvas templates.

- **No hidden state:** the writer and the reader live in memory the caller lends them (a
  workspace) and hold nothing else; there is no global state and nothing to initialise or shut
  down. `minipsd_write` takes a complete document and produces a complete file in one call.
- **Writes a subset, reads any PSD:** it writes 8- and 16-bit RGB documents
  ([What goes into the file](#what-goes-into-the-file)); it describes every PSD of version 1 and
  decodes the pixels of 8- and 16-bit RGB ones ([What is read](#what-is-read)).
- **Lightweight first, extensible second:** a feature is added only when it can be added
  without making the simple case harder.

## PSD in five minutes

A PSD file is a sequence of five sections, all integers **big-endian**:

| # | Section | What minipsd puts there |
|---|---|---|
| 1 | **File header** (26 bytes): signature `8BPS`, version (1 = PSD, 2 = PSB), channel count, height, width, bit depth, colour mode | version 1, 4 channels, 8 or 16 bits, RGB |
| 2 | **Colour mode data**: a length, then a palette for indexed/duotone modes | empty (length 0) |
| 3 | **Image resources**: a length, then `8BIM` blocks, each with a numeric ID | 1005 resolution, optionally 1039 ICC profile |
| 4 | **Layer and mask information**: layer records (bounds, channels, blend mode, opacity, flags, name, *additional layer info* blocks such as `luni`), then each layer's channel pixel data | the layers |
| 5 | **Image data**: the merged ("composite") image, used by readers that ignore layers and for thumbnails | the composite |

Channel pixel data is either raw or **PackBits RLE**: a per-row byte-count table followed by
the compressed rows; a 16-bit sample is two bytes, high first, and RLE packs a row's bytes.
Photoshop compresses 16-bit layers with ZIP instead, and keeps their records in a global block
`Lr16` after an empty layer info. Sides are capped at 30 000 px in PSD; PSB (version 2) lifts that to
300 000 px and widens several length fields.

Reference: Adobe's *Photoshop File Formats Specification*. When it and real readers disagree,
real readers win (see [Verification](#verification)).

## Form

- **Header-only, stb style:** `#define MINIPSD_IMPLEMENTATION` in exactly one translation unit
  before `#include "minipsd.h"`; include it plainly everywhere else.
- **One public header:** `include/minipsd.h`, included as `"minipsd.h"`, is the whole API: every
  type, constant, macro and function declaration. A change to it is an API change.
- **The implementation** sits in `include/minipsd/` (`base.h`, `writer.h`, and shared internals
  in `detail/`) and comes in only under `MINIPSD_IMPLEMENTATION`; nothing there is public.
  [CODE-ARCH.md](CODE-ARCH.md#files-and-modules) has the layout and its rules.
- **C99, also valid C++** (C++17/20): explicit `void *` casts, no designated initialisers;
  declarations are `extern "C"` under C++.
- **No dependencies** beyond the C standard library.
- **Source is pure ASCII.**
- **Warning-free** under MSVC `/W4 /WX /permissive-` and gcc/clang
  `-Wall -Wextra -Wpedantic -Werror`, compiled both as C and as C++. As C, also under
  `-Wconversion -Wsign-conversion -Wshadow -Wcast-qual -Wundef -Wdouble-promotion
  -Wnull-dereference -Wformat=2 -Wstrict-prototypes -Wmissing-prototypes -Wold-style-definition`,
  and with gcc `-Wcast-align -Wduplicated-cond -Wlogical-op` (the list is in
  `tests/CMakeLists.txt`).
- Configuration macros (define before the implementation include):

| Macro | Effect |
|---|---|
| `MINIPSD_STATIC` | every function is `static`, nothing is exported |
| `MINIPSD_MALLOC` / `MINIPSD_FREE` | replace `malloc` / `free` (define both) |
| `MINIPSD_NO_MALLOC` | omit every allocation: `minipsd_write` and `<stdlib.h>`; the pull writer stays |
| `MINIPSD_NO_STDIO` | accepted, no effect: minipsd never includes `<stdio.h>` |

- **No I/O.** minipsd opens no file, pipe or socket; bytes go to the caller's buffer or callback.
  Complete programs for a file, a buffer and a non-blocking pipe are in `examples/`, built and
  run by CI, not installed.

## API

The declarations, with every parameter and return code, are in
[minipsd.h](../include/minipsd.h); this section says what each part is for.

| Function | For |
|---|---|
| `minipsd_doc_check(doc)` | validating a document without writing it |
| `minipsd_writer_workspace_size(doc, &size)` | the workspace the pull writer needs for this document |
| `minipsd_writer_init(&w, doc, workspace, size)` | building the writer inside the workspace and running the sizing pass; nothing is allocated |
| `minipsd_writer_total(w)` | the file size, known before the first byte |
| `minipsd_writer_fill(w, buf, cap, &produced)` | the next bytes, into the caller's buffer: `MINIPSD_MORE` until `MINIPSD_OK` |
| `minipsd_write(doc, fn, user)` | the whole file through a callback, on a workspace from `MINIPSD_MALLOC`; absent under `MINIPSD_NO_MALLOC` |
| `minipsd_reader_workspace_size(&size)` | the reader's workspace: fixed, known before the file |
| `minipsd_reader_init(&r, data, size, workspace, workspace_size, limits, &info)` | the file's description and its resources, from the whole file or a prefix of it |
| `minipsd_reader_needed(r)` | after `MINIPSD_ERR_SHORT`: the least buffer size with which the call goes further |
| `minipsd_reader_next_resource(r, &res)` | the image resources in file order, each a raw view: `MINIPSD_MORE` per resource, then `MINIPSD_OK` |
| `minipsd_reader_next_layer(r, &layer)` | the layer records, bottom first, with views of their channel data: `MINIPSD_MORE` per layer, then `MINIPSD_OK` |
| `minipsd_layer_name(layer, utf8, cap, &needed)` | a layer's name as UTF-8 (`luni`, else the legacy name as Latin-1) |
| `minipsd_layer_next_block(layer, &pos, &block)` | a layer's additional-information blocks, each a raw view |
| `minipsd_target_rgba(&target, rgba, row_stride)` | a target for interleaved RGBA, the common case |
| `minipsd_target_rgba16(&target, rgba, row_stride)` | the same for 16-bit samples, `uint16_t` |
| `minipsd_decode_layer(r, layer, target, opts)` | a layer's pixels, or a band of its rows, as stored (opacity and visibility not applied), into the caller's planes; 8- or 16-bit RGB, raw or PackBits |
| `minipsd_decode_composite(r, target, opts)` | the composite (merged) image, or a band of its rows, un-matted to straight alpha unless `keep_matte` |
| `minipsd_decode_mask(r, layer, plane, opts)` | a layer's mask, or a band of its rows, into one plane |
| `minipsd_read_size(data, size, limits, opts, &block_size)` | the block `minipsd_read_into` needs for a whole file |
| `minipsd_read_into(data, size, limits, opts, block, block_size, &doc, &lost)` | a whole file into a document in the caller's block, everything copied, so the file may go; `lost` says what it could not carry; `opts->stored` keeps each layer's channels as stored instead of decoding them |
| `minipsd_read(data, size, limits, opts, &doc, &lost)`, `minipsd_doc_free(doc)` | the same in one `MINIPSD_MALLOC` block, and its free; absent under `MINIPSD_NO_MALLOC` |
| `minipsd_error_string(code)` | a short English text for a return code |

The reader describes any version-1 PSD, whatever its colour mode or depth; it decodes the pixels
of 8- and 16-bit RGB files, layers and composite.

- **Depth.** A decode call writes samples of `opts->depth`: 8 (and 0, `NULL` opts), or 16, a
  `uint16_t` in host order. It must be the file's depth; there is no conversion. The steps of a
  plane count bytes either way.
- **Un-matting.** The file stores the composite's colour matted against white; the reader undoes
  it, `c = max - (max - s) * max / a` rounded and clamped (`max` is 255, or 65535 at 16 bits),
  `0` where `a == 0`. That is exact where `a == max` and within about `max / (2a)` levels below:
  matting has already merged nearby colours. It reads each alpha back from the target, so it needs
  the target's `A` plane. `keep_matte` returns the samples as stored.
- **A composite without transparency** (a positive layer count) decodes as opaque; a fourth
  channel is then not transparency and is not read.

| Type | What it is |
|---|---|
| `minipsd_doc`, `minipsd_layer` | the document: canvas, dpi, layers bottom first, ICC profile, compression |
| `minipsd_workspace` | `void`: caller memory lent to the writer or the reader, sized by its `*_workspace_size` function |
| `minipsd_writer` | opaque; built by `minipsd_writer_init` inside the workspace |
| `minipsd_write_fn` | the callback of `minipsd_write`: non-zero to go on, 0 to abort |
| `minipsd_reader` | opaque; built by `minipsd_reader_init` inside the workspace |
| `minipsd_limits` | what the reader refuses: largest side (canvas, and layers when set), most layers, most bytes one decode call writes; zero fields keep the format's caps |
| `minipsd_info` | what the file says about itself: size, depth, colour mode, dpi, ICC profile, layer count |
| `minipsd_bytes` | a read-only view into the caller's buffer |
| `minipsd_resource`, `minipsd_layer_view`, `minipsd_block` | an image resource, a layer record, an additional-information block, as views |
| `minipsd_target`, `minipsd_plane` | where decoded samples go: per channel R, G, B, A a pointer, a pixel step and a row step; any layout |
| `minipsd_decode_opts` | the rows to decode, the target's `depth` and, for the composite, `keep_matte`; `NULL` for all rows, 8-bit, un-matted |

**Defaults and field meanings**
- A zero-initialised document selects RLE (`MINIPSD_RLE = 0`).
- `opacity = 0` means a fully transparent layer; it is **not** "default opaque".
- `rgba = NULL` is an empty layer; `name = NULL` is an empty name.
- Layers are listed bottom first.

**Return codes**: errors are negative, so `rc < 0` is the failure test for every function.

| Code | Value | Meaning |
|---|---|---|
| `MINIPSD_OK` | 0 | success |
| `MINIPSD_MORE` | 1 | not an error: `minipsd_writer_fill` has more bytes, an iterator produced an item, or a name needs more room; call again |
| `MINIPSD_ERR_ARG` | -1 | NULL document or callback, no layers, ICC size without data, or a dpi that 16.16 fixed point cannot hold (see below) |
| `MINIPSD_ERR_SIZE` | -2 | a side is 0 or above `MINIPSD_MAX_SIDE`, or a section is longer than its 32-bit length field allows (see below) |
| `MINIPSD_ERR_WRITE` | -3 | the callback returned 0 |
| `MINIPSD_ERR_ALLOC` | -4 | `MINIPSD_MALLOC` returned NULL |
| `MINIPSD_ERR_OPEN` | -5 | not returned by minipsd; reserved for the caller's file code (`examples/write_file.c`) |
| `MINIPSD_ERR_FORMAT` | -6 | reader: not a well-formed PSD |
| `MINIPSD_ERR_UNSUPPORTED` | -7 | reader: a valid PSD outside what minipsd reads: a PSB (version 2); for decoding, a file not 8- or 16-bit RGB, of another depth than `opts->depth`, or a ZIP-compressed channel |
| `MINIPSD_ERR_SHORT` | -8 | reader: the buffer ends inside data the call needs; read more, `minipsd_reader_needed` says how much |
| `MINIPSD_ERR_LIMIT` | -9 | reader: over `minipsd_limits`: the canvas's or a layer's side, the layer count, or the bytes of a decode call; the file is not known to be bad |

## Memory and ownership

minipsd owns nothing the caller passes in and never copies a document it writes. It allocates
only inside `minipsd_write`, freeing it before it returns, and inside `minipsd_read`, one block
the caller frees with `minipsd_doc_free`. Everything else is the caller's memory:

| Name | Owner | Size | Must stay valid | May live in |
|---|---|---|---|---|
| `minipsd_doc`, `minipsd_layer[]` | caller; borrowed | `sizeof`; `layer_count` layers | the call; for the pull writer, from init for as long as the writer is used | stack, static, heap |
| raw document data: layer pixels (`rgba`) or stored channels (`stored`), names, ICC profile, the supplied `composite`, kept `extra`, `resources` and `layer_tail` | caller; borrowed, read in place, never copied | a layer's rectangle times 4 samples, of 1 or 2 bytes (or its channels' stored size); `width * height * 4` samples for the composite | as `minipsd_doc` | wherever the application keeps it |
| `minipsd_workspace` of the writer | caller; lent to the writer | `minipsd_writer_workspace_size()` | for as long as the writer is used | heap; stack or static sized for the largest document |
| `minipsd_writer` | inside `minipsd_workspace`; opaque | part of the workspace | as the workspace | the caller keeps only the pointer |
| raw PSD bytes | passed to the callback of `minipsd_write()` | pieces of up to 4 KiB, in order | the callback copies what it keeps | the callback decides |
| output buffer, `buf` of `minipsd_writer_fill()` | caller | any `cap`, 1 included; a few rows (64 KiB) keeps re-encoding negligible | the call | stack, static or heap |
| `minipsd_write()` internals | minipsd: the workspace through `MINIPSD_MALLOC` / `MINIPSD_FREE`, a 4 KiB chunk on the stack | `minipsd_writer_workspace_size()` | the call | heap and stack |
| the PSD to read (`data` of `minipsd_reader_init()`) | caller; borrowed, read in place, never copied | the file, or a prefix of it | for as long as the reader or any view from it is used, unchanged | wherever it was read, mapped or received |
| `minipsd_workspace` of the reader | caller; lent to the reader | `minipsd_reader_workspace_size()`, the same for every file | for as long as the reader is used | stack, static or heap |
| `minipsd_reader` | inside its workspace; opaque | part of the workspace | as the workspace | the caller keeps only the pointer |
| `minipsd_info`, `minipsd_resource`, `minipsd_layer_view`, `minipsd_block` | caller; filled by the reader | `sizeof` | their views point into the PSD: valid as the PSD | stack |
| layer name, `utf8` of `minipsd_layer_name()` | caller | `needed` bytes, the NUL included | the call | stack, static or heap |
| decoded pixels, the planes of `minipsd_target` | caller; written, never read | per plane, `width x rows` samples at its steps: the layer's rectangle and the band | the call | anywhere; the library writes those samples and nothing else |
| the PSD read by `minipsd_read*()` | caller; read during the call, never kept | the whole file | the call only: the document holds copies | anywhere; free it as soon as the call returns |
| the document from `minipsd_read_into()` and all it holds | in the caller's block, laid out by minipsd | `minipsd_read_size()` | for as long as the document is used | heap, stack or static, any alignment |
| the document from `minipsd_read()` | one `MINIPSD_MALLOC` block, the document at its start | as `minipsd_read_size()` | until `minipsd_doc_free()` | heap |

- **The document is read-only and frozen while in use.** The pull writer sizes everything in
  `minipsd_writer_init`; a document changed afterwards would no longer match the sizes already
  recorded.
- **Nothing to release** but a document from `minipsd_read`. The library holds no other memory
  between calls: the writer ends with its workspace, which the caller may free or reuse for
  another document (if large enough).
- **No global or static mutable state.** Writers and readers in separate threads never interact;
  one workspace serves one writer or reader at a time.

## What goes into the file

- PSD version 1, RGB, 4 channels (R, G, B, transparency), 8 bits per channel or, with `depth`
  16, 16: every sample (`rgba`, mask `pixels`, `composite`) is then a `uint16_t` in host order,
  written big-endian. The layers go in the layer info at both depths, never in `Lr16`.
- **Layers:**
  - each at its rectangle (`top, left, bottom, right`; all zero is the full canvas), which may
    reach past the canvas or lie wholly outside it; a side of at most 30 000 px, since RLE row
    counts are 16 bits;
  - or empty: no pixels, a rectangle without area (by default 0x0), channels that hold no data;
  - name twice: a legacy Pascal string (ASCII, every non-ASCII character replaced by `?`) and a
    `luni` block in UTF-16 with surrogate pairs;
  - opacity;
  - visibility: flag bit `0x02` set means hidden; the other flag bits as given (`flags`);
  - the blend key as given, `norm` by default (the keys: [Blend modes](#blend-modes)), and
    clipping as given;
  - after the writer's own `luni`, the blocks kept from a file (`extra`), as they are, except a
    kept `luni`: the name wins;
  - channel data encoded from `rgba`, or copied as it is from `stored` (any compression the
    file had, ZIP included), so a layer that was not changed is not encoded again; a stored
    layer without transparency gets an opaque transparency channel;
  - groups (`kind`), bottom first: a divider (`lsct` type 3), the group's layers, then the
    group's top record (type 1 open, 2 closed) with its name, visibility and mode in its `lsct`
    block (`blend`, Pass Through by default; the record keeps `norm`). A kept `lsct` of the same
    kind is written instead, as it is. Groups must nest;
  - a layer mask (`mask`): its rectangle, colour outside it and flags in the mask data, then
    what was kept after them (parameters, the real mask's rectangle) or two bytes of padding; its
    channel (id -2) encoded from `pixels` or copied from `stored`, and a kept real mask (id -3)
    as it is.
- **Composite image:**
  - the caller's `composite` when set (the full canvas, straight alpha), whatever the layers
    hold: flattening layers is rendering, the caller's domain;
  - otherwise composed as a **best effort, with no guarantee**: the visible layers combined with
    straight-alpha "over", each at its rectangle, clipped to the canvas. Blend modes, masks,
    clipping, groups and effects are not drawn;
  - transparency is the first extra channel, signalled by writing the layer count as a
    **negative** number, as the format specifies;
  - **colour is matted against white:** `c*a + max*(1-a)`, `max` being 255 or 65535. Photoshop
    stores it this way and readers subtract the white back out. The writer mattes, a supplied composite too, unless
    `composite_matted` says it already is: then it is written byte for byte.
- **Image resources:**
  - 1005 ResolutionInfo: dpi as 16.16 fixed point, unit inches; a kept 1005 that says the same
    dpi is written instead, with its own units and vertical resolution;
  - 1039 ICC profile, only when `icc` is set;
  - then the resources kept from a file (`resources`), in order, with their names, except 1005
    and 1039: `dpi` and `icc` win;
  - a resource of odd length is padded to even.
- **After the layer info:** the kept `layer_tail` (global layer mask, global blocks) as it is,
  or an empty global layer mask.
- **Compression:** PackBits RLE (runs from 3 bytes, packets up to 128 bytes) or raw.
  Row byte counts are computed in a separate sizing pass, so output streams forward through the
  callback and never seeks back.
- **Allocations:** see [Memory and ownership](#memory-and-ownership).
- **Limits** (all checked before anything is written):
  - each side 1 .. 30 000 px, otherwise `MINIPSD_ERR_SIZE`;
  - 1 .. 32 767 layers, otherwise `MINIPSD_ERR_ARG`;
  - dpi must round to 1 .. 2^32 - 1 in 16.16 fixed point, i.e. from about 0.0000076 to just below
    65 536 (NaN and infinity excluded), otherwise `MINIPSD_ERR_ARG`;
  - PSD version 1 stores section lengths in 32 bits, so the layer section (records and channel
    data) and the image resources (resolution + ICC profile) must each stay under 4 GiB,
    otherwise `MINIPSD_ERR_SIZE`. Example: two full 30 000 x 30 000 layers in RAW are about
    7.2 GB and are refused; the same in RLE usually fits. The composite has no length field.

## What is read

The reader works on bytes the caller holds, the whole file or its start, and hands out views of
them; nothing is copied or allocated. Results on real files from other programs:
[COMPATIBILITY.md](COMPATIBILITY.md).

- **Which files:**
  - every PSD of version 1 is described, whatever its colour mode (bitmap, grey, indexed, RGB,
    CMYK, multichannel, duotone, Lab) or depth (1, 8, 16, 32 bits);
  - pixels are decoded from 8- and 16-bit RGB files only, raw or PackBits; any other file, and
    a ZIP-compressed channel (Photoshop's at 16 bits), gives `MINIPSD_ERR_UNSUPPORTED` from the
    decode call;
  - the layers of a file whose layer info is empty come from its global block `Lr16` or `Lr32`
    (Photoshop's 16- and 32-bit files);
  - a PSB (version 2) is refused by `minipsd_reader_init` with `MINIPSD_ERR_UNSUPPORTED`; four
    wrong bytes at the start are `MINIPSD_ERR_FORMAT`.
- **Header and resources:** size, channels, depth, colour mode; dpi from resource 1005
  (horizontal, 0 without it); the ICC profile from 1039. Every resource, these two included, is
  also returned as a raw view, with its name, in file order.
- **Layer records**, bottom first:
  - the rectangle; one stored inverted (Photoshop's fill layers) comes back empty;
  - opacity, visibility, the blend mode's key, clipping and the flag bits, as stored;
  - the name: the `luni` block when there is one, otherwise the legacy name read as Latin-1;
    an unpaired surrogate becomes U+FFFD and a U+0000 ends the name;
  - the channels red, green, blue and transparency as views of their stored data; any other
    channel (alpha, spot, a mask channel without mask data) is counted, not read;
  - what the record is (`kind`, from its `lsct` block): a layer, a group's top record, a divider;
  - its mask: rectangle, colour, flags, the rest of the mask data, the mask channel and the real
    mask channel as views; `minipsd_decode_mask` decodes the mask's pixels;
  - every additional-information block as a raw view (key and bytes), the known ones included.
- **Pixels come as stored:** a layer's opacity, visibility, blend mode and mask are not applied;
  they are in its view, and so is the group structure.
- **The composite:** un-matted to straight alpha (see [API](#api)), or as stored with
  `keep_matte`; a file whose layer count is positive has no transparency in it, and its composite
  decodes as opaque.
- **The layer tail:** the global layer mask information and the global additional-information
  blocks that follow the layer info, as one raw view (`minipsd_info.layer_tail`), an `Lr16` the
  layers came from included. Global blocks are padded to four bytes past the length they state.
- **Not read:** the colour mode data (palettes).
- **As a document** (`minipsd_read`): 8- and 16-bit RGB files, at their depth; each layer's
  rectangle, name, opacity, visibility, blend key, clipping, flags, kind, mask, decoded pixels
  (or, with `opts->stored`, its channels as stored, ZIP ones too) and blocks, the composite as
  stored (`composite_matted`), the dpi (72 when none is stored), the ICC profile, every resource
  and the layer tail without the `Lr16` its layers came from, all copied into one block. A file without layers gets one,
  "Background", holding its image. Anything else is reported in `lost` (`MINIPSD_LOST_*`), never
  dropped silently.
- **A prefix is enough** for what lies inside it: `MINIPSD_ERR_SHORT` with
  `minipsd_reader_needed` says how much more a call needs.
- **Limits:** every length is checked against the buffer and the section it lies in; the
  caller's `minipsd_limits` add the canvas's and the layers' side, the layer count and the bytes
  one decode call writes, all checked before anything is decoded.

## Blend modes

A layer's blend mode is a four-character key in its record. minipsd writes `minipsd_layer.blend`
as given and reads it into `minipsd_layer_view.blend`; it does not blend with it: the composite
it composes draws every mode as Normal (supply the composite for the real result). Photoshop's
keys, in its menu order; the tests write each and read it back with `psd-tools` (`pass` on a
group, where it belongs):

| Group | Mode | Key |
|---|---|---|
| Normal | Normal | `norm` |
| | Dissolve | `diss` |
| Darken | Darken | `dark` |
| | Multiply | `mul ` |
| | Color Burn | `idiv` |
| | Linear Burn | `lbrn` |
| | Darker Color | `dkCl` |
| Lighten | Lighten | `lite` |
| | Screen | `scrn` |
| | Color Dodge | `div ` |
| | Linear Dodge (Add) | `lddg` |
| | Lighter Color | `lgCl` |
| Contrast | Overlay | `over` |
| | Soft Light | `sLit` |
| | Hard Light | `hLit` |
| | Vivid Light | `vLit` |
| | Linear Light | `lLit` |
| | Pin Light | `pLit` |
| | Hard Mix | `hMix` |
| Inversion | Difference | `diff` |
| | Exclusion | `smud` |
| Cancellation | Subtract | `fsub` |
| | Divide | `fdiv` |
| Component | Hue | `hue ` |
| | Saturation | `sat ` |
| | Color | `colr` |
| | Luminosity | `lum ` |
| Groups only | Pass Through | `pass` |

A key outside this list is written as given too; what a reader makes of it is its own choice.

## Working with libvips

minipsd reads and writes PSD; it does not process images. [libvips](https://www.libvips.org/)
does, and its memory images are minipsd's buffers, with no conversion:

| | minipsd | libvips, 8-bit RGBA |
|---|---|---|
| layout | interleaved RGBA, rows top down, `width * 4` bytes per row | bands interleaved, rows contiguous |
| sample | `uint8_t` | `VIPS_FORMAT_UCHAR` |
| alpha | straight | straight; premultiplying is an explicit `vips_premultiply` |
| colour | sRGB, or the document's ICC profile | `interpretation` sRGB, the profile in `icc-profile-data` |

- **From libvips to minipsd:** `vips_image_write_to_memory` gives a buffer that is a layer's
  `rgba` as it is; a 3-band image takes `vips_bandjoin_const1(image, 255)` first.
- **From minipsd to libvips:** decode with `minipsd_target_rgba`, then wrap the buffer with
  `vips_image_new_from_memory(buffer, size, width, height, 4, VIPS_FORMAT_UCHAR)`, no copy.
- **Rendering layers:** `vips_composite` takes the layers with their positions (the layer
  rectangle); opacity is a multiplication of the alpha band, hidden layers are left out. Blend
  modes with a libvips counterpart:

  | PSD key | libvips | PSD key | libvips |
  |---|---|---|---|
  | `norm` | `VIPS_BLEND_MODE_OVER` | `div ` | `VIPS_BLEND_MODE_COLOUR_DODGE` |
  | `mul ` | `VIPS_BLEND_MODE_MULTIPLY` | `idiv` | `VIPS_BLEND_MODE_COLOUR_BURN` |
  | `scrn` | `VIPS_BLEND_MODE_SCREEN` | `hLit` | `VIPS_BLEND_MODE_HARD_LIGHT` |
  | `over` | `VIPS_BLEND_MODE_OVERLAY` | `sLit` | `VIPS_BLEND_MODE_SOFT_LIGHT` (the PDF formula, close to Photoshop's) |
  | `dark` | `VIPS_BLEND_MODE_DARKEN` | `diff` | `VIPS_BLEND_MODE_DIFFERENCE` |
  | `lite` | `VIPS_BLEND_MODE_LIGHTEN` | `smud` | `VIPS_BLEND_MODE_EXCLUSION` |
  | `lddg` | `VIPS_BLEND_MODE_ADD` | | |

  The other modes (linear burn, darker and lighter colour, vivid, linear and pin light, hard
  mix, subtract, divide, hue, saturation, colour, luminosity, dissolve) need code of the
  caller's own. libvips computes in floating point, Photoshop in integers, so results differ by
  about one level.
- **Memory:** libvips computes lazily, in strips; minipsd takes a whole layer, so a large
  pipeline materialises each layer before writing. Reading in bands of rows
  (`minipsd_decode_opts`) fits libvips' strips.
- minipsd never depends on libvips.

## Consumer requirements

Platemaker is the first consumer. Breaking any of these breaks it:

1. **Writing through a callback to memory.** Platemaker hashes (SHA-256) the bytes in memory
   before writing the file, because reading back a just-written file on Google Drive can return
   stale data. The API must never force writing straight to a file.
2. **Deterministic output:** the same document always yields the same bytes (Platemaker
   re-verifies template hashes). No timestamps, no uninitialised padding.
3. **Composite matted against white.** Without it Procreate, GIMP and `psd-tools` show the wrong
   margin colour.
4. **`MINIPSD_ERR_SIZE` stays a distinct code** (Platemaker maps it to a readable error).
5. **`MINIPSD_STATIC` keeps working and minipsd pulls in no `stdio`**, so the host exports no
   minipsd symbol; `MINIPSD_NO_STDIO`, which Platemaker defines, stays accepted.
6. **The template layout stays expressible:** a "Template" layer plus an optional empty "Layer"
   above it.

## Verification

- **A self-written reader is not enough.** The first version wrote the composite without the
  white matte, and the test reader shared the same wrong assumption, so tests passed;
  `psd-tools` then read the margin pink as `(255, 0, 105)` instead of `(255, 105, 180)`.
  **Every change to the bytes written is checked with an independent reader**: `psd-tools`
  automatically (`psd-tools-roundtrip`, required in CI), and by hand in GIMP, Krita, Photopea
  and Procreate when relevant.
- The round trip builds its documents and expected results in Python and calls the real writer
  through a shared build of minipsd, so no expectation comes from the code under test. It
  checks size, depth, mode, dpi, layer names (Unicode), opacity, visibility, bounds, every layer
  pixel exactly, and the composite against an independent "over" within the matting tolerance.
  Removing the white matte makes it fail with the `(255, 0, 105)` pink. A 16-bit document is
  checked the same way, every channel's samples as psd-tools decodes them.
- 8-bit rounding through matting and un-matting may shift a colour by one level (the pink
  returns as `(255, 105, 179)`); Photoshop behaves the same, so this is expected, not a bug.
- Verified by hand: GIMP opens the output; Procreate opens a 1600x10240 template as a canvas
  with the correct dpi.
- The same tests also run instrumented in CI: under AddressSanitizer and UBSan, under clang-tidy
  (`.clang-tidy`, findings are errors), and with coverage of `minipsd.h`: **lines must stay at
  100 %** and branches at 95 % or more, so new code comes with the test that executes it,
  error paths included (`test_alloc_failure.cpp` fails each allocation in turn).
- **Single file:** the same tests also run against the single-file `minipsd.h` the release
  attaches, and `minipsd.h` is compiled alone and then with its implementation, with no internal
  function left unused.
- **Golden bytes:** `test_golden.cpp` writes a fixed set of documents and compares each file's
  SHA-256 with the hash recorded from 0.1.0. A change meant to keep the output must leave every
  hash alone; changing a hash there is the explicit, reviewed sign of a byte change.
- **Reader round trip:** each golden file is read back, its document rebuilt from the reader's
  views and decoded pixels, and written again: the bytes must be the same
  (`encode(decode(x)) == x`).
- **Files from other programs:** every `.psd` in `tests/data/` (committed, origin and licence in
  its README) and `tests/fixtures/` (local, never committed) is read whole and in parts, and every
  layer and the composite of an 8- or 16-bit RGB one must decode, but for ZIP-compressed channels,
  which must go through `minipsd_read` as stored and come back byte for byte; `examples/read_file.c`
  runs on each.
  With `MINIPSD_PSD_TOOLS_FILES` the same test reads psd-tools' 279 test files, fetched at a
  pinned release; a file known to be malformed is listed with the error it must give.
- **Fuzzing:** a libFuzzer target takes any bytes through every reader call under ASan and UBSan,
  run locally (the `linux-fuzz` preset), never in CI. Each input it ever failed on is kept in
  `tests/fuzz/regressions/` and replayed by the ordinary tests on every toolchain.

## Distribution and SBOM

> **Status:** implemented and covered by the `consumer-*` tests; the release assets come from
> the release workflow ([GITHUB-RELEASE.md](GITHUB-RELEASE.md)).

minipsd must be usable in each of these ways, and in each one a consumer must be able to state
**name, version, licence, supplier and source** of the copy it uses:

| Integration | What the consumer does | Where the version comes from |
|---|---|---|
| **Raw copy** | copies `include/` into its tree, or the release's single-file `minipsd.h` | `MINIPSD_VERSION_*` macros and SPDX tags inside the headers |
| **git submodule** | `add_subdirectory(minipsd)` | `minipsd_VERSION` from `project()` |
| **CMake FetchContent** | `FetchContent_Declare(minipsd GIT_TAG X.Y.Z)` | `minipsd_VERSION` from `project()` |
| **Installed package** (distro, `cmake --install`) | `find_package(minipsd CONFIG)` or `pkg-config minipsd` | `minipsd-config-version.cmake` / `minipsd.pc` |

Rules that make this work:

- **The headers are self-describing**, because a raw copy carries nothing else: every file has
  `SPDX-License-Identifier: MIT` and `SPDX-FileCopyrightText:`; `minipsd.h` also has the version
  macros, the project URL, and the full MIT licence text at the end of the file.
- **One version source: the header.** `CMakeLists.txt` reads the `MINIPSD_VERSION_*` macros
  from `minipsd.h` and passes them to `project(VERSION)`, so the version cannot drift between
  the header, the CMake package, `minipsd.pc` and the SBOM fragment.
- **CMake target:** `minipsd::minipsd`, an `INTERFACE` library, identical whether it comes
  from `add_subdirectory`, FetchContent or `find_package`. The package version file is
  `ARCH_INDEPENDENT` (header-only).
- **Consuming must not cost the consumer anything:** tests, presets and test-only dependencies
  are built only when minipsd is the top-level project (`PROJECT_IS_TOP_LEVEL`).
- **SBOM fragment:** the build produces an SPDX 2.3 document for minipsd itself (single
  package, no dependencies, PURL `pkg:github/ShadobaDev/minipsd@x.y.z`, every header listed with
  its SHA-1 and SHA-256, and the package verification code derived from them), installed to
  `share/doc/minipsd/` and exposed to CMake consumers as the target property
  `MINIPSD_SBOM_FILE` on `minipsd::minipsd`. A property rather than a variable, because
  it reads the same for `add_subdirectory`, FetchContent and `find_package`.
- **Installed layout** (everything is architecture-independent, so nothing goes to `lib/`):
  `include/minipsd.h`, `include/minipsd/`, `share/cmake/minipsd/`, `share/pkgconfig/minipsd.pc`,
  `share/doc/minipsd/` (licence, SBOM). The package version file accepts the same MINOR only
  (`SameMinorVersion`: pre-1.0, a breaking change bumps the MINOR).
- **Releases are git tags `X.Y.Z`** (bare semver, no `v` prefix); a GitHub release attaches
  `include/` joined into one `minipsd.h` (by `tools/amalgamate.cmake`) and its `.sha256`, for
  raw-copy users who prefer a single file.

## Extensibility rules

Rules for adding features without breaking existing callers:

- **The library version is the contract version.** Structs carry no `version` or `size` field:
  the header is compiled into its consumer, so the struct layout always matches the code using
  it. A break is announced by the version of the whole library (pre-1.0: the MINOR).
- **Zero means "previous behaviour".** Callers zero-initialise structs; a new field must keep
  the old output when it is 0/NULL.
- **New struct fields go at the end.** Positional initialisers (C99 without designated
  initialisers, C++) must keep compiling. gcc and clang flag them under `-Wextra`
  (`-Wmissing-field-initializers`), so the examples and the manual name the fields.
- **New error codes and enum values are added, never renumbered.**
- **The composite is not a renderer.** A feature that changes how layers look (blend modes,
  masks, groups) is written and read as data. The composite the writer composes is a best
  effort: it learns such a feature only on demand, and the docs say which features it leaves
  out.
- **Optional subsystems hide behind macros**, the way `MINIPSD_NO_MALLOC` does, so the minimal
  build stays minimal. The reader needs none: it allocates nothing, and a function the program
  does not call costs nothing once linked.
