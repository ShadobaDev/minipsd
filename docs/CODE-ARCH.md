# Code architecture

> **Status: accepted design, not implemented yet.**
> [SPECIFICATION.md](SPECIFICATION.md) stays the contract until a stage below lands and moves its
> part of this file into the specification. [USE-CASES.md](USE-CASES.md) lists the situations
> this design serves; [ROADMAP.md](ROADMAP.md) orders the work.

This file describes:
- what the library is meant to become;
- how its code is divided into files and modules;
- the rules that keep it dependency-free;
- the order in which it grows.

The C++ facade is out of scope here: it is a distant layer on top of this one and changes
nothing below it.

| Section | Covers |
|---|---|
| [Principle](#principle) | structure to bytes, bytes to structure; I/O belongs to the caller |
| [Dependency rules](#dependency-rules) | what the core may include, call and allocate |
| [Files and modules](#files-and-modules) | the header split, each module's struct and functions |
| [I/O scenarios](#io-scenarios) | memory, blocking stream, non-blocking stream |
| [Writer](#writer) | the encoder as a resumable state machine |
| [Reader](#reader) | the decoder as zero-copy views over a buffer |
| [Features through the modules](#features-through-the-modules) | where each planned feature lands |
| [Distribution of a multi-file library](#distribution-of-a-multi-file-library) | amalgamated single header, install tree, SBOM |
| [How development proceeds](#how-development-proceeds) | stages, the rules every stage follows |
| [Decisions](#decisions) | what the review settled |

---

## Principle

minipsd converts in two directions, and nothing else:

```
encode:  minipsd_doc (structs, plain types)  ->  PSD bytes in a buffer
decode:  PSD bytes in a buffer               ->  structs and views, pixels into caller buffers
```

- **The library never touches files, pipes or sockets.** Bytes leave it in two ways:
  - into a buffer the caller passes in;
  - through a function pointer with a fixed signature, which the caller implements.
- **Every concrete I/O implementation lives in `examples/` and `tests/`, never in `include/`.**
  This includes the stdio file writer, the memory writer and the non-blocking pipe writer.
  The examples are built and run in CI, so they stay correct, but they are not part of the API.
- **The same document always produces the same bytes**, whichever I/O path carries them. A
  test checks that every path gives identical output.

## Dependency rules

The rules apply to `include/` only. Tests and examples may use anything.

| Allowed in the core | Not allowed |
|---|---|
| `<stddef.h>`, `<stdint.h>` | `<stdio.h>`, OS headers (`<windows.h>`, `<unistd.h>`) |
| `<string.h>`: `memcpy`, `memset`, `memcmp` only | `<math.h>` (libm), locale, time, threads |
| `float` / `double` arithmetic without libm calls | global or `static` mutable state |
| `<stdlib.h>` only behind the 0.1-compatible `minipsd_write` (see [Writer](#writer)) | allocation anywhere else |

- **Objects with state are opaque** (the writer, later the reader): `minipsd.h` declares the
  type only, and `init` builds the object inside the caller's workspace, a `minipsd_workspace *`
  (`typedef void`: a name for memory whose size depends on the document). Plain data buffers stay
  `void *` or `uint8_t *`.
- **Public structs carry data only:**
  - fixed-width integers, `size_t`, `double`, `const` pointers to bytes, and nested structs of
    the same;
  - no function pointers. A callback is always a function parameter.
- **Memory comes from the caller.** A function that needs scratch space works like this:
  1. a `*_workspace_size` call reports how many bytes it needs;
  2. the caller passes a buffer of that size;
  3. the library aligns inside it with pointer arithmetic, so any `void *` works.
- **Borrowed, never owned.** Every pointer the caller gives must stay valid and unchanged for as
  long as the call or the object that holds it lives. The library never frees or copies it.
- **Re-entrant.** No hidden state: two writers or readers in two threads never interact. The
  decode calls only read the reader (but for `needed`, set on `MINIPSD_ERR_SHORT`), so they may
  run at once on one reader; a field they write would break that (linux-tsan checks it).

## Files and modules

**`include/minipsd.h` is the whole public API**: every type, constant, macro and function
declaration, with its documentation, in sections (version and linkage, return codes, document,
writer, and later reader). It is the one file a user reads. Everything under `include/minipsd/`
is implementation: it comes in only under `MINIPSD_IMPLEMENTATION` and promises nothing.

A module is one C "class": a struct plus the functions whose first parameter is a pointer to
it. Its declarations are a section of `minipsd.h`; its implementation is a file in
`include/minipsd/`, and the internal helpers it shares sit in `include/minipsd/detail/`.

```
include/
  minipsd.h                    the public API; under MINIPSD_IMPLEMENTATION it includes the files below
  minipsd/
    base.h                     return-code texts
    writer.h                   the encoder (pull), plus the 0.1-compatible minipsd_write (push)
    views.h                    typed views: a kept resource or a layer's blocks parsed into plain values
    reader.h                   the decoder: views into the caller's bytes
    read.h                     minipsd_read: a whole file into one standalone document, on the reader
    detail/
      bytes.h                  big-endian put/get on uint8_t pointers, bounds-checked cursor
      samples.h                samples per colour mode and depth: a mode's colour channels, row
                               bytes, a stored byte's place in memory, ZIP prediction, 1-bit rows
      utf.h                    UTF-8 <-> UTF-16 for layer names and other text
      packbits.h               PackBits encode, decode, size bound; strided source and target
      composite.h              blending one row "over", white matte and un-matte
      format.h                 the file's layout: each structure's fields and sizes, and what PSD and PSB
                               differ in (length and row-count widths, long keys, blocks)
examples/                      the I/O implementations; built and run in CI, never installed
  hello_write.c, hello_read.c  the shortest writer and reader: the manual's Getting started
  demo.h                       the document the other writers write (the golden "template-rle")
  write_file.c                 stdio, UTF-8 path on Windows (minipsd_write_file until 0.2.0)
  write_memory.c               exact-size buffer from minipsd_writer_total
  write_nonblocking.c          O_NONBLOCK pipe + poll (POSIX)
  read_file.c                  whole file into memory, described, every layer and the composite decoded
  resave.c                     minipsd_read with stored, one layer changed, written back: the round trip
tests/
  test_<module>.cpp            one file per module; detail modules are tested directly
  fuzz/                        the reader's libFuzzer target, run locally (linux-fuzz preset);
                               replay.c runs it on regressions/, the inputs that once failed
tools/
  amalgamate.cmake             joins include/ into one file, a release artifact
```

| Module (section of `minipsd.h`) | Struct | Functions (sketch) | Implementation |
|---|---|---|---|
| Version, linkage, return codes | `minipsd_bytes { const uint8_t *data; size_t size; }` | `minipsd_error_string`; new codes, added never renumbered: `MINIPSD_MORE = 1` (positive: not an error, call again), `MINIPSD_ERR_FORMAT = -6`, `MINIPSD_ERR_UNSUPPORTED = -7`, `MINIPSD_ERR_SHORT = -8`, `MINIPSD_ERR_LIMIT = -9`, `MINIPSD_ERR_NOT_FOUND = -10` | `base.h` |
| Document | `minipsd_doc`, `minipsd_layer` (unchanged from 0.1) | `minipsd_doc_check(doc, NULL)`: the validation `minipsd_write` does today, now callable on its own | `writer.h` today; its own file if the reader shares it |
| Writer | `minipsd_writer` (opaque, built in the workspace) | `minipsd_writer_workspace_size`, `_init`, `_total`, `_fill`; `minipsd_write` | `writer.h`; detail: `bytes`, `utf`, `packbits`, `composite` |
| Reader | `minipsd_reader` (opaque, like the writer), `minipsd_info`, `minipsd_resource`, `minipsd_layer_view`, `minipsd_target` (of `minipsd_plane`), `minipsd_decode_opts`, `minipsd_limits` | `minipsd_reader_workspace_size`, `_init`, `_needed`, `_next_resource`, `_find_resource`, `_next_layer`, `minipsd_layer_name`, `minipsd_decode_layer`, `minipsd_decode_composite`, `minipsd_decode_mask`, `minipsd_target_rgba`, `minipsd_target_rgba16` | `reader.h`; detail: `bytes`, `utf`, `packbits`, `composite` |
| Typed views | `minipsd_resolution_info`, `minipsd_version_info`, `minipsd_grid_and_guides`, `minipsd_guide`, `minipsd_block_settings`; `MINIPSD_RES_*`, `MINIPSD_PRESENT_*` | `minipsd_resource_*` (a resource from the reader or a document in, a struct out; lists walked with a cursor), `minipsd_layer_block_settings` (a layer view in), `minipsd_utf16be_to_utf8`, `minipsd_resource_id_name` (Adobe's table of ids, looked up from index id - 1000 down); no reader | `views.h`; detail: `bytes`, `format`, `utf` |
| Document from a file | `MINIPSD_LOST_*` | `minipsd_read_size`, `minipsd_read_into`, `minipsd_read`, `minipsd_doc_free` | `read.h`, on the public reader (and its record offsets, to tell what is lost) |

Rules for the split:

- **A public declaration lives only in `minipsd.h`.** Adding, changing or removing anything
  there is an API change and is judged by the semver rules; a change under `minipsd/` is not.
  `grep MINIPSDDEF include/minipsd.h` lists every function.
- **`minipsd.h` needs nothing before it**, and including it a second time after
  `MINIPSD_IMPLEMENTATION` adds the implementation in that file.
- **One implementation define for the whole library.**
  - `MINIPSD_IMPLEMENTATION` is defined once, as today.
  - Each implementation file is guarded by its own `MPSD__<MODULE>_DONE`, so it is built once
    however often `minipsd.h` is included.
  - `MINIPSD_STATIC` keeps working; its functions are marked possibly unused, since a file calls
    only some.
- **No unused internals.** The full implementation, compiled with extern linkage, leaves no
  `static` function unused: that is a warning, and warnings are errors. Without the reader or
  the writer (`MINIPSD_NO_READER`, `_NO_WRITER`) the helpers in `detail/` are marked possibly
  unused (`MPSD__SHARED`), so this check still holds for the full build. `minipsd.h` includes
  `detail/` before either part, so the single file keeps them whichever part is left out.
- **Optional subsystems hide behind macros in `minipsd.h`**, the way `MINIPSD_NO_MALLOC`,
  `MINIPSD_NO_READER` and `MINIPSD_NO_WRITER` do.
- **Promotion is additive.** `packbits` and `composite` stay internal until a consumer needs
  them. Making one public means adding a declaration to `minipsd.h`, a PATCH change, never a
  rename of anything existing.
- **Implementation sections follow the order of the PSD file** they write or read, as
  [CODING_STYLE_C.md](CODING_STYLE_C.md) already requires; that rule now applies per file.

## I/O scenarios

The file is always written front to back and never seeks, so every scenario reduces to one of
three:

| Scenario | Write | Read |
|---|---|---|
| **Memory** (whole buffer, `mmap`, upload body) | `minipsd_writer_total` gives the exact size; one `minipsd_writer_fill` call fills the buffer | `minipsd_reader_init` over the buffer |
| **Blocking stream** (file, blocking pipe or socket) | `minipsd_write(doc, NULL, fn, user)`: the library calls `fn` until done | the caller reads everything into memory, then as above |
| **Non-blocking stream** (`O_NONBLOCK`, event loop, async I/O) | `minipsd_writer_fill` into the caller's buffer whenever the target is writable; the library never waits | as above; a streaming parser is not planned (see [Reader](#reader)) |

The one function pointer type stays the one from 0.1:

```c
typedef int (*minipsd_write_fn)(void *user, const void *data, size_t size);  /* non-zero: ok */
```

There is no read callback. A PSD cannot be decoded without random access into its layer
section, so reading always starts from a buffer. Collecting a stream into a buffer is the
caller's job, shown in `examples/read_file.c`.

## Writer

The encoder becomes a resumable state machine. The caller drives it, and it produces as many
bytes as the caller has room for.

```c
int      minipsd_writer_workspace_size(const minipsd_doc *doc, size_t *size);
int      minipsd_writer_init (minipsd_writer **w, const minipsd_doc *doc, minipsd_workspace *workspace, size_t size);
uint64_t minipsd_writer_total(const minipsd_writer *w);              /* the file size, known after init */
int      minipsd_writer_fill (minipsd_writer *w, void *buf, size_t cap, size_t *produced);
                             /* MINIPSD_MORE: call again; MINIPSD_OK: the file is complete */
```

- **`init` does all the sizing work.** It runs today's counting pass: every layer channel's
  compressed size and every composite row's size. So `total` is exact before the first byte,
  which a buffer target or a `Content-Length` header needs. All validation happens here too,
  so `fill` cannot fail on the document.
- **The writer is opaque and lives in the workspace.** `minipsd.h` declares only
  `typedef struct minipsd_writer minipsd_writer;`; the struct is defined in `minipsd/writer.h`,
  and `init` builds it at the aligned start of the workspace and returns a pointer to it. So the
  workspace is the writer's whole memory, nothing is allocated, and a change to the writer's
  fields is not a change to `minipsd.h`, the contract.
- **The workspace holds:**
  - the writer itself;
  - the per-channel size table (`layer_count * 4` values);
  - the composite row sizes (`4 * height` values);
  - one composite row (`width * 4` bytes).

  That is `minipsd_write`'s allocations plus the composite row sizes, which it recomputes.
- **Cursor.** The writer's state is a position in a fixed sequence of small pieces: the head, each
  layer record, each layer channel's compression, row count and row, the tail, each composite row
  count and row. It also tracks how many bytes of the current piece have been handed out.
- **No staging buffer.** `fill` produces the current piece again from its start, through a sink
  that copies only the bytes not yet handed out and only as many as fit. A piece is one row at
  most (the ICC profile is copied, not re-encoded), so a call that stops inside a row costs one
  more encoding of that row: nothing with a buffer of a few rows, quadratic in the row only for
  tiny caps. The composite row being produced is blended once and kept. A staging buffer would
  remove that cost at the price of workspace; it is left out until a caller needs tiny caps.
  - Any `cap >= 1` works, so a caller with a 1-byte buffer still gets the whole file.
  - A non-blocking caller writes as much of `buf` as the target accepts and keeps the rest for
    the next round. The library never needs to know about partial writes.
- **CPU, not waiting.** `init` and `fill` do work proportional to the image and never block on
  I/O. An event loop that cannot afford the compression time runs them on a worker thread, as
  with any encoder.
- **`minipsd_write` keeps its 0.1 signature and behaviour.**
  - It allocates the workspace through `MINIPSD_MALLOC`, runs `init`, then calls `fill` and
    `fn` in a loop.
  - `MINIPSD_NO_MALLOC` removes it, together with `<stdlib.h>`, for callers that want no heap at
    all; they write that loop themselves, as `examples/` shows.
- **Byte-identical to 0.1.0.** The state machine reorders the code, not the output. The golden
  test (see [How development proceeds](#how-development-proceeds)) holds it to that.

## Reader

The decoder parses a buffer in place. It allocates nothing and copies nothing except the pixels
it decodes into caller buffers.

```c
int    minipsd_reader_workspace_size(size_t *size);
int    minipsd_reader_init (minipsd_reader **r, const void *data, size_t size,
                            minipsd_workspace *workspace, size_t workspace_size,
                            const minipsd_limits *limits,          /* NULL: the defaults */
                            minipsd_info *info);
uint64_t minipsd_reader_needed(const minipsd_reader *r);           /* after MINIPSD_ERR_SHORT */

int minipsd_reader_next_resource(const minipsd_reader *r, size_t *pos, minipsd_resource *res);  /* MORE / OK */
int minipsd_reader_next_layer   (minipsd_reader *r, minipsd_layer_view *layer);  /* MORE: got one; OK: no more */
int minipsd_layer_name          (const minipsd_layer_view *layer, char *utf8, size_t cap, size_t *needed);
int minipsd_layer_next_block    (const minipsd_layer_view *layer, size_t *pos, minipsd_block *block);  /* MORE / OK */
int minipsd_layer_blending_range(const minipsd_layer_view *layer, uint32_t i, minipsd_blending_range *range);

int minipsd_decode_layer    (minipsd_reader *r, const minipsd_layer_view *layer,      /* r: for needed */
                             const minipsd_target *target, const minipsd_decode_opts *opts);
int minipsd_decode_composite(minipsd_reader *r,
                             const minipsd_target *target, const minipsd_decode_opts *opts);

void minipsd_target_rgba(minipsd_target *t, uint8_t *rgba, size_t row_stride);  /* the common case */
void minipsd_target_rgba16(minipsd_target *t, uint16_t *rgba, size_t row_stride);  /* the same, 16 bits */
```

The reader follows the writer's pattern: opaque, built by `init` inside a caller workspace, which
never grows with the file (the optional index for `R5` is a separate, later call with its own
workspace). Every pointer below is a borrowed view into the blob or the caller's memory.

### Data the reader returns

- **`minipsd_info`** (`init` fills it; fixed size, on the caller's stack):
  - `width`, `height`, `depth`, `color_mode`, `channels`, as stored;
  - `dpi` (`0` when the file has no resolution resource) and `icc` as a `minipsd_bytes`
    (`size == 0` when absent);
  - `layer_count`;
  - `composite_has_alpha`: the file's layer count is negative, so the composite's first extra
    channel is transparency;
  - `format`, `MINIPSD_PSD` or `MINIPSD_PSB`: how the views are laid out. The layer view
    carries it too, so `minipsd_layer_next_block` walks a PSB's blocks with nothing else at hand.
- **`minipsd_resource`** (`R2`, `R10`): `{ uint16_t id; minipsd_bytes data; }`. The reader
  decodes 1005 (into `dpi`) and 1039 (into `icc`) and still lists them. Every other resource,
  thumbnail 1036 included, is only a raw view. There is no find-by-id: resources are few, and the
  caller loops over `next_resource`.
- **`minipsd_layer_view`** (`next_layer` fills it; layers come in file order, bottom first, as
  in `minipsd_doc`):
  - the record's rectangle, opacity, hidden flag and four-character blend key;
  - two raw name views, the Pascal string and the `luni` UTF-16BE block (empty when absent);
  - the colour channels R, G, B and A (`MINIPSD_CH_*`), each as a `minipsd_bytes` view of its
    stored data, compression marker first. `size == 0`: the layer has no such channel. `data ==
    NULL` with a size: the channel exists but the buffer ends before it, as in a prefix;
    `channel_offset` says where in the file each one starts, so a caller reading in parts knows
    how far to read for a layer;
  - `other_channels`, the number of channels not read (masks, extra channels), so a caller knows
    something was left out; they become views when layer masks land;
  - the record's additional-info region as one `minipsd_bytes` view, walked with
    `minipsd_layer_next_block(layer, &pos, &block)`, where `pos` is the caller's cursor (start at
    0) and `block` is `{ key, bytes }`. The reader decodes `luni` itself and lists it too; every
    other block is only a raw view (`R10`);
  - the blending ranges as a view and a count; `minipsd_layer_blending_range` decodes one into
    the typed `minipsd_blending_range` (a view cannot point a typed array into the caller's
    buffer), the way `minipsd_layer_name` decodes the name.
- **`minipsd_layer_name`** converts to UTF-8. It uses `luni` when present and otherwise the Pascal
  string, taken as Latin-1. An unpaired surrogate becomes U+FFFD: a name is for display, so it is
  not an error. It always sets `*needed` (with the terminating NUL) and returns `MINIPSD_MORE`
  when `cap` is too small.

### The pixel target (`X2`)

The caller describes where pixels go; the library never allocates for them.

```c
typedef struct minipsd_plane {
    uint8_t  *data;        /* sample (0, 0) of this channel; NULL: do not write it */
    ptrdiff_t pixel_step;  /* bytes from a sample to its right neighbour */
    ptrdiff_t row_step;    /* bytes from a row to the next; may be negative (bottom-up) */
} minipsd_plane;

typedef struct minipsd_target { minipsd_plane channel[MINIPSD_PLANES]; } minipsd_target;   /* colours, then A */
```

- **One shape covers every layout.**
  - Interleaved RGBA: the four `data` pointers are one byte apart, `pixel_step` is 4.
    `minipsd_target_rgba` fills exactly this.
  - BGRA: swap two pointers. Planar: four buffers, `pixel_step` 1.
  - A sub-rectangle of a bigger canvas: offset `data`; the width and height come from the layer
    or the band, never from the target.
- **Planes follow the mode.** The mode's colour channels come first, in file order, then
  transparency at `minipsd_info.color_channels`: R, G, B, A in RGB (`MINIPSD_CH_*`), grey and A,
  C, M, Y, K and A. Five planes (`MINIPSD_PLANES`) hold CMYK with transparency; planes past the
  mode's are not read, so an RGBA target over a grey file gets grey and A in its first two.
  `minipsd_target_interleaved` fills any count of interleaved planes.
- **A `NULL` plane is skipped.** So a caller that wants RGB only, or one channel, leaves the
  others `NULL`. If the file has no alpha and the target has an `A` plane, it is filled with the
  full sample (255, 65535, 1.0).
- **The caller guarantees the memory**: the library cannot see the size of a buffer, so it writes
  `width * rows` samples per non-`NULL` plane and nothing else. `info`, the layer view and the
  band tell the caller how big that is before it allocates.
- **Samples are of the file's depth**, 1, 8, 16 or 32 bits: `minipsd_decode_opts.depth` says
  which the caller expects (0 and `NULL` options: 8), and another than the file's is
  `MINIPSD_ERR_UNSUPPORTED`; there is no conversion. A 16-bit sample is a `uint16_t` in host
  order, a 32-bit one a `float`, a 1-bit one a byte of 0 or 1 (1 black, as stored); steps count
  bytes, and `minipsd_target_rgba16` fills interleaved 16-bit RGBA.
  The depth sits in the options, not in the target: `minipsd_decode_mask` takes a bare plane, and
  a target filled by `minipsd_target_rgba` would leave a new field of its own undefined. So an
  8-bit caller handed a 16-bit file gets the same `MINIPSD_ERR_UNSUPPORTED` as before 16 bits,
  never twice the bytes it allocated.

### Pixel conventions (`X3`)

- **Straight (not premultiplied) alpha.** It is what `minipsd_layer.pixels` takes, so a decoded
  layer can be written back unchanged (`E4`). A caller that wants premultiplied does the
  multiplication; a flag can be added later without breaking anything.
- **A layer is decoded as stored.** Its opacity and hidden flag stay in the view and are not
  applied to the pixels, as on the writer side. A layer without an alpha channel decodes as
  opaque.
- **The composite is un-matted by default.** The file stores it matted against white
  (`c*a + max*(1-a)`, `max` 255, or 65535 at 16 bits); the reader inverts that,
  `c = max - (max - s) * max / a` clamped to 0..max, and `0` where `a == 0`. This gives the
  straight-alpha image the writer took in: exactly where `a == max`, and within about
  `max / (2a)` levels below, since matting has already merged nearby colours (see the spec's
  Verification). `keep_matte` returns it as stored.
  Un-matting reads each pixel's alpha back from the target, so the target must have an `A` plane
  (otherwise `MINIPSD_ERR_ARG`) unless `keep_matte` is set or the file has no composite alpha.
- **Channel order is the target's business.** The library's own order is R, G, B, A.

### Decode options and row bands (`R3`, `R4`)

```c
typedef struct minipsd_decode_opts {
    uint32_t y_first, y_count;  /* rows to decode; y_count 0: from y_first to the end */
    int      keep_matte;        /* composite only: do not remove the white matte */
    int      depth;             /* the target's samples: 0 or 8, or 16; the file's */
    minipsd_inflate_fn inflate; /* ZIP channels: the caller's inflate, NULL: unsupported */
    void    *inflate_user;
    uint8_t *scratch;           /* where a ZIP channel is inflated whole: one channel, or a
                                   part each when it has room for all */
    size_t   scratch_size;
    int      scratch_kept;      /* bands: the parts still hold this layer's channels */
} minipsd_decode_opts;           /* NULL opts: the whole image, 8-bit, un-matted */
```

- **The target describes the band, not the canvas**: its row 0 receives row `y_first`. So a
  caller processing a huge canvas in strips reuses one small buffer.
- **How a row is reached.** Raw data is addressed directly. For RLE, the row-count table is
  summed up to `y_first`, which reads two bytes per skipped row, with no allocation. For a
  composite that sum runs over every row of the channels before it. Strips therefore cost
  `O(height)` reads per call. That is cheap next to decoding, and the optional index (`R5`,
  later) removes it if a caller needs.
- A band outside the image is `MINIPSD_ERR_ARG`.

### Limits (`X4`) and errors (`X7`)

```c
typedef struct minipsd_limits {
    uint32_t max_side;           /* canvas: 0 is the format's (MINIPSD_MAX_SIDE, _PSB); layers: 0 is no limit */
    uint32_t max_layers;         /* 0: 32767, the format's own cap */
    uint64_t max_decoded_bytes;  /* per decode call, width * rows * planes; 0: no limit */
} minipsd_limits;                /* nesting depth goes at the end with layer groups */
```

- **Always checked; the defaults are the format's own caps**, so with `NULL` the reader accepts
  every valid file and an untrusted-upload service tightens the fields (`R8`). The limits are
  tested where each quantity first becomes known: the canvas's sides and the layer count in
  `init`, a layer's sides in `next_layer`, the bytes of one call before `decode_*` writes
  anything.
- **A layer has no side limit of its own by default.** Its rectangle may reach past the canvas,
  and past 30 000 px, in a valid file, so only a `max_side` the caller sets applies to layers.
- **Bytes are counted per call, not per layer.** A layer too large for the limit can still be
  decoded in bands of rows, which is what strips (`R4`) are for.
- **New return codes**, added after `MINIPSD_ERR_UNSUPPORTED = -7`, never renumbered:

  | Code | Value | Meaning | Caller's reply |
  |---|---|---|---|
  | `MINIPSD_ERR_SHORT` | -8 | the blob ends inside data the call needs; the file may be fine | read more (see below) |
  | `MINIPSD_ERR_LIMIT` | -9 | a value is over a limit; the file is not known to be bad | refuse, or raise the limit |
  | `MINIPSD_ERR_FORMAT` | -6 | malformed | reject the file |
  | `MINIPSD_ERR_UNSUPPORTED` | -7 | valid, outside the supported subset | reject, or use another tool |

  Bad arguments stay `MINIPSD_ERR_ARG`. The reader allocates nothing, so it has no
  `MINIPSD_ERR_ALLOC`.
- **What the target holds after an error.** `ARG`, `LIMIT` and `SHORT` are decided before the
  first sample is written, so the target is untouched. `FORMAT` found mid-stream (a bad RLE row)
  leaves it unspecified.

### Reading from a prefix (`R1`)

`init` needs only the bytes up to the layer count: the header, the colour-mode data, the
image resources (so the whole ICC profile and any thumbnail) and the first fields of the layer
section. If the blob ends sooner, it returns `MINIPSD_ERR_SHORT`, and the reader it built still
answers `minipsd_reader_needed`: the exact least length that lets the same call go further.

```c
while ((rc = minipsd_reader_init(&r, buf, have, ws, ws_size, NULL, &info)) == MINIPSD_ERR_SHORT)
    have = read_more(&buf, minipsd_reader_needed(r));   /* grows to at least `needed` */
```

- **The count is exact for the next step, not for the whole file**: the section lengths that
  fix the next step are inside the bytes already read, so a file manager reads a few steps at
  most, then has everything `info` needs.
- `next_layer` works on the same terms. Its first call walks every record once to find where the
  channel data begins, so it needs the whole records section, names included, and says `SHORT`
  until it has it. After that a layer is returned even when its channel data is not in the buffer
  yet: that channel's view says so (see above), and `decode_*` answers `SHORT` for it, with
  `needed` at the end of the last channel the target asks for. A decode therefore takes a
  non-`const` reader: the reader is where `needed` is kept.
- **A longer prefix means calling `init` again** on the same workspace; layer iteration then
  starts from the first layer. Every earlier view is valid only if the buffer did not move.

### Layout and memory

- **Memory**, planned on the writer's pattern; it moves into the spec's
  [Memory and ownership](SPECIFICATION.md#memory-and-ownership) table when the reader lands:

  | Memory | Owner | Notes |
  |---|---|---|
  | the file's bytes (the blob) | caller; borrowed, never copied | read in, `mmap`ed or received; must stay valid while views point into it |
  | `minipsd_workspace` | caller; lent to the reader | the opaque `minipsd_reader` and its state (section offsets, the resource and layer cursors); a fixed small size; heap, stack or static. The index for `R5` is a later call with its own workspace |
  | `minipsd_info`, `minipsd_layer_view` | caller | fixed size, stack; their `minipsd_bytes` fields point into the blob |
  | decoded pixels | caller | the target descriptor (`X2`) says where; one layer at a time (`R6`) or all of them (`R7`) |
  | a whole `minipsd_doc` (`R7`) | caller | `minipsd_read`: one block, copied out of the file (see [Round trip](#round-trip)) |

- **Forward iteration, no table.** Layer records have variable length, and the channel data of
  all layers follows the last record, so where layer 0's data begins is known only after every
  record has been read. The first `next_layer` therefore walks the records once, keeping one
  offset; each call then reads its record again and adds the record's channel lengths to a
  running offset, so each view points at its pixel data. No second table, no storage.
- **The input is untrusted.**
  - Every length is checked against the bytes remaining, in the form
    `if (remaining < n) fail`, never as `p + n > end`.
  - Every size computation is overflow-checked.
  - A malformed file returns `MINIPSD_ERR_FORMAT`.
  - **What is described, and what is decoded.** `init` and `next_layer` describe any PSD or
    PSB, whatever its colour mode or depth, so a file manager can list a CMYK or 16-bit file
    (`R1`), and decodes their pixels as stored, in every mode and depth, with no colour
    conversion; a feature it does not decode yet is `MINIPSD_ERR_UNSUPPORTED` from `decode_*`.
    The rules of samples (a mode's colour channels, row bytes, prediction, 1-bit rows) live in
    `detail/samples.h`, which the writer shares. ZIP is decoded through the
    caller's inflate (`minipsd_inflate_fn`), into the caller's scratch of one channel, so the
    reader still allocates nothing and implements no deflate; without one it is unsupported.
  - **PSB is a layout, not a feature.** The reader keeps one flag, and every width that depends on
    it comes from `detail/format.h`, which the writer shares: lengths (`mpsd__getlen`,
    `mpsd__putlen`), RLE row counts (`mpsd__count`, `mpsd__putcount`), and a block's header, whose
    length is eight bytes for the keys in `mpsd__long_key` (psd-tools' list, which Photoshop's
    files bear out: Adobe's names 13 of the 21). Views keep the file's layout and say so (`format` in the info and the view). A section
    length of 2^62 or more is malformed, so offsets built from lengths never wrap; one above
    `SIZE_MAX` (a 32-bit platform) is `MINIPSD_ERR_UNSUPPORTED`, since views could not state their
    sizes.
  - **Layers in `Lr16`.** Photoshop leaves the layer info of a 16- or 32-bit file empty and keeps
    the same structure in a global block, `Lr16` or `Lr32`. With an empty layer info, `init`
    walks the global blocks' headers (each padded to four bytes past its stated length) and,
    finding one, reads the layer count there; `next_layer` then walks its records like any
    others. The block stays in `info.layer_tail`, which is raw bytes.
  - A libFuzzer target runs locally under ASan and UBSan, seeded from the test files; it is a
    tool, not a CI job. Each finding, once fixed, stays as an input that the ordinary tests
    replay, so CI guards against its return.
- **Symmetric with the writer.** For the subset minipsd writes, the decoded views and pixels
  rebuild a `minipsd_doc`, and writing it again gives the original bytes. That round trip
  `encode(decode(x)) == x` is the reader's main test, next to files saved by GIMP, Krita and
  Photopea.
- **Not planned:** a streaming (push) parser. It is possible, since the format is forward-only
  apart from the record/data split, but it would need buffering inside the library that
  contradicts [Dependency rules](#dependency-rules). It is added only if a real consumer needs
  it.

## Round trip

A file read, changed and written again (`R7`, `E1`, `E2`, `W7`). Built on the reader and the pull
writer; every addition is a field at the end of an existing struct whose zero keeps the previous
bytes, so nothing here breaks the API.

### What a round trip keeps

| Input | Kept |
|---|---|
| a file minipsd wrote | the same bytes (`E4`) |
| a file from another program | the same content: every layer's pixels, name, rectangle, opacity, visibility, blend key, clipping, kept blocks and resources, and the composite's pixels. With `opts->stored`, the same bytes too, for most files (S6.7: what is left is listed there) |

What the document cannot carry is reported (see [Loss](#loss)).

### The composite: supplied, or composed as a best effort

The composite is the flattened image that readers ignoring layers show: previews, thumbnails,
viewers. Flattening layers is rendering, which is the caller's domain; minipsd only encodes the
image the way PSD stores it.

```c
/* at the end of minipsd_doc */
const uint8_t *composite;          /* the full canvas, width * height * 4, straight alpha; NULL: composed */
int            composite_matted;   /* non-zero: composite is already matted, written as it is */
```

- **Supplied (`composite` set): the right way.** The writer writes exactly that image, whatever
  the layers hold: blend modes, masks, effects, adjustments. Rendering it is the caller's job;
  libvips does most of it (the spec's [Working with libvips](SPECIFICATION.md#working-with-libvips)).
- **Composed (`composite == NULL`): a best effort, no guarantee.** For a caller that only needs
  a plausible preview, as Platemaker does. The writer composes the visible layers with "over",
  with their opacity and at their rectangles. Every other blend mode is drawn as `norm`; masks,
  clipping, groups and effects are left out. It is today's behaviour, so a document that does
  not set `composite` keeps its bytes.
- **The matte is always the writer's.** PSD stores the composite's colour matted against white;
  that is the format's encoding, not rendering, so the caller gives straight alpha, as for
  layers, and the writer mattes it. Only `composite_matted` skips it: `minipsd_read` sets it,
  because un-matting and matting again drift where alpha is low (8-bit matting merges nearby
  colours), and a file's own composite must come back pixel for pixel.
- **Workspace.** A supplied composite is read row by row like a layer; the composed one keeps
  its one row in the workspace, as now.
- **Kept (`composite_stored`, when `composite == NULL`): a file's own, copied.** The composite as
  the file stores it, one compression marker and one count table over all its channels' rows,
  with `composite_channels` (3 to 56) and `composite_alpha` (the fourth is transparency). The writer copies it like a stored layer
  channel (its count table re-written when the format changes), and the header's channel count
  and the layer count's sign follow it. Re-encoding would give the same pixels in other bytes:
  every program packs PackBits its own way. `minipsd_read` sets it with `opts->stored`.

### What the document gains

`minipsd_layer`, at the end:

| Field | Zero means | Notes |
|---|---|---|
| `int32_t top, left, bottom, right` | all four zero: the full canvas (with `pixels`; stored channels take the rectangle as it is) | the fields of `minipsd_layer_view`, in its order. `pixels` holds `(right - left) * (bottom - top) * 4` bytes. A rectangle may reach past the canvas; an inverted one, or an area without `pixels`, is `MINIPSD_ERR_ARG`; a side above `MINIPSD_MAX_SIDE` is `MINIPSD_ERR_SIZE` (RLE row counts are 16 bits). Row tables and channel sizes follow the rectangle; the composed composite blends at the offset and clips to the canvas |
| `char blend_mode[4]` | `norm` | written as given |
| `uint8_t clipping` | not clipped | written as given |
| `uint8_t flags` | none | the record's other flag bits (0x01 transparency locked, 0x08 and 0x10 pixel data irrelevant), written as given; 0x02 comes from `hidden` |
| `minipsd_bytes name_pascal` | the writer's own names | the legacy name kept from a file (`minipsd_read` sets it); while `name` says what the kept names say (a kept `luni`, else this one as Latin-1), both are written as they are, otherwise the writer's own |
| `minipsd_bytes additional_info` | no extra blocks | the layer's additional-information blocks as one blob, in the format of the reader's `additional_info` view; written after the writer's own blocks. `minipsd_doc_check` walks them: a malformed run is `MINIPSD_ERR_ARG` |
| `const minipsd_bytes *stored` | encode `pixels` | four channel blobs (R, G, B, A, as `minipsd_layer_view.channel`), each the stored data with its compression marker and row counts; transparency of size 0 is none. The writer copies each as one piece instead of encoding `pixels`, and writes no transparency channel where none is stored; the sizing pass takes their sizes. `minipsd_doc_check` checks each against the rectangle (raw rows, or RLE counts adding up within the blob; ZIP passes as it is); a layer with `stored` and no `pixels` needs a supplied `composite`, since nothing could compose it. Name, opacity, visibility, `blend_mode`, clipping and `additional_info` still come from the layer |

`minipsd_doc`, at the end, after `composite` and `composite_matted`:

| Field | Zero means | Notes |
|---|---|---|
| `const minipsd_resource *resources`, `size_t resource_count` | no other resources | written in order after the writer's own 1005 and 1039, which take the kept ones' names; the reader's `minipsd_resource` type, which carries the resource's name (`name`, a Pascal string's bytes, at most 255) |
| `minipsd_bytes layer_tail` | an empty global layer mask (four zero bytes) | the rest of the layer and mask section after the layer info: the global layer mask information and the global additional blocks (`Txt2`, `Patt`, `Lr16`, ...), written back as they are |

The reader gives the matching fields: `minipsd_layer_view.clipping` and `.flags`,
`minipsd_resource.name`, and `minipsd_info.layer_tail` (data `NULL` with a size when the buffer
ends before it). `minipsd_bytes`, `minipsd_resource` and `minipsd_block` are declared with the
document, which uses them.

**When the document and kept data both say something, the document's field wins** and the kept
data with that key is skipped: `name` over the `luni` block in `additional_info`, `dpi` over resource 1005,
`icc` over resource 1039. So a layer is renamed by changing `name` alone, and kept blocks and
resources pass through whole, unfiltered. One refinement: a kept 1005 that says the same dpi is
written in place of the writer's own, so its units (centimetres) and vertical resolution
survive. Data that goes stale on an edit, such as the thumbnail (1036), stays the caller's to
drop.

### `minipsd_read`

Bytes to a document, the mirror of `minipsd_write`, built on the public reader:

```c
/* Two phases: the caller owns the memory. */
int minipsd_read_size(const void *data, size_t size, const minipsd_limits *limits,
                      const minipsd_read_opts *opts, size_t *block_size);
int minipsd_read_into(const void *data, size_t size, const minipsd_limits *limits,
                      const minipsd_read_opts *opts, void *block, size_t block_size,
                      minipsd_doc **doc, uint32_t *lost);

/* One call, through MINIPSD_MALLOC; gone with MINIPSD_NO_MALLOC, like minipsd_write. */
int  minipsd_read    (const void *data, size_t size, const minipsd_limits *limits,
                      const minipsd_read_opts *opts, minipsd_doc **doc, uint32_t *lost);
void minipsd_doc_free(minipsd_doc *doc);   /* MINIPSD_FREE, so a replaced allocator stays right */

typedef struct minipsd_read_opts {
    int stored;   /* non-zero: each layer keeps its stored channels (stored) instead of pixels */
    minipsd_inflate_fn inflate;   /* ZIP channels decoded; the block holds the scratch */
    void *inflate_user;
} minipsd_read_opts;   /* NULL: decode every layer */
```

| Writing | Reading |
|---|---|
| `minipsd_writer_*`: low level, the caller's memory | `minipsd_reader_*`: low level, views into the file |
| `minipsd_write`: a document to bytes, one call | `minipsd_read`: bytes to a document, one call |

- **One block, and nothing points into the file.** The document lives in one block, the
  caller's (`minipsd_read_into`, any alignment) or one `MINIPSD_MALLOC` with the document at its
  start. Everything is copied into it, so the file's buffer may be freed as soon as the call
  returns:

  | In the block | Size |
  |---|---|
  | the `minipsd_doc`, the `minipsd_layer` array (aligned) | `sizeof`; `layer_count` entries |
  | names, UTF-8 with their NUL | each name's `needed` |
  | each layer's pixels, or with `opts->stored` its stored channels | `w * h * 4`, or the channels' stored size |
  | the composite, as stored (matted), `composite_matted` set; with `opts->stored`, its bytes | `width * height * 4`, or its stored size |
  | `additional_info` blobs, resources, `layer_tail`, the ICC profile | their stored size |

  Non-pixel data is usually kilobytes (text, effects, a thumbnail); an embedded smart object can
  reach megabytes and is copied too, the price of a document that stands alone. One function
  lays the block out twice: measuring (`minipsd_read_size`), then placing and decoding.
- **What the writer needs, the reader fills in.** A file without layers gets one, "Background",
  the composite un-matted over the full canvas; a file without a resolution gets 72 dpi;
  `compression` follows the composite's. A layer side above `MINIPSD_MAX_SIDE` is
  `MINIPSD_ERR_UNSUPPORTED`, since the writer could not take it.
- **`opts->stored`** decodes nothing: each layer keeps its compressed channels, a fraction of its
  pixels' size. The caller decodes the one layer it edits through the reader, from the same
  file, sets that layer's `pixels` and clears its `stored`, then writes. Untouched layers are never
  decoded nor encoded again, and the file's own composite is copied, neither rendered nor
  decoded (`E1`), alpha and spot channels with it (the resources naming them are kept). ZIP
  channels pass this way without an inflate; a layer without a transparency channel is written
  without one.
- **`opts->inflate`** decodes ZIP channels: the measuring pass finds the largest ZIP channel it
  decodes, and the block holds that much scratch after the document.
- **ZIP written** (`compression` `MINIPSD_ZIP`, `_PREDICTED`, with `minipsd_write_opts.deflate`):
  a channel's size is in its record before its data, and the writer never seeks back, so
  `mpsd__measure_layers` deflates each channel once and the `MPSD__ZIP` piece once more, as RLE
  rows are packed twice. The workspace holds the largest channel's raw rows (gathered big-endian,
  predicted in place) and its stream; the stream is kept while a piece of it is handed out in
  parts, and a second stream of another size is `MINIPSD_ERR_WRITE`. The composite stays RLE.
- **Limits:** `minipsd_limits` as in the reader; `max_decoded_bytes` bounds the sum of the call's
  decoded pixels. A block larger than `SIZE_MAX` is `MINIPSD_ERR_SIZE`.
- **What it accepts:** what the writer takes back: the document has the file's `color_mode`,
  `color_mode_data` and `depth`, and a file the writer would refuse (a mode at a depth it does not
  write, layers in a flat mode) is `MINIPSD_ERR_UNSUPPORTED`, checked first with the writer's own
  `mpsd__doc_mode_ok` on the file's header. Layers read from `Lr16` or `Lr32` are the document's, so
  `layer_tail` keeps that block empty, where it was, and the writer puts them back in it.
- **No allocator passed at run time.** The caller's block and `MINIPSD_MALLOC` cover both cases;
  a `{alloc, free, user}` variant is an addition when a caller needs one.

### Loss

`lost` receives a bitmask over the whole document; 0 is the expected value.

| Flag | What is lost |
|---|---|
| `MINIPSD_LOST_BLENDING_RANGES` | no longer set: the ranges are kept (`minipsd_layer.blending_ranges`) |
| `MINIPSD_LOST_CHANNELS` | channels other than R, G, B, transparency and masks, of a layer, or of a decoded composite (a positive layer count makes a fourth composite channel an alpha channel), and a mask channel without mask data |

- **Every known loss is tracked.** The code that sets a bit carries a `TODO` naming the ROADMAP
  step that will carry the content, so the list of bits is the list of open work.
- **A loud warning, and a request.** The MANUAL asks whoever sees a non-zero `lost` to open an
  issue with the file or a description of it; the bits make the report precise.
- **Not loss:** groups and layer masks (fields), text, effects, smart objects, adjustments and
  vector masks (blocks and `layer_tail`), blend keys, clipping and flags (fields). The composite is copied, never lost. Blending ranges left at their
  default (`0 0 255 255` for every pair) are not loss either.
- **Document-wide.** Which layer has a mask, the reader tells; a per-layer list is an addition
  when a caller needs it.

## Features through the modules

Each planned feature, and where it lands. All of them follow the
[extensibility rules](SPECIFICATION.md#extensibility-rules): a new field goes at the end of its
struct, and zero keeps the previous output. So each one is a PATCH addition unless noted. A
feature is serialization: written and read as data. The composite composed as a best effort
(see [Round trip](#round-trip)) learns a feature only on demand; a supplied composite needs none.

| Feature | Document (in `minipsd.h`) | `writer.h` | `reader.h` | detail | Notes |
|---|---|---|---|---|---|
| **Module split** | — | — | — | all | no API or byte change; first stage; the API stays in `minipsd.h` |
| **Pull writer + workspace** | `minipsd_doc_check` | `minipsd_writer_*` | — | `bytes` | `minipsd_write` rebuilt on top, same bytes |
| **`minipsd_write_file` to `examples/`** | — | removed | — | — | the only **breaking** item: 0.2.0 |
| **Reader, own subset** | — | — | new | `bytes`, `utf`, `packbits`, `composite` (un-matte) | 8-bit RGB, full-canvas or empty layers |
| **Layers smaller than the canvas** | done: `top, left, bottom, right` on `minipsd_layer`, as in the view; all zero = full canvas | done: bounds, row tables | done: the view's rectangle | done: `composite`: offset blend, clipped to the canvas | |
| **Blend modes** | done: `blend_mode[4]`; zero = `norm` | done: key in the record | done: the view's key | — | the composed composite draws every mode as `norm` |
| **Supplied composite** | done: `composite`, `composite_matted`; NULL = composed | done: matted unless marked, composing skipped | — | `composite`: the row source | `W7` |
| **Round trip** | done: `blend_mode`, `clipping`, `flags`, `additional_info`, `stored`; `resources`, `layer_tail` | done: kept data written back, stored channels copied | done: `clipping`, `flags`, resource names, `layer_tail`; `minipsd_read` with `opts->stored` | `bytes`: the block walker, shared | see [Round trip](#round-trip) |
| **Layer groups (`lsct`)** | done: `section_divider_type` (`MINIPSD_SECTION_*`); zero = layer | done: the `lsct` block, the group's mode in it; a kept one of the same type as it is; nesting checked | done: `section_divider_type` in the view | — | the composed composite draws groups flat |
| **Layer masks** | done: `mask` (`minipsd_mask`: rectangle, colour, flags, pixels or stored, kept real mask and rest) | done: mask data, channels -2 and -3 | done: the view's mask, `minipsd_decode_mask` | — | the composed composite ignores masks |
| **16-bit channels** | done: `depth` on `minipsd_doc`; zero = 8 | done: 2-byte samples, big-endian, in the layer info | done: `depth` in `minipsd_decode_opts`, layers from `Lr16`, `minipsd_read` | done: `bytes` (`mpsd__at`, samples), `packbits` over a row's bytes, `composite` against 65535 | ZIP channels decode through the caller's inflate (S6.9), or pass as stored |
| **Typed views** | — (output only: `minipsd_resolution_info`, `minipsd_version_info`, `minipsd_grid_and_guides`, `minipsd_guide`, `minipsd_block_settings`) | — | `views.h`: a resource or a layer view in, a struct out; `find_resource`, `next_resource` with the caller's `pos` | `format`, `utf` | the writer keeps resources and blocks as bytes; nothing to write |
| **PSB (version 2)** | done: `format` on `minipsd_doc` (`MINIPSD_PSD`, `MINIPSD_PSB`; zero = PSD): the layout of everything in the document, set by `minipsd_read`; the output format a write option, zero = the document's (decided over two document fields and over a converting copy: one field never describes a wish, and a translating writer stays zero-copy, as `tiffsave`'s `bigtiff` does) | done: 8-byte lengths, 4-byte RLE counts; `minipsd_write_opts.format` on the write calls, kept bytes translated while streaming (RLE count tables, long keys' lengths), by a policy per long key in `format.h` | done: both versions, `format` in the info and the view, `minipsd_read` sets the document's | `format`: widths, long keys, blocks | sides up to 300 000 px |

## Distribution of a multi-file library

- **The directory is the library.** A raw copy means copying `include/` (`minipsd.h` plus
  `minipsd/`).
- **A single file is a release artifact.** `tools/amalgamate.cmake` joins the headers in
  dependency order into one `minipsd.h` and strips the internal `#include "minipsd/..."` lines.
  It needs CMake only, no Python. The release workflow attaches the result for users who prefer
  one file; nothing in the design depends on it.
- **CI builds the test suite twice:** once against `include/` and once against the amalgamated
  file. The two must produce identical bytes.
- **Install tree.** It gains `include/minipsd/` next to `include/minipsd.h`; the CMake target,
  the `.pc` file and the include line do not change.
- **SBOM.** The fragment lists every installed header with its SHA-256. The release asset gets
  its own `.sha256`, as today.
- **Recipes.** The Conan recipe in `temp/packaging/` copies `minipsd.h` alone and must copy
  `minipsd/` too. The vcpkg port uses our install rules and needs no change.

## How development proceeds

Stages in order. Each stage is one or more turns that end at a stop for review, with a proposed
commit message. [ROADMAP.md](ROADMAP.md) turns these into numbered steps.

| Stage | Content | Version |
|---|---|---|
| **0. Golden baseline** | a test that writes a fixed set of documents and compares SHA-256 against hashes recorded from 0.1.0 | — |
| **1. Split** | the files above, code moved verbatim, amalgamation, double CI build; golden test unchanged | 0.1.x |
| **2. Pull writer** | `minipsd_writer_*`, `minipsd_doc_check`, `minipsd_write` on top; a test with random `cap` and a fake non-blocking target that matches the golden bytes | 0.1.x |
| **3. I/O out of the core** | `examples/` with the three writers; `minipsd_write_file` removed; spec, INSTALL and README updated | **0.2.0** |
| **4. Reader** | `reader.h` for the own subset, then real-application files; fuzzing | 0.2.1 |
| **5. Round trip** | layer rectangles in the writer, a supplied composite, full import, kept unknown content, pass-through | 0.2.x |
| **6. Features** | one row of the table above per stage, writer and reader together | 0.2.x |
| **7. Interoperability** | libvips: an example and a MANUAL chapter | 0.2.x |

Every stage follows these rules:

- **Regression first.** The golden test and the full suite pass before a stage changes
  anything. A stage that is not meant to change bytes keeps them identical.
- **Independent reader for every byte change.** `psd-tools` in CI checks new bytes, as
  [Verification](SPECIFICATION.md#verification) requires. minipsd's own reader is never the
  only check of its own writer.
- **Coverage stays at 100 % of lines** and at least 95 % of branches, per module. A new module
  comes with its `test_<module>.cpp`.
- **The spec follows the code.** When a stage lands, its part of this file moves into
  [SPECIFICATION.md](SPECIFICATION.md), and this file keeps only what is still ahead.

Work reaches `master` only through a pull request whose CI passes. Its branch is named
`<prefix>/<short-name>` (lower case, words joined by `-`, an issue's number first when there is
one: `fix/12-real-mask-table`), the prefix saying what kind of change it is:

| Prefix | The change |
|---|---|
| `feature/` | a new function or addition, a ROADMAP step or a requested feature |
| `fix/` | a repair of existing code, a reported issue |
| `refactor/` | code changed without changing what it does: the same bytes, the same results |
| `ci/` | the CI workflows |
| `docs/` | files that are not source code |

A branch holding several kinds is named by the main one, as its commits' types (`feat`, `fix`,
`refactor`, `ci`, `docs`) say.

## Decisions

Taken at review, 2026-10-14.

1. **`minipsd_write_file` leaves the library in 0.2.0** and becomes `examples/write_file.c`.
   `MINIPSD_ERR_OPEN` stays defined, never renumbered. `MINIPSD_NO_STDIO` stays accepted, with
   no effect, so existing builds that define it keep compiling.
2. **The allocating `minipsd_write` stays**, with its 0.1 signature, behind `MINIPSD_NO_MALLOC`.
3. **The repository ships a directory, not a single file.** `include/minipsd.h` plus
   `include/minipsd/` is the form every integration path uses. A single amalgamated header is
   only a **release artifact**, built by the release workflow for convenience. It is not a
   design constraint.
4. **The reader's first stage** covers what minipsd writes, plus any 8-bit RGB file with
   full-canvas layers. Everything else returns `MINIPSD_ERR_UNSUPPORTED`.
5. **Module names:** `base`, `document`, `writer`, `reader`, and `detail/` for the internal
   ones. After stage 1 the public names are contract.
6. **`minipsd.h` holds the whole public API** (2026-10-14, revising 5): types, constants and
   every declaration, so a user reads one file. `include/minipsd/` holds only implementation,
   so "document" became a section of `minipsd.h` rather than a file.

Taken at the S4.1 review, 2026-10-15 (the reader):

7. **The writer's layer source does not share the pixel descriptor.** `minipsd_layer.pixels` stays.
   A strided or row-by-row source (`W6`, `W7`) would be a field added at the end of
   `minipsd_layer`, with the same shape but `const` pointers and `pixels` as the default when it
   is zero: the reader writes through the pointers and the writer only reads, so one type for
   both would need a cast.
8. **Short input is `MINIPSD_ERR_SHORT` plus `minipsd_reader_needed`**, not `MINIPSD_MORE`:
   `MORE` already means "an item was produced, call again" in the iterators.
9. **Limits default to the format's own caps.** The checks always run; only a caller that sets a
   limit gets a stricter reader.
10. **Un-matting needs an `A` plane in the target**, so the workspace stays independent of the
    file.
11. **One `minipsd_decode_opts`** carries the row band, the matte option, the depth and the
    inflate; `NULL` is the defaults, and new options go at its end. So does every options struct,
    which is why a caller zeroes it whole: a field it never heard of is still read.
12. **Resources are iterated, not looked up by id.**
13. **A Pascal-only name is read as Latin-1**; the raw view is there for a caller that knows the
    code page.
14. **Layer views carry R, G, B and A only**, with a count of the channels left out; masks join
    with the layer-mask feature.
15. **`init` and `next_layer` describe any PSD**; `UNSUPPORTED` comes from the pixel
    decoding (S4.4, S4.5). Found while implementing S4.2: listing a CMYK file's layers is `R1`'s
    job. Since S6.6 the same holds for a PSB.
