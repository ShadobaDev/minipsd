# Manual

How to use minipsd. **Skeleton:** the chapters below are filled as the API settles, step by
step ([ROADMAP.md](ROADMAP.md)); until then the reference is [minipsd.h](../include/minipsd.h)
and the contract is [SPECIFICATION.md](SPECIFICATION.md).

This manual explains and shows; it does not repeat the reference. Every parameter and return
code is documented once, in `minipsd.h`. The code in this manual is taken from
[examples/](../examples/), complete programs that CI builds and runs.

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
and are read in place. minipsd does no I/O: it hands bytes to a callback or puts them into a buffer of yours, and the
file, pipe or socket is yours. Two ways produce the file, and both give the same bytes:

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

There is nothing to release in minipsd: the writer lives in the workspace. The whole program:
[examples/write_memory.c](../examples/write_memory.c).

### To a blocking stream

`minipsd_write` drives the pull writer to the end and hands each piece, up to 4 KiB, to a
callback; returning 0 stops it with `MINIPSD_ERR_WRITE`:

```c
static int file_out(void *user, const void *data, size_t size)
{
    return fwrite(data, 1, size, (FILE *)user) == size;
}

static int write_file(const minipsd_doc *doc, const char *path)
{
    int   rc;
    FILE *f = open_utf8(path);
    if (!f) return MINIPSD_ERR_OPEN;
    rc = minipsd_write(doc, file_out, f);
    if (fclose(f) != 0 && rc == MINIPSD_OK) rc = MINIPSD_ERR_OPEN;   /* the last bytes, flushed */
    return rc;
}
```

`open_utf8` is `fopen` everywhere but Windows, where the narrow `fopen` reads the ANSI code page:
there the UTF-8 path goes through `MultiByteToWideChar` to `_wfopen`. The whole program,
`open_utf8` included: [examples/write_file.c](../examples/write_file.c). `MINIPSD_ERR_OPEN` is
not returned by minipsd; it is kept for code like this.

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
The whole program, an `O_NONBLOCK` pipe driven by `poll` with its reading end in the same loop:
[examples/write_nonblocking.c](../examples/write_nonblocking.c).

## Reading

The reader describes a file and decodes its pixels, layers and composite. It works on bytes you already hold, never copies them, and allocates nothing: its
workspace has a fixed size, so it can live on the stack.

```c
uint8_t            ws[256];   /* at least minipsd_reader_workspace_size(), the same for every file */
minipsd_reader    *r;
minipsd_info       info;
minipsd_layer_view layer;
char               name[256];
int                rc;

rc = minipsd_reader_init(&r, psd, psd_size, ws, sizeof ws, NULL, &info);   /* NULL: the format's limits */
if (rc < 0) return rc;
/* info: width, height, depth, colour mode, dpi, the ICC profile, the layer count */
while ((rc = minipsd_reader_next_layer(r, &layer)) == MINIPSD_MORE)
    if (minipsd_layer_name(&layer, name, sizeof name, NULL) == MINIPSD_OK) puts(name);
```

Everything the reader returns points into `psd`, which must stay where it is, unchanged, while
any of it is used.

**A prefix is enough to describe a file.** `minipsd_reader_init` needs the bytes up to the layer
count only. Given less, it returns `MINIPSD_ERR_SHORT`, and `minipsd_reader_needed` says how
long the buffer must be for the next step; read that much and call `init` again. Layer names need
the whole records section; a layer whose pixel data is not in the buffer yet still comes back,
with that channel's view `data == NULL`.

**Pixels go where you say.** A layer decodes into a `minipsd_target`: for each of R, G, B and
A a pointer, a pixel step and a row step, so interleaved RGBA, BGRA, planar, bottom-up or a spot
inside a larger canvas are all the same call. A `NULL` plane is not written. The size comes from
the layer's rectangle; the library writes exactly those samples:

```c
uint32_t       w = (uint32_t)(layer.right - layer.left), h = (uint32_t)(layer.bottom - layer.top);
uint8_t       *rgba = malloc((size_t)w * h * 4);
minipsd_target t;

minipsd_target_rgba(&t, rgba, (size_t)w * 4);
rc = minipsd_decode_layer(r, &layer, &t, NULL);   /* NULL: every row */
```

Pixels come as stored: straight alpha, the layer's opacity and visibility left in the view. A
`minipsd_decode_opts` with `y_first` and `y_count` decodes a band of rows into the target's first
row, so a huge layer can go through a small buffer in strips. Only 8-bit RGB files decode
(`MINIPSD_ERR_UNSUPPORTED` otherwise); if the layer's pixel data is not in the buffer yet, the
call says `MINIPSD_ERR_SHORT` and `minipsd_reader_needed` how far to read.

**The composite** is the flattened image a viewer that ignores layers shows, the full canvas
(`info.width` by `info.height`). `minipsd_decode_composite` takes the same target and bands. The
file stores it matted against white; it comes back un-matted, straight alpha like the layers,
which needs the target's `A` plane since the alpha is read back from there. Set `keep_matte`
for the bytes as stored, for instance to show them on white as they are:

```c
minipsd_target_rgba(&t, rgba, (size_t)info.width * 4);
rc = minipsd_decode_composite(r, &t, NULL);   /* every row, un-matted */
```

**Untrusted files.** Every length is checked; a malformed file is `MINIPSD_ERR_FORMAT`. Pass a
`minipsd_limits` to refuse large sides or many layers early (`MINIPSD_ERR_LIMIT`).

## Return codes

*To be written.*

## Configuration

*To be written.*
