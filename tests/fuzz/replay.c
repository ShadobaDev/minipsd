/**
 * \file
 * \brief Runs fuzz_reader.c's target once on each file named, without libFuzzer: how the tests
 * replay tests/fuzz/regressions/ on every toolchain.  Exits with 1 when a file cannot be read.
 *
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: Copyright (c) 2026 Bartlomiej Mucha <shadobadev@gmail.com>
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int main(int argc, char **argv)
{
    int i;
    for (i = 1; i < argc; ++i) {
        uint8_t *data = NULL;
        long     n    = -1;
        FILE    *f    = fopen(argv[i], "rb");
        if (f && fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) >= 0 && fseek(f, 0, SEEK_SET) == 0
            && (data = (uint8_t *)malloc(n ? (size_t)n : 1)) != NULL && fread(data, 1, (size_t)n, f) == (size_t)n) {
            LLVMFuzzerTestOneInput(data, (size_t)n);
        } else {
            fprintf(stderr, "replay: cannot read %s\n", argv[i]);
            return 1;
        }
        free(data);
        fclose(f);
    }
    return 0;
}
