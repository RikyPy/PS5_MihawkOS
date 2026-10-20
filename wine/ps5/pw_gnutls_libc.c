/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The C library calls GnuTLS makes that no stub a title links against provides (tools/build-wine-ps5.sh links
 * these into libgnutls.prx): a failed assertion, broken-down UTC time, the password database, which a title has
 * none of, and gnulib's ending of a thread, which it names in ISO C's terms. */
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <time.h>
#include "pw_civil_time.h"

struct passwd;

void __assert(const char *function, const char *file, int line, const char *expression)
{
    fprintf(stderr, "libgnutls: assertion \"%s\" failed: %s:%d (%s)\n", expression, file, line,
            function ? function : "?");
    abort();
}

struct tm *gmtime_r(const time_t *when, struct tm *out)
{
    return pw_civil_time(when, out);
}

int getpwuid_r(uid_t uid, struct passwd *entry, char *buffer, size_t size, struct passwd **result)
{
    (void)uid; (void)entry; (void)buffer; (void)size;
    *result = NULL;
    return 0;   /* no such user: there is no password database */
}

void thrd_exit(int result) __attribute__((noreturn));
void thrd_exit(int result)
{
    pthread_exit((void *)(intptr_t)result);
}
