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
| `linux-asan` | Linux / WSL | GCC + AddressSanitizer and UBSan (no round trip) |
| `linux-tsan` | Linux / WSL | Clang + ThreadSanitizer; on new kernels it needs `sudo sysctl vm.mmap_rnd_bits=28` |
| `linux-tidy` | Linux / WSL | Clang + clang-tidy on the tests and the header |
| `linux-coverage` | Linux / WSL | GCC + `--coverage`; report with gcovr below |
| `linux-fuzz` | Linux / WSL | Clang + libFuzzer, ASan and UBSan; local only, see [Fuzzing](#fuzzing) |

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

## Quality checks (as in CI)

The tools are pinned in `tests/requirements-qa.txt`, so a local run reports what CI reports.

```bash
pip install -r tests/requirements-qa.txt -r tests/requirements.txt

cmake --preset linux-asan && cmake --build --preset linux-asan && ctest --preset linux-asan
cmake --preset linux-tidy && cmake --build --preset linux-tidy   # findings fail the build

cmake --preset linux-coverage && cmake --build --preset linux-coverage && ctest --preset linux-coverage
find build/linux-coverage \( -name '*.c.gcda' -o -name '*.c.gcno' \) -delete   # the C units: see below
gcovr --root . --filter include/ --merge-lines --exclude-throw-branches \
      --txt-metric branch --txt build/linux-coverage          # lines and branches missing a test
```

`--merge-lines` counts each line of a header once, although several test files compile it;
`--exclude-throw-branches` drops the exception edges of the C++ builds, which C code never takes.
The C units (the C smoke test, the examples, the fuzz replay) are left out, as CI leaves them
out: they check the header builds and behaves as C, and gcc numbers a line's branches
differently in C, so gcovr would list those lines twice and the duplicates would hide which test
is missing. Coverage data adds up across runs: delete `build/linux-coverage` for a clean report.
CI fails below 100 % of lines or 95 % of branches and publishes the report to Coveralls.

## Fuzzing

A tool for finding bugs, not a test: it runs locally for as long as you like and never in CI.
`tests/fuzz/fuzz_reader.c` takes any bytes through the whole reader (init, resources, layers,
names, blocks, every decode, whole and a band) and also aborts when the API breaks a promise: a
`MINIPSD_ERR_SHORT` that does not ask for more than the buffer holds, a decode over the limit that
succeeds, an inverted layer rectangle.

```bash
cmake --preset linux-fuzz -DMINIPSD_PSD_TOOLS_FILES=ON && cmake --build --preset linux-fuzz
ctest --preset linux-fuzz      # writes the example file, the first seed
cd build/linux-fuzz && mkdir -p fuzz-corpus findings
tests/minipsd-fuzz-reader -max_total_time=3600 -artifact_prefix=findings/ fuzz-corpus \
    examples/out-write_memory ../../tests/data ../../tests/fixtures \
    _deps/minipsd_psd_tools_files-src/tests/psd_files
```

The first directory is the corpus libFuzzer grows; the others are seeds, read only: the writer's
own file, the committed and local test files, and psd-tools' files. A finding lands in `findings/`
with the input that causes it; replay it with `tests/minipsd-fuzz-reader findings/<file>`. Once
fixed, the input goes to `tests/fuzz/regressions/`, where the ordinary tests replay it on every
toolchain (`fuzz.replay.<file>`).

clang-15's sanitizers crash at random on recent kernels (6.x, WSL2 included) whose address-space
randomisation they do not expect: a `SEGFAULT` in an unrelated test or at startup. Run under
`setarch -R` (no randomisation): `setarch -R ctest --preset linux-fuzz`, and the same before the
fuzzer.

## The single-file header

The release attaches `include/` joined into one `minipsd.h`; build the same file locally with

```bash
cmake -DOUTPUT=build/minipsd.h -P tools/amalgamate.cmake
```

The tests build it at configure time and run against it as `amalgamated.*`.

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
dpkg-deb -I dist/libminipsd-dev_0.2.0_all.deb       # metadata
dpkg-deb -c dist/libminipsd-dev_0.2.0_all.deb       # files
sudo apt install ./dist/libminipsd-dev_0.2.0_all.deb
sudo apt remove libminipsd-dev
```

Install from the archive instead (the tree is relocatable):

```bash
sudo tar -xzf dist/minipsd-0.1.0.tar.gz --strip-components=1 -C /usr/local
```

## What the tests are

| Test | Checks |
|---|---|
| `Minipsd<Module>Test.*` | one suite per module (`test_<module>.cpp`): the writer read back field by field, the internal modules tested directly (C++20, plus one C99 translation unit) |
| `MinipsdGoldenTest.*` | a fixed set of documents still gives the bytes of 0.1.0 (SHA-256) |
| `amalgamated.*` | the same tests against the single-file `minipsd.h` |
| `minipsd-cxx17` (build target) | the header compiles warning-free as C++17 |
| `minipsd-include-check` (build target) | `minipsd.h` compiles alone, then with its implementation, leaving no internal function unused |
| `psd-tools-roundtrip` | documents written through minipsd, read back by `psd-tools` |
| `install` | `cmake --install` into the build tree (`tests/prefix/`) |
| `consumer-copy` | a project with `include/` copied in |
| `consumer-subdirectory` | `add_subdirectory` (git submodule) |
| `consumer-fetchcontent` | `FetchContent_MakeAvailable` |
| `consumer-package` | `find_package(minipsd 0.x.y EXACT CONFIG)` on the install |
| `consumer-pkgconfig` | `pkg-config minipsd` on the install; only if pkg-config is found |

Each consumer test builds and runs a small program and checks the header version; the CMake
paths also check the SBOM fragment's version and that it lists the headers.

## Options

| Option | Default | Effect |
|---|---|---|
| `MINIPSD_BUILD_TESTS` | ON only as the top-level project | the tests above |
| `MINIPSD_INSTALL` | ON only as the top-level project | install rules |
| `MINIPSD_REQUIRE_PSD_TOOLS` | OFF (CI: ON) | fail the configure instead of skipping the round trip |
| `MINIPSD_VIPS_EXAMPLE` | OFF (CI: ON in `msvc`) | build and run `examples/vips_composite.c`: on Windows fetch libvips' prebuilt "web" build (8.18.7, 11 MB, pinned by hash), elsewhere find libvips with pkg-config; with `tests/fixtures/Test-g-example.psd` present, also on that file |
| `MINIPSD_PSD_TOOLS_FILES` | OFF (CI: ON in `linux-gcc`) | fetch psd-tools' 315 test files, 278 PSD and 37 PSB (v1.25.0, about 22 MB, pinned by hash) and read every one |
| `MINIPSD_TEST_FIXTURES` | ON (`linux-coverage`: OFF) | read the local PSDs in `tests/fixtures/` too; off, a local coverage run measures what CI's does |
| `MINIPSD_CLANG_TIDY` | empty (`linux-tidy`: `clang-tidy`) | run clang-tidy on the test targets |

GoogleTest comes from the system if installed (`libgtest-dev`), otherwise it is fetched.

The round trip needs Python with `psd-tools`; without it the test is skipped. A venv keeps it
out of the system Python:

```bash
python -m venv .venv
.venv/bin/pip install -r tests/requirements.txt          # Windows: .venv\Scripts\pip
cmake --preset mingw -DPython3_EXECUTABLE=<path to .venv's python>
```
