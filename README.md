# minipsd

[![CI](https://github.com/ShadobaDev/minipsd/actions/workflows/ci.yml/badge.svg?branch=master)](https://github.com/ShadobaDev/minipsd/actions/workflows/ci.yml)
[![Coverage](https://coveralls.io/repos/github/ShadobaDev/minipsd/badge.svg?branch=master)](https://coveralls.io/github/ShadobaDev/minipsd?branch=master)
[![Release](https://img.shields.io/github/v/release/ShadobaDev/minipsd)](https://github.com/ShadobaDev/minipsd/releases/latest)
[![License: MIT](https://img.shields.io/github/license/ShadobaDev/minipsd)](LICENSE.MIT)

> A header-only, dependency-free C99 library that writes and reads layered Photoshop (PSD) files.

- **Header-only**, stb style: `minipsd.h` is the whole API; its implementation (`minipsd/`)
  compiles in one source file. C99, also compiles as C++.
- **Simple to call:** describe the document in a struct, get the file; read a file you hold in
  memory, get its layers and pixels. No global state, nothing to initialise.
- **Streams through a callback**, so the output can go to memory, a file, a pipe or a socket;
  or pull the bytes at your own pace, for non-blocking targets.
- **Allocates at most once**, in `minipsd_write`; the pull writer and the reader allocate
  nothing. Pixels are read in place, and a file being read is never copied.
- **Deterministic:** the same document always produces the same bytes.
- **MIT licensed.**

minipsd was written for [Platemaker](https://github.com/ShadobaDev/PlateMaker), a canvas tool
for webcomic artists, to save canvas templates that Photoshop, GIMP, Krita, Photopea and
Procreate open as layered documents.

# Status

**release: 0.3.0**
The API is [include/minipsd.h](include/minipsd.h); the implementation is in
[include/minipsd/](include/minipsd/).

## Example

```c
#define MINIPSD_IMPLEMENTATION      /* in exactly one source file */
#include "minipsd.h"

minipsd_layer layers[2] = {
    { .name = "Template", .pixels = rgba, .opacity = 255 },   /* bottom, the full canvas */
    { .name = "Layer", .opacity = 255 },                    /* empty layer above it    */
};
minipsd_doc doc = { .width = width, .height = height, .dpi = 300.0, .layers = layers, .layer_count = 2 };

static int to_file(void *user, const void *data, size_t size)   /* or to memory, a socket, ... */
{
    return fwrite(data, 1, size, (FILE *)user) == size;
}

int rc = minipsd_write(&doc, NULL, to_file, f);
if (rc != MINIPSD_OK) puts(minipsd_error_string(rc));
```

minipsd does no I/O itself: bytes go to your callback, or into your buffer through the pull
writer (`minipsd_writer_*`, no allocation, non-blocking targets included). Complete programs
for a file (UTF-8 paths on Windows), a buffer and a non-blocking pipe are in
[examples/](examples/), with the shortest writer and reader, `hello_write.c` and `hello_read.c`.

## What it writes

A document in any colour mode at the depths Photoshop has it (RGB, grey, CMYK, Lab, duotone and
multichannel at 8 and 16 bits, RGB and grey at 32, indexed at 8, bitmap at 1), no colour
converted: layers anywhere on or off the canvas (by default the
full canvas), or empty, with UTF-8 names, opacity and visibility; the merged composite image,
yours or composed as a best effort; the resolution; an optional ICC profile; groups, layer
masks, blend modes, clipping, and blocks and resources kept from a file (text, effects), as
they are.
Channels are PackBits RLE or raw, or ZIP through your deflate (zlib's `compress2`: minipsd
implements none). Each side is limited to 30 000 px, or 300 000 px written as a
PSB (Photoshop's large document format).

The writer composes RGB only: other modes and 32 bits bring their composite. The full contract
is in [docs/SPECIFICATION.md](docs/SPECIFICATION.md).

## What it reads

Any PSD or PSB: its size, colour mode, depth, dpi, ICC profile, resources, and every layer
with its name, rectangle, opacity, visibility, blend mode and raw additional data. Metadata
parsed into plain values: resources found by id and named by Adobe's table, the resolution,
version info (whether the composite is real), guides, alpha channel names, and a layer's id,
colour label, fill opacity and locks. Pixels of
every colour mode and depth (1, 8, 16, 32 bits) decode as stored, layers and composite, into
memory of any layout; and whole into a document that `minipsd_write` takes back. ZIP-compressed channels (Photoshop's 16- and 32-bit layers)
decode through the caller's inflate (zlib's `uncompress`, libdeflate or miniz: minipsd implements
no deflate), and are copied as they are when such a file is read and written back. A file can be read from its start only, and untrusted
files are bounded by caller limits. How it does on 315 files from
other programs: [docs/COMPATIBILITY.md](docs/COMPATIBILITY.md).

## Adding it to a project

Copy the `include/` directory, or use CMake (FetchContent, git submodule, `find_package`),
pkg-config, an archive or a `.deb`; see [docs/INSTALL.md](docs/INSTALL.md).

## Configuration

Define before the implementation include:

| Macro | Effect |
|---|---|
| `MINIPSD_STATIC` | make every function `static` (nothing exported); those a file does not call do not warn |
| `MINIPSD_MALLOC` / `MINIPSD_FREE` | replace `malloc` / `free` |
| `MINIPSD_NO_MALLOC` | leave out every allocation (`minipsd_write`); the pull writer stays |
| `MINIPSD_NO_READER` | leave out the reader, the typed views and `minipsd_read`: a program that only writes |
| `MINIPSD_NO_WRITER` | leave out the writer and `minipsd_read`: a program that only reads |

## Documentation

| Document | Contents |
|---|---|
| [docs/INSTALL.md](docs/INSTALL.md) | every way of adding minipsd to a project, SBOM |
| [docs/MANUAL.md](docs/MANUAL.md) | how to use it: a PSD written and read in a page of code, then every way of writing and reading |
| [examples/](examples/) | complete programs: the shortest writer and reader, a file, a buffer, a non-blocking pipe, a full dump, a round trip that changes one layer |
| [docs/SPECIFICATION.md](docs/SPECIFICATION.md) | the contract: API, file contents, guarantees, PSD primer |
| [docs/COMPATIBILITY.md](docs/COMPATIBILITY.md) | the reader on 315 files from other programs (psd-tools' test set): decoded, described, refused |
| [docs/CODING_STYLE_C.md](docs/CODING_STYLE_C.md) | C rules: language, naming, comments |
| [docs/CODING_STYLE_CPP.md](docs/CODING_STYLE_CPP.md) | C++ rules (tests, the planned wrapper) |
| [docs/CODE-ARCH.md](docs/CODE-ARCH.md) | the target architecture: modules, I/O model, reader |
| [docs/USE-CASES.md](docs/USE-CASES.md) | what users may want from the reader and the writer |
| [docs/ROADMAP.md](docs/ROADMAP.md) | the order of work, step by step |
| [docs/TODO.md](docs/TODO.md) | releases, distribution, open decisions |
| [docs/CHANGELOG.md](docs/CHANGELOG.md) | changes between releases |
| [docs/CHEATSHEET.md](docs/CHEATSHEET.md) | build and test commands |
| [docs/GITHUB-RELEASE.md](docs/GITHUB-RELEASE.md) | how a release is cut |
| [docs/ISSUES.md](docs/ISSUES.md) | known issues |

## Licence

MIT — see [LICENSE.MIT](LICENSE.MIT).

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). Security issues: [SECURITY.md](SECURITY.md).
