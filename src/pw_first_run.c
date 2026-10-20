/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pw_first_run.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int pw_first_run_dev_conf(const char *path)
{
    static const char setting[] = PW_FIRST_RUN_DEV_CONF;
    const size_t length = sizeof(setting) - 1;
    char text[sizeof(setting) + 1];
    int fd = open(path, O_RDONLY);
    if (fd >= 0) {
        const ssize_t got = read(fd, text, sizeof(text));
        const int error = got < 0 ? errno : 0;
        close(fd);
        if (error) return -error;
        return (size_t)got == length && !memcmp(text, setting, length) ? PW_FIRST_RUN_PRESENT : PW_FIRST_RUN_OTHER;
    }
    if (errno != ENOENT) return -errno;
    char part[1024];
    if (snprintf(part, sizeof(part), "%s.mihawkos-part", path) >= (int)sizeof(part)) return -ENAMETOOLONG;
    fd = open(part, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0644);
    if (fd < 0) return -errno;
    int error = write(fd, setting, length) == (ssize_t)length ? 0 : errno ? errno : EIO;
    if (!error && fsync(fd) != 0) error = errno;
    if (close(fd) != 0 && !error) error = errno;
    if (!error && rename(part, path) != 0) error = errno;
    if (error) {
        unlink(part);
        return -error;
    }
    return PW_FIRST_RUN_WRITTEN;
}
