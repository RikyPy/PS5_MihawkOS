/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pw_wine_profile.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum { PATH_MAX_BYTES = 1024, TABLE_MAX = 4096 };   /* the link table as Wine's Unix side reads it, at most */

/* Whether text holds word (lowercase letters) here, in either case. */
static int same_word(const char *text, const char *word, size_t length)
{
    for (size_t i = 0; i < length; i++)
        if ((text[i] | 0x20) != word[i]) return 0;
    return 1;
}

/* The length of the path separator that ends just before text[at]: 2 for the escaped backslash, 1 for '/', else 0. */
static size_t separator_before(const char *text, size_t at)
{
    if (at >= 2 && text[at - 1] == '\\' && text[at - 2] == '\\') return 2;
    return at >= 1 && text[at - 1] == '/';
}

/* Whether the former name, which starts after done[0..out) (the text already written) and is followed by after, is
 * the profile's. */
static int is_profile_name(const char *done, size_t out, char after)
{
    if (out >= 1 && done[out - 1] == '"' && after == '"') return 1;
    if (!strchr("\\/\";,]", after)) return 0;
    const size_t separator = separator_before(done, out);
    if (!separator || out < separator + 5) return 0;
    const size_t users = out - separator - 5;
    return same_word(done + users, "users", 5) && separator_before(done, users);
}

size_t pw_wine_profile_rewrite(char *text, size_t length, size_t *replaced)
{
    const size_t former = strlen(PW_WINE_PROFILE_FORMER), user = strlen(PW_WINE_PROFILE_USER);
    size_t in = 0, out = 0, count = 0;

    /* The new name is shorter, so the text written never passes the text read. */
    while (in < length) {
        if (length - in > former && same_word(text + in, PW_WINE_PROFILE_FORMER, former) &&
            is_profile_name(text, out, text[in + former])) {
            memcpy(text + out, PW_WINE_PROFILE_USER, user);
            out += user;
            in += former;
            count++;
        } else {
            text[out++] = text[in++];
        }
    }
    if (replaced) *replaced = count;
    return out;
}

/* Append "/name" to path, a buffer of PATH_MAX_BYTES. */
static int append(char *path, const char *name)
{
    const size_t used = strlen(path);
    const int length = snprintf(path + used, PATH_MAX_BYTES - used, "/%s", name);
    if (length <= 0 || (size_t)length >= PATH_MAX_BYTES - used) { path[used] = 0; errno = ENAMETOOLONG; return -1; }
    return 0;
}

static int join(char *out, const char *folder, const char *name)
{
    const int length = snprintf(out, PATH_MAX_BYTES, "%s", folder);
    if (length <= 0 || length >= PATH_MAX_BYTES) { errno = ENAMETOOLONG; return -1; }
    return append(out, name);
}

/* Move what from holds into to (see the header, step 1). from and to are buffers of PATH_MAX_BYTES, returned as given. */
static int merge(char *from, char *to, PwWineProfileMove *move)
{
    struct stat from_state, to_state;

    if (stat(from, &from_state) != 0) return errno == ENOENT ? 0 : -1;
    if (stat(to, &to_state) != 0) {
        if (errno != ENOENT || rename(from, to) != 0) return -1;
        move->moved++;
        return 0;
    }
    if (!S_ISDIR(from_state.st_mode) || !S_ISDIR(to_state.st_mode)) {
        move->kept++;
        return 0;
    }
    DIR *dir = opendir(from);
    if (!dir) return -1;
    const size_t from_length = strlen(from), to_length = strlen(to);
    int status = 0;
    for (struct dirent *entry; !status && (entry = readdir(dir));) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        status = append(from, entry->d_name) || append(to, entry->d_name) ? -1 : merge(from, to, move);
        from[from_length] = 0;
        to[to_length] = 0;
    }
    const int saved = errno;
    closedir(dir);
    errno = saved;
    /* A folder that still holds what both had stays. */
    if (!status && rmdir(from) != 0 && errno != ENOTEMPTY && errno != EEXIST) status = -1;
    return status;
}

/* Write length bytes of text to path through a synced file renamed over it. */
static int replace_file(const char *path, const char *text, size_t length)
{
    char part[PATH_MAX_BYTES];
    const int name = snprintf(part, sizeof(part), "%s.pw-profile-new", path);
    if (name <= 0 || name >= (int)sizeof(part)) { errno = ENAMETOOLONG; return -1; }
    const int fd = open(part, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    size_t done = 0;
    while (done < length) {
        const ssize_t put = write(fd, text + done, length - done);
        if (put < 0 && errno == EINTR) continue;
        if (put <= 0) break;
        done += (size_t)put;
    }
    int failed = done != length || fsync(fd) != 0;
    const int saved = errno ? errno : EIO;
    if (close(fd) != 0) failed = 1;
    if (failed || rename(part, path) != 0) {
        const int error = failed ? saved : errno;
        unlink(part);
        errno = error;
        return -1;
    }
    return 0;
}

/* Step 2: users/.pw-symlinks names steamuser -> Mihawk, the table's other lines kept. */
static int ensure_link(const char *users)
{
    static const char line[] = PW_WINE_PROFILE_FORMER "\t" PW_WINE_PROFILE_USER "\n";
    char table[PATH_MAX_BYTES], text[TABLE_MAX + 1];
    size_t have = 0;

    if (join(table, users, ".pw-symlinks")) return -1;
    FILE *file = fopen(table, "rb");
    if (file) {
        have = fread(text, 1, TABLE_MAX, file);
        const int unread = ferror(file) || !feof(file);
        fclose(file);
        if (unread) { errno = have == TABLE_MAX ? EFBIG : EIO; return -1; }
    } else if (errno != ENOENT) {
        return -1;
    }
    text[have] = 0;
    for (const char *at = text; at && *at; at = strchr(at, '\n') ? strchr(at, '\n') + 1 : NULL)
        if (!strncmp(at, PW_WINE_PROFILE_FORMER "\t", sizeof(PW_WINE_PROFILE_FORMER "\t") - 1)) return 0;
    if (have && text[have - 1] != '\n') text[have++] = '\n';
    if (have + sizeof(line) - 1 > TABLE_MAX) { errno = EFBIG; return -1; }
    memcpy(text + have, line, sizeof(line) - 1);
    return replace_file(table, text, have + sizeof(line) - 1);
}

/* Step 3 for one registry file: rewritten when it names the former profile; a missing file is none to do. */
static int rewrite_registry(const char *path, PwWineProfileMove *move)
{
    struct stat state;
    if (stat(path, &state) != 0) return errno == ENOENT ? 0 : -1;
    char *text = malloc((size_t)state.st_size + 1);
    if (!text) { errno = ENOMEM; return -1; }
    FILE *file = fopen(path, "rb");
    const size_t got = file ? fread(text, 1, (size_t)state.st_size + 1, file) : 0;
    const int whole = file && !ferror(file) && feof(file);
    const int saved = errno;
    if (file) fclose(file);
    if (!whole) { free(text); errno = file ? EIO : saved; return -1; }
    size_t replaced = 0;
    const size_t length = pw_wine_profile_rewrite(text, got, &replaced);
    int status = 0;
    if (replaced) {
        status = replace_file(path, text, length);
        if (!status) { move->files++; move->replaced += replaced; }
    }
    free(text);
    return status;
}

static int fail(PwWineProfileMove *move, const char *step)
{
    move->step = step;
    return -1;
}

int pw_wine_profile_migrate(const char *prefix, PwWineProfileMove *move)
{
    static const char *const registry[] = {"system.reg", "user.reg", "userdef.reg"};
    char marker[PATH_MAX_BYTES], users[PATH_MAX_BYTES], from[PATH_MAX_BYTES], to[PATH_MAX_BYTES], path[PATH_MAX_BYTES];
    struct stat state;

    if (!move) { errno = EINVAL; return -1; }
    *move = (PwWineProfileMove){0};
    if (!prefix || !*prefix) { errno = EINVAL; return fail(move, "folder"); }
    if (join(marker, prefix, PW_WINE_PROFILE_MARKER) || join(users, prefix, "drive_c/users") ||
        join(from, users, PW_WINE_PROFILE_FORMER) || join(to, users, PW_WINE_PROFILE_USER))
        return fail(move, "folder");
    char have[sizeof(PW_WINE_PROFILE_USER) + 1] = {0};
    FILE *file = fopen(marker, "rb");
    if (file) {
        const size_t got = fread(have, 1, sizeof(have) - 1, file);
        fclose(file);
        if (got == sizeof(PW_WINE_PROFILE_USER) && !memcmp(have, PW_WINE_PROFILE_USER "\n", got)) {
            move->already = 1;
            return 0;
        }
    }
    if (join(path, prefix, "system.reg")) return fail(move, "registry");
    const int existing = stat(path, &state) == 0;
    if (merge(from, to, move)) return fail(move, "folder");
    if (existing && stat(from, &state) != 0) {
        if (errno != ENOENT || ensure_link(users)) return fail(move, "link");
        move->linked = 1;
    }
    for (size_t i = 0; i < sizeof(registry) / sizeof(registry[0]); i++)
        if (join(path, prefix, registry[i]) || rewrite_registry(path, move)) return fail(move, "registry");
    if (replace_file(marker, PW_WINE_PROFILE_USER "\n", sizeof(PW_WINE_PROFILE_USER))) return fail(move, "marker");
    return 0;
}
