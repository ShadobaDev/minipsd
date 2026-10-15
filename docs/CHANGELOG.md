# Changelog

Changes between releases, for users of minipsd. Versioning follows the pre-1.0 rules in
[TODO.md](TODO.md): breaking changes bump the MINOR, additions and fixes the PATCH.

## [Unreleased]

### Added
- `minipsd_doc_check`: validates a document without writing it.
- `MINIPSD_MORE` (positive, not an error): errors stay negative, so `rc < 0` tests for failure.
- The pull writer, for memory, blocking and non-blocking targets alike, with no allocation:
  `minipsd_writer_workspace_size`, `minipsd_writer_init` and `minipsd_writer_total` give the exact
  file size before the first byte; `minipsd_writer_fill` then produces the file into the caller's
  buffer, any size at a time, never waiting.
- `MINIPSD_NO_MALLOC`: a build without any allocation, and without `<stdlib.h>`; the pull
  writer stays, `minipsd_write` and `minipsd_write_file` go.

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
