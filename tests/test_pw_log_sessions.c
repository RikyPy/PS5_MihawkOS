/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The session log folders (src/pw_log_sessions.h): rotation, its limits, and what a failure leaves. */
#include "../src/pw_log_sessions.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char root[128];

static void put(const char *name)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", root, name);
    FILE *file = fopen(path, "w");
    assert(file);
    fputs(name, file);
    fclose(file);
}
static int has(const char *name)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", root, name);
    return access(path, F_OK) == 0;
}
/* How many entries a session folder holds, -1 when it is not there. */
static int entries(const char *name)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", root, name);
    DIR *directory = opendir(path);
    if (!directory) return -1;
    int count = 0;
    for (struct dirent *entry; (entry = readdir(directory));) count += strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..");
    closedir(directory);
    return count;
}
static void folder(const char *name)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", root, name);
    assert(!mkdir(path, 0777));
}

int main(void)
{
    char base[] = "/tmp/proton-log-sessions-XXXXXX";
    unsigned removed = 99;
    assert(mkdtemp(base));
    snprintf(root, sizeof(root), "%s/sessions", base);

    /* No root yet: it and an empty 0 are made. */
    assert(!pw_log_sessions_rotate(root, PW_LOG_SESSIONS_KEEP, &removed) && removed == 0);
    assert(entries("0") == 0 && entries("1") == -1);

    /* A session's logs move to 1; the next session's 0 is empty. */
    put("0/a.log");
    put("0/b.log");
    assert(!pw_log_sessions_rotate(root, PW_LOG_SESSIONS_KEEP, &removed) && removed == 0);
    assert(entries("0") == 0 && has("1/a.log") && has("1/b.log") && entries("2") == -1);

    /* Then to 2, with the next one in 1. */
    put("0/c.log");
    assert(!pw_log_sessions_rotate(root, PW_LOG_SESSIONS_KEEP, &removed) && removed == 0);
    assert(entries("0") == 0 && has("1/c.log") && entries("1") == 1 && has("2/a.log") && has("2/b.log"));

    /* The oldest goes: its two files are removed. */
    assert(!pw_log_sessions_rotate(root, PW_LOG_SESSIONS_KEEP, &removed) && removed == 2);
    assert(entries("0") == 0 && entries("1") == 0 && has("2/c.log") && entries("2") == 1 && entries("3") == -1);

    /* 0 missing, 1 there: 1 becomes 2 and 0 is made. */
    char command[1024];
    snprintf(command, sizeof(command), "rm -rf %s/0 %s/2", root, root);
    assert(!system(command));
    put("1/d.log");
    assert(!pw_log_sessions_rotate(root, PW_LOG_SESSIONS_KEEP, &removed));
    assert(entries("0") == 0 && entries("1") == -1 && has("2/d.log"));

    /* Bad arguments change nothing. */
    put("0/e.log");
    assert(pw_log_sessions_rotate(root, 1, &removed) == EINVAL && removed == 0);
    assert(pw_log_sessions_rotate(root, PW_LOG_SESSIONS_MAX_KEEP + 1, NULL) == EINVAL);
    assert(pw_log_sessions_rotate(NULL, PW_LOG_SESSIONS_KEEP, NULL) == EINVAL);
    assert(has("0/e.log") && has("2/d.log"));

    /* The oldest holds a folder: ENOTEMPTY, nothing removed or renamed, and the new session shares 0 with the last. */
    snprintf(command, sizeof(command), "rm -rf %s/1 %s/2", root, root);
    assert(!system(command));
    folder("1");
    put("1/f.log");
    folder("2");
    put("2/g.log");
    folder("2/inner");
    assert(pw_log_sessions_rotate(root, PW_LOG_SESSIONS_KEEP, &removed) == ENOTEMPTY && removed == 0);
    assert(has("0/e.log") && has("1/f.log") && has("2/g.log") && has("2/inner"));

    /* The oldest is a file, not a folder: ENOTDIR, the same. */
    snprintf(command, sizeof(command), "rm -rf %s/2", root);
    assert(!system(command));
    put("2");
    assert(pw_log_sessions_rotate(root, PW_LOG_SESSIONS_KEEP, &removed) == ENOTDIR);
    assert(has("0/e.log") && has("1/f.log") && has("2"));

    /* 0 missing on a failure: it is still made. */
    snprintf(command, sizeof(command), "rm -rf %s/0", root);
    assert(!system(command));
    assert(pw_log_sessions_rotate(root, PW_LOG_SESSIONS_KEEP, &removed) == ENOTDIR && entries("0") == 0);

    /* A symbolic link as the oldest is not followed: what it points at keeps its files. */
    snprintf(command, sizeof(command), "rm -rf %s/2 && mkdir %s/elsewhere && touch %s/elsewhere/keep.log && ln -s %s/elsewhere %s/2",
             root, base, base, base, root);
    assert(!system(command));
    assert(pw_log_sessions_rotate(root, PW_LOG_SESSIONS_KEEP, &removed) == ENOTDIR);
    snprintf(command, sizeof(command), "test -f %s/elsewhere/keep.log", base);
    assert(!system(command));

    /* A root too long for the folder names. */
    char long_root[300];
    memset(long_root, 'a', sizeof(long_root) - 1);
    long_root[0] = '/';
    long_root[sizeof(long_root) - 1] = 0;
    assert(pw_log_sessions_rotate(long_root, PW_LOG_SESSIONS_KEEP, &removed) == ENAMETOOLONG);

    snprintf(command, sizeof(command), "rm -rf %s", base);
    assert(!system(command));
    puts("pw_log_sessions: ok");
    return 0;
}
