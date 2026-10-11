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
  `-Wall -Wextra -Wpedantic -Werror`, as C and (for headers) as C++.
- **Fixed-width integers** (`uint8_t`, `uint32_t`, ...) for anything with a defined size on disk
  or on the wire; `size_t` for in-memory sizes. Compute sizes that can overflow in `uint64_t` and
  check them before narrowing.
- **No mutable global or static state** in a library.
- **Every allocation is freed on every path**, error paths included. Libraries allocate through
  overridable macros (`<PREFIX>_MALLOC` / `<PREFIX>_FREE`), never `malloc` directly.

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

## Single-header libraries

stb-style layout, in this order:

1. File heading: what it is, usage, configuration macros, scope.
2. Declarations, guarded by `<PREFIX>_H`: includes, macros, enums, types, functions. Public
   functions are declared through one linkage macro (`<PREFIX>DEF`) that becomes `static` under
   `<PREFIX>_STATIC`.
3. Implementation, guarded by `<PREFIX>_IMPLEMENTATION` (and a `_DONE` guard against a second
   inclusion), grouped in sections, each opened by a one-line divider comment.
4. Licence text.

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

Implementation sections follow the order of the PSD file they write.
