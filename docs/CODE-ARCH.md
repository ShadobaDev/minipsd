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
- **Re-entrant.** No hidden state: two writers or readers in two threads never interact.

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
    reader.h                   the decoder (later stage)
    detail/
      bytes.h                  big-endian put/get on uint8_t pointers, bounds-checked cursor
      utf.h                    UTF-8 <-> UTF-16 for layer names
      packbits.h               PackBits encode, decode, size bound; strided source and target
      composite.h              blending one row "over", white matte and un-matte
examples/                      the I/O implementations; built and run in CI, never installed
  demo.h                       the document every example writes (the golden "template-rle")
  write_file.c                 stdio, UTF-8 path on Windows (minipsd_write_file until 0.2.0)
  write_memory.c               exact-size buffer from minipsd_writer_total
  write_nonblocking.c          O_NONBLOCK pipe + poll (POSIX)
  read_file.c                  whole file into memory, then decode (later stage)
tests/
  test_<module>.cpp            one file per module; detail modules are tested directly
  fuzz/                        libFuzzer target for the reader (later stage)
tools/
  amalgamate.cmake             joins include/ into one file, a release artifact
```

| Module (section of `minipsd.h`) | Struct | Functions (sketch) | Implementation |
|---|---|---|---|
| Version, linkage, return codes | `minipsd_bytes { const uint8_t *data; size_t size; }` | `minipsd_error_string`; new codes, added never renumbered: `MINIPSD_MORE = 1` (positive: not an error, call again), `MINIPSD_ERR_FORMAT = -6`, `MINIPSD_ERR_UNSUPPORTED = -7`, `MINIPSD_ERR_SHORT = -8`, `MINIPSD_ERR_LIMIT = -9` | `base.h` |
| Document | `minipsd_doc`, `minipsd_layer` (unchanged from 0.1) | `minipsd_doc_check(doc)`: the validation `minipsd_write` does today, now callable on its own | `writer.h` today; its own file if the reader shares it |
| Writer | `minipsd_writer` (opaque, built in the workspace) | `minipsd_writer_workspace_size`, `_init`, `_total`, `_fill`; `minipsd_write` | `writer.h`; detail: `bytes`, `utf`, `packbits`, `composite` |
| Reader | `minipsd_reader` (opaque, like the writer), `minipsd_info`, `minipsd_resource`, `minipsd_layer_view`, `minipsd_target` (of `minipsd_plane`), `minipsd_decode_opts`, `minipsd_limits` | `minipsd_reader_workspace_size`, `_init`, `_needed`, `_next_resource`, `_next_layer`, `minipsd_layer_name`, `minipsd_decode_layer`, `minipsd_decode_composite`, `minipsd_target_rgba` | `reader.h`; detail: `bytes`, `utf`, `packbits`, `composite` |

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
  - `MINIPSD_STATIC` keeps working unchanged.
- **No unused internals.** The full implementation, compiled with extern linkage, leaves no
  `static` function unused: that is a warning, and warnings are errors.
- **Optional subsystems hide behind macros in `minipsd.h`**, the way `MINIPSD_NO_MALLOC` does.
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
| **Blocking stream** (file, blocking pipe or socket) | `minipsd_write(doc, fn, user)`: the library calls `fn` until done | the caller reads everything into memory, then as above |
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

int minipsd_reader_next_resource(minipsd_reader *r, minipsd_resource   *res);    /* MORE: got one; OK: no more */
int minipsd_reader_next_layer   (minipsd_reader *r, minipsd_layer_view *layer);  /* MORE: got one; OK: no more */
int minipsd_layer_name          (const minipsd_layer_view *layer, char *utf8, size_t cap, size_t *needed);
int minipsd_layer_next_block    (const minipsd_layer_view *layer, size_t *pos, minipsd_block *block);  /* MORE / OK */

int minipsd_decode_layer    (const minipsd_reader *r, const minipsd_layer_view *layer,
                             const minipsd_target *target, const minipsd_decode_opts *opts);
int minipsd_decode_composite(const minipsd_reader *r,
                             const minipsd_target *target, const minipsd_decode_opts *opts);

void minipsd_target_rgba(minipsd_target *t, uint8_t *rgba, size_t row_stride);  /* the common case */
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
    channel is transparency.
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
  - `other_channels`, the number of channels not read (masks, extra channels), so a caller knows
    something was left out; they become views when layer masks land;
  - the record's additional-info region as one `minipsd_bytes` view, walked with
    `minipsd_layer_next_block(layer, &pos, &block)`, where `pos` is the caller's cursor (start at
    0) and `block` is `{ key, bytes }`. The reader decodes `luni` itself and lists it too; every
    other block is only a raw view (`R10`).
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

typedef struct minipsd_target { minipsd_plane channel[4]; } minipsd_target;   /* R, G, B, A */
```

- **One shape covers every layout.**
  - Interleaved RGBA: the four `data` pointers are one byte apart, `pixel_step` is 4.
    `minipsd_target_rgba` fills exactly this.
  - BGRA: swap two pointers. Planar: four buffers, `pixel_step` 1.
  - A sub-rectangle of a bigger canvas: offset `data`; the width and height come from the layer
    or the band, never from the target.
- **A `NULL` plane is skipped.** So a caller that wants RGB only, or one channel, leaves the
  others `NULL`. If the file has no alpha and the target has an `A` plane, it is filled with 255.
- **The caller guarantees the memory**: the library cannot see the size of a buffer, so it writes
  `width * rows` samples per non-`NULL` plane and nothing else. `info`, the layer view and the
  band tell the caller how big that is before it allocates.
- **Samples are one byte (depth 8) for now.** A depth field goes at the end of `minipsd_target`
  with the 16-bit stage; zero keeps 8.

### Pixel conventions (`X3`)

- **Straight (not premultiplied) alpha.** It is what `minipsd_layer.rgba` takes, so a decoded
  layer can be written back unchanged (`E4`). A caller that wants premultiplied does the
  multiplication; a flag can be added later without breaking anything.
- **A layer is decoded as stored.** Its opacity and hidden flag stay in the view and are not
  applied to the pixels, as on the writer side. A layer without an alpha channel decodes as
  opaque.
- **The composite is un-matted by default.** The file stores it matted against white
  (`c*a + 255*(1-a)`); the reader inverts that, `c = 255 - (255 - s) * 255 / a` clamped to
  0..255, and `0` where `a == 0`. This gives the straight-alpha image the writer took in, within
  one level of rounding (see the spec's Verification). `keep_matte` returns it as stored.
  Un-matting reads each pixel's alpha back from the target, so the target must have an `A` plane
  (otherwise `MINIPSD_ERR_ARG`) unless `keep_matte` is set or the file has no composite alpha.
- **Channel order is the target's business.** The library's own order is R, G, B, A.

### Decode options and row bands (`R3`, `R4`)

```c
typedef struct minipsd_decode_opts {
    uint32_t y_first, y_count;  /* rows to decode; y_count 0: from y_first to the end */
    int      keep_matte;        /* composite only: do not remove the white matte */
} minipsd_decode_opts;           /* NULL opts: the whole image, un-matted */
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
    uint32_t max_side;           /* 0: MINIPSD_MAX_SIDE */
    uint32_t max_layers;         /* 0: 32767, the format's own cap */
} minipsd_limits;                /* S4.6 adds, at the end, max_decoded_bytes (per decode call);
                                    nesting depth follows with layer groups */
```

- **Always checked; the defaults are the format's own caps**, so with `NULL` the reader accepts
  every valid file and an untrusted-upload service tightens the fields (`R8`). The limits are
  tested where each quantity first becomes known: sides and layer count in `init`, a layer's
  area in `next_layer`, the bytes of one call before `decode_*` writes anything.
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
  yet: that channel's view says so (see above), and `decode_*` answers `SHORT` for it.
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
  | a whole `minipsd_doc` (`R7`) | caller | stage 5: sizes first, then the caller's buffers or allocator; never a copy kept by the reader |

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
  - **What is described, and what is decoded.** `init` and `next_layer` describe any PSD of
    version 1, whatever its colour mode or depth, so a file manager can list a CMYK or 16-bit file
    (`R1`). Only the pixel decoding is limited to 8-bit RGB (`MINIPSD_ERR_UNSUPPORTED`, from
    `decode_*`, S4.4 and S4.5), as is any feature it does not decode yet. A PSB (version 2) is
    refused by `init`, since its lengths are wider.
  - A libFuzzer target runs in CI under ASan, seeded from the test files.
- **Symmetric with the writer.** For the subset minipsd writes, the decoded views and pixels
  rebuild a `minipsd_doc`, and writing it again gives the original bytes. That round trip
  `encode(decode(x)) == x` is the reader's main test, next to files saved by GIMP, Krita and
  Photopea.
- **Not planned:** a streaming (push) parser. It is possible, since the format is forward-only
  apart from the record/data split, but it would need buffering inside the library that
  contradicts [Dependency rules](#dependency-rules). It is added only if a real consumer needs
  it.

## Features through the modules

Each planned feature, and where it lands. All of them follow the
[extensibility rules](SPECIFICATION.md#extensibility-rules): a new field goes at the end of its
struct, and zero keeps the previous output. So each one is a PATCH addition unless noted.

| Feature | Document (in `minipsd.h`) | `writer.h` | `reader.h` | detail | Notes |
|---|---|---|---|---|---|
| **Module split** | — | — | — | all | no API or byte change; first stage; the API stays in `minipsd.h` |
| **Pull writer + workspace** | `minipsd_doc_check` | `minipsd_writer_*` | — | `bytes` | `minipsd_write` rebuilt on top, same bytes |
| **`minipsd_write_file` to `examples/`** | — | removed | — | — | the only **breaking** item: 0.2.0 |
| **Reader, own subset** | — | — | new | `bytes`, `utf`, `packbits`, `composite` (un-matte) | 8-bit RGB, full-canvas or empty layers |
| **Layers smaller than the canvas** | `left, top, width, height` on `minipsd_layer`; all zero = full canvas | bounds, row tables | bounds into the view | `composite`: offset blend | first feature that needs both directions |
| **Blend modes** | `blend` key; zero = `norm` | key in the record | key in the view | `composite`: one function per mode | composite checked against `psd-tools` |
| **Layer groups (`lsct`)** | `kind`: layer / group open / group close; zero = layer | divider records, `lsct` block | group structure in the view | `composite`: pass-through, hidden groups | |
| **Layer masks** | optional mask (rect + bytes) per layer | mask record, channel `-2` | mask view | `composite`: mask multiply | |
| **16-bit channels** | `depth` on `minipsd_doc`; zero = 8 | 2-byte samples | 2-byte samples | `packbits` over byte planes, `composite` in 16-bit | |
| **PSB (version 2)** | `format` on `minipsd_doc`; zero = PSD | 8-byte lengths, 4-byte RLE counts | both versions | `bytes`: 64-bit fields | sides up to 300 000 px |

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
| **4. Reader** | `reader.h` for the own subset, then real-application files; fuzzing | 0.2.x |
| **5. Features** | one row of the table above per stage, writer and reader together | 0.2.x |

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

7. **The writer's layer source does not share the pixel descriptor.** `minipsd_layer.rgba` stays.
   A strided or row-by-row source (`W6`, `W7`) would be a field added at the end of
   `minipsd_layer`, with the same shape but `const` pointers and `rgba` as the default when it
   is zero: the reader writes through the pointers and the writer only reads, so one type for
   both would need a cast.
8. **Short input is `MINIPSD_ERR_SHORT` plus `minipsd_reader_needed`**, not `MINIPSD_MORE`:
   `MORE` already means "an item was produced, call again" in the iterators.
9. **Limits default to the format's own caps.** The checks always run; only a caller that sets a
   limit gets a stricter reader.
10. **Un-matting needs an `A` plane in the target**, so the workspace stays independent of the
    file.
11. **One `minipsd_decode_opts`** carries the row band and the matte option; `NULL` is the
    defaults, and new options go at its end.
12. **Resources are iterated, not looked up by id.**
13. **A Pascal-only name is read as Latin-1**; the raw view is there for a caller that knows the
    code page.
14. **Layer views carry R, G, B and A only**, with a count of the channels left out; masks join
    with the layer-mask feature.
15. **`init` and `next_layer` describe any version-1 PSD**; `UNSUPPORTED` comes from the pixel
    decoding (S4.4, S4.5) and, in `init`, for a PSB only. Found while implementing S4.2: listing
    a CMYK file's layers is `R1`'s job.
