/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The C library calls FFmpeg makes that no stub a title links against provides (tools/build-wine-ps5.sh links these
 * into winedmo.prx; __assert and isatty come from ntdll.prx): broken-down time, for log time stamps and container
 * creation dates; random bytes, for av_get_random_seed; and temporary files, which only encoders' two-pass logs ask
 * for and this build has no encoders. */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include "pw_civil_time.h"

struct tm *gmtime_r(const time_t *when, struct tm *out)
{
    return pw_civil_time(when, out);
}

struct tm *localtime_r(const time_t *when, struct tm *out)
{
    return pw_civil_time(when, out);
}

/* shortcut: not cryptographic (splitmix64 over the monotonic clock); FFmpeg only seeds dithering and stream ids
 * with it here, upgrade if a TLS or key path ever links it. */
void arc4random_buf(void *buffer, size_t size)
{
    static uint64_t state;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t s = __atomic_add_fetch(&state, (uint64_t)now.tv_nsec ^ ((uint64_t)now.tv_sec << 32), __ATOMIC_RELAXED);
    uint64_t z = 0;
    for (size_t i = 0; i < size; ++i) {
        if (!(i & 7)) {
            z = (s += 0x9e3779b97f4a7c15ull);
            z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
            z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
            z ^= z >> 31;
        }
        ((uint8_t *)buffer)[i] = (uint8_t)(z >> (8 * (i & 7)));
    }
}

int mkstemp(char *pattern)
{
    (void)pattern;
    errno = ENOSYS;
    return -1;
}
