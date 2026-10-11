# Contributing

Thank you for your interest in contributing to minipsd!

## Before you start

Read [docs/SPECIFICATION.md](docs/SPECIFICATION.md) (the contract, including the
[extensibility rules](docs/SPECIFICATION.md#extensibility-rules)) and
[docs/CODING_STYLE_C.md](docs/CODING_STYLE_C.md) (for C++ code:
[docs/CODING_STYLE_CPP.md](docs/CODING_STYLE_CPP.md)). minipsd stays small on purpose: open an
issue to discuss a new feature before writing it.

## Submitting a pull request

1. Fork the repository and create a branch from `main`.
2. Make your changes; the tests must pass as C and as C++, without warnings.
3. A change to the bytes written must come with a test, and must be checked with an independent
   reader (`psd-tools`), not only with the project's own test reader.
4. Add a line to `docs/CHANGELOG.md` if users will notice the change.
5. Open a pull request with a clear description of what and why.
