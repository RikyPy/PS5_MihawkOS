/* SPDX-License-Identifier: GPL-3.0-or-later
 * LD_PRELOAD for the Linux baseline of Wine's conformance tests: no child processes, as on the console.
 *
 * A PS5 title is one process; patch 0550 makes Windows process creation fail. Many tests start the test
 * program again as a child, and everything they then do with it fails on the console. Run on a Linux
 * Wine where fork() fails, the same tests fail the same way, so what remains of a comparison is what the
 * console does differently for another reason. Start the wineserver of the prefix before the test
 * (tools/run-wine-tests.py does), since starting one is a fork too. */
#define _GNU_SOURCE
#include <errno.h>
#include <sys/types.h>

pid_t fork(void)
{
    errno = EAGAIN;
    return -1;
}

pid_t vfork(void)
{
    errno = EAGAIN;
    return -1;
}
