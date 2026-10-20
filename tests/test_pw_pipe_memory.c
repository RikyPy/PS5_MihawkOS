/* SPDX-License-Identifier: GPL-3.0-or-later */
/* What a fresh pipe takes (src/pw_pipe_memory.h): some bytes on the host, and the line the title draws. */
#include "../src/pw_pipe_memory.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    const long taken = pw_pipe_buffer_probe();
    assert(taken > 0 && !pw_pipe_memory_low(taken));
    assert(pw_pipe_memory_low(4096) && !pw_pipe_memory_low(16384) && !pw_pipe_memory_low(65536) && !pw_pipe_memory_low(-1));
    printf("test_pw_pipe_memory: ok (a fresh pipe took %ld bytes on the host)\n", taken);
    return 0;
}
