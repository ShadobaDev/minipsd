# Installing minipsd

Every way of adding minipsd to a project, and the SBOM fragment each one carries.

**Copy the headers.** Copy the `include/` directory (`minipsd.h` and `minipsd/`) into your
tree and include `minipsd.h`. Every file carries SPDX tags, and `minipsd.h` its version and the
licence, so the copy stays identifiable in an SBOM. From 0.2.0 on, each GitHub Release also
attaches the same headers joined into one `minipsd.h`, for those who prefer a single file.

**CMake FetchContent**

```cmake
include(FetchContent)
FetchContent_Declare(minipsd
    GIT_REPOSITORY https://github.com/ShadobaDev/minipsd.git
    GIT_TAG        0.4.0)
FetchContent_MakeAvailable(minipsd)
target_link_libraries(my-app PRIVATE minipsd::minipsd)
```

**[CPM.cmake](https://github.com/cpm-cmake/CPM.cmake)**

```cmake
CPMAddPackage("gh:ShadobaDev/minipsd#0.4.0")
target_link_libraries(my-app PRIVATE minipsd::minipsd)
```

Use `#0.4.0`, not `@0.4.0`: the `@` form looks for a tag `v0.4.0`, and minipsd tags have no `v`.

**git submodule**

```bash
git submodule add https://github.com/ShadobaDev/minipsd.git external/minipsd
```
```cmake
add_subdirectory(external/minipsd)
target_link_libraries(my-app PRIVATE minipsd::minipsd)
```

As a subproject, minipsd builds no tests, fetches nothing and enables no language.

The libvips plugin, minipsd as libvips' loader and saver, comes its own way:
[The libvips plugin](#the-libvips-plugin) below.

**Installed package.** minipsd installs as a tree with nothing platform-specific in it:

```
include/minipsd.h         the public API
include/minipsd/          its implementation
share/cmake/minipsd/      CMake package (find_package)
share/pkgconfig/minipsd.pc
share/doc/minipsd/        LICENSE.MIT, minipsd.spdx.json
```

You can get that tree in any of these ways:

- **From the source, with CMake** (any system):
  ```bash
  cmake -S minipsd -B build -DMINIPSD_BUILD_TESTS=OFF
  cmake --install build --prefix /usr/local        # Linux: with sudo
  ```
- **As an archive**, `minipsd-X.Y.Z.tar.gz` or `.zip`: use it unpacked where it is, or extract
  it into a prefix. The tree is relocatable, so nothing needs regenerating:
  ```bash
  sudo tar -xzf minipsd-0.4.0.tar.gz --strip-components=1 -C /usr/local
  ```
- **As a Debian/Ubuntu package**, `libminipsd-dev_X.Y.Z_all.deb`:
  ```bash
  sudo apt install ./libminipsd-dev_0.4.0_all.deb
  ```

The archive and the `.deb` are built with CPack, see
[CHEATSHEET.md](CHEATSHEET.md#packages-and-installing).

Then, in CMake (add `-DCMAKE_PREFIX_PATH=<tree>` unless the tree is in a system prefix):

```cmake
find_package(minipsd CONFIG REQUIRED)
target_link_libraries(my-app PRIVATE minipsd::minipsd)
```

or, without CMake, `pkg-config --cflags minipsd`.

**SBOM.** Every CMake path gives the same SPDX 2.3 fragment for minipsd, ready to merge into your
own SBOM:

```cmake
get_target_property(minipsd_sbom minipsd::minipsd MINIPSD_SBOM_FILE)
```

## The libvips plugin

The plugin makes minipsd libvips' loader and saver of PSD and PSB (the manual's
[PSD in libvips](MANUAL.md#psd-in-libvips-the-plugin)). It is built from source, as part of your
program, against your libvips' headers: its classes extend libvips' own, so a compiled one would
hold for one libvips version only. Link `minipsd::vips`, then call `minipsd_vips_init()` after
`VIPS_INIT`.

**From the release.** Each GitHub Release has `minipsd-vips-X.Y.Z.zip` (and its `.sha256`): the
plugin as a CMake project of its own, with minipsd's single header inside it, private to the
plugin, so a program's own minipsd, of any version, is left alone:

```cmake
include(FetchContent)
FetchContent_Declare(minipsd_vips
    URL      https://github.com/ShadobaDev/minipsd/releases/download/0.4.0/minipsd-vips-0.4.0.zip
    URL_HASH SHA256=<the hash in minipsd-vips-0.4.0.zip.sha256>)
FetchContent_MakeAvailable(minipsd_vips)
target_link_libraries(my-app PRIVATE minipsd::vips)
```

`add_subdirectory` of the unpacked zip does the same. On its own, the zip builds and tests its
example (`cmake -S`, `cmake --build`, `ctest`); without CMake, its README says what to compile.

**From minipsd's source.** In any of the ways at the top, turn on `MINIPSD_VIPS_PLUGIN` before
adding minipsd:

```cmake
set(MINIPSD_VIPS_PLUGIN ON)
FetchContent_MakeAvailable(minipsd)
target_link_libraries(my-app PRIVATE minipsd::vips)
```

**Where libvips comes from.** Either way the plugin enables C and, unless told otherwise, finds
libvips itself ([cmake/minipsd_libvips.cmake](../cmake/minipsd_libvips.cmake)):

- **Linux, macOS:** pkg-config, the system's libvips and zlib. Nothing is fetched.
- **Windows:** libvips' official prebuilt build, `vips-dev-x64-web`, pinned, and zlib's headers
  (zlib 1.3.1, pinned, about 1.5 MB), which that build lacks; the plugin links the zlib the
  build carries, `libz1`, for ZIP channels.

A program that fetches libvips' Windows build itself points the plugin at it, before adding the
plugin. Then one libvips is fetched, and the plugin is compiled against the headers of the DLLs
the program ships:

```cmake
# after the program's own FetchContent_MakeAvailable(my_vips) of vips-dev-x64-*.zip
FetchContent_GetProperties(my_vips SOURCE_DIR my_vips_dir)
set(FETCHCONTENT_SOURCE_DIR_MINIPSD_LIBVIPS "${my_vips_dir}")   # the plugin takes this libvips
FetchContent_MakeAvailable(minipsd_vips)                        # or minipsd, with MINIPSD_VIPS_PLUGIN
```

The directory is libvips' Windows build unpacked as libvips releases it: the plugin takes
`include/`, `lib/` (the import libraries MSVC links) and `bin/` (the DLLs MinGW links) from it.
zlib's headers are still fetched: `zlib.h` is always there, whichever libvips is used.

A program whose libvips comes from anywhere else defines two targets before adding the plugin,
and nothing is fetched or found: `minipsd_libvips` (libvips, glib, gobject) and
`minipsd_libvips_zlib` (zlib's headers and the zlib that libvips links), each an `INTERFACE`
library or `IMPORTED GLOBAL`.
