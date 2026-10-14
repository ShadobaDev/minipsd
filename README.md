# minipsd

[![CI](https://github.com/ShadobaDev/minipsd/actions/workflows/ci.yml/badge.svg?branch=master)](https://github.com/ShadobaDev/minipsd/actions/workflows/ci.yml)
[![Coverage](https://coveralls.io/repos/github/ShadobaDev/minipsd/badge.svg?branch=master)](https://coveralls.io/github/ShadobaDev/minipsd?branch=master)
[![Release](https://img.shields.io/github/v/release/ShadobaDev/minipsd)](https://github.com/ShadobaDev/minipsd/releases/latest)
[![License: MIT](https://img.shields.io/github/license/ShadobaDev/minipsd)](LICENSE.MIT)

> A header-only, dependency-free C99 library that writes layered Photoshop (PSD) files.

- **Header-only**, stb style: `minipsd.h` is the whole API; its implementation (`minipsd/`)
  compiles in one source file. C99, also compiles as C++.
- **Stateless:** describe the document in a struct, get the file. No handles, no global state.
- **Streams through a callback**, so the output can go to memory, a file, a pipe or a socket.
- **Small allocations only:** layer pixels are read in place, never copied.
- **Deterministic:** the same document always produces the same bytes.
- **MIT licensed.**

minipsd was written for [Platemaker](https://github.com/ShadobaDev/PlateMaker), a canvas tool
for webcomic artists, to save canvas templates that Photoshop, GIMP, Krita, Photopea and
Procreate open as layered documents.

# Status

**release: 0.1.0**
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

int rc = minipsd_write_file(&doc, "template.psd");
if (rc != MINIPSD_OK) puts(minipsd_error_string(rc));
```

To keep the bytes in memory instead, call `minipsd_write(&doc, callback, user)`.

## What it writes

An 8-bit RGB document with transparency: full-canvas or empty layers with UTF-8 names,
opacity and visibility; the merged composite image; the resolution; an optional ICC profile.
Channels are PackBits RLE or raw. Each side is limited to 30 000 px.

Not written (yet): masks, groups, blend modes other than normal, 16-bit, PSB, reading.
The full contract is in [docs/SPECIFICATION.md](docs/SPECIFICATION.md).

## Adding it to a project

Copy the `include/` directory, or use CMake (FetchContent, git submodule, `find_package`),
pkg-config, an archive or a `.deb`; see [docs/INSTALL.md](docs/INSTALL.md).

## Configuration

Define before the implementation include:

| Macro | Effect |
|---|---|
| `MINIPSD_STATIC` | make every function `static` (nothing exported) |
| `MINIPSD_MALLOC` / `MINIPSD_FREE` | replace `malloc` / `free` |
| `MINIPSD_NO_STDIO` | leave out `minipsd_write_file` and `<stdio.h>` |

## Documentation

| Document | Contents |
|---|---|
| [docs/INSTALL.md](docs/INSTALL.md) | every way of adding minipsd to a project, SBOM |
| [docs/SPECIFICATION.md](docs/SPECIFICATION.md) | the contract: API, file contents, guarantees, PSD primer |
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
