# Use cases

What users may want from the reader and the writer, and what the library would need to offer
for each case. This is a catalogue of situations and requirements, not a contract and not code.
[CODE-ARCH.md](CODE-ARCH.md) turns the chosen cases into design, and
[ROADMAP.md](ROADMAP.md) schedules them.

The IDs (`R*`, `W*`, `E*`, `X*`) are stable. [ROADMAP.md](ROADMAP.md) refers to them, so a case
is never renumbered; a dropped case is marked as dropped.

---

## Who uses it

| Actor | Typical goal | What matters to them |
|---|---|---|
| **Generator app** (Platemaker, chart or report tools, procedural art) | write a fresh layered file | simple API, deterministic bytes, a buffer in memory |
| **Editor / importer** (paint app, game engine import) | open a whole document, edit it, save it | complete data, fidelity, round trip |
| **Viewer / thumbnailer** (file manager, web gallery, asset browser) | show a picture quickly | speed, little memory, the composite only |
| **Asset pipeline / CLI converter** (export each layer to PNG, sprite sheets) | walk the layers, one at a time | memory bounded by one layer, batch speed |
| **Server** (upload validation, conversion) | accept untrusted files safely | limits, no crash on garbage, no decompression bomb |
| **Embedded / constrained** | write (rarely read) with no heap | fixed workspace, small output buffer |

## A fact that drives every strategy: the file layout

```
header | colour mode | resources | layer records (with every channel's length) | layer channel data | composite
  26 B     4 B + 0      small        small                                          bulk                 bulk
```

- **Metadata comes first.** Everything except pixels sits in the first few kilobytes.
  - Lengths at fixed places let a reader skip whole sections.
  - The layer records hold every channel's byte length, so the offset of any channel is a sum
    of earlier lengths, without decoding anything.
  - So a **prefix** of the file is enough for all metadata.
- **Channel data is planar.** For each layer, channel by channel, all rows of R, then all rows
  of G, and so on. With RLE, each channel starts with a table of row byte counts, so a reader
  can find any row without decoding the rows above it.
- **The composite is last and has no length field.** It runs to the end of the file.
- **Writing is forward-only, but the counts come first.** Every channel's compressed size must
  be written in the records before any pixel data. So an RLE writer must know every layer's
  compressed size before it emits the first pixel, which today means a sizing pass.
  - RAW sizes are known without compressing.
  - The composite needs every visible layer at the same row.

---

## Reader

Input is in principle a blob: the whole file, or a prefix of it, in memory (read in or
`mmap`ed).

### Reader use cases

| ID | Situation | Reads | Memory beyond the blob | What the library needs |
|---|---|---|---|---|
| **R1** | **Inspect:** size, dpi, colour mode, layer names, list in a file manager or validator | header, resources, records | none | metadata views into the blob; works on a **prefix**, and when the prefix is too short, says how many bytes it needs |
| **R2** | **Thumbnail from resource 1036** (the JPEG Photoshop embeds) | resources | none | the resource as a raw `bytes` view; decoding the JPEG is the caller's job |
| **R3** | **Flat preview:** a viewer that ignores layers | composite | the target image, or one band of rows | skip to the composite by section lengths; decode into the caller's buffer; un-matting optional |
| **R4** | **Region / band decode:** a huge canvas, show only the viewport, or process in strips | rows y0..y1 of one image | one band | row access through the RLE row table; output for a row range |
| **R5** | **One layer on demand:** "export layer *Template*", lazy loading for a layers panel | one layer's channels | that layer | find a layer by index or name without decoding the others; optional caller-held **index** (offset per layer) for O(1) repeated access |
| **R6** | **Walk all layers, one at a time:** batch export, sprite sheet | each layer in turn | one layer at a time | forward iteration; decode, hand over, the caller reuses its buffer |
| **R7** | **Full import:** an editor loads everything | all | all decoded pixels | either (a) two phases: sizes first, the caller allocates, the library fills; or (b) the library allocates through a **passed allocator** and returns a complete structure with one free call |
| **R8** | **Untrusted upload:** validate, or validate and convert | depends | bounded | caller-set **limits** (max side, max layers, max decoded bytes) checked before decoding; a distinct error for "over the limit" versus "malformed" |
| **R9** | **Streaming read without the whole blob:** convert while a network upload arrives | in file order | records + one channel | a push parser fed in chunks that emits events: info, layer record, channel rows, composite rows. It must buffer the records section. The most complex case, and the only one without a blob |
| **R10** | **Preserve what is not understood:** text layers, effects, smart objects | records | none | raw views of each unknown "additional layer info" block (key + bytes) and unknown resources, so an editor can carry them into the next save (see E2) |

### How the reader returns data: the duplicate-memory question

What costs memory is the following:
- **The blob.** It is RLE-compressed, usually much smaller than the decoded pixels for flat
  art.
  - Read into RAM, it costs its size.
  - `mmap`ed, it costs almost nothing resident.
- **The decoded pixels.** An editor needs them anyway; they are its working data.
- **A real duplicate appears only when pixels are decoded twice**: into a library buffer, then
  copied into the application's own format. That is avoided by decoding **straight into the
  caller's target**. So the target must be described flexibly, as in X2 below:
  interleaved RGBA, planar, any stride, a sub-rectangle of a bigger canvas.

Three ways to return data, and when each fits:

| Strategy | Fits | Peak memory | Library needs |
|---|---|---|---|
| **Query fragments:** metadata, then layer *i*'s metadata, then layer *i*'s pixels on request | R1-R6, R8; also the base for the other two | blob + what the caller asks for | nothing from the caller; zero-copy views |
| **Two-phase complete:** the library reports every size, the caller allocates, the library fills a full structure | R7 for a caller with its own allocator or pool | blob + decoded | nothing; the caller owns all memory |
| **Library-allocated complete structure** | R7, the simplest call for a small tool | blob + decoded (+ structure bookkeeping) | a **passed allocator** (or an arena); a matching free function |

The first strategy is the primitive, and the other two are built on it. So "fragments" is the
core, and "complete" is a convenience layer, the way `minipsd_write` sits on top of the writer
state machine.

---

## Writer

The writer turns a project structure into raw bytes, ready to store or send.

### Writer use cases

| ID | Situation | Constraint | What the library needs |
|---|---|---|---|
| **W1** | **One shot into memory:** Platemaker hashes the bytes, then saves | exact size wanted | the **total size** before encoding; then one buffer, from the caller or from a passed allocator |
| **W2** | **Blocking stream:** file, blocking pipe or socket | none | a push callback (as today) |
| **W3** | **Non-blocking stream:** event loop, async socket, HTTP with `Content-Length` | must never wait | the pull state machine (`fill` into the caller's buffer); total size for the header |
| **W4** | **No heap:** embedded, kernel-ish, strict real-time | fixed memory | workspace size query plus a caller workspace; works with a tiny output buffer |
| **W5** | **Cancel and progress:** long save in a GUI | responsiveness | from the pull model: done/total is progress, stopping the pulls is cancel. With push: return 0 from the callback |
| **W6** | **Pixels not all in memory:** huge documents, tiles on disk, GPU readback, procedurally generated rows | memory bounded by rows, not layers | a **row provider** callback (layer, y) in place of `pixels` pointers. With RLE every row is asked for twice (sizing pass and data), so the provider must be deterministic. Alternatives: RAW (no sizing pass), or keep the compressed rows in caller-provided storage |
| **W7** | **Caller supplies the composite:** the app already has its flattened render (more exact for blend modes it implements), or wants speed | CPU | an optional composite source (buffer or row provider); the library then skips blending |
| **W8** | **Layer-at-a-time producer:** a render pipeline makes layer 1, saves, frees it, makes layer 2 | one layer in memory | the records need all sizes first, so either all layer metadata and sizes up front (RAW makes sizes trivial), or each layer is compressed into caller storage first. The composite must be built incrementally or supplied (W7) |
| **W9** | **Parallel encoding:** big documents on many cores | speed | a split API: "encode channel *k* of layer *i* into a buffer" (thread-safe, independent), then "assemble the file from these pieces". The caller runs its own thread pool; the library starts no threads |
| **W10** | **Size estimate:** disk space check, UI | cheap | an upper bound without compressing (RAW size, or the PackBits worst case) |

### Updating an existing file (the "fragmentary update" question)

What a change touches in the bytes:

| Change | Size of the change | Effect on the file |
|---|---|---|
| opacity, visible flag, blend key | fixed-size bytes in one record | **patchable in place**, but the composite becomes stale |
| pixels, RAW, same bounds | same length | **patchable in place**, composite stale |
| pixels, RLE | new compressed length | the record's length changes, and every later byte shifts |
| name, bounds, add/remove layer | the record's length changes | every later byte shifts; section lengths change |
| composite | RAW: same length; RLE: may change | it is the last section, so a new length only moves the end of the file |

Conclusions:
- **True in-place patching** is narrow: fixed-size fields, and RAW pixels with the same bounds.
  It always needs the composite rewritten too, because it depends on all layers. Worth offering
  only as a small "patch these fields, then recompute the composite" helper, if at all.
- **A cheap re-save is the general answer.** Unchanged layers are copied as **already
  compressed bytes** from the old blob, with no decode or re-encode. Only changed layers and
  the composite are encoded. Cost: one sequential write at memcpy speed.

### Editing and round trip

| ID | Situation | What the library needs |
|---|---|---|
| **E1** | **Re-save with pass-through:** read, change one layer, write | a layer source that can be "compressed channel bytes + compression + row counts" (views from the reader) in place of RGBA. The writer copies them and does not count them in the sizing pass |
| **E2** | **Keep unknown content:** a text layer survives a save by a tool that does not understand it | the reader exposes unknown blocks (R10); the writer accepts opaque extra blocks per layer and per document and writes them back unchanged |
| **E3** | **Zero-copy save (scatter-gather):** a huge file where most bytes are unchanged | the writer emits a **list of segments** (pointer + length), some pointing into the old blob and some into new buffers; the caller sends them with `writev` / `WriteFileGather` without assembling a new file in memory |
| **E4** | **Lossless round trip test:** decode, encode, compare | for the subset minipsd writes, `encode(decode(x)) == x` (already planned) |

---

## Cross-cutting needs

| ID | Need | Options |
|---|---|---|
| **X1** | **Memory source** | (a) none: caller workspace after a size query; (b) **passed allocator**: a struct with `alloc`, `free` and `user`, given at run time; (c) an **arena**: one caller block that the library carves up, freed all at once by the caller; (d) the compile-time `MINIPSD_MALLOC` (today). (a) is the core; (b) and (c) serve the "complete structure" conveniences; (d) stays for 0.1 compatibility |
| **X2** | **Pixel target and source description** | per channel: base pointer, step between pixels, step between rows. One description covers interleaved RGBA (four pointers one byte apart, step 4), planar, BGRA, any stride and a sub-rectangle |
| **X3** | **Pixel conventions** | straight versus premultiplied alpha; composite un-matted or as stored; channel order |
| **X4** | **Limits** | max side, layer count, decoded bytes, nesting depth for groups; on by default for the reader |
| **X5** | **Determinism** | the same input gives the same bytes on every path: push, pull, segments, parallel, pass-through |
| **X6** | **Threads** | the library never starts threads; independent calls are thread-safe; W9 gives the parallelism |
| **X7** | **Errors** | "need more bytes" (with how many), "over a limit", "malformed", "unsupported", "write failed", "out of memory": each distinct |

---

## Rough grouping

This is not the roadmap, just where each case fits against the current plan in
[CODE-ARCH.md](CODE-ARCH.md):

- **Done:**
  - writer: W1-W5, W10 (total);
  - reader: R1 (prefix reads included), R2-R8, R10 (R7 with the caller's block or one
    `MINIPSD_MALLOC`, no allocator passed);
  - E1 (stored channels copied), E2 (blocks, resources and the layer tail kept), E4; X2 as the decode target, X3, X4, X7.
- **Moderate:** a separate design step each ([ROADMAP.md](ROADMAP.md), stage 5):
  - W7 (supplied composite);
  - X1 (b) and (c).
- **Large, only on real demand:**
  - R9 (streaming parser): PSD's planar layout means a whole compressed layer held when read
    forward only (ROADMAP S6.15);
  - W6 and W8 (row provider, layer-at-a-time); W6's own case, a saver that reads its image once,
    is served without it: the channels encoded by the program, row by row, and given as stored
    (`minipsd_encode_row`, the libvips plugin's `psdsave`);
  - W9 (parallel split API);
  - E3 (segment output);
  - in-place patching.
