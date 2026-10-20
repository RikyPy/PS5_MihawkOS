/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pw_memory_file.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

static int make_path(char *path, size_t capacity, const char *directory, const char *name)
{
    int n = snprintf(path, capacity, "%s/%s", directory, name);
    if (n < 0 || (size_t)n >= capacity)
    {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

int pw_memory_file_alloc(const char *directory, uint64_t bytes)
{
    static unsigned serial;
    char name[80], path[PATH_MAX];
    int fd = -1;
    if (!directory || !bytes || bytes > INT64_MAX)
    {
        errno = EINVAL;
        return -1;
    }
    if (mkdir(directory, 0700) && errno != EEXIST)
        return -1;
    for (unsigned attempt = 0; attempt < 256; ++attempt)
    {
        snprintf(name, sizeof(name), "section-%u-%u.tmp", (unsigned)getpid(), ++serial);
        if (make_path(path, sizeof(path), directory, name))
            return -1;
        fd = open(path, O_CREAT | O_EXCL | O_RDWR | O_NOFOLLOW, 0600);
        if (fd >= 0 || errno != EEXIST)
            break;
    }
    if (fd < 0)
        return -1;
    if (flock(fd, LOCK_SH | LOCK_NB) || ftruncate(fd, (off_t)bytes))
    {
        int saved = errno;
        close(fd);
        unlink(path);
        errno = saved;
        return -1;
    }
    return fd;
}

int pw_memory_file_collect(const char *directory)
{
    DIR *dir = opendir(directory);
    if (!dir)
        return errno == ENOENT ? 0 : -1;
    struct dirent *entry;
    int removed = 0, saved = 0;
    while ((entry = readdir(dir)))
    {
        unsigned pid, serial;
        int length = 0;
        char path[PATH_MAX];
        struct stat st;
        if (sscanf(entry->d_name, "section-%u-%u.tmp%n", &pid, &serial, &length) != 2 ||
            entry->d_name[length])
            continue;
        if (make_path(path, sizeof(path), directory, entry->d_name))
        {
            saved = errno;
            break;
        }
        int fd = open(path, O_RDWR | O_NOFOLLOW);
        if (fd < 0)
        {
            if (errno != ENOENT)
                saved = errno;
            continue;
        }
        if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_nlink != 1)
        {
            close(fd);
            continue;
        }
        if (flock(fd, LOCK_EX | LOCK_NB))
        {
            if (errno != EWOULDBLOCK && errno != EAGAIN)
                saved = errno;
            close(fd);
            continue;
        }
        int flushed = fsync(fd), flush_error = errno;
        int closed = close(fd), close_error = errno;
        if (flushed || closed)
        {
            saved = flushed ? flush_error : close_error;
            continue;
        }
        if (!unlink(path))
            ++removed;
        else if (errno != ENOENT)
            saved = errno;
    }
    closedir(dir);
    if (saved)
    {
        errno = saved;
        return -1;
    }
    return removed;
}
