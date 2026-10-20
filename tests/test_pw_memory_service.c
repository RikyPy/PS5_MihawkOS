/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "../src/pw_memory_service.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

/* The served process's memory. After the fork the parent's copy stays as it was, so whatever the parent reads back
 * through the service is the child's. */
static unsigned char served[3 * 32768 + 123];

int main(void)
{
    int report[2], title_end = -1;
    long page = sysconf(_SC_PAGESIZE);
    unsigned char *hole, *read_only;
    uint32_t done;

    for (size_t i = 0; i < sizeof(served); ++i) served[i] = (unsigned char)(i * 7);
    /* A page that cannot be read (PROT_NONE, so nothing else is mapped there later), and one that cannot be
     * written, beside one that can. */
    hole = mmap(NULL, 3 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(hole != MAP_FAILED);
    assert(!mprotect(hole + page, page, PROT_NONE));
    read_only = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(read_only != MAP_FAILED);
    memset(read_only, 0x44, page);
    assert(!mprotect(read_only, page, PROT_READ));
    memset(hole, 0x55, page);

    /* Copy over a deliberately small pipe. Short capacity writes must keep
     * going, while protection boundaries still report the completed prefix. */
    int small[2];
    static unsigned char small_copy[sizeof(served)];
    assert(!pipe(small));
    assert(!fcntl(small[1], F_SETFL, O_NONBLOCK));
#ifdef F_SETPIPE_SZ
    assert(fcntl(small[1], F_SETPIPE_SZ, 4096) == 4096);
#endif
    assert(pw_memory_pipe_copy(small, (char *)small_copy, (char *)served, sizeof(served)) == sizeof(served));
    assert(!memcmp(small_copy, served, sizeof(served)));
    assert(pw_memory_pipe_copy(small, (char *)small_copy, (char *)hole, 2 * page) == (uint32_t)page);
    assert(pw_memory_pipe_copy(small, (char *)read_only, (char *)served, 64) == 0);
    close(small[0]); close(small[1]);

    assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, report));
    const pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        /* The helper: serve, hand the title its end, then live until the title closes it. */
        int end;
        struct msghdr message = {0};
        char space[CMSG_SPACE(sizeof(int))] = {0};
        char byte = 'x';
        struct iovec io = {&byte, 1};
        close(report[0]);
        for (size_t i = 0; i < sizeof(served); ++i) served[i] = (unsigned char)(i * 13 + 1);
        if (pw_memory_service_start(&end)) _exit(2);
        message.msg_iov = &io;
        message.msg_iovlen = 1;
        message.msg_control = space;
        message.msg_controllen = sizeof(space);
        struct cmsghdr *header = CMSG_FIRSTHDR(&message);
        header->cmsg_level = SOL_SOCKET;
        header->cmsg_type = SCM_RIGHTS;
        header->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(header), &end, sizeof(end));
        if (sendmsg(report[1], &message, 0) != 1) _exit(3);
        close(end);
        /* Wait for the parent to say it is done, then check what it wrote. */
        if (read(report[1], &byte, 1) != 1) _exit(4);
        for (size_t i = 0; i < 40000; ++i)
            if (served[1000 + i] != (unsigned char)(i * 3)) _exit(5);
        _exit(served[0] == (unsigned char)1 ? 0 : 6);
    }
    close(report[1]);
    {
        struct msghdr message = {0};
        char space[CMSG_SPACE(sizeof(int))] = {0};
        char byte;
        struct iovec io = {&byte, 1};
        message.msg_iov = &io;
        message.msg_iovlen = 1;
        message.msg_control = space;
        message.msg_controllen = sizeof(space);
        assert(recvmsg(report[0], &message, 0) == 1);
        memcpy(&title_end, CMSG_DATA(CMSG_FIRSTHDR(&message)), sizeof(title_end));
    }

    /* A read larger than a chunk comes back whole, and is the child's. */
    static unsigned char copy[sizeof(served)];
    assert(!pw_memory_service_copy(title_end, 0, (uintptr_t)served, copy, sizeof(copy), &done, 5000));
    assert(done == sizeof(copy));
    for (size_t i = 0; i < sizeof(copy); ++i) assert(copy[i] == (unsigned char)(i * 13 + 1));

    /* A write larger than a chunk lands in the child (checked there, at its exit). */
    static unsigned char pattern[40000];
    for (size_t i = 0; i < sizeof(pattern); ++i) pattern[i] = (unsigned char)(i * 3);
    assert(!pw_memory_service_copy(title_end, 1, (uintptr_t)served + 1000, pattern, sizeof(pattern), &done, 5000));
    assert(done == sizeof(pattern));

    /* Reading up to a page that cannot be read stops there; the child does not fault. */
    assert(!pw_memory_service_copy(title_end, 0, (uintptr_t)hole, copy, 2 * page, &done, 5000));
    assert(done == (uint32_t)page && copy[0] == 0x55 && copy[page - 1] == 0x55);
    /* Writing into a page that cannot be written copies nothing; the child does not fault. */
    assert(!pw_memory_service_copy(title_end, 1, (uintptr_t)read_only, pattern, 64, &done, 5000));
    assert(done == 0);
    /* Reading it works. */
    assert(!pw_memory_service_copy(title_end, 0, (uintptr_t)read_only, copy, 64, &done, 5000));
    assert(done == 64 && copy[0] == 0x44);

    /* The service still answers after those refusals, and ends when the title's end closes. */
    assert(!pw_memory_service_copy(title_end, 0, (uintptr_t)served, copy, 1, &done, 5000) && done == 1);
    assert(write(report[0], "x", 1) == 1);
    int status = 0;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);
    close(title_end);
    /* A closed service fails rather than hanging. */
    int pair[2];
    assert(!socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair));
    close(pair[1]);
    assert(pw_memory_service_copy(pair[0], 0, (uintptr_t)served, copy, 16, &done, 500) == -1);
    close(pair[0]);
    puts("pw_memory_service: ok");
    return 0;
}
