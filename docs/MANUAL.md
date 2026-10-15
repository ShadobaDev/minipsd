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

A document is a `minipsd_doc` and its layers; the pixels stay where the application keeps them
and are read in place. Two ways produce the file, and both give the same bytes:

| You have | Use |
|---|---|
| a heap and a target that takes bytes as they come (a file, a blocking socket, a hash) | `minipsd_write` with a callback |
| no heap, a non-blocking target, or a buffer for the whole file | the pull writer, `minipsd_writer_*` |

Every example below starts from the same document:

```c
minipsd_layer layers[2] = {
    { "Template", rgba, 255, 0 },   /* name, pixels (width * height * 4, straight alpha), opacity, hidden */
    { "Layer",    NULL, 255, 0 },   /* an empty layer above it */
};
minipsd_doc doc = { width, height, 300.0, layers, 2, NULL, 0, MINIPSD_RLE };
```

The document, its pixels and the workspace must stay valid, and unchanged, until the last byte
is out: the writer sized everything from them at the start.

### To memory

The pull writer knows the file's size before writing it, so one buffer of exactly that size takes
the whole file in one call:

```c
size_t          need, n;
minipsd_writer *w;
void           *workspace;
uint8_t        *file;
uint64_t        total;

if (minipsd_writer_workspace_size(&doc, &need) < 0) return;          /* the document is refused */
workspace = malloc(need);                                            /* or the stack, a pool, ... */
if (!workspace || minipsd_writer_init(&w, &doc, workspace, need) < 0) return;
total = minipsd_writer_total(w);                                     /* check it fits a size_t on 32-bit */
file  = malloc((size_t)total);
minipsd_writer_fill(w, file, (size_t)total, &n);                     /* MINIPSD_OK, n == total */
free(workspace);                                                     /* the writer ends with it */
```

There is nothing to release in minipsd: the writer lives in the workspace.

### To a blocking stream

`minipsd_write` drives the pull writer to the end and hands each piece, up to 4 KiB, to a
callback; returning 0 stops it with `MINIPSD_ERR_WRITE`:

```c
static int to_file(void *user, const void *data, size_t size)
{
    return fwrite(data, 1, size, (FILE *)user) == size;
}

int rc = minipsd_write(&doc, to_file, f);
```

It allocates the workspace once, through `MINIPSD_MALLOC`. With `MINIPSD_NO_MALLOC`, write the
same loop yourself over `minipsd_writer_fill`, on a workspace of your own.

### To a non-blocking stream

The writer never waits. Whenever the target can take bytes, send what is pending; when nothing
is pending, pull the next chunk. A target that takes only part of the chunk is fine: the rest
stays pending in your buffer.

```c
uint8_t chunk[64 * 1024];   /* a few rows: a call re-encodes the row it stops in */
size_t  have = 0, sent = 0;
int     rc   = MINIPSD_MORE;

/* each time the socket is writable: */
if (sent == have && rc == MINIPSD_MORE) {
    rc   = minipsd_writer_fill(w, chunk, sizeof chunk, &have);       /* rc < 0: an error */
    sent = 0;
}
sent += send_some(sock, chunk + sent, have - sent);                  /* may take less, or nothing */

/* finished when rc == MINIPSD_OK and sent == have */
```

`minipsd_writer_total` gives the size up front, for a `Content-Length` header or a progress bar.

## Reading

*Not available yet* ([ROADMAP.md](ROADMAP.md), stage 4).

## Return codes

*To be written.*

## Configuration

*To be written.*
