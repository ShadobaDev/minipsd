/**
 * \file
 * \brief Example: a document into a non-blocking pipe, with the pull writer and poll.  POSIX.
 *
 * The writer never waits: each minipsd_writer_fill returns at once with the next bytes, and the
 * event loop sends them when the pipe has room, keeping what a short write left over for the next
 * turn.  The reading end of the pipe sits in the same loop and copies what it reads to a file; in
 * a real program it would be another process, a socket peer, or the next stage of a pipeline.
 *
 * Usage: write_nonblocking path.
 *
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: Copyright (c) 2026 Bartlomiej Mucha <shadobadev@gmail.com>
 */

#define _GNU_SOURCE   /* F_SETPIPE_SZ, on Linux; POSIX otherwise */

#define MINIPSD_IMPLEMENTATION
#include "minipsd.h"

#include "demo.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* Runs the loop: the writer into *wfd (non-blocking), closed and set to -1 after the last byte;
   rfd read into out until the end. */
static int pump(minipsd_writer *w, int *wfd, int rfd, FILE *out)
{
    static uint8_t pending[64 * 1024], in[4096];
    size_t         have = 0, sent = 0;
    int            rc = MINIPSD_MORE;   /* what the last fill returned: more to produce */
    for (;;) {
        struct pollfd p[2];
        ssize_t       n;
        p[0].fd = rfd; p[0].events = POLLIN;  p[0].revents = 0;
        p[1].fd = *wfd; p[1].events = POLLOUT; p[1].revents = 0;   /* a negative fd is skipped */
        if (poll(p, 2, -1) < 0 && errno != EINTR) return MINIPSD_ERR_WRITE;

        if (p[1].revents) {
            if (sent == have && rc == MINIPSD_MORE) {   /* everything sent: the next bytes */
                rc = minipsd_writer_fill(w, pending, sizeof pending, &have);
                if (rc < 0) return rc;
                sent = 0;
            }
            n = write(*wfd, pending + sent, have - sent);
            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) return MINIPSD_ERR_WRITE;
            if (n > 0) sent += (size_t)n;               /* a short write keeps the rest */
            if (sent == have && rc == MINIPSD_OK) {     /* the last byte is in the pipe */
                close(*wfd);
                *wfd = -1;
            }
        }
        if (p[0].revents) {
            n = read(rfd, in, sizeof in);
            if (n == 0) return *wfd < 0 ? MINIPSD_OK : MINIPSD_ERR_WRITE;   /* end of the pipe */
            if (n < 0 && errno != EAGAIN && errno != EINTR) return MINIPSD_ERR_WRITE;
            if (n > 0 && fwrite(in, 1, (size_t)n, out) != (size_t)n) return MINIPSD_ERR_WRITE;
        }
    }
}

int main(int argc, char **argv)
{
    static demo     d;
    minipsd_writer *w;
    void           *workspace;
    size_t          need = 0;
    int             fds[2], rc;
    FILE           *out;
    if (argc < 2) { fputs("usage: write_nonblocking path\n", stderr); return 2; }
    demo_init(&d);

    rc = minipsd_writer_workspace_size(&d.doc, &need);
    if (rc != MINIPSD_OK) { fprintf(stderr, "write_nonblocking: %s\n", minipsd_error_string(rc)); return 1; }
    workspace = malloc(need);
    if (!workspace) { fputs("write_nonblocking: out of memory\n", stderr); return 1; }
    rc = minipsd_writer_init(&w, &d.doc, workspace, need);

    out = fopen(argv[1], "wb");
    if (rc == MINIPSD_OK && (!out || pipe(fds) != 0)) rc = MINIPSD_ERR_OPEN;
    if (rc == MINIPSD_OK) {
#ifdef F_SETPIPE_SZ
        fcntl(fds[1], F_SETPIPE_SZ, 4096);   /* a small pipe, so writes come up short */
#endif
        fcntl(fds[1], F_SETFL, fcntl(fds[1], F_GETFL) | O_NONBLOCK);
        rc = pump(w, &fds[1], fds[0], out);
        close(fds[0]);
        if (fds[1] >= 0) close(fds[1]);
    }
    if (out && fclose(out) != 0 && rc == MINIPSD_OK) rc = MINIPSD_ERR_WRITE;
    free(workspace);
    if (rc != MINIPSD_OK) { fprintf(stderr, "write_nonblocking: %s\n", minipsd_error_string(rc)); return 1; }
    return 0;
}
