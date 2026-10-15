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
- [x] CPack packages of the install tree: `minipsd-X.Y.Z.tar.gz` / `.zip` and
      `libminipsd-dev_X.Y.Z_all.deb` (commands in [CHEATSHEET.md](CHEATSHEET.md)).
- [ ] **Decide:** attach those packages to the GitHub Release too, or only `minipsd.h`?
- [x] `git init`, push to `ShadobaDev/minipsd`.
- [x] Tag `0.1.0` (released 2026-10-12).
- [x] After the first publish: check that GitHub detects the licence from `LICENSE.MIT` (it does:
      MIT; no rename needed).

**Open decisions** (each one is a contract change, so only before `0.1.0`)
- [x] **Opacity default.** Kept: `opacity` is the PSD byte, 0 fully transparent, 255 opaque. A
      zero-initialised layer is therefore invisible; that is documented, not special-cased.
- [x] **Struct versioning.** No `size`/`version` field: a break shows in the library version,
      not per struct (see [Extensibility rules](SPECIFICATION.md#extensibility-rules)).
- [x] **Test framework.** GoogleTest kept: the existing tests ported as-is; a system install is
      used if present, otherwise it is fetched, and only for top-level builds.
- [x] **Own test reader *and* `psd-tools`.** Both: the own reader checks every field and runs
      everywhere; `psd-tools` checks compatibility and is required in CI.
- [x] **Contributor terms.** Inbound = outbound: contributions are licensed under MIT, stated in
      `CONTRIBUTING.md`; no CLA.
- [x] **Copyright holder.** `Bartlomiej Mucha <shadobadev@gmail.com>`, ASCII everywhere
      (licence, header, sources, SBOM); `ShadobaDev` stays the GitHub account in URLs and the PURL.

**Quality** (after `0.1.0`)
- [x] CI job `analysis`: AddressSanitizer + UBSan, clang-tidy, coverage of `minipsd.h` (gcovr;
      lines 100 %, branches >= 95 %, allocation failures tested); presets `linux-asan`,
      `linux-tidy`, `linux-coverage`.
- [x] README badges: CI, coverage, release, licence.
- [x] Repository enabled on coveralls.io.

**Distribution** (after `0.1.0`; none of these changes the library)
- [x] GitHub repository topics and description.
- [x] CPM.cmake line in [INSTALL.md](INSTALL.md).
- [ ] vcpkg port (PR to `microsoft/vcpkg`).
- [ ] Conan Center recipe (PR to `conan-io/conan-center-index`).
- [ ] Listing in `nothings/single_file_libs`.
- [ ] Listing in `awesome-c`.
- [ ] Announcement (r/C_Programming, r/cpp "Show and tell").

---

## PATCH — additions

The planned features and their order are in [ROADMAP.md](ROADMAP.md); the design behind them
is in [CODE-ARCH.md](CODE-ARCH.md). Each must follow the
[extensibility rules](SPECIFICATION.md#extensibility-rules).

---

## MINOR — breaking

- [x] `0.2.0`: `minipsd_write_file` moves to `examples/` ([ROADMAP.md](ROADMAP.md), stage 3).

---

## Release history & coordination

**released:** `0.1.0` — 2026-10-12, together with Platemaker's switch to it.
