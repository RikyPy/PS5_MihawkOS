/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Settings > Logging's session folders (src/pw_detailed_logs.h). */
#include "pw_detailed_logs.h"
#include "pw_log_sessions.h"
#include "../wine/ps5/pw_civil_time.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char readme[] =
    "MihawkOS detailed logs: one session\n"
    "\n"
    "This folder holds every log of one MihawkOS session. MihawkOS wrote it because Settings > Logging is On.\n"
    "Its name is the time the session started, in UTC.\n"
    "\n"
    "To report a problem:\n"
    "1. Zip this whole folder, with every file in it.\n"
    "2. Post the zip with your report: what you ran, what you did, and what went wrong.\n"
    "\n"
    "Before you post:\n"
    "- Mask your console's IP address: four numbers with dots between them, like 192.168.x.y. MihawkOS replaces\n"
    "  the digits of every address it finds with x, but look through the files and mask any it missed.\n"
    "- The logs name the programs you ran and the folders they are in (for example C:\\Games\\<name>), and they hold\n"
    "  what those programs printed. Leave out the zip if you do not want to share that.\n"
    "- The logs hold no game files and no saves.\n"
    "\n"
    "If the console froze or MihawkOS closed during the session, start MihawkOS once more: it adds the session's\n"
    "last logs to this folder then.\n"
    "\n"
    "klog.txt, the console's kernel log, is here only when klogsrv was loaded during the session (it comes with the\n"
    "payload loader some consoles run). Without klogsrv there is no klog.txt; the other logs are complete anyway.\n"
    "\n"
    "MihawkOS keeps the 5 newest of these folders, at most 96 MB each, and removes older ones. Turn Logging off in\n"
    "Settings when you no longer need it: sessions write more while it is on.\n";

int pw_detailed_logs_load(int *enabled, const char *path)
{
    char key[32], value[32], extra[32];
    int error = 0;
    *enabled = 0;
    FILE *file = fopen(path, "r");
    if (!file) return errno;
    if (fscanf(file, "%31s %31s", key, value) != 2 || strcmp(key, "logging") ||
        (strcmp(value, "on") && strcmp(value, "off")) || fscanf(file, "%31s", extra) == 1)
        error = EINVAL;
    if (!error && ferror(file)) error = EIO;
    fclose(file);
    if (!error) *enabled = !strcmp(value, "on");
    return error;
}

int pw_detailed_logs_save(int enabled, const char *path)
{
    char temporary[1024];
    if (snprintf(temporary, sizeof(temporary), "%s.tmp", path) >= (int)sizeof(temporary)) return ENAMETOOLONG;
    FILE *file = fopen(temporary, "w");
    if (!file) return errno;
    int error = 0;
    if (fprintf(file, "logging %s\n", enabled ? "on" : "off") < 0) error = EIO;
    if (fflush(file) || fsync(fileno(file))) error = errno;
    if (fclose(file) && !error) error = errno;
    if (!error && rename(temporary, path)) error = errno;
    if (error) unlink(temporary);
    return error;
}

static int write_all(int fd, const char *data, size_t size)
{
    while (size) {
        const ssize_t done = write(fd, data, size);
        if (done < 0 && errno == EINTR) continue;
        if (done <= 0) return done < 0 ? errno : EIO;
        data += done;
        size -= (size_t)done;
    }
    return 0;
}

static int is_part(char c)
{
    return (c >= '0' && c <= '9') || c == '.';
}

/* One whole run of digits and dots: its digits become 'x' when it is a dotted IPv4 address (a sentence's dots around
 * it left out). */
static void mask_run(char *run, size_t n)
{
    while (n && *run == '.') ++run, --n;
    while (n && run[n - 1] == '.') --n;
    unsigned groups = 1, digits = 0;
    for (size_t i = 0; i < n; ++i) {
        if (run[i] != '.') {
            if (++digits > 3) return;
        } else if (!digits) {
            return;
        } else {
            ++groups;
            digits = 0;
        }
    }
    if (groups != 4) return;
    for (size_t i = 0; i < n; ++i)
        if (run[i] != '.') run[i] = 'x';
}

size_t pw_detailed_logs_mask(char *text, size_t length, int final, int *continued)
{
    size_t i = 0;
    if (continued && *continued) {
        while (i < length && is_part(text[i])) ++i;
        if (i == length) return 0;   /* still the run that was settled */
        *continued = 0;
    }
    while (i < length) {
        if (!is_part(text[i])) {
            ++i;
            continue;
        }
        size_t start = i;
        while (i < length && is_part(text[i])) ++i;
        if (i == length && !final) {
            while (start < length && text[start] == '.') ++start;   /* leading dots never change the verdict */
            if (start == length) return 0;
            if (length - start <= PW_DETAILED_LOGS_CARRY) return length - start;
            /* settled as it stands (an address with more dots than that is still masked); what follows is no address */
            if (continued) *continued = 1;
        }
        mask_run(text + start, i - start);
    }
    return 0;
}

int pw_detailed_logs_stream(int from, int to, unsigned long long cap, unsigned long long *written)
{
    char buffer[16384];
    size_t carry = 0;
    unsigned long long total = 0;
    int error = 0, continued = 0;
    for (;;) {
        const ssize_t got = read(from, buffer + carry, sizeof(buffer) - carry);
        if (got < 0 && errno == EINTR) continue;
        if (got < 0) error = errno;
        const int final = got <= 0;
        const size_t have = carry + (got > 0 ? (size_t)got : 0);
        carry = pw_detailed_logs_mask(buffer, have, final, &continued);
        size_t ready = have - carry;
        if (ready > cap - total) ready = (size_t)(cap - total);
        const int failed = write_all(to, buffer, ready);
        if (failed && !error) error = failed;
        total += ready;
        memmove(buffer, buffer + have - carry, carry);
        if (final || failed || total >= cap) break;
    }
    if (written) *written = total;
    return error;
}

/* A name pw_detailed_logs_begin gives: a digit first (the year), nothing a path could misread. */
static int dated(const char *name)
{
    if (name[0] < '0' || name[0] > '9' || strlen(name) >= 32) return 0;
    for (const char *c = name; *c; ++c)
        if (!(*c >= '0' && *c <= '9') && *c != '-' && *c != '_') return 0;
    return 1;
}

/* Leave at most keep - 1 dated folders under root, removing the oldest by name (the names sort by time). */
static int prune(const char *root, unsigned keep, unsigned *removed)
{
    /* shortcut: looks at 64 folders of one listing; more exist only if something else writes there, then the oldest of
     * those 64 go first */
    char names[64][32];
    size_t count = 0;
    DIR *directory = opendir(root);
    if (!directory) return errno;
    char path[PW_DETAILED_LOGS_PATH];
    struct stat status;
    for (struct dirent *entry; count < 64 && (entry = readdir(directory));) {
        if (!dated(entry->d_name) || (size_t)snprintf(path, sizeof(path), "%s/%s", root, entry->d_name) >= sizeof(path) ||
            lstat(path, &status) || !S_ISDIR(status.st_mode))
            continue;
        /* in name order as they come (no qsort: the title has never imported it) */
        size_t at = count++;
        for (; at && strcmp(names[at - 1], entry->d_name) > 0; --at) memcpy(names[at], names[at - 1], sizeof(names[0]));
        strcpy(names[at], entry->d_name);
    }
    closedir(directory);
    int error = 0;
    size_t left = count;
    for (size_t i = 0; i < count && left >= keep; ++i) {
        snprintf(path, sizeof(path), "%s/%s", root, names[i]);
        const int dropped = pw_log_sessions_drop(path, removed);
        if (!dropped) --left;
        else if (dropped != ENOTEMPTY && !error) error = dropped;   /* ENOTEMPTY: it stays, and the next oldest goes */
    }
    return error;
}

int pw_detailed_logs_begin(const char *root, unsigned keep, time_t now, char *folder, size_t size, unsigned *removed)
{
    if (removed) *removed = 0;
    if (!root || !folder || !keep || strlen(root) > PW_DETAILED_LOGS_PATH - 40) return EINVAL;
    if (mkdir(root, 0777) && errno != EEXIST) return errno;
    const int pruned = prune(root, keep, removed);
    struct tm when;
    pw_civil_time(&now, &when);
    char path[PW_DETAILED_LOGS_PATH];
    int made = EEXIST;
    for (int attempt = 1; made == EEXIST && attempt <= 10; ++attempt) {
        int length = snprintf(path, sizeof(path), "%s/%04d-%02d-%02d_%02d-%02d-%02d", root, when.tm_year + 1900,
                              when.tm_mon + 1, when.tm_mday, when.tm_hour, when.tm_min, when.tm_sec);
        if (attempt > 1) snprintf(path + length, sizeof(path) - (size_t)length, "-%d", attempt);
        made = mkdir(path, 0777) ? errno : 0;
    }
    if (made) return made;
    if ((size_t)snprintf(folder, size, "%s", path) >= size) return ENAMETOOLONG;
    char file[PW_DETAILED_LOGS_PATH + 16];
    snprintf(file, sizeof(file), "%s/README.txt", path);
    int fd = open(file, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    int error = fd < 0 ? errno : write_all(fd, readme, sizeof(readme) - 1);
    if (fd >= 0 && close(fd) && !error) error = errno;
    snprintf(file, sizeof(file), "%s/pending", root);
    fd = open(file, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    int noted = fd < 0 ? errno : write_all(fd, path, strlen(path));
    if (fd >= 0 && fsync(fd) && !noted) noted = errno;
    if (fd >= 0 && close(fd) && !noted) noted = errno;
    if (noted && !error) error = noted;
    return error ? error : pruned;
}

int pw_detailed_logs_pending(const char *root, char *folder, size_t size)
{
    char file[PW_DETAILED_LOGS_PATH + 16], path[PW_DETAILED_LOGS_PATH];
    snprintf(file, sizeof(file), "%s/pending", root);
    const int fd = open(file, O_RDONLY);
    if (fd < 0) return errno;
    const ssize_t got = read(fd, path, sizeof(path) - 1);
    close(fd);
    unlink(file);
    if (got <= 0) return EINVAL;
    path[got] = 0;
    const size_t prefix = strlen(root);
    struct stat status;
    if (strncmp(path, root, prefix) || path[prefix] != '/' || !dated(path + prefix + 1) || lstat(path, &status) ||
        !S_ISDIR(status.st_mode))
        return EINVAL;
    return (size_t)snprintf(folder, size, "%s", path) >= size ? ENAMETOOLONG : 0;
}

/* The bytes of the folder's regular files. */
static unsigned long long folder_bytes(const char *folder)
{
    unsigned long long total = 0;
    DIR *directory = opendir(folder);
    if (!directory) return 0;
    char path[PW_DETAILED_LOGS_PATH + 64];
    struct stat status;
    for (struct dirent *entry; (entry = readdir(directory));) {
        if ((size_t)snprintf(path, sizeof(path), "%s/%s", folder, entry->d_name) < sizeof(path) && !lstat(path, &status) &&
            S_ISREG(status.st_mode))
            total += (unsigned long long)status.st_size;
    }
    closedir(directory);
    return total;
}

/* One file into folder/name, its end when it is longer than the room left. */
static int copy_file(const char *source, const char *folder, const char *name, unsigned long long *room,
                     unsigned *copied, unsigned *left_out)
{
    char target[PW_DETAILED_LOGS_PATH + 64];
    if ((size_t)snprintf(target, sizeof(target), "%s/%s", folder, name) >= sizeof(target)) return ENAMETOOLONG;
    const int from = open(source, O_RDONLY | O_NOFOLLOW);
    if (from < 0) return errno == ENOENT ? 0 : errno;
    struct stat status;
    if (fstat(from, &status) || !S_ISREG(status.st_mode)) {
        close(from);
        return 0;
    }
    unsigned long long allowed = *room < PW_DETAILED_LOGS_FILE_BYTES ? *room : PW_DETAILED_LOGS_FILE_BYTES;
    if (allowed < 4096 && (unsigned long long)status.st_size > allowed) {
        close(from);
        if (left_out) ++*left_out;
        return 0;
    }
    const int to = open(target, O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (to < 0) {
        const int error = errno;
        close(from);
        return error == EEXIST ? 0 : error;
    }
    int error = 0;
    unsigned long long written = 0;
    if ((unsigned long long)status.st_size > allowed) {
        allowed -= 128;   /* the note's room */
        off_t start = (off_t)((unsigned long long)status.st_size - allowed);
        char head[4096], note[128];
        const ssize_t got = pread(from, head, sizeof(head), start);
        const char *line = got > 0 ? memchr(head, '\n', (size_t)got) : NULL;
        if (line) start += line - head + 1;   /* from a whole line, so no address is cut in two */
        const int length = snprintf(note, sizeof(note), "[the first %lld bytes are left out: this copy keeps the end]\n",
                                    (long long)start);
        if (lseek(from, start, SEEK_SET) < 0) error = errno;
        if (!error) error = write_all(to, note, (size_t)length);
        written += (unsigned long long)length;
    }
    unsigned long long streamed = 0;
    if (!error) error = pw_detailed_logs_stream(from, to, allowed, &streamed);
    written += streamed;
    if (close(to) && !error) error = errno;
    close(from);
    *room = written < *room ? *room - written : 0;
    if (!error && copied) ++*copied;
    return error;
}

static int log_name(const char *name)
{
    const size_t length = strlen(name);
    return length > 4 && (!strcmp(name + length - 4, ".log") || !strcmp(name + length - 4, ".txt"));
}

int pw_detailed_logs_fill(const char *folder, const char *const *sources, size_t count, unsigned *copied,
                          unsigned *left_out)
{
    if (copied) *copied = 0;
    if (left_out) *left_out = 0;
    if (!folder || (!sources && count)) return EINVAL;
    const unsigned long long used = folder_bytes(folder);
    unsigned long long room = used < PW_DETAILED_LOGS_FOLDER_BYTES ? PW_DETAILED_LOGS_FOLDER_BYTES - used : 0;
    int first = 0;
    char path[PW_DETAILED_LOGS_PATH + 64];
    for (size_t i = 0; i < count; ++i) {
        struct stat status;
        if (lstat(sources[i], &status)) {
            if (errno != ENOENT && !first) first = errno;
            continue;
        }
        if (S_ISREG(status.st_mode)) {
            const char *slash = strrchr(sources[i], '/');
            const int error = copy_file(sources[i], folder, slash ? slash + 1 : sources[i], &room, copied, left_out);
            if (error && !first) first = error;
            continue;
        }
        if (!S_ISDIR(status.st_mode)) continue;
        DIR *directory = opendir(sources[i]);
        if (!directory) {
            if (!first) first = errno;
            continue;
        }
        for (struct dirent *entry; (entry = readdir(directory));) {
            if (!log_name(entry->d_name)) continue;
            if ((size_t)snprintf(path, sizeof(path), "%s/%s", sources[i], entry->d_name) >= sizeof(path)) {
                if (!first) first = ENAMETOOLONG;
                continue;
            }
            const int error = copy_file(path, folder, entry->d_name, &room, copied, left_out);
            if (error && !first) first = error;
        }
        closedir(directory);
    }
    return first;
}
