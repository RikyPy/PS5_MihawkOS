/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "../src/pw_memory_file.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

int main(void)
{
    char directory[] = "/tmp/proton-memory-files-XXXXXX";
    assert(mkdtemp(directory));
    int fd = pw_memory_file_alloc(directory, (uint64_t)13824 << 20);
    assert(fd >= 0 && pw_memory_file_collect(directory) == 0);
    int pair[2];
    assert(!socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair));
    char data = 1, space[CMSG_SPACE(sizeof(int))] = {0};
    struct iovec io = {&data, 1};
    struct msghdr msg = {
        .msg_iov = &io, .msg_iovlen = 1, .msg_control = space, .msg_controllen = sizeof(space)};
    struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    *(int *)CMSG_DATA(c) = fd;
    assert(sendmsg(pair[0], &msg, 0) == 1 && !close(fd));
    assert(pw_memory_file_collect(directory) == 0); /* queued descriptor owns the lease */
    msg.msg_controllen = sizeof(space);
    assert(recvmsg(pair[1], &msg, 0) == 1);
    fd = *(int *)CMSG_DATA(CMSG_FIRSTHDR(&msg));
    int retained = dup(fd);
    assert(retained >= 0 && !close(fd));
    unsigned char *page =
        mmap(NULL, 16384, PROT_READ | PROT_WRITE, MAP_SHARED, retained, (off_t)8 << 30);
    assert(page != MAP_FAILED && page[0] == 0);
    page[0] = 0x71;
    assert(pw_memory_file_collect(directory) == 0);
    assert(!msync(page, 16384, MS_SYNC) && !munmap(page, 16384) && !close(retained));
    assert(pw_memory_file_collect(directory) == 1 && pw_memory_file_collect(directory) == 0);
    close(pair[0]);
    close(pair[1]);
    assert(!rmdir(directory));
    puts("pw_memory_file: sparse sizing, queued SCM_RIGHTS lease, retained mapping and cleanup pass");
}
