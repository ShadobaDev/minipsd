# minipsd's libvips plugin

minipsd as libvips' loader and saver of PSD and PSB: `vips_image_new_from_file` and
`vips_image_write_to_file` take `.psd` and `.psb` in your program. It reads a file's composite or
any of its layers, with their look, masks and groups, and writes one image, or several as the
layers of one document (`psdsave_layers`). MIT, as minipsd
(`LICENSE.MIT`). Project: <https://github.com/ShadobaDev/minipsd>; the full description is its
manual's "PSD in libvips: the plugin".

## What is here

| | |
|---|---|
| `minipsd_vips.h`, `minipsd_vips.c` | the plugin's API (what a program includes) and its registration |
| `foreign/` | the loader and the saver, laid out as `libvips/foreign/` |
| `foreign/minipsd/minipsd.h` | minipsd, one file, compiled into the plugin and private to it |
| `cmake/minipsd_libvips.cmake` | how libvips is found: Windows fetches libvips' official prebuilt build (pinned), elsewhere pkg-config |
| `CMakeLists.txt` | builds `minipsd::vips`, a static library |
| `example.c` | an image saved and read back through the plugin: its test |
| `minipsd-vips.spdx.json` | the SBOM fragment (SPDX 2.3): every other file here, with its checksums; `get_target_property(sbom minipsd::vips MINIPSD_SBOM_FILE)` |

## Build

In your CMake project:

```cmake
add_subdirectory(minipsd-vips-<version>)
target_link_libraries(my-app PRIVATE minipsd::vips)
```

Or, with FetchContent, straight from the release:

```cmake
include(FetchContent)
FetchContent_Declare(minipsd_vips
    URL      https://github.com/ShadobaDev/minipsd/releases/download/<version>/minipsd-vips-<version>.zip
    URL_HASH SHA256=<the hash in minipsd-vips-<version>.zip.sha256>)
FetchContent_MakeAvailable(minipsd_vips)
```

**One libvips.** Linux and macOS take the system's libvips and zlib through pkg-config. Windows
fetches libvips' prebuilt build, and zlib's headers, which that build lacks. A program that
fetches libvips' Windows build itself points the plugin at it first, so that one libvips is
fetched and the plugin is compiled against the headers of the DLLs the program ships:

```cmake
FetchContent_GetProperties(my_vips SOURCE_DIR my_vips_dir)       # vips-dev-x64-*.zip, unpacked
set(FETCHCONTENT_SOURCE_DIR_MINIPSD_LIBVIPS "${my_vips_dir}")   # the plugin takes this libvips
FetchContent_MakeAvailable(minipsd_vips)
```

zlib's headers are still fetched, so `zlib.h` is always there. A program whose libvips comes from
anywhere else defines two targets first, and nothing is fetched or found: `minipsd_libvips`
(libvips, glib, gobject) and `minipsd_libvips_zlib` (zlib's headers and the zlib libvips links),
each an `INTERFACE` library or `IMPORTED GLOBAL`.

On its own, with its example as the test:

```bash
cmake -S minipsd-vips-<version> -B build
cmake --build build
ctest --test-dir build        # Windows: add -C Debug (or the configuration built)
```

Without CMake, compile `minipsd_vips.c` and `foreign/*.c` with the zip's root and
`foreign/minipsd/` on the include path and libvips' and zlib's flags
(`pkg-config --cflags vips zlib`), and link the objects into your program.

## Use

Register the plugin once, after `VIPS_INIT` (libvips' prebuilt Windows builds load no modules):

```c
#include "minipsd_vips.h"

if (VIPS_INIT(argv[0])) vips_error_exit(NULL);
minipsd_vips_init();
VipsImage *page  = vips_image_new_from_file("page.psd", NULL);               /* the composite */
VipsImage *third = vips_image_new_from_file("page.psd", "layer", 2, NULL);   /* the third layer up */
vips_image_write_to_file(page, "copy.psd[compression=zip]", NULL);
```

- **Loading** (`psdload`, `psdload_buffer`, `psdload_source`): the composite, or with `layer` one
  layer, bottom first, placed by `xoffset` and `yoffset`, its look in `psd-layer-*` fields
  (name, opacity, hidden, blend mode, clipping), its mask an image in `psd-layer-mask`, a
  group's records marked by `psd-layer-section`, a record without pixels one transparent pixel
  with `psd-layer-empty`.
- **Saving** (`psdsave`, `psdsave_buffer`, `psdsave_target`): an image with alpha as one layer and
  the composite; without, flat (the composite only), or with `layered` one layer as well. Grey,
  RGB or CMYK at 8 or 16 bits, grey and RGB as floats. Options: `compression` (`rle`, the
  default; `zip`; `zip-predicted`; `none`), `psb`, `layer_name`, `layered`. The image is read
  once, in bands: memory about the file's size. A PSB when asked, for a `.psb` file, or when a
  PSD cannot hold the image.
- **Saving layers** (`psdsave_layers`, `_buffer`, `_target`; in C `minipsd_vips_psdsave_layers`,
  declared in `minipsd_vips.h`): an array of images, one a layer, bottom first, each read from
  the same fields psdload sets, so a file's layers loaded and saved come back. The composite is
  given (`composite`), or composed by `vips_composite`. The canvas: `width`, `height`, `left`,
  `top`. Text, effects, smart objects and adjustments' settings are not carried.

  ```c
  VipsImage *layers[2] = { page, blank };   /* blank: any image with psd-layer-empty set */
  vips_image_set_int(blank, "psd-layer-empty", 1);
  minipsd_vips_psdsave_layers(layers, 2, "template.psd", "composite", page, NULL);
  ```

minipsd is compiled into the plugin privately, so a program that also uses minipsd itself, at
any version, keeps its own.
