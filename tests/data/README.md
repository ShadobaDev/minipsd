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
