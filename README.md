# minipsd

> A single-header, dependency-free C99 library that writes layered Photoshop (PSD) files.

- **One header**, stb style. C99, also compiles as C++.
- **Stateless:** describe the document in a struct, get the file. No handles, no global state.
- **Streams through a callback**, so the output can go to memory, a file, a pipe or a socket.
- **Small allocations only:** layer pixels are read in place, never copied.
- **Deterministic:** the same document always produces the same bytes.
- **MIT licensed.**

minipsd was written for [Platemaker](https://github.com/ShadobaDev/PlateMaker), a canvas tool
for webcomic artists, to save canvas templates that Photoshop, GIMP, Krita, Photopea and
Procreate open as layered documents.

> **Status:** pre-release, not yet tagged. The header is [include/minipsd.h](include/minipsd.h);
> CI and the release workflow are next, see [docs/TODO.md](docs/TODO.md).

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

> The tag `0.1.0` below does not exist yet; everything else is in place and tested.

**Copy the header.** Drop `minipsd.h` into your tree. The file carries its own version, licence
and SPDX tags, so it stays identifiable in an SBOM.

**CMake FetchContent**

```cmake
include(FetchContent)
FetchContent_Declare(minipsd
    GIT_REPOSITORY https://github.com/ShadobaDev/minipsd.git
    GIT_TAG        0.1.0)
FetchContent_MakeAvailable(minipsd)
target_link_libraries(my-app PRIVATE minipsd::minipsd)
```

**git submodule**

```bash
git submodule add https://github.com/ShadobaDev/minipsd.git external/minipsd
```
```cmake
add_subdirectory(external/minipsd)
target_link_libraries(my-app PRIVATE minipsd::minipsd)
```

**Installed package** (a distro package or `cmake --install`)

```cmake
find_package(minipsd CONFIG REQUIRED)
target_link_libraries(my-app PRIVATE minipsd::minipsd)
```

or `pkg-config --cflags minipsd`.

As a subproject, minipsd builds no tests, fetches nothing and enables no language.

**SBOM.** Every CMake path gives the same SPDX 2.3 fragment for minipsd, ready to merge into your
own SBOM:

```cmake
get_target_property(minipsd_sbom minipsd::minipsd MINIPSD_SBOM_FILE)
```

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
| [docs/SPECIFICATION.md](docs/SPECIFICATION.md) | the contract: API, file contents, guarantees, PSD primer |
| [docs/CODING_STYLE_C.md](docs/CODING_STYLE_C.md) | C rules: language, naming, comments |
| [docs/CODING_STYLE_CPP.md](docs/CODING_STYLE_CPP.md) | C++ rules (tests, the planned wrapper) |
| [docs/TODO.md](docs/TODO.md) | roadmap and open decisions |
| [docs/CHANGELOG.md](docs/CHANGELOG.md) | changes between releases |
| [docs/CHEATSHEET.md](docs/CHEATSHEET.md) | build and test commands |
| [docs/GITHUB-RELEASE.md](docs/GITHUB-RELEASE.md) | how a release is cut |
| [docs/ISSUES.md](docs/ISSUES.md) | known issues |

## Licence

MIT — see [LICENSE.MIT](LICENSE.MIT).

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). Security issues: [SECURITY.md](SECURITY.md).
