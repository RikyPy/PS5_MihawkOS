/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Windows shortcuts written by the title: the contract, and what Wine and Explorer read, is in pw_shell_link.h. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "pw_shell_link.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

enum {
    PW_SHELL_LINK_FILE_ATTRIBUTE_NORMAL = 0x80,
    PW_SHELL_LINK_SW_SHOWNORMAL = 1,
    PW_SHELL_LINK_DRIVE_FIXED = 3,
    PW_SHELL_LINK_VOLUME_ID_AND_LOCAL_BASE_PATH = 1,
    PW_SHELL_LINK_PATH_MAX = 1024,  /* a folder or manifest path and a file name; FreeBSD's PATH_MAX */
};

/* CLSID_ShellLink, 00021401-0000-0000-C000-000000000046, as stored: Data1-3 little-endian, Data4 as bytes. */
static const unsigned char pw_shell_link_clsid[16] = {
    0x01, 0x14, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46,
};
static const char pw_shell_link_device_suffix[] = " (game)";
static const char pw_shell_link_temporary[] = ".pw-shell-link.tmp";

static int pw_shell_link_present(const char *text)
{
    return text && *text;
}

static unsigned char pw_shell_link_lower(unsigned char c)
{
    return c >= 'A' && c <= 'Z' ? (unsigned char)(c - 'A' + 'a') : c;
}

/* Equal ignoring ASCII case, the first n bytes of a and the whole of b. */
static int pw_shell_link_equal_nocase(const char *a, size_t n, const char *b)
{
    size_t i = 0;
    for (; i < n && b[i]; ++i)
        if (pw_shell_link_lower((unsigned char)a[i]) != pw_shell_link_lower((unsigned char)b[i])) return 0;
    return i == n && !b[i];
}

/* One code point of UTF-8; the bytes it took, or 0 when invalid (cut, overlong, a surrogate, over U+10FFFF). */
static size_t pw_shell_link_utf8(const unsigned char *s, uint32_t *code)
{
    uint32_t value, least;
    size_t size;
    if (s[0] < 0x80) {
        *code = s[0];
        return 1;
    }
    if ((s[0] & 0xE0) == 0xC0) size = 2, value = s[0] & 0x1Fu, least = 0x80;
    else if ((s[0] & 0xF0) == 0xE0) size = 3, value = s[0] & 0x0Fu, least = 0x800;
    else if ((s[0] & 0xF8) == 0xF0) size = 4, value = s[0] & 0x07u, least = 0x10000;
    else return 0;
    for (size_t i = 1; i < size; ++i) {
        if ((s[i] & 0xC0) != 0x80) return 0;  /* also the string's NUL: a cut sequence */
        value = value << 6 | (s[i] & 0x3Fu);
    }
    if (value < least || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) return 0;
    *code = value;
    return size;
}

/* The UTF-16 units of text, or EILSEQ. */
static int pw_shell_link_units(const char *text, size_t *units)
{
    size_t count = 0;
    for (const unsigned char *s = (const unsigned char *)text; *s;) {
        uint32_t code;
        size_t used = pw_shell_link_utf8(s, &code);
        if (!used) return EILSEQ;
        count += code >= 0x10000 ? 2 : 1;
        s += used;
    }
    *units = count;
    return 0;
}

static unsigned char *pw_shell_link_put16(unsigned char *p, uint32_t value)
{
    p[0] = (unsigned char)value;
    p[1] = (unsigned char)(value >> 8);
    return p + 2;
}

static unsigned char *pw_shell_link_put32(unsigned char *p, uint32_t value)
{
    p = pw_shell_link_put16(p, value & 0xFFFFu);
    return pw_shell_link_put16(p, value >> 16);
}

/* A StringData string (shelllink.c:487): the unit count, then UTF-16LE, no terminator. text is valid UTF-8. */
static unsigned char *pw_shell_link_put_string(unsigned char *p, const char *text, size_t units)
{
    p = pw_shell_link_put16(p, (uint32_t)units);
    for (const unsigned char *s = (const unsigned char *)text; *s;) {
        uint32_t code = 0;
        s += pw_shell_link_utf8(s, &code);
        if (code >= 0x10000) {
            code -= 0x10000;
            p = pw_shell_link_put16(p, 0xD800u | code >> 10);
            p = pw_shell_link_put16(p, 0xDC00u | (code & 0x3FFu));
        } else {
            p = pw_shell_link_put16(p, code);
        }
    }
    return p;
}

int pw_shell_link_build(const PwShellLink *link, unsigned char *out, size_t capacity, size_t *bytes)
{
    if (!link || !bytes || (capacity && !out)) return EINVAL;
    const char *target = link->target;
    if (!pw_shell_link_present(target)) return EINVAL;
    size_t target_bytes = strlen(target);
    for (size_t i = 0; i < target_bytes; ++i)
        if ((unsigned char)target[i] >= 0x80) return EINVAL;  /* LocalBasePath is read as CP_ACP (shelllink.c:609) */
    unsigned char drive = pw_shell_link_lower((unsigned char)target[0]);
    if (drive < 'a' || drive > 'z' || target[1] != ':' || (target[2] != '\\' && target[2] != '/')) return EINVAL;
    if (target_bytes > PW_SHELL_LINK_UNITS_MAX) return ENAMETOOLONG;

    /* StringData in the order Wine reads it (shelllink.c:798-835); RELATIVE_PATH is never written. */
    const char *strings[4] = {
        link->description,
        link->working_directory,
        link->arguments,
        pw_shell_link_present(link->icon_path) ? link->icon_path : target,
    };
    static const uint32_t string_flags[4] = {
        PW_SHELL_LINK_HAS_NAME,
        PW_SHELL_LINK_HAS_WORKING_DIR,
        PW_SHELL_LINK_HAS_ARGUMENTS,
        PW_SHELL_LINK_HAS_ICON_LOCATION,
    };
    size_t units[4] = {0};
    uint32_t flags = PW_SHELL_LINK_HAS_LINK_INFO | PW_SHELL_LINK_IS_UNICODE;
    size_t total = PW_SHELL_LINK_HEADER_BYTES;
    size_t info = PW_SHELL_LINK_INFO_HEADER_BYTES + PW_SHELL_LINK_VOLUME_BYTES + target_bytes + 1 + 1;
    total += info;
    for (int i = 0; i < 4; ++i) {
        if (!pw_shell_link_present(strings[i])) continue;
        int error = pw_shell_link_units(strings[i], &units[i]);
        if (error) return error;
        if (units[i] > PW_SHELL_LINK_UNITS_MAX) return ENAMETOOLONG;
        flags |= string_flags[i];
        total += 2 + 2 * units[i];
    }
    if (units[3] >= PW_SHELL_LINK_ICON_UNITS_MAX) return ENAMETOOLONG;  /* Explorer's MAX_PATH (desktop.c:324) */
    total += 4;
    if (capacity < total) {
        *bytes = total;
        return ENOSPC;
    }

    memset(out, 0, total);
    unsigned char *p = out;
    /* ShellLinkHeader: times, file size, hot key and the reserved fields stay zero. */
    pw_shell_link_put32(p, PW_SHELL_LINK_HEADER_BYTES);
    memcpy(p + 0x04, pw_shell_link_clsid, sizeof pw_shell_link_clsid);
    pw_shell_link_put32(p + 0x14, flags);
    pw_shell_link_put32(p + 0x18, PW_SHELL_LINK_FILE_ATTRIBUTE_NORMAL);
    pw_shell_link_put32(p + 0x38, (uint32_t)link->icon_index);
    pw_shell_link_put32(p + 0x3C, PW_SHELL_LINK_SW_SHOWNORMAL);
    p += PW_SHELL_LINK_HEADER_BYTES;

    /* LinkInfo, as Wine's Stream_WriteLocationInfo lays it out (shelllink.c:908). */
    uint32_t volume = PW_SHELL_LINK_INFO_HEADER_BYTES;
    uint32_t base_path = volume + PW_SHELL_LINK_VOLUME_BYTES;
    uint32_t suffix = base_path + (uint32_t)target_bytes + 1;
    pw_shell_link_put32(p + 0x00, (uint32_t)info);
    pw_shell_link_put32(p + 0x04, PW_SHELL_LINK_INFO_HEADER_BYTES);
    pw_shell_link_put32(p + 0x08, PW_SHELL_LINK_VOLUME_ID_AND_LOCAL_BASE_PATH);
    pw_shell_link_put32(p + 0x0C, volume);
    pw_shell_link_put32(p + 0x10, base_path);
    pw_shell_link_put32(p + 0x14, 0);  /* CommonNetworkRelativeLinkOffset */
    pw_shell_link_put32(p + 0x18, suffix);
    pw_shell_link_put32(p + volume + 0x0, PW_SHELL_LINK_VOLUME_BYTES);
    pw_shell_link_put32(p + volume + 0x4, PW_SHELL_LINK_DRIVE_FIXED);
    pw_shell_link_put32(p + volume + 0x8, 0);    /* DriveSerialNumber */
    pw_shell_link_put32(p + volume + 0xC, 0x10); /* VolumeLabelOffset: the empty label's NUL, at 0x10 */
    memcpy(p + base_path, target, target_bytes); /* its NUL, and the empty CommonPathSuffix, are the zeroed bytes */
    p += info;

    for (int i = 0; i < 4; ++i)
        if (flags & string_flags[i]) p = pw_shell_link_put_string(p, strings[i], units[i]);
    p += 4;  /* TerminalBlock, zero */
    *bytes = (size_t)(p - out);
    return 0;
}

/* Whether Wine takes the n bytes s for a device (ntdll/path.c:66 RtlIsDosDeviceName_U). */
static int pw_shell_link_device(const char *s, size_t n)
{
    static const char *const three[] = {"AUX", "CON", "NUL", "PRN"};
    if (n == 3)
        for (size_t i = 0; i < sizeof three / sizeof three[0]; ++i)
            if (pw_shell_link_equal_nocase(s, 3, three[i])) return 1;
    if (n == 4 && (pw_shell_link_equal_nocase(s, 3, "COM") || pw_shell_link_equal_nocase(s, 3, "LPT")))
        return s[3] >= '1' && s[3] <= '9';
    return (n == 6 && pw_shell_link_equal_nocase(s, 6, "CONIN$")) ||
           (n == 7 && pw_shell_link_equal_nocase(s, 7, "CONOUT$"));
}

static int pw_shell_link_dropped(unsigned char c)
{
    switch (c) {
    case '<': case '>': case ':': case '"': case '/': case '\\': case '|': case '?': case '*': return 1;
    default: return c < 0x20;
    }
}

int pw_shell_link_file_name(const char *display_name, char *out, size_t capacity)
{
    if (!display_name || (capacity && !out)) return EINVAL;
    for (const unsigned char *s = (const unsigned char *)display_name; *s;) {
        uint32_t code;
        size_t used = pw_shell_link_utf8(s, &code);
        if (!used) return EILSEQ;
        s += used;
    }
    /* The kept bytes; length counts them all, kept is what fits, and the trimmed length is what counts. */
    char kept[PW_SHELL_LINK_NAME_MAX + 1];
    size_t length = 0, trimmed = 0;
    for (const unsigned char *s = (const unsigned char *)display_name; *s; ++s) {
        if (pw_shell_link_dropped(*s)) continue;
        if (length < sizeof kept) kept[length] = (char)*s;
        ++length;
        if (*s != '.' && *s != ' ') trimmed = length;
    }
    if (!trimmed) return EINVAL;
    if (trimmed > PW_SHELL_LINK_NAME_MAX) return ENAMETOOLONG;
    size_t stem = 0;
    while (stem < trimmed && kept[stem] != '.') ++stem;
    while (stem && kept[stem - 1] == ' ') --stem;
    size_t insert = pw_shell_link_device(kept, stem) ? sizeof pw_shell_link_device_suffix - 1 : 0;
    size_t total = trimmed + insert + 4;
    if (total > PW_SHELL_LINK_NAME_MAX) return ENAMETOOLONG;
    if (total >= capacity) return ENOSPC;
    memcpy(out, kept, stem);
    memcpy(out + stem, pw_shell_link_device_suffix, insert);
    memcpy(out + stem + insert, kept + stem, trimmed - stem);
    memcpy(out + trimmed + insert, ".lnk", 5);
    return 0;
}

/* --- pw_shell_link_sync --- */

static void pw_shell_link_note(PwShellLinkSyncResult *result, int error, const char *step, long entry)
{
    result->failed++;
    if (result->first_error) return;
    result->first_error = error ? error : EIO;
    result->first_error_step = step;
    result->first_error_entry = entry;
}

/* "<a><separator><b>" into out (PW_SHELL_LINK_PATH_MAX bytes), or ENAMETOOLONG. */
static int pw_shell_link_join(char *out, const char *a, const char *separator, const char *b)
{
    size_t na = strlen(a), ns = strlen(separator), nb = strlen(b);
    if (na + ns + nb >= PW_SHELL_LINK_PATH_MAX) return ENAMETOOLONG;
    memcpy(out, a, na);
    memcpy(out + na, separator, ns);
    memcpy(out + na + ns, b, nb + 1);
    return 0;
}

static int pw_shell_link_in_folder(char *out, const char *folder, const char *name)
{
    size_t n = strlen(folder);
    return pw_shell_link_join(out, folder, n && folder[n - 1] == '/' ? "" : "/", name);
}

static int pw_shell_link_write_all(int fd, const void *data, size_t size)
{
    const unsigned char *p = data;
    while (size) {
        ssize_t done = write(fd, p, size);
        if (done < 0) {
            if (errno == EINTR) continue;
            return errno;
        }
        if (!done) return EIO;
        p += done;
        size -= (size_t)done;
    }
    return 0;
}

static int pw_shell_link_write_line(int fd, const char *name)
{
    int error = pw_shell_link_write_all(fd, name, strlen(name));
    return error ? error : pw_shell_link_write_all(fd, "\n", 1);
}

/* Visits each line of the file at path (without its '\n') until visit returns non-zero. Empty lines, lines over
 * PW_SHELL_LINK_NAME_MAX bytes and lines holding a NUL are skipped. A missing file has no lines. */
typedef int (*PwShellLinkVisit)(void *context, const char *line);

static int pw_shell_link_line_end(char *line, size_t *length, int *skip, PwShellLinkVisit visit, void *context)
{
    line[*length] = 0;
    int stop = *length && !*skip && visit(context, line);
    *length = 0;
    *skip = 0;
    return stop;
}

static int pw_shell_link_lines(const char *path, PwShellLinkVisit visit, void *context)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT ? 0 : errno;
    char chunk[512], line[PW_SHELL_LINK_NAME_MAX + 1];
    size_t length = 0;
    int skip = 0, error = 0;
    for (;;) {
        ssize_t got = read(fd, chunk, sizeof chunk);
        if (got < 0) {
            if (errno == EINTR) continue;
            error = errno;
            break;
        }
        if (!got) {
            pw_shell_link_line_end(line, &length, &skip, visit, context);  /* a last line without '\n' */
            break;
        }
        for (ssize_t i = 0; i < got; ++i) {
            if (chunk[i] == '\n') {
                if (pw_shell_link_line_end(line, &length, &skip, visit, context)) goto done;
            } else if (!chunk[i] || length == PW_SHELL_LINK_NAME_MAX) {
                skip = 1;
            } else {
                line[length++] = chunk[i];
            }
        }
    }
done:
    close(fd);
    return error;
}

/* A manifest line this module could have written: "<something>.lnk" with no byte pw_shell_link_file_name drops (no
 * separator, no control byte). Anything else there (a hand edit, "../x.lnk") is never acted on. */
static int pw_shell_link_owned_name(const char *name)
{
    size_t n = strlen(name);
    if (n < 5 || n > PW_SHELL_LINK_NAME_MAX || !pw_shell_link_equal_nocase(name + n - 4, 4, ".lnk")) return 0;
    for (size_t i = 0; i < n; ++i)
        if (pw_shell_link_dropped((unsigned char)name[i])) return 0;
    return 1;
}

/* One search of a manifest. By name: whether it lists name exactly, and its first spelling ignoring ASCII case.
 * By file (same set): its first owned-name line that is, in folder, the file same describes (another spelling of
 * it, on a file system that folds case or normalization). */
typedef struct PwShellLinkFind {
    const char *name;
    const char *folder;
    const struct stat *same;
    int exact;
    int found;
    char match[PW_SHELL_LINK_NAME_MAX + 1];  /* the listed spelling */
} PwShellLinkFind;

static int pw_shell_link_find_visit(void *context, const char *line)
{
    PwShellLinkFind *find = context;
    size_t n = strlen(line);
    if (find->same) {
        char path[PW_SHELL_LINK_PATH_MAX];
        struct stat info;
        if (!pw_shell_link_owned_name(line) || pw_shell_link_in_folder(path, find->folder, line) ||
            lstat(path, &info) || info.st_dev != find->same->st_dev || info.st_ino != find->same->st_ino)
            return 0;
    } else if (!strcmp(line, find->name)) {
        find->exact = 1;
    } else if (find->found || !pw_shell_link_equal_nocase(line, n, find->name)) {
        return 0;
    }
    if (!find->found) memcpy(find->match, line, n + 1);
    find->found = 1;
    return find->same || find->exact;
}

static void pw_shell_link_find_reset(PwShellLinkFind *find)
{
    memset(find, 0, sizeof *find);
}

/* Whether the manifest at path lists name: exact, and found with the first spelling ignoring ASCII case. */
static int pw_shell_link_find(const char *path, const char *name, PwShellLinkFind *find)
{
    pw_shell_link_find_reset(find);
    find->name = name;
    return pw_shell_link_lines(path, pw_shell_link_find_visit, find);
}

/* Whether the manifest at path lists, in folder, the file info describes, under any spelling. */
static int pw_shell_link_find_file(const char *path, const char *folder, const struct stat *info,
                                   PwShellLinkFind *find)
{
    pw_shell_link_find_reset(find);
    find->folder = folder;
    find->same = info;
    return pw_shell_link_lines(path, pw_shell_link_find_visit, find);
}

enum { PW_SHELL_LINK_ABSENT, PW_SHELL_LINK_SAME, PW_SHELL_LINK_DIFFERENT, PW_SHELL_LINK_OTHER };

/* What is at path: nothing, a regular file holding exactly these bytes or other bytes, or something else; info
 * is its lstat. */
static int pw_shell_link_compare(const char *path, const unsigned char *bytes, size_t size, int *state,
                                 struct stat *info_out)
{
    struct stat info;
    if (lstat(path, &info)) {
        if (errno != ENOENT) return errno;
        *state = PW_SHELL_LINK_ABSENT;
        return 0;
    }
    *info_out = info;
    if (!S_ISREG(info.st_mode)) *state = PW_SHELL_LINK_OTHER;
    else if (info.st_size < 0 || (unsigned long long)info.st_size != size) *state = PW_SHELL_LINK_DIFFERENT;
    else *state = PW_SHELL_LINK_SAME;
    if (*state != PW_SHELL_LINK_SAME) return 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return errno;
    unsigned char chunk[1024];
    size_t at = 0;
    int error = 0;
    for (;;) {
        ssize_t got = read(fd, chunk, sizeof chunk);
        if (got < 0) {
            if (errno == EINTR) continue;
            error = errno;
            break;
        }
        if (!got) break;
        if ((size_t)got > size - at || memcmp(chunk, bytes + at, (size_t)got)) {
            at = size + 1;  /* grew, or differs */
            break;
        }
        at += (size_t)got;
    }
    close(fd);
    if (!error && at != size) *state = PW_SHELL_LINK_DIFFERENT;
    return error;
}

/* A new file at path, this module's name: whatever is there is unlinked first (a leftover, or a symbolic link
 * someone put there), and O_EXCL never opens through a link or into another file. */
static int pw_shell_link_create(const char *path)
{
    unlink(path);
    return open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
}

/* bytes into temporary, synced, renamed over path; the temporary is gone either way. */
static int pw_shell_link_replace(const char *temporary, const char *path, const unsigned char *bytes, size_t size)
{
    int fd = pw_shell_link_create(temporary);
    if (fd < 0) return errno;
    int error = pw_shell_link_write_all(fd, bytes, size);
    if (!error && fsync(fd)) error = errno;
    if (close(fd) && !error) error = errno;
    if (!error && rename(temporary, path)) error = errno;
    if (error) unlink(temporary);
    return error;
}

/* Appends name to the existing manifest before its file is created, so a sync that stops still owns the file. The
 * leading '\n' ends a last line written without one; empty lines are skipped when read. */
static int pw_shell_link_claim(const char *manifest_path, const char *name)
{
    int fd = open(manifest_path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) return errno;
    int error = pw_shell_link_write_all(fd, "\n", 1);
    if (!error) error = pw_shell_link_write_line(fd, name);
    if (!error && fsync(fd)) error = errno;
    if (close(fd) && !error) error = errno;
    return error;
}

/* Whether an earlier entry has this file name, ignoring ASCII case (O(n) per entry: catalogs are small). */
static int pw_shell_link_duplicate(const PwShellLinkEntry *entries, size_t index, const char *name)
{
    char other[PW_SHELL_LINK_NAME_MAX + 1];
    for (size_t j = 0; j < index; ++j)
        if (!pw_shell_link_file_name(entries[j].display_name, other, sizeof other) &&
            pw_shell_link_equal_nocase(other, strlen(other), name))
            return 1;
    return 0;
}

/* The new manifest, written as entries are decided. A failed write (error), or an entry whose ownership could not
 * be read (partial: an owned name it should keep may be missing), keeps it from deciding removals and from
 * replacing the old one. */
typedef struct PwShellLinkNext {
    char path[PW_SHELL_LINK_PATH_MAX];
    int fd;
    int error;
    int partial;
} PwShellLinkNext;

static void pw_shell_link_keep(PwShellLinkNext *next, const char *name)
{
    if (!next->error) next->error = pw_shell_link_write_line(next->fd, name);
}

typedef struct PwShellLinkRemove {
    const char *folder;
    PwShellLinkNext *next;
    PwShellLinkSyncResult *result;
} PwShellLinkRemove;

/* One old manifest line: its file is removed unless the new manifest keeps the name, or keeps another spelling of
 * the same file (a file system that folds case or normalization). Only a regular file is ever removed: anything
 * else there now (a directory, a symbolic link) was put there by someone else, and is released, not touched. */
static int pw_shell_link_remove_visit(void *context, const char *name)
{
    PwShellLinkRemove *remove = context;
    PwShellLinkFind find;
    char path[PW_SHELL_LINK_PATH_MAX];
    struct stat info;
    if (!pw_shell_link_owned_name(name)) return 0;
    int error = pw_shell_link_find(remove->next->path, name, &find);
    if (!error && find.exact) return 0;
    if (!error) error = pw_shell_link_in_folder(path, remove->folder, name);
    if (!error && lstat(path, &info)) {
        if (errno == ENOENT) return 0;
        error = errno;
    }
    if (!error && !S_ISREG(info.st_mode)) return 0;
    if (!error) error = pw_shell_link_find_file(remove->next->path, remove->folder, &info, &find);
    if (!error && find.found) return 0;
    if (!error && unlink(path)) {
        if (errno == ENOENT) return 0;
        error = errno;
    }
    if (!error) {
        remove->result->removed++;
        return 0;
    }
    pw_shell_link_note(remove->result, error, "remove", -1);
    pw_shell_link_keep(remove->next, name);  /* still there: it stays owned, and the next sync retries */
    return 0;
}

int pw_shell_link_sync(const char *folder, const char *manifest_path, const PwShellLinkEntry *entries, size_t count,
                       PwShellLinkSyncResult *result)
{
    PwShellLinkSyncResult ignored;
    if (!result) result = &ignored;
    memset(result, 0, sizeof *result);
    result->first_error_step = "";
    result->first_error_entry = -1;
    if (!pw_shell_link_present(folder) || !pw_shell_link_present(manifest_path) || (count && !entries)) {
        pw_shell_link_note(result, EINVAL, "arguments", -1);
        return result->first_error;
    }
    char temporary[PW_SHELL_LINK_PATH_MAX], path[PW_SHELL_LINK_PATH_MAX];
    PwShellLinkNext next = {.fd = -1};
    int error = pw_shell_link_in_folder(temporary, folder, pw_shell_link_temporary);
    if (!error) error = pw_shell_link_join(next.path, manifest_path, "", ".tmp");
    if (error) {
        pw_shell_link_note(result, error, "arguments", -1);
        return result->first_error;
    }
    struct stat info;
    if (stat(folder, &info)) {
        if (errno != ENOENT || (mkdir(folder, 0755) && errno != EEXIST)) {
            pw_shell_link_note(result, errno, "folder", -1);
            return result->first_error;
        }
    } else if (!S_ISDIR(info.st_mode)) {
        pw_shell_link_note(result, ENOTDIR, "folder", -1);
        return result->first_error;
    }
    next.fd = pw_shell_link_create(next.path);
    if (next.fd < 0) {
        pw_shell_link_note(result, errno, "manifest", -1);
        return result->first_error;
    }

    unsigned char bytes[PW_SHELL_LINK_SYNC_BYTES];
    for (size_t i = 0; i < count; ++i) {
        long entry = (long)i;
        char name[PW_SHELL_LINK_NAME_MAX + 1];
        PwShellLinkFind owner, alias;
        size_t size = 0;
        int state = PW_SHELL_LINK_ABSENT;
        error = pw_shell_link_file_name(entries[i].display_name, name, sizeof name);
        if (!error) error = pw_shell_link_in_folder(path, folder, name);
        if (error) {
            pw_shell_link_note(result, error, "name", entry);
            continue;
        }
        if (pw_shell_link_duplicate(entries, i, name)) {
            pw_shell_link_note(result, EEXIST, "duplicate", entry);
            continue;
        }
        pw_shell_link_find_reset(&alias);
        error = pw_shell_link_find(manifest_path, name, &owner);
        if (error) {
            pw_shell_link_note(result, error, "manifest", entry);
            next.partial = 1;  /* this entry's file may be owned: nothing is removed this time */
            continue;
        }
        const char *step = "build";
        error = pw_shell_link_build(&entries[i].link, bytes, sizeof bytes, &size);
        if (!error) step = "read", error = pw_shell_link_compare(path, bytes, size, &state, &info);
        if (!error && state == PW_SHELL_LINK_SAME) {
            result->unchanged++;  /* adopted when it was not owned: these are exactly this module's bytes */
            pw_shell_link_keep(&next, name);
            continue;
        }
        /* The file at path is owned when the old manifest lists name exactly, or lists another spelling that is this
         * same file (a file system that folds case); a spelling that is another file (owner.found alone, on a
         * case-sensitive one) owns nothing here. Unless an earlier entry already kept this file. */
        if (!error && state == PW_SHELL_LINK_DIFFERENT) {
            PwShellLinkFind kept;
            pw_shell_link_find_reset(&kept);
            step = "manifest";
            if (!owner.exact) error = pw_shell_link_find_file(manifest_path, folder, &info, &alias);
            if (!error && (owner.exact || alias.found))
                error = pw_shell_link_find_file(next.path, folder, &info, &kept);
            if (error) next.partial = 1;  /* as for owner: nothing is removed this time */
            else if (kept.found) step = "duplicate", error = EEXIST;
        }
        int owned = owner.exact || alias.found;
        if (!error && (state == PW_SHELL_LINK_OTHER || (state == PW_SHELL_LINK_DIFFERENT && !owned)))
            step = "conflict", error = EEXIST;  /* not this module's: never touched, never owned */
        if (!error && !owner.exact) step = "manifest", error = pw_shell_link_claim(manifest_path, name);
        if (!error) step = "write", error = pw_shell_link_replace(temporary, path, bytes, size);
        if (!error) {
            result->written++;
            pw_shell_link_keep(&next, name);
            continue;
        }
        pw_shell_link_note(result, error, step, entry);
        /* What was owned stays owned, under the name its file has, so the next sync retries; a conflicting file
         * at an exactly owned name (not a regular file any more) is released. */
        if (!strcmp(step, "duplicate")) continue;
        if (owner.exact) {
            if (strcmp(step, "conflict")) pw_shell_link_keep(&next, name);
        } else if (alias.found) {
            pw_shell_link_keep(&next, alias.match);
        } else if (owner.found) {
            pw_shell_link_keep(&next, owner.match);
        }
    }

    unlink(temporary);  /* left by a sync that stopped while writing; this module's name, never a user's */
    /* Removals are decided against the new manifest, so only a complete one decides any. */
    error = next.error;
    if (!error && !next.partial) {
        PwShellLinkRemove remove = {folder, &next, result};
        error = pw_shell_link_lines(manifest_path, pw_shell_link_remove_visit, &remove);
        if (!error) error = next.error;
        if (!error && fsync(next.fd)) error = errno;
    }
    /* The new manifest last, and only when it is complete; otherwise the old one, with every claim, stays. */
    if (close(next.fd) && !error) error = errno;
    if (!error && !next.partial && rename(next.path, manifest_path)) error = errno;
    if (error || next.partial) unlink(next.path);
    if (error) pw_shell_link_note(result, error, "manifest", -1);
    return result->first_error;
}
