/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/pw_fd_census.h"
#include <assert.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

int main(void)
{
    struct PwFdCensus before, after;
    int pipes[2], pair[2], file, directory;

    pw_fd_census(&before);
    assert(before.open >= 3 && before.limit >= before.open);

    assert(!pipe(pipes));
    assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
    file = open("/dev/null", O_RDONLY);   /* a character device */
    directory = open("/", O_RDONLY);
    assert(file >= 0 && directory >= 0);

    pw_fd_census(&after);
    assert(after.open == before.open + 6);
    assert(after.fifos == before.fifos + 2);
    assert(after.sockets == before.sockets + 2);
    assert(after.devices == before.devices + 1);
    assert(after.dirs == before.dirs + 1);

    close(pipes[0]); close(pipes[1]); close(pair[0]); close(pair[1]); close(file); close(directory);
    pw_fd_census(&after);
    assert(after.open == before.open);
    return 0;
}
