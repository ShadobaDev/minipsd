/**
 * \file
 * \brief Compiles minipsd.h and its implementation with MINIPSD_NO_MALLOC, as C with extern linkage.
 *        Built, never run: the check is that nothing allocating comes in, neither <stdlib.h> nor
 *        minipsd_write or minipsd_read, and that what remains leaves no internal function unused.
 *
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: Copyright (c) 2026 Bartlomiej Mucha <shadobadev@gmail.com>
 */

#define MINIPSD_NO_MALLOC
#define MINIPSD_IMPLEMENTATION
#include "minipsd.h"

#ifdef EXIT_SUCCESS
#  error "MINIPSD_NO_MALLOC still includes <stdlib.h>"
#endif
#ifdef EOF
#  error "minipsd includes <stdio.h>"
#endif

/* The names are free: a declaration in minipsd.h would conflict with these. */
int minipsd_write(void);
int minipsd_write(void) { return 0; }
int minipsd_read(void);
int minipsd_read(void) { return 0; }
int minipsd_doc_free(void);
int minipsd_doc_free(void) { return 0; }
