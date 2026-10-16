/**
 * \file
 * \brief Example: a document to a file, through minipsd_write and stdio.  Blocking.
 *
 * minipsd itself has no file I/O; this is the whole of it.  The path is UTF-8 on every platform:
 * on Windows the narrow fopen reads the ANSI code page, so the path goes to _wfopen as UTF-16.
 *
 * Usage: write_file [path].  Without a path it writes template-?.psd, the ? an a with ogonek
 * (U+0105, two bytes of UTF-8), to the current directory, to show a name outside ASCII.  On
 * Windows argv is in the ANSI code page, not UTF-8, so a path given there should be ASCII.
 *
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: Copyright (c) 2026 Bartlomiej Mucha <shadobadev@gmail.com>
 */

#define MINIPSD_IMPLEMENTATION
#include "minipsd.h"

#include "demo.h"

#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#  include <windows.h>
#endif

static FILE *open_utf8(const char *path)
{
#ifdef _WIN32
    FILE    *f = NULL;
    wchar_t *wide;
    int      n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    if (n <= 0) return NULL;
    wide = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
    if (!wide) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, n);
#  ifdef _MSC_VER
    if (_wfopen_s(&f, wide, L"wb") != 0) f = NULL;
#  else
    f = _wfopen(wide, L"wb");
#  endif
    free(wide);
    return f;
#else
    return fopen(path, "wb");
#endif
}

static int file_out(void *user, const void *data, size_t size)
{
    return fwrite(data, 1, size, (FILE *)user) == size;
}

/* Writes doc to path, replacing the file.  A failure after the file opened leaves it in place. */
static int write_file(const minipsd_doc *doc, const char *path)
{
    int   rc;
    FILE *f = open_utf8(path);
    if (!f) return MINIPSD_ERR_OPEN;
    rc = minipsd_write(doc, file_out, f);
    if (fclose(f) != 0 && rc == MINIPSD_OK) rc = MINIPSD_ERR_OPEN;   /* the last bytes, flushed */
    return rc;
}

int main(int argc, char **argv)
{
    static demo d;
    int         rc;
    demo_init(&d);
    rc = write_file(&d.doc, argc > 1 ? argv[1] : "template-\xC4\x85.psd");
    if (rc != MINIPSD_OK) {
        fprintf(stderr, "write_file: %s\n", minipsd_error_string(rc));
        return 1;
    }
    return 0;
}
