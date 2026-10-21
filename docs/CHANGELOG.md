# Changelog

Changes between releases, for users of minipsd. Versioning follows the pre-1.0 rules in
[TODO.md](TODO.md): breaking changes bump the MINOR, additions and fixes the PATCH.

## [Unreleased]

**Breaking:** `minipsd_write`, `minipsd_writer_workspace_size`, `minipsd_writer_init` and
`minipsd_doc_check` take a `const minipsd_write_opts *` after the document. To migrate, pass
`NULL`: the document is written as before.

### Added
- The reader, first part: `minipsd_reader_init` describes a PSD held in memory (size, depth,
  colour mode, dpi, ICC profile, layer count), from the whole file or a prefix
  (`MINIPSD_ERR_SHORT` with `minipsd_reader_needed`); `minipsd_reader_next_resource`,
  `minipsd_reader_next_layer`, `minipsd_layer_name` and `minipsd_layer_next_block` walk its
  resources, layer records and their blocks as views, with no allocation and no copy.
- `minipsd_decode_layer`: a layer's pixels, or a band of its rows, into caller memory of any
  layout, described by `minipsd_target` (`minipsd_target_rgba` for interleaved RGBA,
  `minipsd_target_interleaved` for any mode). Every colour mode and depth decodes as stored, with
  no colour conversion. A target's planes are the mode's colour channels, then transparency
  (`minipsd_info.color_channels`, `MINIPSD_PLANES`). Samples are a byte of 0 or 1 at 1 bit, a
  `float` at 32 bits. The palette or duotone data is `minipsd_info.color_data`.
- `minipsd_decode_composite`: the merged image, or a band of its rows, un-matted to straight
  alpha in RGB (or as stored, with `keep_matte`). Photoshop's 16- and 32-bit files mark the
  composite's transparency with a global `Mtrn`, `Mt16` or `Mt32` block rather than a negative
  layer count; both count.
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
- `minipsd_read`: a whole 8- or 16-bit RGB file into a `minipsd_doc` that `minipsd_write` takes as it
  is, every byte copied into one block so the file can be freed at once; `minipsd_doc_free`;
  `minipsd_read_size` and `minipsd_read_into` for the caller's own memory. `lost`
  (`MINIPSD_LOST_*`) says what the document could not carry: extra channels.
- Layer groups: `kind` on `minipsd_layer` and the layer view (`MINIPSD_KIND_*`), written as
  `lsct` blocks with the group's mode, nesting checked by `minipsd_doc_check`.
- Blending ranges ("Blend If"): `minipsd_blend_range` and `minipsd_blending_range`, as the
  file stores them; `blending_ranges` and `blending_range_count` on `minipsd_layer`, written as
  given (none: a length of 0, as before); the same in the layer view, read with
  `minipsd_layer_blending_range`. `minipsd_read` keeps them, so a Photoshop file's ranges are
  written back as they were; `MINIPSD_LOST_BLENDING_RANGES` is no longer set.
- Layer masks: `minipsd_mask` on `minipsd_layer` (rectangle, colour, flags, pixels or the stored
  channel, and a file's real mask and mask parameters kept), the mask in the layer view, and
  `minipsd_decode_mask`.
- 16-bit documents: `depth` on `minipsd_doc`, every sample a `uint16_t` in host order; the
  reader decodes 16-bit layers, masks and composites with `depth` in `minipsd_decode_opts` and
  `minipsd_target_rgba16`, and finds the layers Photoshop keeps in its `Lr16` block.
- ZIP channels (compression 2, and 3 with prediction), Photoshop's 16-bit layers and masks,
  decode through the caller's inflate: `minipsd_inflate_fn`, given with a scratch buffer of one
  channel in `minipsd_decode_opts`, or in `minipsd_read_opts`, whose block then holds the scratch.
  minipsd implements no deflate; zlib's `uncompress` fits in a line. Without one, decoding them
  stays `MINIPSD_ERR_UNSUPPORTED`; read with `stored`, they are copied as they are. Decoded in
  bands, a scratch of every channel and `scratch_kept` inflate each channel once, not once a band.
- The decode calls may run at once on one reader, from any number of threads, each with its own
  target and scratch, when the buffer holds the whole file
  ([Threads](SPECIFICATION.md#threads), which says this for every call).
  `minipsd_read_opts` and `minipsd_decode_opts` grow by these fields: a caller that sets
  `stored` or `depth` on an uninitialised struct now passes garbage as `inflate`. Zero the whole
  struct first (`= {0}`, a designated initialiser, or `memset`), as every options struct asks.
- ZIP written: `MINIPSD_ZIP` and `MINIPSD_ZIP_PREDICTED` as the document's `compression` compress
  the layers' and masks' channels through the caller's deflate, `minipsd_deflate_fn` in
  `minipsd_write_opts` (zlib's `compress2` in a few lines); the composite stays RLE. Each channel
  is deflated twice (sizing, then writing), so the deflate must be deterministic; a failing one
  is `MINIPSD_ERR_WRITE`. A `compression` that is none of the four is now `MINIPSD_ERR_ARG`
  (any value but `MINIPSD_RAW` used to mean RLE).
- PSB, Photoshop's large document format, up to 300 000 px a side (`MINIPSD_MAX_SIDE_PSB`):
  `format` (`MINIPSD_PSD`, `MINIPSD_PSB`) at the end of `minipsd_doc` writes one, and the reader
  reads one through the same calls, `format` in `minipsd_info` and the layer view saying how its
  views are laid out. `minipsd_read` gives a document in the file's format, written back as it
  was. `minipsd_write_opts.format` writes a document in the other format, its kept content
  translated; what cannot be translated yet is `MINIPSD_ERR_UNSUPPORTED`, a PSB too large for a
  PSD `MINIPSD_ERR_SIZE`.
- Pass-through re-save: `minipsd_read_opts.stored` keeps each layer's channels as the file
  stores them (`minipsd_layer.stored`), and the writer copies them instead of encoding pixels, so
  changing one layer encodes only that one. A stored layer without transparency gets an opaque
  transparency channel.
- Content kept from a file and written back as it is: `blend`, `clipping`, `flags` and `extra`
  (additional-information blocks: text, effects, groups, smart objects) on `minipsd_layer`;
  `resources` and `layer_tail` (global layer mask and blocks) on `minipsd_doc`. The document's
  own fields win over kept data that says the same (`name`, `dpi`, `icc`). The reader gives
  `clipping` and `flags` in the layer view, resource names, and `layer_tail` in `minipsd_info`.
- `examples/read_file.c` also reads the file as a document and prints what it would lose.
- `examples/resave.c` and a Round trip chapter in [MANUAL.md](MANUAL.md): a file read, one layer
  changed, the others copied as stored, written back.
- Return codes `MINIPSD_ERR_FORMAT`, `MINIPSD_ERR_UNSUPPORTED`, `MINIPSD_ERR_SHORT`,
  `MINIPSD_ERR_LIMIT`, and `minipsd_limits` for untrusted files: the largest side (canvas, and
  layers when set), the most layers, the most bytes one decode call writes.

### Changed
- `minipsd_read` with `opts->stored` keeps the composite as the file stores it, in
  `composite_stored` and `composite_channels` at the end of `minipsd_doc` (`composite` is then
  `NULL`), and the writer copies it: no decoding, no encoding, every channel written as
  the file has it, alpha and spot channels too (`MINIPSD_LOST_CHANNELS` no longer set for
  them), and `composite_alpha` says whether the fourth is transparency. A supplied `composite`
  still wins.
- A stored layer without a transparency channel is written without one, as its file has it,
  instead of with an opaque one.
- A layer's all-zero rectangle means the full canvas only with `rgba`: stored channels take the
  rectangle as it is.
- A layer's names kept from a file: `name_pascal` at the end of `minipsd_layer` holds its legacy
  name as stored, and while `name` still says what the file's names say, the legacy name and the
  kept `luni` are written as they are (another program's code page, a counted NUL); a renamed
  layer gets the writer's own.
- Photoshop's 16-bit files keep their layers in `Lr16`: `minipsd_read` leaves the block in
  `layer_tail`, empty, and the writer writes the layers there (the layer info empty), also into
  the other format. With `opts->stored`, a file without layers stays without: `layer_count` 0,
  which `minipsd_doc_check` takes with a composite; it is written flat.
- A file read and written back keeps all its content; layout the format leaves to the writer
  is minipsd's own, and bytes outside the format are not reproduced (the spec's round trip
  rule): the writer's own resources and `luni` come first. `tests/psd_tools/roundtrip.py`
  checks every file of psd-tools' set that `minipsd_read` takes: its content, compared through
  psd-tools, and a second pass, which changes no byte.

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
