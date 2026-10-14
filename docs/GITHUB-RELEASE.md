# Cutting a release

**The model:** a release is a pushed git tag `X.Y.Z` (bare semver, no `v` prefix). Consumers pin
that tag (FetchContent, submodule), so a tag is never moved once pushed.

Pushing the tag starts the [`Release`](../.github/workflows/release.yml) workflow:

1. the full [`CI`](../.github/workflows/ci.yml) on every toolchain, including the `psd-tools`
   round trip;
2. a check that the tag equals the `MINIPSD_VERSION_*` macros in `include/minipsd/base.h` and
   that `docs/CHANGELOG.md` has a section for that version;
3. `minipsd.h` as a single file: `include/` joined by `tools/amalgamate.cmake`;
4. a build-provenance attestation for that file;
5. a GitHub Release with `minipsd.h` and `minipsd.h.sha256`, its notes taken from the changelog
   section.

There is nothing to build for consumers: the source tree at the tag *is* the release. The two
assets are for users who prefer one file to copying `include/`.

## 0. Pre-flight

- `MINIPSD_VERSION_MAJOR/MINOR/PATCH` in `include/minipsd/base.h` are the version you are releasing.
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
- **Releases** → `0.1.0` has `minipsd.h`, `minipsd.h.sha256` and the changelog notes.
- Provenance: `gh attestation verify minipsd.h --repo ShadobaDev/minipsd`.
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
