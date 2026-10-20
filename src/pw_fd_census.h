/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * How many file descriptors a process has open, and of what kind. The console's limit is 13,952, a
 * Wine thread holds about a dozen, and a program that ran out of them (Direct3D's tests, at about a
 * thousand threads) only showed it as "kqueue failed: Too many open files" at the first place that
 * wanted one more. The census says who holds them.
 */
#ifndef PW_FD_CENSUS_H
#define PW_FD_CENSUS_H

#ifdef __cplusplus
extern "C" {
#endif

struct PwFdCensus {
    unsigned open;      /* descriptors open */
    unsigned limit;     /* the table size the scan covered */
    unsigned sockets;
    unsigned fifos;
    unsigned files;     /* regular files */
    unsigned dirs;
    unsigned devices;   /* character and block devices */
    unsigned other;     /* what fstat names none of: kqueues, event ports, ... */
};

void pw_fd_census(struct PwFdCensus *out);

#ifdef __cplusplus
}
#endif

#endif
