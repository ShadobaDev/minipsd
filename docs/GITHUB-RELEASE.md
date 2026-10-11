# Cutting a release

> The release workflow does not exist yet (see [TODO.md](TODO.md)). This is the procedure it
> must support.

**The model:** a release is a pushed git tag `X.Y.Z` (bare semver, no `v` prefix). Consumers pin
that tag (FetchContent, submodule), so a tag is never moved once pushed. CI then creates the GitHub Release and attaches
`minipsd.h` with its `minipsd.h.sha256` for raw-copy users.

There is nothing to build for consumers: the source tree at the tag *is* the release.

## 0. Pre-flight

- The `MINIPSD_VERSION_*` macros in `minipsd.h` match the tag you are about to push. (CMake reads
  its version from the header, so there is no second place to update.)
- `docs/CHANGELOG.md` has the entry for this version.
- CI is green on `master` for every toolchain, including the `psd-tools` round trip.

## 1. Publish

```bash
git checkout master
git pull
git tag 0.1.0
git push origin 0.1.0
```

## 2. Verify

- The Release `0.1.0` exists with `minipsd.h` and `minipsd.h.sha256`.
- A FetchContent consumer pinned to `GIT_TAG 0.1.0` configures and builds.
- Paste the highlights from `docs/CHANGELOG.md` into the release notes.

## 3. Then the consumers

Bump the pinned tag in Platemaker's own repository: its minipsd FetchContent declaration and
`lib/cmake/third_party.json`.

## Rollback

A tag already fetched by someone cannot be taken back safely. If a release is broken, fix it and
publish the next patch version instead of re-tagging. Delete a tag only if it was pushed by
mistake minutes ago:

```bash
git tag -d 0.1.0
git push origin :refs/tags/0.1.0
```
