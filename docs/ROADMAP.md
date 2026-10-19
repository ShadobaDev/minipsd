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
| [8. Test quality](#8-test-quality) | branch coverage of compound conditions | with any release |
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
- [x] **S5.3** Composite supplied by the caller (`W7`): `composite` and `composite_matted`; the
      writer mattes a supplied composite unless it is marked matted, and skips composing.
      `NULL` keeps today's composed composite, documented as a best effort.
      *Done when:* golden test unchanged; a supplied composite equal to the composed one gives
      the same bytes; a matted one is written as it is; composing is skipped (counted in a test).
- [x] **S5.4** `minipsd_read` (`R7`), into a `minipsd_doc` the writer accepts as it is:
      - `minipsd_read_size` and `minipsd_read_into`, the caller's block;
      - `minipsd_read` through `MINIPSD_MALLOC`, `minipsd_doc_free`;
      - everything copied, so the file's buffer can be freed at once; the file's composite kept
        as stored, `composite_matted` set; `lost` with a `TODO` per known loss.
      *Done when:* every file of the golden set reads and writes back to the same bytes (`E4`
      through `minipsd_read`); every 8-bit RGB file of psd-tools' set reads, and its `lost`
      matches what `examples/read_file.c` shows of it; the test frees the file's buffer before
      writing, under ASan.
- [x] **S5.5** Unknown content kept (`E2`): `blend`, `clipping` and `extra` on the layer,
      `resources` and `layer_tail` on the document, written back as given; the reader gains
      `clipping` in the view and `layer_tail` in `minipsd_info`; the document's fields win over
      kept data with the same key (`luni`, 1005, 1039). `MINIPSD_LOST_EXTRA` goes: no release had
      it. Also kept: the record's other flag bits (`flags`) and resource names.
      *Done when:* a file of psd-tools' set with a text layer survives `minipsd_read` and
      `minipsd_write`: every kept block identical byte for byte (checked by `psd-tools`); GIMP
      opens it with the text layer recognised, checked by hand once.
- [x] **S5.6** Pass-through re-save (`E1`): `stored` on the layer and `opts->stored` in
      `minipsd_read`; `opts` is a parameter of the three read functions. The writer copies stored
      channels instead of encoding pixels.
      *Done when:* reading a file with `stored`, changing one layer and writing again encodes
      only that layer (counted in a test); the untouched layers' channel bytes are identical to
      the input's.
- [x] **S5.7** Docs: a Round trip chapter in [MANUAL.md](MANUAL.md) with
      `examples/resave.c` (read, change one layer, write), built and run by CI, and the request
      to report a non-zero `lost`; the spec's What goes into the file and Memory and ownership.

## 6. Format features

Each step is serialization: the document model, writer and reader, verified by `psd-tools`
and by hand in one real application. The composite composed as a best effort does not learn the
feature; a supplied composite needs nothing. See
[Features through the modules](CODE-ARCH.md#features-through-the-modules).

- [x] **S6.1** Layers smaller than the canvas: moved to S5.2, which the round trip needs first.
- [x] **S6.2** Blend modes. Each mode is a four-character key in the layer record: the reader
      returns it in `minipsd_layer_view.blend`, the writer writes `minipsd_layer.blend` as given
      (S5.5); the composed composite draws them all as `norm`. Photoshop's keys are in the
      spec's [Blend modes](SPECIFICATION.md#blend-modes).
      *Done when:* a document with a layer in every mode reads back with every key from
      `psd-tools` (`roundtrip.py`, "blend-modes"); GIMP shows each mode, checked by hand once.
- [x] **S6.3** Layer groups (`lsct`): `kind` on the layer and in the view; the writer writes
      the `lsct` block with the group's mode (`pass` by default), or a kept one of the same kind
      as it is; `minipsd_doc_check` checks the nesting.
      *Done when:* nested groups, one closed, written from scratch read back by `psd-tools` with
      their structure and modes; every group file of psd-tools' set keeps its blocks through
      `minipsd_read` and `minipsd_write`; GIMP shows the groups, the closed one closed, checked by
      hand once.
- [x] **S6.4** Layer masks: `minipsd_mask` on the layer (rectangle, colour, flags, pixels or
      stored channel; a file's real mask, -3, and mask parameters kept as they are), the mask in
      the view, `minipsd_decode_mask`; `MINIPSD_LOST_MASK` goes, no release had it.
      *Done when:* a mask written from scratch reads back from `psd-tools` with its rectangle,
      colour and pixels; every mask of psd-tools' set survives `minipsd_read` and
      `minipsd_write` with the same mask data and pixels; the fuzzer decodes masks; GIMP shows the
      mask, checked by hand once.
- [x] **S6.5** 16-bit channels: `depth` on `minipsd_doc` (samples `uint16_t` in host order,
      written big-endian, layers in the layer info); `depth` on `minipsd_decode_opts`, which must
      be the file's, and `minipsd_target_rgba16`; layers Photoshop keeps in its global `Lr16` (or
      `Lr32`) block read as the file's; `minipsd_read` at 16 bits, `Lr16` dropped from
      `layer_tail`. A stored layer without transparency is copied and gets an opaque one, so
      Photoshop's 16-bit files, whose channels are ZIP, round trip with `opts->stored`.
      *Done when:* a 16-bit document written from scratch reads back from `psd-tools` with every
      channel's samples and the composite (`roundtrip.py`, "deep-16"); psd-tools' 16-bit RGB
      files go through `minipsd_read` (stored) and `minipsd_write` with their blocks and channel
      data byte for byte; the fuzzer decodes at 16 bits; GIMP opens `deep-16.psd`, checked by
      hand once.
- [x] **S6.6** PSB (version 2): the same format with wider lengths (sections, channels, the blocks
      of 21 keys) and RLE row counts of four bytes. Decided: one `format` on the document, the
      layout of everything in it; the output format a write option; the writer translates kept
      bytes when the two differ (see [CODE-ARCH.md](CODE-ARCH.md), PSB row).
  - [x] **S6.6a** Read: `format` in `minipsd_info` and the layer view, `MINIPSD_MAX_SIDE_PSB`,
        wide lengths and counts in the reader, the long keys in the block walks and the `Lr16`
        search; `minipsd_read` refuses a PSB (`MINIPSD_ERR_UNSUPPORTED`) until S6.6b.
        *Done when:* every PSB of psd-tools' set is read, whole and in parts; its layers and
        composite decode to what psd-tools decompresses from the same file (`roundtrip.py`);
        COMPATIBILITY.md refuses none.
  - [x] **S6.6b** Write: `format` on `minipsd_doc`, the document written in its own format; v2
        layout; `minipsd_doc_check` against that format's limits; `minipsd_read` of a PSB.
        Documents from scratch, and PSB to PSB with kept bytes copied. `detail/format.h` holds
        what the two formats differ in, for the reader and the writer.
        *Done when:* a PSB written from scratch reads back from `psd-tools`; psd-tools' PSB files
        go through `minipsd_read` and `minipsd_write` with their blocks and channels byte for
        byte; GIMP opens a PSB written from scratch, checked by hand once.
  - [x] **S6.6c** Convert: an output format on the write calls, zero being the document's; kept
        bytes translated both ways when the formats differ (RLE count tables, long keys'
        lengths); PSB to PSD refused with `MINIPSD_ERR_SIZE` when it does not fit. Long keys by
        policy: neutral ones (descriptors, parameters, paths) have their length translated;
        layer structures (`Lr16`, `Lr32`, `Layr`) are refused until their insides are; the
        unverified (`Alph`, `FEid`, `FXid`, `FELS`, embedded files until their structure is
        checked) are refused until verification puts each in one group or the other.
        *Done when:* psd-tools' PSB files written as PSD, and PSD files as PSB, read back from
        `psd-tools` with the same layers, blocks and pixels.
- [x] **S6.7** A file read and written back is the same file, byte for byte: what `minipsd_read`
      keeps, `minipsd_write` puts back where the file had it.
      *Done when:* every file of psd-tools' set that `minipsd_read` takes comes back identical
      through `minipsd_read` and `minipsd_write`, but for a listed few with the reason for each;
      `roundtrip.py` checks it.
  - [x] **S6.7a** Blending ranges ("Blend If") kept on the layer, typed as the file stores them;
        `MINIPSD_LOST_BLENDING_RANGES` no longer set.
  - [x] **S6.7b** Order: the writer's resolution and profile where the file's were, its `luni`
        where the file's was.
  - [x] **S6.7c** The composite as the file stores it: its channels, its encoding, the layer
        count's sign.
  - [x] **S6.7d** A layer without a transparency channel written without one.
  - [x] **S6.7e** Names: the legacy name and `luni` kept as stored while the name is unchanged.
  - [x] **S6.7f** An empty layer's channels kept as stored; the layer info's padding (Photoshop:
        4, others 2 or none).
  - [x] **S6.7g** An empty layer's inverted rectangle, a layer tail the file does not have.
  - [x] **S6.7h** Layers in `Lr16`, files without layers.
- [ ] **S6.8** Channels beyond RGBA (alpha and spot channels, of layers and of the composite)
      kept, in the file's order (GIMP writes transparency last); until then
      `MINIPSD_LOST_CHANNELS`.
- [ ] **S6.9** ZIP channels (compression 2, and 3 with prediction), Photoshop's 16- and 32-bit
      layers, decoded through the caller's inflate: minipsd implements no deflate. A function
      pointer and its `user` at the end of `minipsd_decode_opts` (and of `minipsd_read_opts`, so
      `minipsd_read` decodes them without `stored`), shaped as `inflate(user, src, src_size, dst,
      dst_size)`; zlib's `uncompress`, libdeflate or miniz fit it in a few lines. The stream is
      zlib (RFC 1950), not a ZIP archive; its size after inflating is known from the view: the
      rectangle times the sample size. minipsd hands the stream and that size, undoes the
      prediction (part of the format: byte sums at 8 bits, big-endian sample sums at 16, the
      byte shuffle at 32) and spreads the samples into the target as for RLE. The reader still
      allocates nothing: a dense plane (`pixel_step` the sample size) takes the inflated channel
      directly; any other target needs a scratch buffer of one channel from the caller. minipsd
      implements no deflate: ZIP is written only from the caller's buffers (stored ZIP channels,
      copied as they are), and later perhaps through the caller's deflate (Not scheduled).
      Without the function, decoding them stays `MINIPSD_ERR_UNSUPPORTED`.
      *Done when:* psd-tools' 16-bit RGB files decode, layers and masks, to the samples psd-tools
      gives, through zlib in the tests; the fuzzer decodes ZIP channels through zlib.

## 7. Interoperability

- [ ] **S7.1** `examples/vips_composite.c`: a document's composite rendered with libvips
      (`vips_composite`, blend keys mapped as in the spec's
      [Working with libvips](SPECIFICATION.md#working-with-libvips)), then written with it;
      built and run only when pkg-config finds `vips`. A "With libvips" chapter in
      [MANUAL.md](MANUAL.md).

## 8. Test quality

- [ ] **S8.1** Branch coverage of compound conditions: each operand of an `&&` / `||` taken on its
      own, where today only the main path is tested. The list as gcovr gives it (CI's
      `linux-coverage`, "Missing" with `--txt-metric branch`): blend keys with a zero first byte
      (`writer.h`, the group mode and the record's key); `lsct` blocks shorter than 4 bytes or of
      another kind than the layer's; `flags` with 0x02 and `hidden` 0; a kept 1005 not 16 bytes
      long; a resource name with a size and no data; mask and `stored` combinations without area;
      each header field refused on its own (a version 0 or 3 among them); each NULL argument of
      `minipsd_decode_mask`; `minipsd_read` over its limit through a layer rather than the
      composite.
      *Done when:* those lines leave gcovr's "Missing" column. What stays there is unreachable by
      construction (the second pass of `mpsd__read`, a block above `SIZE_MAX` on 64 bits, a
      `malloc` that does not align), which puts the ceiling at about 97-98 %.

## Not scheduled

Large cases. Each would start with a *design* step, and only when a real consumer asks for it.

- The composed composite learning blend modes, masks, clipping or groups: a supplied composite
  is the way to get them right.
- Streaming parser without a whole blob (`R9`).
- Row provider for pixels not in memory (`W6`); layer-at-a-time producer (`W8`).
- Parallel encoding split API (`W9`).
- ZIP written through the caller's deflate, a field of `minipsd_write_opts` beside the inflate of
  S6.9: compression and the composite stay outside minipsd, as libvips provides both.
- Segment output for zero-copy saves (`E3`).
- In-place patching of fixed-size fields.
- Making `packbits` or `composite` public.
- The C++ facade.
