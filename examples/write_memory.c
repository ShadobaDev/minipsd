/**
 * \file
 * \brief Example: a document into one buffer of the exact size, with the pull writer.
 *
 * The pull writer knows the file's size before its first byte (minipsd_writer_total), so the
 * buffer is allocated once and filled by one call.  Two allocations in all, both the caller's:
 * the workspace and the buffer.
 *
 * Usage: write_memory path.  The buffer goes to path, to be checked.
 *
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: Copyright (c) 2026 Bartlomiej Mucha <shadobadev@gmail.com>
 */

#define MINIPSD_IMPLEMENTATION
#include "minipsd.h"

#include "demo.h"

#include <stdio.h>
#include <stdlib.h>

/* The file for doc in a new buffer; *size receives its size.  NULL on failure, with *rc set. */
static uint8_t *write_memory(const minipsd_doc *doc, size_t *size, int *rc)
{
    minipsd_writer *w;
    void           *workspace;
    uint8_t        *buf = NULL;
    size_t          need = 0, produced = 0;
    *rc = minipsd_writer_workspace_size(doc, NULL, &need);
    if (*rc != MINIPSD_OK) return NULL;
    workspace = malloc(need);
    if (!workspace) { *rc = MINIPSD_ERR_ALLOC; return NULL; }
    *rc = minipsd_writer_init(&w, doc, NULL, workspace, need);
    if (*rc == MINIPSD_OK) {
        const uint64_t total = minipsd_writer_total(w);
        buf = (size_t)total == total ? (uint8_t *)malloc((size_t)total) : NULL;   /* fits size_t */
        if (!buf) *rc = MINIPSD_ERR_ALLOC;
        else *rc = minipsd_writer_fill(w, buf, (size_t)total, &produced);   /* all of it: MINIPSD_OK */
    }
    free(workspace);   /* the writer is done with it */
    if (*rc != MINIPSD_OK) { free(buf); return NULL; }
    *size = produced;
    return buf;
}

int main(int argc, char **argv)
{
    static demo d;
    uint8_t    *psd;
    size_t      size = 0;
    int         rc;
    FILE       *f;
    if (argc < 2) { fputs("usage: write_memory path\n", stderr); return 2; }
    demo_init(&d);
    psd = write_memory(&d.doc, &size, &rc);
    if (!psd) { fprintf(stderr, "write_memory: %s\n", minipsd_error_string(rc)); return 1; }
    f = fopen(argv[1], "wb");
    rc = f && fwrite(psd, 1, size, f) == size;
    if (f && fclose(f) != 0) rc = 0;
    free(psd);
    if (!rc) { fprintf(stderr, "write_memory: cannot write %s\n", argv[1]); return 1; }
    return 0;
}
