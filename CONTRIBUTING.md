# Contributing

Thank you for your interest in contributing to minipsd!

## Before you start

Read [docs/SPECIFICATION.md](docs/SPECIFICATION.md) (the contract, including the
[extensibility rules](docs/SPECIFICATION.md#extensibility-rules)) and
[docs/CODING_STYLE_C.md](docs/CODING_STYLE_C.md) (for C++ code:
[docs/CODING_STYLE_CPP.md](docs/CODING_STYLE_CPP.md)). minipsd stays small on purpose: open an
issue to discuss a new feature before writing it.

## Licensing of contributions

minipsd is MIT licensed, and so are contributions to it: by submitting a pull request you agree
that your contribution is licensed under the [MIT License](LICENSE.MIT), and that you have the
right to submit it. There is no separate contributor agreement to sign.

## Submitting a pull request

1. Fork the repository and create a branch from `master`.
2. Make your changes; the tests must pass as C and as C++, without warnings.
3. A change to the bytes written must come with a test, and must be checked with an independent
   reader (`psd-tools`), not only with the project's own test reader.
4. Update `docs/SPECIFICATION.md` and `docs/MANUAL.md` with the behaviour, and add an entry to
   `docs/CHANGELOG.md` if users will notice the change.
5. Open a pull request with a clear description of what and why.

Everything in the repository — code, comments, documentation, commit messages — is in English.

## Commit messages

- Subject: `type(scope): summary`, the scope always given — `feat(reader): …`, `fix(plugin): …`.
- The body says what changed in behaviour or architecture, and why. A comment brought in line with the
  code is not a change, and tests are part of the change they cover: mention them only when the
  commit is about tests.

## The CHANGELOG

It lists what changed for a user between two **published** releases.

- One bullet per change. The title is often enough; add one or two sentences only when it is not.
- A fix may take up to three sentences — how it was, what changed, how it is now — when it needs them.
- Never list a fix for a bug that no published release had.
- One feature serving several scenarios is one bullet.
- No rationale and no options considered: those belong in `docs/`.
- No file names, internal functions, tests or CI unless a user sees or must act on them.
- Breaking changes come first, each with what the user must change.

## AI-assisted contributions

Contributions written with an AI assistant are welcome; the author answers for every line. Commits
made with one say so in a `Co-Authored-By` trailer. [CLAUDE.md](CLAUDE.md) gives an agent the map
of this repository.
