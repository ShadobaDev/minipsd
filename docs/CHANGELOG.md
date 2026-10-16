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
