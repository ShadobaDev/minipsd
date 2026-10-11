# Security Policy

## Supported versions

minipsd is a small project maintained by one person. Only the **latest release** receives
fixes. The library is pre-1.0, so consumers should expect to track the current release.

| Version | Supported |
|---|---|
| Latest release | ✅ |
| Anything older | ❌ — please update first |

## Reporting a vulnerability

**Please report security issues privately, not as a public issue.**

Use GitHub's private reporting form:
**<https://github.com/ShadobaDev/minipsd/security/advisories/new>**

Please include, as far as you can: what the issue is, the minipsd version, the compiler and
platform, and a minimal document description (sizes, layer count, options) that triggers it.

This is a hobby project, so please allow a few days for a first response. You will get credit in
the advisory and release notes unless you prefer to stay anonymous.

## What is in scope

minipsd **writes** PSD files from a document description held in the caller's memory. It parses
no files and does no network communication. In scope:

- out-of-bounds reads or writes, or integer overflow in size calculations, for any document that
  passes minipsd's own validation (sides, layer count, dpi);
- invalid or truncated output, or memory leaks, on any error path;
- `minipsd_write_file` writing anywhere other than the path it was given.

Out of scope: a caller breaking the documented contract, e.g. a layer `rgba` buffer smaller than
`width * height * 4` bytes or a `name` that is not NUL-terminated. minipsd cannot detect these.

If reading PSD files is ever added, malformed input files become the primary attack surface and
this policy will be extended.
