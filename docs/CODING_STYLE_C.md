# Coding style — C

Rules for C code. The general sections apply to any C project; everything specific to this
repository is in [This project](#this-project) at the end. C++ code follows
[CODING_STYLE_CPP.md](CODING_STYLE_CPP.md).

## Language

- **C99.** A library header must also compile as C++17/20: cast `void *` explicitly; no
  designated initialisers, no compound literals, no VLAs, no `restrict` in public declarations;
  wrap public declarations in `extern "C"` under C++.
- **Standard library only.** No compiler extensions without a portable fallback.
- **Pure ASCII source.** Non-ASCII text in strings or comments is escaped or spelled in ASCII.
- **Warning-free** under MSVC `/W4 /WX /permissive-` and gcc/clang
  `-Wall -Wextra -Wpedantic -Werror`, as C and (for headers) as C++. C is also held to the
  conversion, shadowing, cast and prototype warnings (`-Wconversion -Wsign-conversion -Wshadow
  -Wcast-qual ...`; each project lists its set). Not C++: a header that is also valid C needs
  C-style casts, which `-Wold-style-cast` and `-Wuseless-cast` reject.
- **Fixed-width integers** (`uint8_t`, `uint32_t`, ...) for anything with a defined size on disk
  or on the wire; `size_t` for in-memory sizes. Compute sizes that can overflow in `uint64_t` and
  check them before narrowing.
- **No mutable global or static state** in a library.
- **Every allocation is freed on every path**, error paths included. Libraries allocate through
  overridable macros (`<PREFIX>_MALLOC` / `<PREFIX>_FREE`), never `malloc` directly.

## Numbers

A number that states a fact of a file format, or of the library's own layout, gets a name when it
appears in more than one place or differs between variants of the format: a named constant
(`<SHORT>__<NAME>`) for a fixed size, a `static` function for a size that depends on something
(a width by format version, a structure's size by its count of entries). A function rather than
a macro: its arguments are typed and evaluated once. Names and the layout they come from live
together, under a table of the structure's fields and sizes, in the module the reader and writer
share. An offset read once, right where the table shows it, may stay a number. Plain arithmetic
and alignment (`pad(n, 4)`, `* 2` for UTF-16 units) are not facts to name.

Floating-point values are never compared with `==` or `!=`: within a tolerance, a named epsilon,
through `detail/floats.h` (`mpsd__floats_equal`), which also says whether a value is finite
(`mpsd__floats_positive`, `mpsd__floats_not_negative`), all without libm. The tests do the same
(`EXPECT_FLOAT_EQ`, `EXPECT_DOUBLE_EQ`). Byte identity of copied data is not a float comparison.

## Naming

| Kind | Form |
|---|---|
| Public function | `<prefix>_<verb>[_<object>]` |
| Public type (struct tag the same) | `<prefix>_<noun>` |
| Public macro, enum constant | `<PREFIX>_<NAME>` |
| Configuration macro | `<PREFIX>_<NAME>`, documented in the file heading |
| Internal function, type | `<short>__<name>` (double underscore), always `static` |
| Internal macro | `<SHORT>__<NAME>` |
| Local variable, parameter | `snake_case` |

## Header-only libraries

stb style: the user includes one public header, and defines `<PREFIX>_IMPLEMENTATION` in exactly
one source file to compile the implementation there.

**The public header** (`<prefix>.h`) is the whole API, in this order:

1. File heading: what it is, its sections, usage, configuration macros, scope.
2. Declarations, guarded by `<PREFIX>_H`: includes, macros, enums, types, functions, grouped in
   sections, each opened by a one-line divider comment. Public functions are declared through
   one linkage macro (`<PREFIX>DEF`) that becomes `static` under `<PREFIX>_STATIC`.
3. Under `<PREFIX>_IMPLEMENTATION`, the `#include` of each implementation file.
4. Licence text.

**Implementation files** (`<prefix>/<module>.h`) hold no declaration of their own: they define
what the public header declares, plus `static` internals. Each is guarded by its own
`<SHORT>__<MODULE>_DONE`, so it is compiled once however often the public header is included.
Internals shared by several modules go to `<prefix>/detail/`. Sections within a file are opened
by one-line divider comments.

A public header that needs nothing before it, and an implementation that leaves no internal
function unused, are both checked by a build target.

## Comments

Doxygen comments with the backslash `\` notation for everything public. Internal code gets a
plain `/* */` comment only when *why* is not obvious from the code. One comment style
throughout: `//` is valid C99 but is not used.

### File heading

```c
/**
 * \file
 * \brief Short description of the file.
 *
 * Detailed description: usage, configuration macros, what is and is not covered.
 *
 * SPDX-License-Identifier: [licence]
 * SPDX-FileCopyrightText: Copyright (c) [year] [owner]
 */
```

### Function

```c
/**
 * \brief Short description of the function.
 *
 * Detailed description of the behaviour.
 *
 * \param first  Description of the first argument.
 * \param second Description of the second argument.
 * \return Description of the return value.
 *
 * \retval VALUE_A When it is returned.
 * \retval VALUE_B When it is returned.
 */
```

### Struct and fields

```c
/**
 * \brief Short description of the struct.
 *
 * Detailed description: ownership, lifetime, what zero-initialisation means.
 */
typedef struct prefix_thing {
    const char *name;  /*!< Brief description of the field. */
    uint8_t     value; /*!< Brief description of the field. */
} prefix_thing;
```

### Enum

```c
/** \brief Short description of the group of constants. */
enum {
    PREFIX_FIRST  = 0, /*!< Brief description of the value. */
    PREFIX_SECOND = 1  /*!< Brief description of the value. */
};
```

### Macro, typedef

```c
/** \brief Short description, including the unit. */
#define PREFIX_LIMIT 30000
```

---

## This project

| Placeholder | minipsd |
|---|---|
| `<prefix>` / `<PREFIX>` | `minipsd` / `MINIPSD` |
| `<short>` / `<SHORT>` (internal) | `mpsd` / `MPSD` — e.g. `mpsd__packbits`, `MPSD__STR` |
| linkage macro | `MINIPSDDEF` |
| allocation macros | `MINIPSD_MALLOC` / `MINIPSD_FREE` |
| public header / implementation | `include/minipsd.h` / `include/minipsd/<module>.h`, internals in `include/minipsd/detail/` |
| implementation guards | `MPSD__<MODULE>_DONE` |

Implementation sections follow the order of the PSD file they write or read. The file's layout,
with the names of its sizes ([Numbers](#numbers)), is in `detail/format.h`.
