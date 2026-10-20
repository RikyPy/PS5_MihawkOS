/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pw_wine_launch.h"
#include "pw_wine_profile.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *value_of(const char *word, const char *key)
{
    size_t length = strlen(key);

    return !strncmp(word, key, length) && word[length] == '=' ? word + length + 1 : NULL;
}

int pw_wine_launch_prefix_id(const char *requested, const char *game_id, char *out, size_t size)
{
    const char *id = requested && *requested ? requested : !game_id || !*game_id ? "probe" : "mihawk";
    size_t length = strlen(id);

    if (!out || !size) return -1;
    out[0] = 0;
    if (!length || length > 64 || length >= size) return -1;
    for (const char *p = id; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) return -1;
    memcpy(out, id, length + 1);
    return 0;
}

/* An absolute Windows path: a drive letter, ':' and '\'. */
static int valid_path(const char *path)
{
    size_t length = strlen(path);

    return length >= 3 && length < PW_WINE_LAUNCH_PATH_MAX &&
           ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
           path[1] == ':' && path[2] == '\\';
}

static int parse_count(const char *text, uint32_t *out)
{
    uint64_t value = 0;

    if (!*text) return -1;
    for (; *text; text++) {
        if (*text < '0' || *text > '9') return -1;
        if ((value = value * 10u + (uint64_t)(*text - '0')) > UINT32_MAX) return -1;
    }
    *out = (uint32_t)value;
    return 0;
}

int pw_wine_launch_parse(int argc, char *const *argv, const PwWineApp *apps, size_t count,
                         PwWineLaunch *out)
{
    const char *profile = NULL, *path = NULL;

    if (!out || (!apps && count)) return -1;
    memset(out, 0, sizeof(*out));
    out->mode = PW_WINE_LAUNCH_LAUNCHER;
    for (int i = 0; argv && i < argc; i++) {
        const char *word = argv[i], *value;
        if (!word) continue;
        if ((value = value_of(word, "profile"))) profile = value;
        else if ((value = value_of(word, "path"))) path = value;
        else if ((value = value_of(word, "cycle")) && parse_count(value, &out->cycle)) out->cycle = 0;
    }
    if (profile) {
        for (size_t i = 0; i < count && !out->app; i++)
            if (apps[i].id && !strcmp(apps[i].id, profile)) out->app = &apps[i];
        if (!out->app) { out->refused = 1; return 0; }
        if (!path) path = out->app->executable;
    }
    if (!path) return 0;
    if (!valid_path(path)) { out->app = NULL; out->refused = 1; return 0; }
    memcpy(out->executable, path, strlen(path) + 1);
    out->mode = PW_WINE_LAUNCH_GAME;
    return 0;
}

/* Append "key=value" to storage at *used; 0, or -1 when it does not fit. */
static int add_word(char *storage, size_t size, size_t *used, const char *key, const char *value)
{
    size_t key_length = strlen(key), value_length = strlen(value);

    if (*used + key_length + 1 + value_length + 1 > size) return -1;
    memcpy(storage + *used, key, key_length);
    storage[*used + key_length] = '=';
    memcpy(storage + *used + key_length + 1, value, value_length + 1);
    *used += key_length + 1 + value_length + 1;
    return 0;
}

size_t pw_wine_launch_argv(const PwWineApp *app, uint32_t cycle, char *storage, size_t size,
                           char **argv, size_t max)
{
    char digits[11], number[11];
    size_t used = 0, words = 0, n = 0;

    if (!storage || !argv || max < PW_WINE_LAUNCH_ARGS) return 0;
    do digits[n++] = (char)('0' + cycle % 10u); while ((cycle /= 10u) && n < sizeof(digits));
    for (size_t i = 0; i < n; i++) number[i] = digits[n - 1 - i];
    number[n] = 0;
    if (app) {
        if (!app->id || !app->executable) return 0;
        argv[words++] = storage + used;
        if (add_word(storage, size, &used, "profile", app->id)) return 0;
        argv[words++] = storage + used;
        if (add_word(storage, size, &used, "path", app->executable)) return 0;
    } else {
        argv[words++] = storage + used;
        if (add_word(storage, size, &used, "launcher", "1")) return 0;
    }
    argv[words++] = storage + used;
    if (add_word(storage, size, &used, "cycle", number)) return 0;
    argv[words] = NULL;
    return words;
}

int pw_wine_launch_split(const char *text, char *storage, size_t size, const char **words,
                         size_t max)
{
    size_t used = 0;
    int count = 0;

    if (!text || !storage || !words) return -1;
    for (;;) {
        int quoted = 0;

        while (*text == ' ' || *text == '\t') text++;
        if (!*text) return count;
        if ((size_t)count >= max) return -1;
        words[count++] = storage + used;
        for (; *text && (quoted || (*text != ' ' && *text != '\t')); text++) {
            if (*text == '"') { quoted = !quoted; continue; }
            if (used + 1 >= size) return -1;
            storage[used++] = *text;
        }
        if (quoted || used >= size) return -1;
        storage[used++] = 0;
    }
}

int pw_wine_launch_host_dir(const char *prefix, const char *windows_path, char *out, size_t size)
{
    const char *base;
    size_t used;

    if (!prefix || !windows_path || !out || !size || strlen(windows_path) < 3 ||
        windows_path[1] != ':' || windows_path[2] != '\\')
        return -1;
    if (windows_path[0] == 'C' || windows_path[0] == 'c') base = "/drive_c";
    else if (windows_path[0] == 'Z' || windows_path[0] == 'z') base = prefix = "";
    else return -1;
    if (strlen(prefix) + strlen(base) >= size) return -1;
    memcpy(out, prefix, strlen(prefix));
    memcpy(out + strlen(prefix), base, strlen(base));
    used = strlen(prefix) + strlen(base);
    for (const char *p = windows_path + 2; *p; p++) {
        if (used + 1 >= size) return -1;
        out[used++] = *p == '\\' ? '/' : *p;
    }
    /* No trailing separator, except for the root itself. */
    while (used > 1 && out[used - 1] == '/') used--;
    out[used] = 0;
    return 0;
}

/* tools/deploy-wine.py makes the same folders when it deploys a prefix. */
#define PROFILE "users/" PW_WINE_PROFILE_USER
static const char *const profile_dirs[] = {
    "users", PROFILE, PROFILE "/AppData", PROFILE "/AppData/Local", PROFILE "/AppData/Local/Temp",
    PROFILE "/AppData/Local/Microsoft", PROFILE "/AppData/Local/Microsoft/Windows",
    PROFILE "/AppData/Local/Microsoft/Windows/INetCache", PROFILE "/AppData/LocalLow", PROFILE "/AppData/Roaming",
    PROFILE "/Desktop", PROFILE "/Documents", PROFILE "/Downloads", PROFILE "/Music", PROFILE "/Pictures",
    PROFILE "/Videos", PROFILE "/Saved Games", "users/Public", "users/Public/Desktop",
    "users/Public/Documents", "users/Public/Downloads", "users/Public/Music", "users/Public/Pictures",
    "users/Public/Videos",
};
#undef PROFILE

int pw_wine_launch_profile_dirs(const char *prefix, int (*make)(const char *path, void *context), void *context)
{
    char path[PW_WINE_LAUNCH_PATH_MAX];
    int failed = 0;

    if (!prefix || !make) return -1;
    for (size_t i = 0; i < sizeof(profile_dirs) / sizeof(profile_dirs[0]); i++) {
        const int length = snprintf(path, sizeof(path), "%s/drive_c/%s", prefix, profile_dirs[i]);
        if (length < 0 || (size_t)length >= sizeof(path)) return -1;
        if (make(path, context)) failed++;
    }
    return failed;
}

static int sync_path(char *out, size_t size, const char *folder, const char *name)
{
    const int length = snprintf(out, size, "%s/%s", folder, name);
    if (length <= 0 || (size_t)length >= size) { errno = ENAMETOOLONG; return -1; }
    return 0;
}

/* The whole of a small file (a stamp) into out, NUL-terminated; -1 when it cannot be read or does not fit. */
static int sync_read_stamp(const char *path, char *out, size_t size)
{
    FILE *file = fopen(path, "rb");
    if (!file) return -1;
    const size_t got = fread(out, 1, size - 1, file);
    const int whole = feof(file) && !ferror(file);
    fclose(file);
    out[got] = 0;
    return whole ? 0 : -1;
}

static int sync_copy(const char *from, const char *to)
{
    char temporary[1024], buffer[65536];
    const int length = snprintf(temporary, sizeof(temporary), "%s.proton-new", to);
    if (length <= 0 || (size_t)length >= sizeof(temporary)) { errno = ENAMETOOLONG; return -1; }
    const int source = open(from, O_RDONLY);
    if (source < 0) return -1;
    const int target = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (target < 0) { const int saved = errno; close(source); errno = saved; return -1; }
    ssize_t got;
    int failed = 0;
    while (!failed && (got = read(source, buffer, sizeof(buffer))) != 0) {
        if (got < 0) { if (errno == EINTR) continue; failed = 1; break; }
        for (ssize_t done = 0; done < got;) {
            const ssize_t put = write(target, buffer + done, (size_t)(got - done));
            if (put < 0 && errno == EINTR) continue;
            if (put <= 0) { failed = 1; break; }
            done += put;
        }
    }
    const int saved = errno ? errno : EIO;
    close(source);
    if (close(target) != 0) failed = 1;
    if (failed || rename(temporary, to) != 0) {
        unlink(temporary);
        errno = saved;
        return -1;
    }
    return 0;
}

int pw_wine_launch_sync(const char *from, const char *to, const char *const *names, size_t count, const char *stamp)
{
    char source[1024], target[1024], have[256], want[256];
    struct stat source_state, target_state;
    if (!from || !to || !names || !stamp) { errno = EINVAL; return -1; }
    if (stat(to, &target_state) != 0 || !S_ISDIR(target_state.st_mode)) { if (!errno) errno = ENOTDIR; return -1; }
    if (sync_path(source, sizeof(source), from, stamp) || sync_read_stamp(source, want, sizeof(want))) return -1;
    int current = sync_path(target, sizeof(target), to, stamp) == 0 &&
                  sync_read_stamp(target, have, sizeof(have)) == 0 && !strcmp(have, want);
    for (size_t i = 0; current && i < count; i++) {
        if (sync_path(source, sizeof(source), from, names[i]) || sync_path(target, sizeof(target), to, names[i]))
            return -1;
        current = stat(source, &source_state) == 0 && stat(target, &target_state) == 0 &&
                  source_state.st_size == target_state.st_size;
    }
    if (current) return 0;
    for (size_t i = 0; i < count; i++) {
        if (sync_path(source, sizeof(source), from, names[i]) || sync_path(target, sizeof(target), to, names[i]) ||
            sync_copy(source, target))
            return -1;
    }
    if (sync_path(source, sizeof(source), from, stamp) || sync_path(target, sizeof(target), to, stamp) ||
        sync_copy(source, target))
        return -1;
    return (int)count + 1;
}
