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
- [ ] **S4.10** Release **0.3.0** per [GITHUB-RELEASE.md](GITHUB-RELEASE.md), with stages 4 to 6
      and S7.1, the API frozen from then:
      `minipsd_write_opts` became a parameter of the write calls, a break, so it is a MINOR under
      the pre-1.0 rules. Before it: regenerate [COMPATIBILITY.md](COMPATIBILITY.md) with the new
      version, and have one CI job fetch psd-tools' files (`MINIPSD_PSD_TOOLS_FILES`; done: `linux-gcc`), so that
      Photoshop's ZIP files are checked through zlib there too, not only locally.

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
- [x] **S5.5** Unknown content kept (`E2`): `blend_mode`, `clipping` and `additional_info` on the layer,
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
      returns it in `minipsd_layer_view.blend_mode`, the writer writes `minipsd_layer.blend_mode` as given
      (S5.5); the composed composite draws them all as `norm`. Photoshop's keys are in the
      spec's [Blend modes](SPECIFICATION.md#blend-modes).
      *Done when:* a document with a layer in every mode reads back with every key from
      `psd-tools` (`roundtrip.py`, "blend-modes"); GIMP shows each mode, checked by hand once.
- [x] **S6.3** Layer groups (`lsct`): `section_divider_type` on the layer and in the view; the writer writes
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
- [x] **S6.8** The composite's channels beyond RGBA (alpha and spot channels) kept, and whether
      its fourth is transparency (`composite_alpha`). Not kept, by the rule in the spec's
      [round trip](SPECIFICATION.md#what-is-read): a layer's channel order (Procreate writes
      transparency last) is the writer's to choose, and the two bytes Procreate writes past the
      layer info are outside the format.
- [x] **S6.9** ZIP channels (compression 2, and 3 with prediction), Photoshop's 16- and 32-bit
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
      implements no deflate: ZIP is written from the caller's buffers (stored ZIP channels,
      copied as they are), and through the caller's deflate (S6.11).
      Without the function, decoding them stays `MINIPSD_ERR_UNSUPPORTED`.
      *Done when:* psd-tools' 16-bit RGB files decode, layers and masks, to the samples psd-tools
      gives, through zlib in the tests; the fuzzer decodes ZIP channels through zlib.
      As done: the scratch is always the caller's (no dense-plane path: one channel of memory
      saved, a second path to test); 32 bits is not decoded at all, so no byte shuffle. zlib is
      Python's, in `roundtrip.py` (CI: minipsd's own files re-encoded as ZIP by psd-tools;
      locally, psd-tools' 16-bit files); the C tests and the fuzzer inflate streams of stored
      blocks (`tests/zlib_stored.h`), which keeps every toolchain free of a fetched zlib.
      `minipsd_read` keeps its scratch in its block.
- [x] **S6.10** S6.7 against the round-trip rule: what it keeps that is layout the writer may
      choose, or outside the format, goes. Kept as content: blending ranges, a layer without
      transparency (Photoshop's background), the legacy name, a file without layers, the
      composite's channels; `composite_stored` for E1, `Lr16` until someone checks Photoshop
      reads 16-bit layers from the layer info. Gone: the order of resources and `luni`, the
      layer info's padding (`layer_info_align`; always even), empty layers' channels and
      inverted rectangles, a missing global layer mask, a section of length 0.
      `roundtrip.py` checks content and a second pass instead of bytes.
- [x] **S6.11** ZIP written through the caller's deflate, the counterpart of S6.9's inflate:
      `minipsd_deflate_fn` and its `user` at the end of `minipsd_write_opts`, and
      `MINIPSD_ZIP` and `MINIPSD_ZIP_PREDICTED` as the document's `compression`, for the
      layers' and masks' channels; the composite is then RLE, as Photoshop writes it. A channel's
      size goes in its record ahead of the data, so the sizing pass deflates each channel once and
      the writing pass once more, as RLE is packed twice; the deflate must give the same bytes
      both times, and the workspace holds the largest channel raw and its stream. A deflate that
      fails, or changes its mind, is `MINIPSD_ERR_WRITE`.
      *Done when:* documents written as ZIP, 8 and 16 bits, predicted or not, decode in psd-tools
      to the samples given (`roundtrip.py`, through Python's zlib), and psd-tools' 16-bit files
      read decoded are written back ZIP-predicted with the same pixels; the unit tests write and
      read ZIP through stored blocks, deflate failures included.

- [x] **S6.12** ZIP in bands without inflating again: a band of a ZIP channel inflates the
      whole channel, so decoding a layer in strips, as libvips asks for it, inflates it once per
      strip. The caller says its scratch already holds that channel (a field at the end of
      `minipsd_decode_opts`), and minipsd takes the rows from there.
      As done: `scratch_kept`. A scratch with room for every ZIP channel the target asks for
      holds each in its own part, R, G, B, A in order; less room shares one part, as before, so
      `minipsd_read` keeps a scratch of one channel. Kept without room for all is
      `MINIPSD_ERR_ARG`.
      *Done when:* a ZIP layer decoded in bands of one row calls the inflate once per channel,
      with the same samples as a whole decode.
- [x] **S6.13** Every colour mode and depth, read and written, as the file stores them, no colour
      conversion (the ICC profile, the palette or the duotone data say what samples mean):
      - *design.* A target that names its channels beyond R, G, B, A: grey, C, M, Y, K
        (inverted, as Photoshop stores them), L, a, b, an index, a multichannel's N channels;
        32-bit samples as `float`, 1-bit ones as bytes of 0 or 1, or packed; the palette
        (colour mode data) and the duotone data as views. Which modes have layers in
        Photoshop's files (bitmap, indexed and multichannel have none) decides what the
        writer refuses.
      - **Read.** The composite first, then layers: grey, CMYK, Lab, multichannel and duotone
        of 8 and 16 bits; grey and RGB of 32; bitmap of 1; indexed of 8.
        As done: `MINIPSD_PLANES` (5) planes, the mode's colour channels then transparency at
        `color_channels`; 1-bit samples as bytes (no packed form); `color_mode_data` a view; the
        composite un-matted in RGB only (psd-tools does the same; no file shows other modes);
        Photoshop's `Mtrn`/`Mt16`/`Mt32` say the composite has transparency. `roundtrip.py`
        checks every psd-tools file's layers and composite against psd-tools. `minipsd_read`
        stays 8- and 16-bit RGB until the writer takes the other modes.
      - **Write.** The same modes at 1, 8, 16 and 32 bits, each where Photoshop allows it; the
        palette and the duotone data written as given. *Moved to S6.19*: S6.13 is done for
        reading.
      *Done when:* psd-tools' files of every mode and depth decode to the samples psd-tools
      gives, and COMPATIBILITY.md has no file described for its mode or depth; documents of
      each mode and depth that minipsd writes decode the same in psd-tools and in minipsd; the
      files that psd-tools lacks come from GIMP or Photopea, private fixtures checked by hand
      there.
- [x] **S6.14** Threads: the reader's decode calls stated thread-safe on one reader, or one
      reader a thread (its workspace is a few hundred bytes), whichever the code supports
      without a lock; a scratch is one per thread. In the spec's Memory and ownership.
      As done: thread-safe on one reader, beside the thread that walks it, when the buffer
      holds the whole file (only `MINIPSD_ERR_SHORT` writes the reader, its `needed`). CI's
      `linux-tsan` runs every test under ThreadSanitizer.
      *Done when:* a test decodes the bands of one layer from several threads at once under
      ThreadSanitizer, or the spec says why it must not.
- [~] **S6.15** Streaming read (`R9`), *deferred* until a consumer needs it. Its trigger is a
      source that is neither memory nor mmap: an asynchronous one (WASM's `Blob.slice`, HTTP
      Range) or a PSB too large for a 32-bit address space. Done so far: the reader reads
      through a window of the file (`base`, `size`), and `MINIPSD_ERR_SHORT` records the whole
      range a call needs; `minipsd_reader_init` sets the window at offset 0.
      - **Why not now.** PSD's layout sets the floor, not minipsd. Records come before every
        layer's data, channels are planar, and the composite comes last with its RLE tables
        first. Read forward only, an interleaved band therefore needs the whole compressed layer
        (or composite) held. A band of a few KiB needs seeks and several ranges a call. mmap
        already gives libvips and files on 64 bits all of that.
      - **The mechanism, when it comes.** Resumable calls: a call returns `MINIPSD_ERR_SHORT`
        with the range it needs, the caller fetches it and hands a new window, and the call is
        repeated. A read callback cannot serve an asynchronous source. Lengths precede the data,
        so the reader knows each unit's size before reading it, and a unit over the caller's
        budget is refused before a byte is read. Units: the resources whole at init, the
        records whole, one channel or one layer for a decode; decoding by offset, not by a
        view's pointer.
- [x] **S6.16** Names, before 0.3.0 (S4.10) freezes them: fields take Adobe's descriptive names in
      snake_case, the four-character key in their comment; where Adobe gives none or an
      ambiguous one, psd-tools'. `kind` and `MINIPSD_KIND_*` (psd-tools' name for `lsct`'s
      "type") become `section_type` and `MINIPSD_SECTION_*`, or stay, decided here.
      As done: the rule in the spec's API (Names), with its exceptions. `kind` became
      `section_divider_type` and `MINIPSD_KIND_*` `MINIPSD_SECTION_*`, keeping their group
      words; `blend` `blend_mode`, `extra` `additional_info`, a mask's `color` `default_color`,
      `color_data` `color_mode_data`. `dpi`, `format` and the two names stay.
      *Done when:* the rule is in the spec, and every public field follows it or says why not.
- [x] **S6.17** Typed views of kept data: what the file's own bytes say, parsed into plain values;
      what needs another tool stays out. No allocation, no tree built: a view in, a small
      struct or one scalar out, as `minipsd_layer_blending_range` does. ImageMagick parses
      only the resolution, the "has merged image" flag and four profiles; psd-tools nearly
      everything. In order:
      - **Resources by name (P1).** Constants for the ids of standard data (EXIF 1058, XMP 1060,
        IPTC 1028, thumbnail 1036, version info 1057, alpha names 1006 and 1045, alpha ids
        1053, transparency index 1047, guides 1032) and `minipsd_reader_find_resource`. EXIF,
        XMP and IPTC stay bytes: other formats, parsed by libexif and expat.
        *Done:* `MINIPSD_RES_*`, and `minipsd_reader_find_resource` returns the first of an id
        or `MINIPSD_ERR_NOT_FOUND` (a new code).
      - **What libvips needs (P1).** Version info's "has real merged data" (whether the
        composite can be trusted; ImageMagick flattens the layers without it); the resolution
        whole, horizontal and vertical with their units.
        *Done:* `minipsd_resource_resolution_info` and `minipsd_resource_version_info` (in
        `views.h`; a resource in, no reader), `minipsd_utf16be_to_utf8` for the names.
        `roundtrip.py` checks both against psd-tools on its whole set.
      - **Fixed-layout layer blocks (P2):** `lyid` layer id, `lclr` colour label, `iOpa` fill
        opacity, `lspf` protection, `knko` knockout, `clbl` and `infx` blend clipped and
        interior elements.
        *Done:* `minipsd_layer_block_settings`, one struct with a `present` bit a block.
      - **Fixed-layout resources (P2):** guides, alpha channel names and ids, an indexed file's
        transparency index.
        *Done:* `minipsd_resource_grid_and_guides` and the walks `next_guide`,
        `next_alpha_name` (1045 or 1006), `next_alpha_identifier`; `transparency_index`.
      - **Out:** EngineData (text styling, a nested text format), rendering effects,
        adjustments, blend modes and vectors, parsing EXIF, XMP or IPTC, colour conversion.
      *Done when:* each step's values match psd-tools' on its test files; the P1 ones are
      what S7.3 sets as libvips metadata.
- [ ] **S6.18** Typed views, further (S6.17's P3, split out until a consumer asks):
      - **Old fixed-layout adjustments:** `levl`, `curv`, `brit`, `hue2` and the like.
      - *design.* One-level descriptor reader: one item a call (key, type, a scalar, or a
        nested descriptor or list as a view, never descended into). Descriptor items carry no
        length, so skipping a nested one walks its shape (depth-limited) without building it;
        decide whether that is acceptable. It would give a text layer's text (`TySh`, key
        `Txt `), a smart object's name, effects' flags.
      *Done when:* as S6.17's.
- [ ] **S6.19** Every colour mode and depth written (S6.13's Write half): grey, CMYK, Lab,
      multichannel and duotone at 8 and 16 bits, grey and RGB at 32, bitmap at 1, indexed at 8,
      each where Photoshop's files have layers (bitmap, indexed and multichannel have none); the
      palette and the duotone data written as given. `minipsd_read` then takes every mode. The
      saver (S7.4) needs it for images that are not RGB.
      *Done when:* documents of each mode and depth that minipsd writes decode the same in
      psd-tools and in minipsd, and `minipsd_read` round-trips psd-tools' files of every mode.

## 7. Interoperability

libvips is where minipsd is meant to be used: as a loader and saver of PSD and PSB, and in
programs that render a composite with libvips and write the layers with minipsd. Steps 7.2 on are
the loader and saver; how they reach libvips is not this roadmap's.

- [ ] **S7.1** `examples/vips_composite.c`: a document's composite rendered with libvips
      (`vips_composite`, blend keys mapped as in the spec's
      [Working with libvips](SPECIFICATION.md#working-with-libvips)), then written with it;
      built and run only when pkg-config finds `vips`. A "With libvips" chapter in
      [MANUAL.md](MANUAL.md). The pattern a program such as Platemaker follows to export layered
      PSDs with 0.3.0.
- [ ] **S7.2** *design.* minipsd as a libvips loader and saver:
      - **Loading.** What the image is: the composite by default, one layer by an option, its
        offset in the metadata. Metadata: ICC profile, resolution, interpretation, sample
        format.
      - **Access.** Strips from several threads (S6.12, S6.14).
      - **Saving.** One image as a layer with its composite, or pages as layers.
      - **What minipsd still needs.** Every colour mode and depth written (S6.19); the row provider (S7.5);
        the metadata ImageMagick gives (S6.17, P1).
      - **Building.** The build dependency: pkg-config, a meson wrap.
- [ ] **S7.3** Loader: a PSD or PSB as a libvips image, the composite or a layer, decoded in
      strips; ZIP through zlib.
      *Done when:* psd-tools' files that minipsd decodes load with the samples psd-tools gives;
      a large PSB loads with memory bounded by strips, not by the image.
- [ ] **S7.4** Saver: an image as a one-layer PSD or PSB with its composite, the image whole in
      memory; RLE, or ZIP through zlib.
      *Done when:* psd-tools and GIMP open what it writes, and the loader reads it back the same.
- [ ] **S7.5** Row provider (`W6`): a layer's or the composite's rows asked from a callback instead
      of an `rgba` pointer, twice with RLE or ZIP (sizing, then writing), so it must be
      deterministic, as libvips pipelines are. The saver then streams.
      *Done when:* the saver writes an image larger than the memory it is given, the bytes the
      same as from memory.

## 8. Test quality

Deferred: lines are at 100 % and branches near 97 %, so it waits for a quiet moment, with any
release.

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
- Layer-at-a-time producer (`W8`); the row provider (`W6`) is S7.5.
- Parallel encoding split API (`W9`).
- Segment output for zero-copy saves (`E3`).
- In-place patching of fixed-size fields.
- Making `packbits` or `composite` public.
- The C++ facade.
- A layer's own alpha and spot channels (ids other than R, G, B, transparency and the masks):
  no file we have holds one, and Photoshop does not write them. Until a file shows one, such a
  layer sets `MINIPSD_LOST_CHANNELS`.
