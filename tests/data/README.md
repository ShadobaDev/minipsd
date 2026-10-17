# Test files from other programs

PSD files saved by other programs, read by `MinipsdReaderTest.FilesFromOtherProgramsAreReadWholeAndInParts`
and by `example.read_file.<name>`: each must be read whole and in parts, and every layer and the
composite of an 8-bit RGB file must decode. They also seed the fuzzer (ROADMAP S4.8).

Only files that may be redistributed belong here: made for this purpose, or under a licence that
allows it. Keep them small (a few hundred KiB); every file is listed below.

| File | Saved by (program, version) | What it covers | Origin | Licence |
|---|---|---|---|---|

Private files (your own projects) go to `tests/fixtures/` instead: the same tests read them, and
git ignores that directory.

A larger set is fetched, not committed: configure with `-DMINIPSD_PSD_TOOLS_FILES=ON` and the same
test also reads psd-tools' own test files (MIT; 279 PSDs from Photoshop and others: every colour
mode and depth, masks, groups, effects, blend modes), pinned to a release by tag and hash. Files
known to be malformed are listed, with the reason, in `knownBad` in `tests/test_reader.cpp`. The
results, file by file, are in [docs/COMPATIBILITY.md](../../docs/COMPATIBILITY.md).
