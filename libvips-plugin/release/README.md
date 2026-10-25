# minipsd's libvips plugin

minipsd as libvips' loader and saver of PSD and PSB: `vips_image_new_from_file` and
`vips_image_write_to_file` take `.psd` and `.psb` in your program. MIT, as minipsd
(`LICENSE.MIT`). Project: <https://github.com/ShadobaDev/minipsd>; the full description is its
manual's "PSD in libvips: the plugin".

## What is here

| | |
|---|---|
| `src/` | the plugin; `minipsd_vips.h` is its API |
| `include/minipsd.h` | minipsd, one file, compiled into the plugin and private to it |
| `cmake/minipsd_libvips.cmake` | how libvips is found: Windows fetches libvips' official prebuilt build (pinned), elsewhere pkg-config |
| `CMakeLists.txt` | builds `minipsd::vips`, a static library |
| `example.c` | an image saved and read back through the plugin: its test |

## Build

In your CMake project:

```cmake
add_subdirectory(minipsd-vips-<version>)
target_link_libraries(my-app PRIVATE minipsd::vips)
```

A project with libvips of its own defines two targets before `add_subdirectory`, and nothing is
fetched or found: `minipsd_libvips` (libvips, glib, gobject) and `minipsd_libvips_zlib` (the zlib
libvips links), each an `INTERFACE` library or `IMPORTED GLOBAL`.

On its own, with its example as the test:

```bash
cmake -S minipsd-vips-<version> -B build
cmake --build build
ctest --test-dir build        # Windows: add -C Debug (or the configuration built)
```

Without CMake, compile `src/*.c` with `include/` on the include path and libvips' and zlib's
flags (`pkg-config --cflags vips zlib`), and link the objects into your program.

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
  layer, bottom first.
- **Saving** (`psdsave`, `psdsave_buffer`, `psdsave_target`): an image with alpha as one layer and
  the composite; without, flat (the composite only), or with `layered` one layer as well. Grey,
  RGB or CMYK at 8 or 16 bits, grey and RGB as floats. Options: `compression` (`rle`, the
  default; `zip`; `zip-predicted`; `none`), `psb`, `layer_name`, `layered`. The image is read
  once, in bands: memory about the file's size. A PSB when asked, for a `.psb` file, or when a
  PSD cannot hold the image.

minipsd is compiled into the plugin privately, so a program that also uses minipsd itself, at
any version, keeps its own.
