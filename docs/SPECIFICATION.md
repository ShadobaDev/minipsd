# Specification

The current contract of minipsd: what it is, what it writes, and what must not break.
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
| [Consumer requirements](#consumer-requirements) | guarantees Platemaker depends on |
| [Verification](#verification) | how a format change is proven correct |
| [Distribution and SBOM](#distribution-and-sbom) | how consumers obtain and declare minipsd |
| [Extensibility rules](#extensibility-rules) | how to grow without breaking callers |

---

## Purpose

minipsd is a **header-only, stateless, dependency-free C99 library that writes layered
Photoshop (PSD) files**. It exists because no header-only PSD writer was available; it was
extracted from [Platemaker](https://github.com/ShadobaDev/PlateMaker), which uses it to save
canvas templates.

- **Stateless:** every call takes a complete document description and produces a complete file.
  No handles, no global state, no init/shutdown.
- **Writer only.** Reading is out of scope today (see [TODO.md](TODO.md)).
- **Lightweight first, extensible second:** a feature is added only when it can be added
  without making the simple case harder.

## PSD in five minutes

A PSD file is a sequence of five sections, all integers **big-endian**:

| # | Section | What minipsd puts there |
|---|---|---|
| 1 | **File header** (26 bytes): signature `8BPS`, version (1 = PSD, 2 = PSB), channel count, height, width, bit depth, colour mode | version 1, 4 channels, 8-bit, RGB |
| 2 | **Colour mode data**: a length, then a palette for indexed/duotone modes | empty (length 0) |
| 3 | **Image resources**: a length, then `8BIM` blocks, each with a numeric ID | 1005 resolution, optionally 1039 ICC profile |
| 4 | **Layer and mask information**: layer records (bounds, channels, blend mode, opacity, flags, name, *additional layer info* blocks such as `luni`), then each layer's channel pixel data | the layers |
| 5 | **Image data**: the merged ("composite") image, used by readers that ignore layers and for thumbnails | the composite |

Channel pixel data is either raw or **PackBits RLE**: a per-row byte-count table followed by
the compressed rows. Sides are capped at 30 000 px in PSD; PSB (version 2) lifts that to
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
| `minipsd_error_string(code)` | a short English text for a return code |

| Type | What it is |
|---|---|
| `minipsd_doc`, `minipsd_layer` | the document: canvas, dpi, layers bottom first, ICC profile, compression |
| `minipsd_workspace` | `void`: caller memory lent to the writer, sized by `minipsd_writer_workspace_size` |
| `minipsd_writer` | opaque; built by `minipsd_writer_init` inside the workspace |
| `minipsd_write_fn` | the callback of `minipsd_write`: non-zero to go on, 0 to abort |

**Defaults and field meanings**
- A zero-initialised document selects RLE (`MINIPSD_RLE = 0`).
- `opacity = 0` means a fully transparent layer; it is **not** "default opaque".
- `rgba = NULL` is an empty layer; `name = NULL` is an empty name.
- Layers are listed bottom first.

**Return codes**: errors are negative, so `rc < 0` is the failure test for every function.

| Code | Value | Meaning |
|---|---|---|
| `MINIPSD_OK` | 0 | success |
| `MINIPSD_MORE` | 1 | not an error: `minipsd_writer_fill` has more bytes, call again |
| `MINIPSD_ERR_ARG` | -1 | NULL document or callback, no layers, ICC size without data, or a dpi that 16.16 fixed point cannot hold (see below) |
| `MINIPSD_ERR_SIZE` | -2 | a side is 0 or above `MINIPSD_MAX_SIDE`, or a section is longer than its 32-bit length field allows (see below) |
| `MINIPSD_ERR_WRITE` | -3 | the callback returned 0 |
| `MINIPSD_ERR_ALLOC` | -4 | `MINIPSD_MALLOC` returned NULL |
| `MINIPSD_ERR_OPEN` | -5 | not returned by minipsd; reserved for the caller's file code (`examples/write_file.c`) |

## Memory and ownership

minipsd owns nothing the caller passes in, never copies a document, and allocates only inside
`minipsd_write`, freeing it before it returns. Everything else is the
caller's memory:

| Name | Owner | Size | Must stay valid | May live in |
|---|---|---|---|---|
| `minipsd_doc`, `minipsd_layer[]` | caller; borrowed | `sizeof`; `layer_count` layers | the call; for the pull writer, from init for as long as the writer is used | stack, static, heap |
| raw document data: layer pixels (`rgba`), names, ICC profile | caller; borrowed, read in place, never copied | `width * height * 4` bytes per layer | as `minipsd_doc` | wherever the application keeps it |
| `minipsd_workspace` | caller; lent to the writer | `minipsd_writer_workspace_size()` | for as long as the writer is used | heap; stack or static sized for the largest document |
| `minipsd_writer` | inside `minipsd_workspace`; opaque | part of the workspace | as the workspace | the caller keeps only the pointer |
| raw PSD bytes | passed to the callback of `minipsd_write()` | pieces of up to 4 KiB, in order | the callback copies what it keeps | the callback decides |
| output buffer, `buf` of `minipsd_writer_fill()` | caller | any `cap`, 1 included; a few rows (64 KiB) keeps re-encoding negligible | the call | stack, static or heap |
| `minipsd_write()` internals | minipsd: the workspace through `MINIPSD_MALLOC` / `MINIPSD_FREE`, a 4 KiB chunk on the stack | `minipsd_writer_workspace_size()` | the call | heap and stack |

- **The document is read-only and frozen while in use.** The pull writer sizes everything in
  `minipsd_writer_init`; a document changed afterwards would no longer match the sizes already
  recorded.
- **Nothing to release.** The library holds no memory between calls: the writer ends with its
  workspace, which the caller may free or reuse for another document (if large enough).
- **No global or static mutable state.** Writers in separate threads never interact; one workspace
  serves one writer at a time.

## What goes into the file

- PSD version 1, 8 bits per channel, RGB, 4 channels (R, G, B, transparency).
- **Layers:**
  - each covers the full canvas, or is empty (a 0x0 rectangle whose channels hold no data);
  - name twice: a legacy Pascal string (ASCII, every non-ASCII character replaced by `?`) and a
    `luni` block in UTF-16 with surrogate pairs;
  - opacity;
  - visibility: flag bit `0x02` set means hidden;
  - blend mode `norm` only.
- **Composite image:**
  - the visible layers combined with straight-alpha "over";
  - transparency is the first extra channel, signalled by writing the layer count as a
    **negative** number, as the format specifies;
  - **colour is matted against white:** `c*a + 255*(1-a)`. Photoshop stores it this way and
    readers subtract the white back out.
- **Image resources:**
  - 1005 ResolutionInfo: dpi as 16.16 fixed point, unit inches;
  - 1039 ICC profile, only when `icc` is set;
  - a resource of odd length is padded to even.
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
  Removing the white matte makes it fail with the `(255, 0, 105)` pink.
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
  initialisers, C++) must keep compiling.
- **New error codes and enum values are added, never renumbered.**
- **The composite must match the layers.** A feature that changes how layers look (blend modes,
  masks, groups) must also be reflected in the composite, or be documented as invisible to
  composite-only readers.
- **Optional subsystems hide behind macros**, the way `MINIPSD_NO_MALLOC` does, so the minimal
  build stays minimal. Reading, if it comes, is a separate opt-in implementation block.
