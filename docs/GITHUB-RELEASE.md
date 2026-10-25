# Cutting a release

**The model:** a release is a pushed git tag `X.Y.Z` (bare semver, no `v` prefix). Consumers pin
that tag (FetchContent, submodule), so a tag is never moved once pushed.

Pushing the tag starts the [`Release`](../.github/workflows/release.yml) workflow:

1. a check that the tagged commit is on `master` and that [`CI`](../.github/workflows/ci.yml)
   (every toolchain, the `psd-tools` round trip) passed on it: CI runs on every push to
   `master`, so the release does not test it again. A tag pushed before that run ends fails
   here; re-run the job once CI is green;
2. a check that the tag equals the `MINIPSD_VERSION_*` macros in `include/minipsd.h` and that
   `docs/CHANGELOG.md` has a section for that version;
3. `minipsd.h` as a single file: `include/` joined by `tools/amalgamate.cmake`;
4. the libvips plugin as `minipsd-vips-X.Y.Z.zip`, a CMake project that builds as it is
   (`tools/package_vips.cmake`), then built from the zip alone against Ubuntu's libvips and its
   example run: a zip that does not build is not published;
5. a build-provenance attestation for `minipsd.h` and the zip;
6. a GitHub Release with `minipsd.h`, the zip and a `.sha256` for each, its notes taken from the
   changelog section.

There is nothing to build for consumers: the source tree at the tag *is* the release. `minipsd.h`
is for users who prefer one file to copying `include/`; the zip, for those who want the plugin
without the repository. CI packs and builds the zip on every push too, so it fails there first.

## 0. Pre-flight

- `MINIPSD_VERSION_MAJOR/MINOR/PATCH` in `include/minipsd.h` are the version you are releasing.
  (CMake reads its version from the header, so there is no second place to update.)
- `docs/CHANGELOG.md` has `## [X.Y.Z] — YYYY-MM-DD` with the user-facing changes.
- Everything is committed and pushed to `master`, and CI is green there.

## 1. Dry run (recommended)

GitHub → **Actions** → **Release** → **Run workflow** on `master`. It runs everything except the
publishing step, including the version and changelog checks. A red run costs nothing; a red tag
leaves a tag pointing at a broken commit.

## 2. Publish

```bash
git checkout master
git pull
git tag -a 0.1.0 -m "minipsd 0.1.0" -m "<one-line summary of the release>"
git push origin 0.1.0
```

An annotated tag (`-a`) records who tagged, when and why. Its message stays short: the release
notes come from `docs/CHANGELOG.md`, not from the tag.

## 3. Verify

- **Actions** → the Release run is green.
- **Releases** → `0.1.0` has `minipsd.h`, `minipsd-vips-0.1.0.zip`, a `.sha256` for each, and
  the changelog notes.
- Provenance: `gh attestation verify minipsd.h --repo ShadobaDev/minipsd`, and the same for the
  zip.
- A FetchContent consumer pinned to `GIT_TAG 0.1.0` configures and builds.

## 4. Then the consumers

Bump the pinned tag in Platemaker's own repository: its minipsd FetchContent declaration and
`lib/cmake/third_party.json`.

## Rollback

A tag already fetched by someone cannot be taken back safely. If a release is broken, fix it and
publish the next patch version instead of re-tagging. Delete a tag only if it was pushed by
mistake minutes ago (and delete its Release on GitHub too):

```bash
git tag -d 0.1.0
git push origin :refs/tags/0.1.0
```
