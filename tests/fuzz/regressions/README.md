# Fuzzing regressions

Inputs the fuzzer once crashed on, kept after the fix. Every file here but this README is replayed
by the ordinary tests (`fuzz.replay.<file>`) through the same target, on every toolchain, so a
finding cannot come back unnoticed. Add the file libFuzzer wrote (`crash-<sha1>`, `oom-...`,
`timeout-...`) as it is, in the commit that fixes it. How to run the fuzzer: `docs/CHEATSHEET.md`.
