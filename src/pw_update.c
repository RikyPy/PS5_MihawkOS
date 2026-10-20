/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pw_update.h"
#include "pw_pack.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PATH 1200

static int join(char *out, const char *base, const char *relative)
{
    return snprintf(out, PATH, "%s/%s", base, relative) < PATH;
}

static int fail(char *failed, size_t size, int error, const char *path)
{
    if (failed && size) snprintf(failed, size, "%s", path);
    return -(error ? error : EIO);
}

static char *read_all(const char *path, size_t *size)
{
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    char *text = NULL;
    size_t used = 0, room = 0, got;
    char buffer[16384];
    while ((got = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        if (used + got > room) {
            char *bigger = realloc(text, room = (used + got) * 2);
            if (!bigger) {
                free(text);
                fclose(file);
                return NULL;
            }
            text = bigger;
        }
        memcpy(text + used, buffer, got);
        used += got;
    }
    fclose(file);
    *size = used;
    return text ? text : calloc(1, 1);
}

/* The folders on the way to `path`. */
static int make_parents(char *path)
{
    for (char *slash = strchr(path + 1, '/'); slash; slash = strchr(slash + 1, '/')) {
        *slash = '\0';
        const int made = mkdir(path, 0777) == 0 || errno == EEXIST;
        *slash = '/';
        if (!made) return 0;
    }
    return 1;
}

/* title/<relative> into previous/, when it is there. */
static int move_aside(const char *title, const char *previous, const char *relative, char *failed, size_t size)
{
    char from[PATH], to[PATH];
    struct stat st;
    if (!join(from, title, relative) || !join(to, previous, relative)) return fail(failed, size, ENAMETOOLONG, relative);
    if (lstat(from, &st) != 0) return errno == ENOENT ? 0 : fail(failed, size, errno, from);
    if (S_ISDIR(st.st_mode)) return fail(failed, size, EISDIR, from);
    if (!make_parents(to) || rename(from, to) != 0) return fail(failed, size, errno, from);
    return 0;
}

/* stage/<relative> into the title folder, the file it replaces into previous/. Done already when it is not in the stage. */
static int move_in(const char *stage, const char *title, const char *previous, const char *relative, char *failed,
                   size_t size)
{
    char from[PATH], to[PATH];
    struct stat st;
    if (!join(from, stage, relative) || !join(to, title, relative)) return fail(failed, size, ENAMETOOLONG, relative);
    if (lstat(from, &st) != 0) return errno == ENOENT ? 0 : fail(failed, size, errno, from);
    const int aside = move_aside(title, previous, relative, failed, size);
    if (aside) return aside;
    if (!make_parents(to) || rename(from, to) != 0) return fail(failed, size, errno, to);
    return 0;
}

static int listed(const PwPackManifest *manifest, const char *path)
{
    for (size_t f = 0; f < manifest->file_count; f++)
        if (!strcmp(manifest->files[f].path, path)) return 1;
    return !strcmp(path, PW_UPDATE_RECORD);
}

/* A folder and everything in it, never through a link. */
static int remove_tree(const char *path)
{
    struct stat st;
    if (lstat(path, &st) != 0) return errno == ENOENT ? 0 : -errno;
    if (!S_ISDIR(st.st_mode)) return unlink(path) == 0 || errno == ENOENT ? 0 : -errno;
    DIR *dir = opendir(path);
    if (!dir) return -errno;
    int status = 0;
    char child[PATH];
    for (struct dirent *entry; !status && (entry = readdir(dir));) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        status = join(child, path, entry->d_name) ? remove_tree(child) : -ENAMETOOLONG;
    }
    closedir(dir);
    return status ? status : rmdir(path) == 0 ? 0 : -errno;
}

static int write_new(const char *path, const char *text)
{
    char part[PATH];
    if (snprintf(part, sizeof(part), "%s.mihawkos-part", path) >= (int)sizeof(part)) return -ENAMETOOLONG;
    const int fd = open(part, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0644);
    if (fd < 0) return -errno;
    const size_t length = strlen(text);
    int error = write(fd, text, length) == (ssize_t)length ? 0 : errno ? errno : EIO;
    if (!error && fsync(fd) != 0) error = errno;
    if (close(fd) != 0 && !error) error = errno;
    if (!error && rename(part, path) != 0) error = errno;
    if (error) unlink(part);
    return -error;
}

int pw_update_mark_ready(const char *update, const char *version)
{
    char path[PATH], text[256];
    if (!join(path, update, "ready") || snprintf(text, sizeof(text), "%s\n", version) >= (int)sizeof(text))
        return -ENAMETOOLONG;
    return write_new(path, text);
}

int pw_update_pending(const char *update, char *version, size_t size)
{
    static const char *const names[] = { "ready", "switching" };
    for (size_t i = 0; i < 2; i++) {
        char path[PATH];
        size_t length = 0;
        char *text = join(path, update, names[i]) ? read_all(path, &length) : NULL;
        if (!text) continue;
        snprintf(version, size, "%.*s", (int)strcspn(text, "\r\n"), text);
        free(text);
        return 1;
    }
    return 0;
}

int pw_update_switch(const char *update, const char *title, char *failed, size_t size)
{
    char ready[PATH], switching[PATH], switched[PATH], stage[PATH], previous[PATH], stage_record[PATH], title_record[PATH];
    if (failed && size) *failed = '\0';
    if (!join(ready, update, "ready") || !join(switching, update, "switching") || !join(switched, update, "switched") ||
        !join(stage, update, "stage") || !join(previous, update, "previous") ||
        !join(stage_record, stage, PW_UPDATE_RECORD) || !join(title_record, title, PW_UPDATE_RECORD))
        return fail(failed, size, ENAMETOOLONG, update);
    struct stat st;
    if (rename(ready, switching) != 0 && errno != ENOENT) return fail(failed, size, errno, ready);
    if (lstat(switching, &st) != 0) return 0;
    /* The new manifest: in the stage until the switch moves it, in the title folder after. */
    const int record_moved = lstat(stage_record, &st) != 0;
    size_t length = 0;
    char *text = read_all(record_moved ? title_record : stage_record, &length);
    PwPackManifest manifest;
    if (!text || pw_pack_parse(text, length, &manifest) != PW_PACK_OK || !manifest.title) {
        free(text);
        return fail(failed, size, EINVAL, record_moved ? title_record : stage_record);
    }
    free(text);
    int status = mkdir(previous, 0777) == 0 || errno == EEXIST ? 0 : fail(failed, size, errno, previous);
    if (!record_moved) {
        for (size_t f = 0; !status && f < manifest.stamp; f++)
            status = move_in(stage, title, previous, manifest.files[f].path, failed, size);
        /* What the old title had and the new one has not: aside, by the old record (none: a folder copied by hand,
         * whose files are all kept). */
        PwPackManifest old;
        char *old_text = status ? NULL : read_all(title_record, &length);
        if (old_text && pw_pack_parse(old_text, length, &old) == PW_PACK_OK) {
            for (size_t f = 0; !status && f < old.file_count; f++)
                if (!listed(&manifest, old.files[f].path))
                    status = move_aside(title, previous, old.files[f].path, failed, size);
            pw_pack_manifest_free(&old);
        }
        free(old_text);
        if (!status) status = move_in(stage, title, previous, PW_UPDATE_RECORD, failed, size);
    }
    for (size_t f = manifest.stamp; !status && f < manifest.file_count; f++)
        status = move_in(stage, title, previous, manifest.files[f].path, failed, size);
    pw_pack_manifest_free(&manifest);
    if (status) return status;
    if (rename(switching, switched) != 0) return fail(failed, size, errno, switching);
    remove_tree(stage);   /* only empty folders are left in it, or files of an older stage no manifest names */
    return 1;
}

int pw_update_confirm(const char *update)
{
    char switched[PATH], previous[PATH];
    struct stat st;
    if (!join(switched, update, "switched") || !join(previous, update, "previous")) return -ENAMETOOLONG;
    if (lstat(switched, &st) != 0) return 0;
    const int status = remove_tree(previous);
    if (status) return status;
    return unlink(switched) == 0 ? 1 : -errno;
}
