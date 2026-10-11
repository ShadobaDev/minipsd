# minipsd — build & test cheatsheet

One preset per toolchain; the same name works for `cmake --preset`, `cmake --build --preset`
and `ctest --preset`. Every preset is a Debug build with tests: the header is not compiled for
consumers, so there is nothing to ship from a build.

| Preset | Host | Toolchain |
|---|---|---|
| `msvc` | Windows | Visual Studio 2022 (Build Tools are enough) |
| `mingw` | Windows | MSYS2 GCC + Ninja on PATH: `C:\msys64\ucrt64\bin` (CI) or `C:\msys64\mingw64\bin` |
| `linux-gcc` | Linux / WSL | GCC + Ninja |
| `linux-clang` | Linux / WSL | Clang + Ninja |

```bash
cmake --preset mingw
cmake --build --preset mingw
ctest --preset mingw
```

Useful variants:

```bash
ctest --preset mingw -R Minipsd          # only the unit tests
ctest --preset mingw -R consumer -V      # the integration tests, with their output
cmake --install build/mingw              # install tree in install/mingw/
cmake --install build/msvc --config Debug   # MSVC is multi-config
```

## Packages and installing

Packages need no compiler and no tests; configure a separate tree with tests off. CPack writes
to `dist/` (ignored by git).

```bash
cmake -S . -B build/package -DMINIPSD_BUILD_TESTS=OFF

# Install the tree into a prefix (Linux: /usr/local with sudo)
cmake --install build/package --prefix <prefix>

# minipsd-X.Y.Z.tar.gz + minipsd-X.Y.Z.zip (any host)
cpack --config build/package/CPackConfig.cmake

# libminipsd-dev_X.Y.Z_all.deb (on Linux)
cpack --config build/package/CPackConfig.cmake -G DEB
```

Check and install the `.deb`:

```bash
dpkg-deb -I dist/libminipsd-dev_0.1.0_all.deb       # metadata
dpkg-deb -c dist/libminipsd-dev_0.1.0_all.deb       # files
sudo apt install ./dist/libminipsd-dev_0.1.0_all.deb
sudo apt remove libminipsd-dev
```

Install from the archive instead (the tree is relocatable):

```bash
sudo tar -xzf dist/minipsd-0.1.0.tar.gz --strip-components=1 -C /usr/local
```

## What the tests are

| Test | Checks |
|---|---|
| `MinipsdTest.*` | the writer, read back field by field (C++20, plus one C99 translation unit) |
| `minipsd-cxx17` (build target) | the header compiles warning-free as C++17 |
| `psd-tools-roundtrip` | documents written through minipsd, read back by `psd-tools` |
| `install` | `cmake --install` into the build tree (`tests/prefix/`) |
| `consumer-copy` | a project with `minipsd.h` copied in |
| `consumer-subdirectory` | `add_subdirectory` (git submodule) |
| `consumer-fetchcontent` | `FetchContent_MakeAvailable` |
| `consumer-package` | `find_package(minipsd 0.x.y EXACT CONFIG)` on the install |
| `consumer-pkgconfig` | `pkg-config minipsd` on the install; only if pkg-config is found |

Each consumer test builds and runs a small program and checks the header version; the CMake
paths also check the SBOM fragment's version.

## Options

| Option | Default | Effect |
|---|---|---|
| `MINIPSD_BUILD_TESTS` | ON only as the top-level project | the tests above |
| `MINIPSD_INSTALL` | ON only as the top-level project | install rules |
| `MINIPSD_REQUIRE_PSD_TOOLS` | OFF (CI: ON) | fail the configure instead of skipping the round trip |

GoogleTest comes from the system if installed (`libgtest-dev`), otherwise it is fetched.

The round trip needs Python with `psd-tools`; without it the test is skipped. A venv keeps it
out of the system Python:

```bash
python -m venv .venv
.venv/bin/pip install -r tests/requirements.txt          # Windows: .venv\Scripts\pip
cmake --preset mingw -DPython3_EXECUTABLE=<path to .venv's python>
```
