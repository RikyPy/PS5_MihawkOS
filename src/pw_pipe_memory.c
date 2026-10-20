/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pw_pipe_memory.h"

#include <fcntl.h>
#include <unistd.h>

long pw_pipe_buffer_probe(void)
{
    static char chunk[4096];
    int ends[2];
    long taken = 0;
    if (pipe(ends)) return -1;
    if (fcntl(ends[1], F_SETFL, fcntl(ends[1], F_GETFL) | O_NONBLOCK) == 0)
        while (taken < (128 << 10)) {
            const ssize_t wrote = write(ends[1], chunk, sizeof(chunk));
            if (wrote <= 0) break;
            taken += wrote;
        }
    close(ends[0]);
    close(ends[1]);
    return taken;
}
