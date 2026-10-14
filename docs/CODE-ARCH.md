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
  write_file.c                 stdio, UTF-8 path on Windows (today's minipsd_write_file, moved)
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
| Version, linkage, return codes | `minipsd_bytes { const uint8_t *data; size_t size; }` | `minipsd_error_string`; new codes, added never renumbered: `MINIPSD_MORE = 1` (positive: not an error, call again), `MINIPSD_ERR_FORMAT = -6`, `MINIPSD_ERR_UNSUPPORTED = -7` | `base.h` |
| Document | `minipsd_doc`, `minipsd_layer` (unchanged from 0.1) | `minipsd_doc_check(doc)`: the validation `minipsd_write` does today, now callable on its own | `writer.h` today; its own file if the reader shares it |
| Writer | `minipsd_writer` (cursor state, fields private) | `minipsd_writer_workspace_size`, `_init`, `_total`, `_fill`; `minipsd_write` | `writer.h`; detail: `bytes`, `utf`, `packbits`, `composite` |
| Reader | `minipsd_reader`, `minipsd_info`, `minipsd_layer_view` | `minipsd_reader_init`, `_next_layer`, `minipsd_decode_layer`, `minipsd_decode_composite`, `minipsd_layer_name` | `reader.h`; detail: `bytes`, `utf`, `packbits`, `composite` |

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
- **Optional subsystems hide behind macros in `minipsd.h`**, the way `MINIPSD_NO_STDIO` does.
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
int      minipsd_writer_init (minipsd_writer *w, const minipsd_doc *doc, void *workspace, size_t size);
uint64_t minipsd_writer_total(const minipsd_writer *w);              /* the file size, known after init */
int      minipsd_writer_fill (minipsd_writer *w, void *buf, size_t cap, size_t *produced);
                             /* MINIPSD_MORE: call again; MINIPSD_OK: the file is complete */
```

- **`init` does all the sizing work.** It runs today's counting pass: every layer channel's
  compressed size and every composite row's size. So `total` is exact before the first byte,
  which a buffer target or a `Content-Length` header needs. All validation happens here too,
  so `fill` cannot fail on the document.
- **The workspace holds:**
  - the per-channel size table (`layer_count * 4` values);
  - the composite row sizes (`4 * height` values);
  - one composite row (`width * 4` bytes);
  - one staging area: one encoded row, at least 64 bytes for the headers.

  That is today's allocations plus the row sizes, which today are recomputed.
- **Cursor.** The writer's state is a position in a fixed sequence of pieces:
  `section, layer, channel, row`. It also tracks how much of the current staged piece has been
  handed out. `fill` stages the next piece, copies what fits, and returns.
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
int minipsd_reader_init      (minipsd_reader *r, const void *data, size_t size, minipsd_info *info);
int minipsd_reader_next_layer(minipsd_reader *r, minipsd_layer_view *layer);   /* MINIPSD_MORE / MINIPSD_OK at the end */
int minipsd_decode_layer     (const minipsd_reader *r, const minipsd_layer_view *layer,
                              uint8_t *rgba, size_t row_stride);
int minipsd_decode_composite (const minipsd_reader *r, uint8_t *rgba, size_t row_stride);   /* white matte removed */
int minipsd_layer_name       (const minipsd_layer_view *layer, char *utf8, size_t cap, size_t *needed);
```

- **`minipsd_info`:** version, size, depth, colour mode, channel count, dpi, the ICC profile as
  a `minipsd_bytes` view, the layer count, and whether the composite carries transparency.
- **`minipsd_layer_view`:** bounds, opacity, flags, blend key, and two name views (Pascal and
  `luni`, raw). It also holds each channel's id and compressed data as a `minipsd_bytes` view
  into the caller's buffer.
- **Forward iteration, no table.** Layer records have variable length, so `next_layer` walks
  them in order. It adds up the channel lengths as it goes, so each view already points at its
  pixel data and needs no second pass and no storage.
- **The input is untrusted.**
  - Every length is checked against the bytes remaining, in the form
    `if (remaining < n) fail`, never as `p + n > end`.
  - Every size computation is overflow-checked.
  - A malformed file returns `MINIPSD_ERR_FORMAT`.
  - A valid file outside the supported subset returns `MINIPSD_ERR_UNSUPPORTED` (for example
    CMYK or 32-bit), and so does any feature the reader does not decode yet.
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
