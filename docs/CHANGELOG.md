# Changelog

Changes between releases, for users of minipsd. Versioning follows the pre-1.0 rules in
[TODO.md](TODO.md): breaking changes bump the MINOR, additions and fixes the PATCH.

## [Unreleased]

### Added
- The reader, first part: `minipsd_reader_init` describes a PSD held in memory (size, depth,
  colour mode, dpi, ICC profile, layer count), from the whole file or a prefix
  (`MINIPSD_ERR_SHORT` with `minipsd_reader_needed`); `minipsd_reader_next_resource`,
  `minipsd_reader_next_layer`, `minipsd_layer_name` and `minipsd_layer_next_block` walk its
  resources, layer records and their blocks as views, with no allocation and no copy.
- `minipsd_decode_layer`: a layer's pixels, or a band of its rows, from an 8-bit RGB file (raw or
  PackBits) into caller memory of any layout, described by `minipsd_target`
  (`minipsd_target_rgba` for interleaved RGBA).
- `minipsd_decode_composite`: the merged image, or a band of its rows, un-matted to straight
  alpha (or as stored, with `keep_matte`).
- `examples/read_file.c`: a PSD described layer by layer, every layer and the composite decoded.
- `examples/hello_write.c` and `examples/hello_read.c`: the shortest programs that write and read
  a PSD, which open a rewritten [MANUAL.md](MANUAL.md): call-order diagrams, a file read in
  parts, memory, return codes and configuration.
- Layers of any size, anywhere: `top, left, bottom, right` at the end of `minipsd_layer`, as in
  `minipsd_layer_view`; all zero keeps the full canvas, and the same bytes. The composite blends
  each layer at its rectangle, clipped to the canvas.
- A composite supplied by the caller: `composite` and `composite_matted` at the end of
  `minipsd_doc`. The writer writes that image, matted against white unless marked as already
  matted, and composes nothing. Without one the layers are composed as before, now documented as
  a best effort.
- `minipsd_read`: a whole 8-bit RGB file into a `minipsd_doc` that `minipsd_write` takes as it
  is, every byte copied into one block so the file can be freed at once; `minipsd_doc_free`;
  `minipsd_read_size` and `minipsd_read_into` for the caller's own memory. `lost`
  (`MINIPSD_LOST_*`) says what the document could not carry: layer masks, blending ranges,
  extra channels.
- Content kept from a file and written back as it is: `blend`, `clipping`, `flags` and `extra`
  (additional-information blocks: text, effects, groups, smart objects) on `minipsd_layer`;
  `resources` and `layer_tail` (global layer mask and blocks) on `minipsd_doc`. The document's
  own fields win over kept data that says the same (`name`, `dpi`, `icc`). The reader gives
  `clipping` and `flags` in the layer view, resource names, and `layer_tail` in `minipsd_info`.
- `examples/read_file.c` also reads the file as a document and prints what it would lose.
- Return codes `MINIPSD_ERR_FORMAT`, `MINIPSD_ERR_UNSUPPORTED`, `MINIPSD_ERR_SHORT`,
  `MINIPSD_ERR_LIMIT`, and `minipsd_limits` for untrusted files: the largest side (canvas, and
  layers when set), the most layers, the most bytes one decode call writes.

## [0.2.0] — 2026-10-15

**Breaking:** `minipsd_write_file` is gone; minipsd does no I/O of its own. To migrate, copy
`write_file` from [examples/write_file.c](../examples/write_file.c) (UTF-8 paths on Windows
included) or pass your own callback to `minipsd_write`. `MINIPSD_NO_STDIO` is still accepted and
does nothing; `MINIPSD_ERR_OPEN` keeps its value.

### Added
- `minipsd_doc_check`: validates a document without writing it.
- `MINIPSD_MORE` (positive, not an error): errors stay negative, so `rc < 0` tests for failure.
- The pull writer, for memory, blocking and non-blocking targets alike, with no allocation:
  `minipsd_writer_workspace_size`, `minipsd_writer_init` and `minipsd_writer_total` give the exact
  file size before the first byte; `minipsd_writer_fill` then produces the file into the caller's
  buffer, any size at a time, never waiting.
- `MINIPSD_NO_MALLOC`: a build without any allocation, and without `<stdlib.h>`; the pull
  writer stays, `minipsd_write` goes.
- `examples/`: complete programs writing to a file, to a buffer and to a non-blocking pipe,
  built and run by CI.

### Removed
- `minipsd_write_file`, and with it every `<stdio.h>` include: see the migration note above.

### Changed
- `minipsd_write` is the pull writer driven to the end: it allocates once (the workspace) instead
  of twice, and its callback receives pieces of up to 4 KiB. The bytes are unchanged.
- The implementation moved out of `minipsd.h` into `include/minipsd/`; `minipsd.h` keeps the
  whole public API. If you copy the header into your tree, copy the `include/` directory instead,
  or take the single-file `minipsd.h` attached to each release.
- The SBOM fragment lists every header with its checksums.

## [0.1.0] — 2026-10-12

### Added
- First standalone release of the PSD writer extracted from Platemaker.
