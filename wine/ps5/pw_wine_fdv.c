/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The one open-file budget of the Wine process (pw_wine_fdv.h), linked into
 * ntdll over the platform layer's module (ps5platform/fdv.h). The kernel calls
 * it uses are the real ones, under the names --wrap leaves them, so nothing
 * here goes through a wrapper itself. */
#include "pw_wine_fdv.h"
#include <ps5platform/fdv.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

int __real_open(const char *path, int flags, ...);
int __real_close(int fd);
off_t __real_lseek(int fd, off_t offset, int whence);
int __real_fstat(int fd, struct stat *status);

static int ops_open(const char *path, int flags, mode_t mode) { return __real_open(path, flags, mode); }
static int ops_close(int fd) { return __real_close(fd); }
static int ops_dup2(int from, int to) { return dup2(from, to); }
static off_t ops_lseek(int fd, off_t offset, int whence) { return __real_lseek(fd, offset, whence); }
static int ops_fstat(int fd, struct stat *status) { return __real_fstat(fd, status); }
static int ops_placeholder(void) { return socket(AF_UNIX, SOCK_STREAM, 0); }
/* stderr reaches the console log through ntdll's output sink. */
static void ops_log(const char *line) { fprintf(stderr, "wine-ps5: %s\n", line); }

static const struct ps5_fdv_ops ops = {
    ops_open, ops_close, ops_dup2, ops_lseek, ops_fstat, ops_placeholder, ops_log,
};

static int api_enter(int fd, uint32_t *pin) { return ps5_fdv_enter(fd, pin); }
static void api_leave(uint32_t pin) { ps5_fdv_leave(pin); }
static int api_sending(int fd, uint32_t *pin) { return ps5_fdv_sending(fd, pin); }
static void api_sent(uint32_t pin, int delivered) { ps5_fdv_sent(pin, delivered); }
static int api_received(int fd) { return ps5_fdv_received(fd); }
static void api_dup_note(int from, int to) { ps5_fdv_dup_note(from, to); }
static void api_forget(int fd) { ps5_fdv_forget(fd); }
static void api_keep(uint32_t pin) { ps5_fdv_keep(pin); }
static void api_renamed(const char *from, const char *to) { ps5_fdv_renamed(from, to); }
static unsigned api_make_room(void) { return ps5_fdv_make_room(); }

static const struct pw_wine_fdv table = {
    ps5_fdv_open, ps5_fdv_close, api_enter, api_leave, api_sending, api_sent, api_received,
    api_dup_note, api_forget, api_keep, api_renamed, api_make_room,
};

/* PW_FDV_OFF=1 in a game's environment leaves every wrapped call on plain libc,
 * to tell what the budget costs a program from what is not its doing. */
static int off(void)
{
    const char *setting = getenv("PW_FDV_OFF");

    return setting && *setting && strcmp(setting, "0");
}

const struct pw_wine_fdv *pw_wine_fdv_table(void)
{
    return off() ? NULL : &table;
}

/* Before anything of ntdll's opens a file: the table is in place when the
 * first wrapped call looks for it. A budget that cannot park (the self-check
 * failed) still tracks nothing and costs a pointer test per call. */
__attribute__((constructor)) static void start(void)
{
    /* The console counts open files for a title and all its child processes
     * together, about 250 (fd-budget-01): a title with Windows programs in
     * child processes parks its idle files earlier (PW_FDV_HIGH_WATER, set by
     * the title and by each child loader), so every process's files fit. */
    const char *high_water = getenv("PW_FDV_HIGH_WATER");
    const unsigned limit = high_water ? (unsigned)strtoul(high_water, NULL, 10) : 0;

    if (off()) return;
    if (ps5_fdv_init(&ops, limit, 0)) fprintf(stderr, "wine-ps5: the open-file budget is off\n");
    pw_wine_fdv = &table;
}

void pw_wine_fdv_stats(PwWineFdvStats *out)
{
    struct ps5_fdv_stats stats;

    ps5_fdv_get_stats(&stats);
    out->live = stats.live;
    out->parked = stats.parked;
    out->high_water = stats.high_water;
    out->parks = stats.parks;
    out->unparks = stats.unparks;
    out->limit_hits = stats.limit_hits;
    out->failures = stats.failures;
    out->disabled = stats.disabled;
}
