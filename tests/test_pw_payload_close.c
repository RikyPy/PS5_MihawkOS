/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Native absence is never awaited with our control endpoint still open.
 * Simulated syscalls make timeout/EPERM and descriptor ownership deterministic;
 * actual SOCK_SEQPACKET hangup behavior remains a console qualification. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <signal.h>
#include <fcntl.h>

static int endpoint_open, closes, polls, sleeps, denied, shutdown_error;
static char trace[4096];
static int trace_open;
static int test_open(const char *path,int flags,...)
{
    assert(!strcmp(path,"/data/homebrew/PPSA99170/payload-lifecycle.log"));
    assert(flags==(O_WRONLY|O_CREAT|O_APPEND) && !trace_open);
    trace_open=1;errno=EDOM;return 38;
}
static ssize_t test_write(int fd,const void *data,size_t size)
{
    assert(fd==38 && trace_open && strlen(trace)+size<sizeof(trace));
    size_t used=strlen(trace);memcpy(trace+used,data,size);trace[used+size]=0;
    errno=EDOM;return size;
}
static int test_shutdown(int fd,int how)
{
    assert(fd==37 && how==SHUT_RDWR && endpoint_open);
    errno=shutdown_error;
    return shutdown_error ? -1 : 0;
}
static int test_close(int fd)
{
    if(fd==38) { assert(trace_open);trace_open=0;errno=EDOM;return 0; }
    assert(fd==37 && endpoint_open);
    endpoint_open=0; ++closes;
    return 0;
}
static int test_kill(pid_t pid,int signal)
{
    assert(pid==101 && signal==0 && !endpoint_open && closes==1);
    ++polls;
    if(denied) { errno=EPERM;return -1; }
    if(polls>=3) { errno=ESRCH;return -1; }
    return 0;
}
static int test_usleep(useconds_t delay)
{
    assert(delay==100000 && !endpoint_open);
    ++sleeps;
    return 0;
}
static int pw_child_send(int fd,const void *data,size_t bytes,const int *fds,unsigned count)
{
    (void)fd;(void)data;(void)bytes;(void)fds;(void)count;
    assert(!"spawn must not run in a close-contract test");
    return -1;
}
#define shutdown test_shutdown
#define close test_close
#define kill test_kill
#define usleep test_usleep
#define open test_open
#define write test_write
#include "../wine/ps5/pw_payload_spawn.h"

static void reset(void)
{
    endpoint_open=1;closes=polls=sleeps=denied=shutdown_error=0;
    trace[0]=0;trace_open=0;
}
int main(void)
{
    (void)payload_spawn;
    setenv("WINETEST_LIFECYCLE_TRACE","1",1);
    reset();
    errno=EAGAIN;payload_trace("errno-check",101,37,0);assert(errno==EAGAIN);
    assert(!payload_close(101,37));
    assert(!endpoint_open && closes==1 && polls==3 && sleeps==2);
    const char *begin=strstr(trace,"phase=shutdown-begin"), *end=strstr(trace,"phase=shutdown-end");
    const char *released=strstr(trace,"phase=endpoint-released"), *absent=strstr(trace,"phase=native-absent");
    assert(begin && end && released && absent && begin<end && end<released && released<absent && !trace_open);
    reset();shutdown_error=ENOTCONN;
    assert(!payload_close(101,37));
    assert(!endpoint_open && closes==1);
    reset();denied=1;
    assert(payload_close(101,37)==-1 && errno==ETIMEDOUT);
    assert(!endpoint_open && closes==1 && polls==100 && sleeps==100);
    assert(strstr(trace,"phase=absence-timeout status=1") && !strstr(trace,"phase=native-absent"));
    puts("payload close: PASS (close before absence, one owner, EPERM is not absence)");
    return 0;
}
