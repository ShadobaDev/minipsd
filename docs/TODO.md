# TODO

Roadmap grouped by **which semver position a change forces**, not by priority or by the order
work happens in.

**How to read this**
- The version numbers are provisional, non-binding hints — a *kind of bump*, not a schedule.
- A section is the **minimum bump the change forces**. This library is pre-1.0, so the shifted
  scale applies: a **breaking** change bumps the **MINOR**, additions and fixes bump the
  **PATCH**. "Breaking" means **removing or modifying** an existing public symbol — a changed
  signature, field, default, or semantics — not only deleting it, and it is judged over the
  *whole* change. A change to the bytes written for an unchanged document counts as breaking
  (consumers hash the output; see [SPECIFICATION.md](SPECIFICATION.md#consumer-requirements)).
- A change that only forces a patch may still be **bundled into a higher release** if it ships
  alongside bigger work. The header is the floor, not an assignment.
- **Cascade.** Whichever section releases first takes its slot; the rest re-derive from the new
  baseline.

---

## Before the first release: 0.1.0

The code and tests are in (`include/`, `tests/`) but nothing builds them yet: `CMakeLists.txt` and
`CMakePresets.json` are still Platemaker's and must be replaced, not adapted.

**Bring the code over**
- [x] Import `minipsd.h` from `PlateMaker/lib/src/infrastructure/psd_writer/` into `include/`.
- [x] Port the writer tests from `PlateMaker/tests/lib-unit-tests/test_minipsd.cpp` (17 writer
      tests; the 4 template tests stay in Platemaker) and `minipsd_c_smoke.c` (the same document
      compiled as C, compared byte for byte with the C++ build) into `tests/`.
- [x] Bring the header comments in line with [CODING_STYLE_C.md](CODING_STYLE_C.md).

**Make it self-describing** (see [Distribution and SBOM](SPECIFICATION.md#distribution-and-sbom))
- [x] `MINIPSD_VERSION_MAJOR/MINOR/PATCH` and `MINIPSD_VERSION_STRING` in the header; the single
      version source (CMake reads it, see the next section).
- [x] `SPDX-License-Identifier` and `SPDX-FileCopyrightText` in the header.
- [x] The project URL and the full MIT text inside the header.

**Build and packaging**
- [x] `CMakeLists.txt`: version parsed from `minipsd.h` (`file(STRINGS)`) and passed to
      `project(VERSION)`; `minipsd::minipsd` INTERFACE target; tests only when
      `PROJECT_IS_TOP_LEVEL`; warnings as errors for C and C++ builds.
- [x] Install rules: header, `minipsd-config.cmake` + `ARCH_INDEPENDENT` version file,
      `minipsd.pc`, licence, SBOM fragment.
- [x] SPDX 2.3 SBOM fragment generated from a template at configure time (model:
      `PlateMaker/lib/cmake/sbom.spdx.json.in`), exposed to consumers as the target property
      `MINIPSD_SBOM_FILE`.
- [x] `CMakePresets.json` for MSVC, MinGW and Linux (gcc, clang). `linux-clang` is not yet run
      anywhere (no clang in the local WSL); CI covers it.
- [x] Integration smoke tests: a tiny consumer project per path (raw copy, `add_subdirectory`,
      FetchContent, `find_package` and pkg-config after install).

**CI and release**
- [x] CI: Windows (MSVC, MinGW), Linux (gcc, clang); each builds the tests as C and as C++.
      Not yet run on GitHub: the first push of `.github/` is its first run.
- [x] `psd-tools` round trip in CI: write documents, read them back, compare pixels and fields.
- [x] Release workflow per [GITHUB-RELEASE.md](GITHUB-RELEASE.md).
- [x] `git init`, push to `ShadobaDev/minipsd`.
- [ ] Tag `0.1.0`.
- [x] After the first publish: check that GitHub detects the licence from `LICENSE.MIT` (it does:
      MIT; no rename needed).

**Last step of the release: switch Platemaker to it**
- [ ] FetchContent with a guard (no download when the header is already available, e.g. via
      `MINIPSD_INCLUDE_DIR`), replacing its private copy of `minipsd.h`.
- [ ] An entry in `lib/cmake/third_party.json` and the minipsd package in its SBOM.

**Open decisions** (each one is a contract change, so only before `0.1.0`)
- [ ] **Opacity default.** Today `opacity = 0` is fully transparent, so a zero-initialised layer
      is invisible. Alternative: treat 0 as 255. Cheaper now than ever again.
- [ ] **Struct versioning.** A `size`/`version` field for ABI growth, or rely on tags? For a
      header compiled into its consumer, tags are usually enough.
- [x] **Test framework.** GoogleTest kept: the existing tests ported as-is; a system install is
      used if present, otherwise it is fetched, and only for top-level builds.
- [x] **Own test reader *and* `psd-tools`.** Both: the own reader checks every field and runs
      everywhere; `psd-tools` checks compatibility and is required in CI.
- [x] **Contributor terms.** Inbound = outbound: contributions are licensed under MIT, stated in
      `CONTRIBUTING.md`; no CLA.
- [x] **Copyright holder.** `Bartlomiej Mucha <shadobadev@gmail.com>`, ASCII everywhere
      (licence, header, sources, SBOM); `ShadobaDev` stays the GitHub account in URLs and the PURL.

---

## PATCH — additions

Candidates, none started. Each must follow the
[extensibility rules](SPECIFICATION.md#extensibility-rules).

| Feature | Notes |
|---|---|
| C++ wrapper | ~40 lines under `#ifdef __cplusplus`: RAII, `std::vector` as output buffer |
| Layers smaller than the canvas | a rectangle per layer; today a layer is full canvas or empty |
| Layer groups (`lsct`) | |
| Layer masks | |
| Blend modes other than `norm` | the composite must compute them too |
| 16-bit channels | not needed for comic strips; for other users |
| PSB (version 2) | sides above 30 000 px; RLE row counts become 32-bit; opt-in, zero keeps PSD |
| Reading | out of scope today; would be a separate opt-in implementation block |

---

## MINOR — breaking

Nothing planned.

---

## Release history & coordination

**planned:** `0.1.0`, released together with Platemaker's switch to it.
