/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Rotating the last Wine sessions' log folders (src/pw_log_sessions.h). */
#include "pw_log_sessions.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int pw_log_sessions_drop(const char *folder, unsigned *removed)
{
    struct stat status;
    if (lstat(folder, &status)) return errno == ENOENT ? 0 : errno;
    if (!S_ISDIR(status.st_mode)) return ENOTDIR;
    char path[PW_LOG_SESSIONS_PATH];
    for (int pass = 0; pass < 2; ++pass) {
        DIR *directory = opendir(folder);
        if (!directory) return errno;
        int error = 0;
        for (struct dirent *entry; !error && (entry = readdir(directory));) {
            if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
            if ((size_t)snprintf(path, sizeof(path), "%s/%s", folder, entry->d_name) >= sizeof(path)) error = ENAMETOOLONG;
            else if (lstat(path, &status)) error = errno;
            else if (!S_ISREG(status.st_mode)) error = ENOTEMPTY;
            else if (pass == 1 && unlink(path)) error = errno;
            else if (pass == 1 && removed) ++*removed;
        }
        closedir(directory);
        if (error) return error;
    }
    return rmdir(folder) ? errno : 0;
}

int pw_log_sessions_rotate(const char *root, unsigned keep, unsigned *removed)
{
    if (removed) *removed = 0;
    if (!root || keep < 2 || keep > PW_LOG_SESSIONS_MAX_KEEP) return EINVAL;
    if (strlen(root) > PW_LOG_SESSIONS_PATH - 16) return ENAMETOOLONG;
    if (mkdir(root, 0777) && errno != EEXIST) return errno;
    char from[PW_LOG_SESSIONS_PATH], to[PW_LOG_SESSIONS_PATH];
    snprintf(to, sizeof(to), "%s/%u", root, keep - 1);
    int error = pw_log_sessions_drop(to, removed);
    for (unsigned i = keep - 1; !error && i-- > 0;) {
        snprintf(from, sizeof(from), "%s/%u", root, i);
        snprintf(to, sizeof(to), "%s/%u", root, i + 1);
        if (rename(from, to) && errno != ENOENT) error = errno;
    }
    snprintf(to, sizeof(to), "%s/0", root);
    if (mkdir(to, 0777) && errno != EEXIST && !error) error = errno;
    return error;
}
