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
    GIT_TAG        0.2.0)
FetchContent_MakeAvailable(minipsd)
target_link_libraries(my-app PRIVATE minipsd::minipsd)
```

**[CPM.cmake](https://github.com/cpm-cmake/CPM.cmake)**

```cmake
CPMAddPackage("gh:ShadobaDev/minipsd#0.2.0")
target_link_libraries(my-app PRIVATE minipsd::minipsd)
```

Use `#0.2.0`, not `@0.2.0`: the `@` form looks for a tag `v0.2.0`, and minipsd tags have no `v`.

**git submodule**

```bash
git submodule add https://github.com/ShadobaDev/minipsd.git external/minipsd
```
```cmake
add_subdirectory(external/minipsd)
target_link_libraries(my-app PRIVATE minipsd::minipsd)
```

As a subproject, minipsd builds no tests, fetches nothing and enables no language.

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
  sudo tar -xzf minipsd-0.2.0.tar.gz --strip-components=1 -C /usr/local
  ```
- **As a Debian/Ubuntu package**, `libminipsd-dev_X.Y.Z_all.deb`:
  ```bash
  sudo apt install ./libminipsd-dev_0.2.0_all.deb
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
