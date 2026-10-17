# Roadmap

The order of work from 0.1.0 to the design in [CODE-ARCH.md](CODE-ARCH.md). Case IDs (`R1`,
`W3`, ...) refer to [USE-CASES.md](USE-CASES.md).

**How to read this**
- **One checkbox is one step:** one turn of work that ends at a stop for review, with a
  proposed commit message. Steps run in order; a later step may start only when the earlier
  ones are ticked or explicitly dropped.
- **"Done when"** is the acceptance condition. Every step also follows the stage rules in
  [CODE-ARCH.md](CODE-ARCH.md#how-development-proceeds):
  - the golden test and the full suite pass first;
  - `psd-tools` checks every byte change;
  - coverage holds;
  - the spec follows the code.
- **Step numbers are not versions.** `S1.2` is stage 1, step 2. Finished steps collect on
  `master` with no release.
- **There are only two planned releases:**
  - **S3.4**, which releases 0.2.0 (stages 1 to 3);
  - **S4.10**, which releases the reader, when the maintainer decides.

  After that, stages 5 and 6 are released in batches, when there is enough to ship and the
  maintainer decides. Version numbers follow the semver rules in [TODO.md](TODO.md): pre-1.0, a break
  bumps the MINOR and an addition bumps the PATCH.
- **Docs follow the API.** A step that adds or changes a public function or buffer also updates
  [MANUAL.md](MANUAL.md) and the spec's
  [Memory and ownership](SPECIFICATION.md#memory-and-ownership) table.
- **Design steps** (marked *design*) change only `docs/`. They exist where
  [USE-CASES.md](USE-CASES.md) raised a question that [CODE-ARCH.md](CODE-ARCH.md) does not yet
  answer.

| Stage | Result | Released in |
|---|---|---|
| [0. Baseline](#0-baseline) | design accepted, byte output frozen by a test | — |
| [1. Module split](#1-module-split) | the API stays in `minipsd.h`, the implementation becomes modules | 0.2.0, at S3.4 |
| [2. Pull writer](#2-pull-writer) | non-blocking, heap-free writing | 0.2.0, at S3.4 |
| [3. I/O out of the core](#3-io-out-of-the-core) | stdio leaves the library; examples instead | **0.2.0**, at S3.4 (breaking) |
| [4. Reader](#4-reader) | buffer to structure, for minipsd's own subset | the next release, at S4.10 |
| [5. Round trip and conveniences](#5-round-trip-and-conveniences) | layer rectangles, full import, cheap re-save, kept unknown content | batched, when decided |
| [6. Format features](#6-format-features) | blend modes, groups, masks, 16-bit, PSB | batched, when decided |
| [7. Interoperability](#7-interoperability) | libvips | batched, when decided |
| [Not scheduled](#not-scheduled) | large cases, only on real demand | — |

---

## 0. Baseline

- [x] **S0.1** Target architecture, [CODE-ARCH.md](CODE-ARCH.md), reviewed; decisions recorded.
- [x] **S0.2** Use cases catalogued, [USE-CASES.md](USE-CASES.md).
- [x] **S0.3** Golden test: `tests/test_golden.cpp` writes a fixed set of documents and compares
      each file's SHA-256 against hashes recorded from the 0.1.0 code. The set covers:
      - RLE and RAW;
      - an empty layer, a hidden layer, partial opacity;
      - an ICC profile with an odd size;
      - a non-ASCII name and a 300-character name;
      - odd widths;
      - a run longer than 128 bytes.
      *Done when:* it passes on every CI configuration, and changing one output byte makes it
      fail.

## 1. Module split

No API change, no byte change. Code moves verbatim.
See [Files and modules](CODE-ARCH.md#files-and-modules).

- [x] **S1.1** `include/minipsd/base.h` and `document.h`; `include/minipsd.h` becomes the
      umbrella. The per-module `MPSD__<MODULE>_DONE` guards are added.
      *Done when:* the existing tests pass unchanged, through the umbrella and through each
      module included alone.
- [x] **S1.2** `detail/bytes.h`, `detail/utf.h`, `detail/packbits.h`, `detail/composite.h`, and
      `writer.h` holding `minipsd_write` and (still) `minipsd_write_file`.
      *Done when:* golden test unchanged, and no unused-function warning in any one-module build.
- [x] **S1.3** Tests split into `test_<module>.cpp`. The existing tests move; no parallel copies.
      Detail modules are tested directly.
      *Done when:* coverage per module is 100 % lines and at least 95 % branches.
- [x] **S1.4** CMake and packaging:
      - install `include/minipsd/`;
      - the SBOM fragment lists every header with its SHA-256;
      - the consumer tests cover a raw copy of the `include/` directory;
      - the Conan recipe in `temp/packaging/` copies the directory, and `check.ps1` passes.
- [x] **S1.5** `tools/amalgamate.cmake`. The CI builds the test suite against the amalgamated file
      too, and the release workflow attaches it as an artifact
      ([Decisions](CODE-ARCH.md#decisions), 3).
      *Done when:* both builds give the golden bytes.
- [x] **S1.6** The whole public API in `include/minipsd.h`; `include/minipsd/` holds only the
      implementation ([Decisions](CODE-ARCH.md#decisions), 6). `document.h` is gone: it had no
      implementation.
      *Done when:* golden test unchanged, `minipsd.h` compiles alone and then with its
      implementation in the same file, with no internal function unused.
- [x] **S1.7** Docs:
      - [SPECIFICATION.md](SPECIFICATION.md): Form, Distribution and SBOM;
      - [CODING_STYLE_C.md](CODING_STYLE_C.md): the multi-header layout, section order per
        module;
      - [CHEATSHEET.md](CHEATSHEET.md);
      - [INSTALL.md](INSTALL.md): raw copy means the directory.

## 2. Pull writer

No byte change. See [Writer](CODE-ARCH.md#writer).

- [x] **S2.1** `minipsd_doc_check` public. New return code `MINIPSD_MORE = 1`, so success means
      `rc >= 0`; documented where callers compare against `MINIPSD_OK`.
- [x] **S2.2** `minipsd_writer_workspace_size`, `minipsd_writer_init`, `minipsd_writer_total`:
      the sizing pass runs into a caller workspace, which may have any alignment. Serves `W1`,
      `W4`, `W10`.
      *Done when:* `total` equals the golden file sizes.
- [x] **S2.3** `minipsd_writer_fill`: the cursor state machine. Serves `W3`, `W5`.
      *Done when:* the golden bytes come out with any `cap` (1, 7, random, the exact total), and
      through a fake non-blocking target that accepts 0 to k bytes per round.
- [x] **S2.4** `minipsd_write` rebuilt on `init` and `fill`; `MINIPSD_NO_MALLOC` removes it and
      `<stdlib.h>`. Serves `W2`.
      *Done when:* golden test unchanged, and `test_alloc_failure.cpp` still fails each
      allocation in turn.
- [x] **S2.5** Docs: the spec's API section; the allocation list in the header comment;
      [MANUAL.md](MANUAL.md), its writing chapters.

## 3. I/O out of the core

The only breaking stage. See [Decisions](CODE-ARCH.md#decisions), 1-2.

- [x] **S3.1** `examples/`, built and run as ctest tests in CI, never installed:
      - `write_file.c`: what `minipsd_write_file` does, rebuilt in user code on
        `minipsd_write` and `fopen` (`_wfopen` for UTF-8 paths on Windows), the replacement
        S3.2 points to;
      - `write_memory.c`: an exact buffer from `minipsd_writer_total`;
      - `write_nonblocking.c`: an `O_NONBLOCK` pipe with `poll`, POSIX only.
      *Done when:* each example's output matches the golden bytes.
- [x] **S3.2** `minipsd_write_file` removed. `MINIPSD_NO_STDIO` accepted with no effect;
      `MINIPSD_ERR_OPEN` stays defined. Its tests move to the examples (`/dev/full` included).
- [x] **S3.3** Docs: spec (API, Consumer requirements 5), [INSTALL.md](INSTALL.md), `README.md`
      usage, [MANUAL.md](MANUAL.md) (code taken from `examples/`), [CHANGELOG.md](CHANGELOG.md)
      with the migration line.
- [x] **S3.4** Release **0.2.0** per [GITHUB-RELEASE.md](GITHUB-RELEASE.md). Then bump the vcpkg
      and Conan recipes in `temp/packaging/` and move Platemaker to 0.2.0 (in its own
      repository).

## 4. Reader

Additive. See [Reader](CODE-ARCH.md#reader).

- [x] **S4.1** *design.* Bring into [CODE-ARCH.md](CODE-ARCH.md#reader):
      - the pixel target descriptor (`X2`): per channel a pointer, a pixel step and a row
        step;
      - pixel conventions (`X3`);
      - caller limits (`X4`);
      - the error set (`X7`): "need more bytes" with a count, "over a limit";
      - reading from a prefix (`R1`);
      - decoding a range of rows (`R4`).
      Decide whether the writer's layer source uses the same descriptor.
      *Done when:* reviewed.
- [x] **S4.2** `minipsd_reader_init`:
      - header and section lengths;
      - resources: dpi, the ICC profile, every other resource as a raw view (`R2`);
      - on a prefix, how many bytes are needed (`R1`).
- [x] **S4.3** `minipsd_reader_next_layer` and `minipsd_layer_name`:
      - records, channel views, both names;
      - unknown additional-info blocks as raw views (`R10`).
      Serves `R6`.
- [x] **S4.4** `minipsd_decode_layer` into the target descriptor. Serves `R5`, `R6`.
- [x] **S4.5** `minipsd_decode_composite` with un-matting, whole image or a range of rows. Serves
      `R3`, `R4`.
- [x] **S4.6** Limits (`R8`, `X4`), checked before any decoding.
- [x] **S4.7** Tests:
      - `encode(decode(x)) == x` over the golden set (`E4`);
      - files from other programs, read whole and in parts, every 8-bit RGB layer and composite
        decoded: psd-tools' 279 test files (Photoshop, one from GIMP), fetched with
        `MINIPSD_PSD_TOOLS_FILES` and not in CI yet, and a 19-layer Procreate project kept
        locally in `tests/fixtures/`. Files saved by Krita and Photopea are still missing: they
        go to `tests/data/` with their origin and licence when someone saves them;
      - `examples/read_file.c`.
- [x] **S4.8** Fuzzing, a local tool and not a CI job: a libFuzzer target under ASan and
      UBSan (`tests/fuzz/`, the `linux-fuzz` preset), seeded from `tests/data/`, the writer's
      own file and psd-tools' files, run by hand for as long as wanted. CI keeps to tests that
      guard against regressions: each finding, once fixed, is kept in `tests/fuzz/regressions/`,
      which the ordinary tests replay on every toolchain.
      *Done when:* a local run of a fixed time (an hour) ends with no finding. Done: an hour,
      5.3 million inputs, none failed.
- [x] **S4.9** Docs: the spec gains the reader:
      - Purpose: writes and reads, no hidden state in place of "stateless", no "writer only";
      - a new section, What is read: which files are described and which decoded, what each
        record gives, what is not read;
      - Extensibility rules: the reader needs no opt-in macro;
      - README (what it reads), the header's brief, the CMake description (SBOM, `.pc`,
        packages), USE-CASES (what is done), CODE-ARCH (stages).
- [ ] **S4.10** Release **0.2.1** per [GITHUB-RELEASE.md](GITHUB-RELEASE.md): nothing since
      0.2.0 broke the API, so it is a PATCH under the pre-1.0 rules. Before it: regenerate
      [COMPATIBILITY.md](COMPATIBILITY.md) with the new version, and decide whether CI fetches
      psd-tools' files (`MINIPSD_PSD_TOOLS_FILES`).

## 5. Round trip and conveniences

Built on the reader and the pull writer: a file read, changed and written again. Every addition
to the document is a field at the end of `minipsd_layer` or `minipsd_doc` whose zero keeps
today's bytes ([Extensibility rules](SPECIFICATION.md#extensibility-rules)), so the stage breaks
no API and the golden test holds throughout.

The design is [Round trip](CODE-ARCH.md#round-trip) in CODE-ARCH. In short: the document
gains the fields a real file needs, `minipsd_read` turns bytes into a standalone document, the
composite is supplied by the caller (or composed by the writer as a best effort, as today), and
whatever cannot be carried is reported.

- [x] **S5.1** *design.* [Round trip](CODE-ARCH.md#round-trip): what a round trip keeps; the
      composite supplied or composed as a best effort; the fields the document and the reader
      gain; `minipsd_read` and its memory (one block, nothing pointing into the file); loss;
      which of two sources wins. The libvips bridge in the spec's
      [Working with libvips](SPECIFICATION.md#working-with-libvips).
- [x] **S5.2** Layer rectangles in the writer: a layer of any size, anywhere, also partly or
      wholly off the canvas; the composed composite blends at the offset and clips to the
      canvas. Moved from S6.1; the reader already returns the rectangle.
      *Done when:* golden test unchanged with every rectangle zero; `psd-tools` reads the
      rectangles and the composite of a test set (smaller, offset, off the canvas, empty);
      a real application shows the layers in place, checked by hand once (GIMP).
- [ ] **S5.3** Composite supplied by the caller (`W7`): `composite` and `composite_matted`; the
      writer mattes a supplied composite unless it is marked matted, and skips composing.
      `NULL` keeps today's composed composite, documented as a best effort.
      *Done when:* golden test unchanged; a supplied composite equal to the composed one gives
      the same bytes; a matted one is written as it is; composing is skipped (counted in a test).
- [ ] **S5.4** `minipsd_read` (`R7`), into a `minipsd_doc` the writer accepts as it is:
      - `minipsd_read_size` and `minipsd_read_into`, the caller's block;
      - `minipsd_read` through `MINIPSD_MALLOC`, `minipsd_doc_free`;
      - everything copied, so the file's buffer can be freed at once; the file's composite kept
        as stored, `composite_matted` set; `lost` with a `TODO` per known loss.
      *Done when:* every file of the golden set reads and writes back to the same bytes (`E4`
      through `minipsd_read`); every 8-bit RGB file of psd-tools' set reads, and its `lost`
      matches what `examples/read_file.c` shows of it; the test frees the file's buffer before
      writing, under ASan.
- [ ] **S5.5** Unknown content kept (`E2`): `blend`, `clipping` and `extra` on the layer,
      `resources` and `layer_tail` on the document, written back as given; the reader gains
      `clipping` in the view and `layer_tail` in `minipsd_info`; the document's fields win over
      kept data with the same key (`luni`, 1005, 1039).
      *Done when:* a file of psd-tools' set with a text layer survives `minipsd_read` and
      `minipsd_write`: every kept block identical byte for byte (checked by `psd-tools`), the
      text shown by Photoshop or Photopea, checked by hand once.
- [ ] **S5.6** Pass-through re-save (`E1`): `stored` on the layer and `opts->stored` in
      `minipsd_read`. The writer copies stored channels instead of encoding pixels.
      *Done when:* reading a file with `stored`, changing one layer and writing again encodes
      only that layer (counted in a test); the untouched layers' channel bytes are identical to
      the input's.
- [ ] **S5.7** Docs: a Round trip chapter in [MANUAL.md](MANUAL.md) with
      `examples/resave.c` (read, change one layer, write), built and run by CI, and the request
      to report a non-zero `lost`; the spec's What goes into the file and Memory and ownership.

## 6. Format features

Each step is serialization: the document model, writer and reader, verified by `psd-tools`
and by hand in one real application. The composite composed as a best effort does not learn the
feature; a supplied composite needs nothing. See
[Features through the modules](CODE-ARCH.md#features-through-the-modules).

- [x] **S6.1** Layers smaller than the canvas: moved to S5.2, which the round trip needs first.
- [ ] **S6.2** Blend modes. Each mode is a four-character key in the layer record: the reader
      already returns it in `minipsd_layer_view.blend`, and the writer writes it from S5.5. What
      is left: every key written, read back by `psd-tools` and shown right by one real
      application, checked by hand; the composed composite draws them all as `norm`.
      Photoshop's full set, in its menu order:

      | Group | Mode | Key |
      |---|---|---|
      | Normal | Normal | `norm` (done) |
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
      | Groups only | Pass Through | `pass` (S6.3) |

      Procreate already writes `sLit` and `colr` (seen in a real file in S4.7).
- [ ] **S6.3** Layer groups (`lsct`).
- [ ] **S6.4** Layer masks.
- [ ] **S6.5** 16-bit channels.
- [ ] **S6.6** PSB (version 2).

## 7. Interoperability

- [ ] **S7.1** `examples/vips_composite.c`: a document's composite rendered with libvips
      (`vips_composite`, blend keys mapped as in the spec's
      [Working with libvips](SPECIFICATION.md#working-with-libvips)), then written with it;
      built and run only when pkg-config finds `vips`. A "With libvips" chapter in
      [MANUAL.md](MANUAL.md).

## Not scheduled

Large cases. Each would start with a *design* step, and only when a real consumer asks for it.

- The composed composite learning blend modes, masks, clipping or groups: a supplied composite
  is the way to get them right.
- Streaming parser without a whole blob (`R9`).
- Row provider for pixels not in memory (`W6`); layer-at-a-time producer (`W8`).
- Parallel encoding split API (`W9`).
- Segment output for zero-copy saves (`E3`).
- In-place patching of fixed-size fields.
- Making `packbits` or `composite` public.
- The C++ facade.
