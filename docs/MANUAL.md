# Manual

How to use minipsd. **Skeleton:** the chapters below are filled as the API settles, step by
step ([ROADMAP.md](ROADMAP.md)); until then the reference is [minipsd.h](../include/minipsd.h)
and the contract is [SPECIFICATION.md](SPECIFICATION.md).

This manual explains and shows; it does not repeat the reference. Every parameter and return
code is documented once, in `minipsd.h`. From 0.2.0 on, the code in this manual is taken from
`examples/`, which CI builds and runs.

| Chapter | Covers |
|---|---|
| [Getting started](#getting-started) | adding minipsd, the one implementation file, a first file |
| [Memory and ownership](#memory-and-ownership) | what to allocate, what may live on the stack, lifetimes |
| [Writing](#writing) | to memory, to a blocking stream, to a non-blocking stream |
| [Reading](#reading) | not available yet |
| [Return codes](#return-codes) | `rc < 0`, `MINIPSD_MORE`, error texts |
| [Configuration](#configuration) | `MINIPSD_STATIC`, allocator macros, optional parts |

---

## Getting started

*To be written.*

## Memory and ownership

*To be written.* The rules are in the spec's
[Memory and ownership](SPECIFICATION.md#memory-and-ownership); this chapter will show them in
practice: a stack document, a heap workspace, a fixed output chunk, a static workspace sized for
the largest document.

## Writing

*To be written.*

### To memory

### To a blocking stream

### To a non-blocking stream

## Reading

*Not available yet* ([ROADMAP.md](ROADMAP.md), stage 4).

## Return codes

*To be written.*

## Configuration

*To be written.*
