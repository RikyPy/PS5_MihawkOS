/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pw_fd_census.h"

#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SCAN_MAX 65536

void pw_fd_census(struct PwFdCensus *out)
{
    int limit = getdtablesize();
    struct stat st;

    memset(out, 0, sizeof(*out));
    if (limit > SCAN_MAX) limit = SCAN_MAX;
    if (limit < 0) limit = 0;
    out->limit = (unsigned)limit;
    for (int fd = 0; fd < limit; fd++) {
        if (fstat(fd, &st)) continue;
        out->open++;
        switch (st.st_mode & S_IFMT) {
        case S_IFSOCK: out->sockets++; break;
        case S_IFIFO: out->fifos++; break;
        case S_IFREG: out->files++; break;
        case S_IFDIR: out->dirs++; break;
        case S_IFCHR: case S_IFBLK: out->devices++; break;
        default: out->other++; break;
        }
    }
}
