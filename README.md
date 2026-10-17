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

**release: 0.2.0**
The API is [include/minipsd.h](include/minipsd.h); the implementation is in
[include/minipsd/](include/minipsd/).

## Example

```c
#define MINIPSD_IMPLEMENTATION      /* in exactly one source file */
#include "minipsd.h"

minipsd_layer layers[2] = {
    { "Template", rgba, 255, 0 },   /* bottom: name, pixels, opacity, hidden */
    { "Layer",    NULL, 255, 0 },   /* empty layer above it                  */
};
minipsd_doc doc = { width, height, 300.0, layers, 2, NULL, 0, MINIPSD_RLE };

static int to_file(void *user, const void *data, size_t size)   /* or to memory, a socket, ... */
{
    return fwrite(data, 1, size, (FILE *)user) == size;
}

int rc = minipsd_write(&doc, to_file, f);
if (rc != MINIPSD_OK) puts(minipsd_error_string(rc));
```

minipsd does no I/O itself: bytes go to your callback, or into your buffer through the pull
writer (`minipsd_writer_*`, no allocation, non-blocking targets included). Complete programs
for a file (UTF-8 paths on Windows), a buffer and a non-blocking pipe are in
[examples/](examples/), with the shortest writer and reader, `hello_write.c` and `hello_read.c`.

## What it writes

An 8-bit RGB document with transparency: full-canvas or empty layers with UTF-8 names,
opacity and visibility; the merged composite image; the resolution; an optional ICC profile.
Channels are PackBits RLE or raw. Each side is limited to 30 000 px.

Not written (yet): layers smaller than the canvas, masks, groups, blend modes other than
normal, 16-bit, PSB. The full contract is in [docs/SPECIFICATION.md](docs/SPECIFICATION.md).

## What it reads

Any PSD (not PSB): its size, colour mode, depth, dpi, ICC profile, resources, and every layer
with its name, rectangle, opacity, visibility, blend mode and raw additional data. Pixels of
8-bit RGB files decode, layers and composite, into memory of any layout. A file can be read from
its start only, and untrusted files are bounded by caller limits. How it does on 315 files from
other programs: [docs/COMPATIBILITY.md](docs/COMPATIBILITY.md).

## Adding it to a project

Copy the `include/` directory, or use CMake (FetchContent, git submodule, `find_package`),
pkg-config, an archive or a `.deb`; see [docs/INSTALL.md](docs/INSTALL.md).

## Configuration

Define before the implementation include:

| Macro | Effect |
|---|---|
| `MINIPSD_STATIC` | make every function `static` (nothing exported) |
| `MINIPSD_MALLOC` / `MINIPSD_FREE` | replace `malloc` / `free` |
| `MINIPSD_NO_MALLOC` | leave out every allocation (`minipsd_write`); the pull writer stays |

## Documentation

| Document | Contents |
|---|---|
| [docs/INSTALL.md](docs/INSTALL.md) | every way of adding minipsd to a project, SBOM |
| [docs/MANUAL.md](docs/MANUAL.md) | how to use it: a PSD written and read in a page of code, then every way of writing and reading |
| [examples/](examples/) | complete programs: the shortest writer and reader, a file, a buffer, a non-blocking pipe, a full dump |
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
