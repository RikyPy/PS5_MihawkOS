/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pw_pack.h"
#include "pw_sha256.h"

#define ZSTD_STATIC_LINKING_ONLY
#include "../external/zstd/zstd.h"

#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#define PART ".mihawkos-part"
#define BUFFER (4u << 20)   /* a title's write() costs about 3.3 ms whatever its size (PROBE.md): write in large pieces */
#define WINDOW_LOG 23       /* zstd -19's default window, 8 MiB: a chunk asking for more is refused */

struct PwPack {
    const PwPackManifest *manifest;
    char root[512];
    uint8_t *in_place;          /* per file */
    uint8_t *buffer;
    ZSTD_DCtx *zstd;
    /* the chunk being fed */
    long chunk;                 /* -1: none */
    size_t file;                /* the file its bytes go to now */
    uint64_t offset, fed_chunk, decoded;
    int frame_done, fd;
    PwSha256 chunk_hash, file_hash;
    uint8_t *ready;             /* per file: a stamp's part complete, renamed by finish */
    uint8_t *chunk_done;
    int error;
    char failed[1024];
    _Atomic uint64_t written, fed;
    _Atomic size_t files_written, chunks_done;
    uint64_t to_write, to_feed;
    size_t files_to_write, chunks_needed;
};

const char *pw_pack_status_text(int status)
{
    switch (status) {
    case PW_PACK_OK: return "installed";
    case PW_PACK_MANIFEST: return "the runtime's manifest in the title folder is damaged";
    case PW_PACK_PATH: return "a link, a device or a misplaced folder is in the way";
    case PW_PACK_CHUNK_SIZE: return "a runtime piece has the wrong size";
    case PW_PACK_CHUNK_HASH: return "a runtime piece is damaged or from another release";
    case PW_PACK_DECODE: return "a runtime piece does not decompress";
    case PW_PACK_FILE_HASH: return "a runtime file came out different from the release";
    case PW_PACK_WRITE: return "the console's storage refused a write";
    case PW_PACK_STATE: return "the install was interrupted";
    case PW_PACK_MEMORY: return "not enough memory";
    }
    return "unknown error";
}

/* ---- the manifest ---- */

static int hex_digest(const char *text, uint8_t out[32])
{
    for (int i = 0; i < 64; i++) {
        const char c = text[i];
        const int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (v < 0) return 0;
        out[i / 2] = (uint8_t)(i % 2 ? out[i / 2] | v : v << 4);
    }
    return text[64] == ' ' || text[64] == 0;
}

static int number(char **cursor, uint64_t *out)
{
    char *p = *cursor;
    uint64_t value = 0;
    if (*p < '0' || *p > '9') return 0;
    for (; *p >= '0' && *p <= '9'; p++) {
        if (value > (UINT64_MAX - 9) / 10) return 0;
        value = value * 10 + (uint64_t)(*p - '0');
    }
    if (*p != ' ' && *p != 0) return 0;
    *cursor = *p ? p + 1 : p;
    *out = value;
    return 1;
}

/* Relative, plain components only; in a runtime pack, under runtime/ or run/. */
static int plain_path(const char *path, int title)
{
    const size_t length = strlen(path);
    if (length == 0 || length > 900 || (!title && strncmp(path, "runtime/", 8) && strncmp(path, "run/", 4))) return 0;
    if (path[length - 1] == '/' || strstr(path, "//")) return 0;
    for (const char *c = path; *c; c++)
        if (*c <= 0x20 || *c >= 0x7f || *c == '\\') return 0;
    for (const char *part = path; part; part = strchr(part, '/') ? strchr(part, '/') + 1 : NULL)
        if (!strncmp(part, "../", 3) || !strcmp(part, "..") || !strncmp(part, "./", 2) || !strcmp(part, ".")) return 0;
    return !strstr(path, PART);
}

static char *next_line(char **cursor)
{
    char *line = *cursor;
    if (!*line) return NULL;
    char *end = strchr(line, '\n');
    if (!end) return NULL;   /* every line ends with a newline: a cut manifest is refused */
    *end = 0;
    *cursor = end + 1;
    return line;
}

/* 1 when `path` is one of the files in path order (all but the stamps). */
static int listed_before(const PwPackManifest *manifest, const char *path)
{
    size_t low = 0, high = manifest->stamp;
    while (low < high) {
        const size_t middle = (low + high) / 2;
        const int order = strcmp(manifest->files[middle].path, path);
        if (!order) return 1;
        if (order < 0) low = middle + 1;
        else high = middle;
    }
    return 0;
}

int pw_pack_parse(const char *text, size_t size, PwPackManifest *out)
{
    memset(out, 0, sizeof(*out));
    if (!text || memchr(text, 0, size)) return PW_PACK_MANIFEST;
    out->text = malloc(size + 1);
    if (!out->text) return PW_PACK_MANIFEST;
    memcpy(out->text, text, size);
    out->text[size] = 0;
    out->text_size = size;
    size_t lines = 0;
    for (size_t i = 0; i < size; i++) lines += text[i] == '\n';
    out->files = calloc(lines + 1, sizeof(*out->files));
    out->chunks = calloc(lines + 1, sizeof(*out->chunks));
    char *cursor = out->text, *line, *stamps[PW_PACK_MAX_LAST];
    size_t last = 0;
    if (!out->files || !out->chunks || !(line = next_line(&cursor)) ||
        (strcmp(line, "mihawkos-runtime-pack 1") && strcmp(line, "mihawkos-title-pack 1")) ||
        !(line = next_line(&cursor)) || strncmp(line, "zstd ", 5))
        goto bad;
    out->title = !strcmp(out->text, "mihawkos-title-pack 1");
    while (*cursor && !strncmp(cursor, "stamp ", 6)) {
        if (last == PW_PACK_MAX_LAST || !(line = next_line(&cursor))) goto bad;
        stamps[last++] = line + 6;
    }
    if (!last) goto bad;
    while ((line = next_line(&cursor))) {
        if (!strncmp(line, "chunk ", 6)) {
            if (out->chunk_count && out->chunks[out->chunk_count - 1].first + out->chunks[out->chunk_count - 1].count !=
                out->file_count) goto bad;
            PwPackChunk *chunk = &out->chunks[out->chunk_count++];
            char *p = line + 6 + 65;
            uint64_t count;
            if (strlen(line) < 6 + 65 || !hex_digest(line + 6, chunk->sha256) || !number(&p, &chunk->packed) ||
                !number(&p, &chunk->plain) || !number(&p, &count) || *p || count == 0 || chunk->packed == 0) goto bad;
            memcpy(chunk->name, line + 6, 64);
            memcpy(chunk->name + 64, ".zst", 5);
            chunk->first = out->file_count;
            chunk->count = (size_t)count;
            out->packed += chunk->packed;
        } else if (!strncmp(line, "file ", 5) && out->chunk_count) {
            PwPackChunk *chunk = &out->chunks[out->chunk_count - 1];
            PwPackFile *file = &out->files[out->file_count];
            char *p = line + 5 + 65;
            if (strlen(line) < 5 + 65 || out->file_count >= chunk->first + chunk->count ||
                !hex_digest(line + 5, file->sha256) || !number(&p, &file->size) || !number(&p, &file->mtime) ||
                !plain_path(p, out->title))
                goto bad;
            file->path = p;
            out->plain += file->size;
            if (file->size > out->largest) out->largest = file->size;
            out->file_count++;
        } else {
            goto bad;
        }
    }
    if (*cursor || !out->chunk_count || out->chunks[out->chunk_count - 1].first + out->chunks[out->chunk_count - 1].count !=
        out->file_count || out->file_count < last)
        goto bad;
    /* The stamps are the last files, in their order; the others strictly in path order; no path twice. */
    out->stamp = out->file_count - last;
    for (size_t i = 0; i < last; i++) {
        if (strcmp(out->files[out->stamp + i].path, stamps[i])) goto bad;
        for (size_t j = 0; j < i; j++)
            if (!strcmp(stamps[i], stamps[j])) goto bad;
    }
    for (size_t f = 1; f < out->stamp; f++)
        if (strcmp(out->files[f - 1].path, out->files[f].path) >= 0) goto bad;
    for (size_t c = 0; c < out->chunk_count; c++) {
        uint64_t plain = 0;
        for (size_t f = out->chunks[c].first; f < out->chunks[c].first + out->chunks[c].count; f++) plain += out->files[f].size;
        if (plain != out->chunks[c].plain) goto bad;
    }
    for (size_t i = 0; i < last; i++)
        if (listed_before(out, stamps[i])) goto bad;
    return PW_PACK_OK;
bad:
    pw_pack_manifest_free(out);
    return PW_PACK_MANIFEST;
}

void pw_pack_manifest_free(PwPackManifest *manifest)
{
    free(manifest->text);
    free(manifest->files);
    free(manifest->chunks);
    memset(manifest, 0, sizeof(*manifest));
}

uint64_t pw_pack_space_needed(const PwPackManifest *manifest, uint64_t extra)
{
    return manifest->plain + manifest->largest + extra;
}

/* ---- the install ---- */

static int fail(PwPack *pack, int status, int error, const char *path)
{
    pack->error = error;
    snprintf(pack->failed, sizeof(pack->failed), "%s", path ? path : "");
    return status;
}

static int join(const PwPack *pack, const char *relative, const char *suffix, char *out, size_t capacity)
{
    return snprintf(out, capacity, "%s/%s%s", pack->root, relative, suffix) < (int)capacity;
}

/* Every folder on the way to `relative`, made when missing; one that is not a folder (a link) is refused. */
static int folders(PwPack *pack, const char *relative)
{
    char path[1200];
    if (!join(pack, relative, "", path, sizeof(path))) return fail(pack, PW_PACK_PATH, ENAMETOOLONG, relative);
    for (char *slash = path + strlen(pack->root) + 1; (slash = strchr(slash, '/')); *slash++ = '/') {
        *slash = 0;
        struct stat st;
        if (mkdir(path, 0777) != 0 && errno != EEXIST) return fail(pack, PW_PACK_WRITE, errno, relative);
        if (lstat(path, &st) != 0 || !S_ISDIR(st.st_mode)) return fail(pack, PW_PACK_PATH, ENOTDIR, relative);
    }
    return PW_PACK_OK;
}

static int same_file(PwPack *pack, const PwPackFile *file)
{
    char path[1200];
    struct stat st;
    if (!join(pack, file->path, "", path, sizeof(path)) || lstat(path, &st) != 0 || !S_ISREG(st.st_mode) ||
        (uint64_t)st.st_size != file->size)
        return 0;
    const int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return 0;
    PwSha256 hash;
    pw_sha256_init(&hash);
    ssize_t got;
    uint64_t total = 0;
    while ((got = read(fd, pack->buffer, BUFFER)) > 0) {
        pw_sha256_update(&hash, pack->buffer, (size_t)got);
        total += (uint64_t)got;
    }
    close(fd);
    uint8_t digest[32];
    pw_sha256_final(&hash, digest);
    return got == 0 && total == file->size && !memcmp(digest, file->sha256, 32);
}

int pw_pack_open(PwPack **out, const PwPackManifest *manifest, const char *root)
{
    *out = NULL;
    if (!manifest || !manifest->file_count || !root || strlen(root) >= sizeof(((PwPack *)0)->root)) return PW_PACK_STATE;
    PwPack *pack = calloc(1, sizeof(*pack));
    if (!pack) return PW_PACK_MEMORY;
    pack->manifest = manifest;
    snprintf(pack->root, sizeof(pack->root), "%s", root);
    pack->chunk = -1;
    pack->fd = -1;
    pack->in_place = calloc(manifest->file_count, 1);
    pack->ready = calloc(manifest->file_count, 1);
    pack->chunk_done = calloc(manifest->chunk_count, 1);
    pack->buffer = malloc(BUFFER);
    pack->zstd = ZSTD_createDCtx();
    if (!pack->in_place || !pack->ready || !pack->chunk_done || !pack->buffer || !pack->zstd ||
        ZSTD_isError(ZSTD_DCtx_setParameter(pack->zstd, ZSTD_d_windowLogMax, WINDOW_LOG))) {
        pw_pack_close(pack);
        return PW_PACK_MEMORY;
    }
    *out = pack;
    for (size_t f = manifest->stamp; f < manifest->file_count; f++) {   /* the stamps go first */
        char stamp[1200];
        struct stat st;
        if (!join(pack, manifest->files[f].path, "", stamp, sizeof(stamp)) ||
            (unlink(stamp) != 0 && errno != ENOENT && !(lstat(stamp, &st) == 0 && S_ISDIR(st.st_mode))))
            return fail(pack, PW_PACK_WRITE, errno, manifest->files[f].path);
    }
    for (size_t c = 0; c < manifest->chunk_count; c++) {
        const PwPackChunk *chunk = &manifest->chunks[c];
        int needed = 0;
        for (size_t f = chunk->first; f < chunk->first + chunk->count; f++) {
            pack->in_place[f] = f < manifest->stamp && same_file(pack, &manifest->files[f]);
            if (!pack->in_place[f]) {
                needed = 1;
                pack->to_write += manifest->files[f].size;
                pack->files_to_write++;
            }
        }
        if (needed) {
            pack->to_feed += chunk->packed;
            pack->chunks_needed++;
        } else {
            pack->chunk_done[c] = 1;
        }
    }
    return PW_PACK_OK;
}

int pw_pack_chunk_needed(const PwPack *pack, size_t chunk)
{
    return pack && chunk < pack->manifest->chunk_count && !pack->chunk_done[chunk];
}

static void drop_part(PwPack *pack)
{
    if (pack->fd >= 0) {
        close(pack->fd);
        pack->fd = -1;
        char part[1200];
        if (join(pack, pack->manifest->files[pack->file].path, PART, part, sizeof(part))) unlink(part);
    }
}

/* Ready the file at pack->file: a part file to write, or nothing when it is in place. Empty files end at once. */
static int start_file(PwPack *pack);

static int end_file(PwPack *pack)
{
    const PwPackFile *file = &pack->manifest->files[pack->file];
    uint8_t digest[32];
    pw_sha256_final(&pack->file_hash, digest);
    if (!pack->in_place[pack->file]) {
        if (memcmp(digest, file->sha256, 32)) {
            drop_part(pack);
            return fail(pack, PW_PACK_FILE_HASH, 0, file->path);
        }
        char part[1200], path[1200];
        join(pack, file->path, PART, part, sizeof(part));
        join(pack, file->path, "", path, sizeof(path));
        const int synced = fsync(pack->fd);
        const int closed = close(pack->fd);
        pack->fd = -1;
        if (synced != 0 || closed != 0) {
            const int error = errno;
            unlink(part);
            return fail(pack, PW_PACK_WRITE, error, file->path);
        }
        if (file->mtime) {
            const struct timeval times[2] = { { (time_t)file->mtime, 0 }, { (time_t)file->mtime, 0 } };
            if (utimes(part, times) != 0) {
                const int error = errno;
                unlink(part);
                return fail(pack, PW_PACK_WRITE, error, file->path);
            }
        }
        if (pack->file >= pack->manifest->stamp) {
            pack->ready[pack->file] = 1;
        } else if (rename(part, path) != 0) {
            const int error = errno;
            unlink(part);
            return fail(pack, PW_PACK_WRITE, error, file->path);
        } else {
            pack->in_place[pack->file] = 1;
        }
        atomic_fetch_add(&pack->files_written, 1);
    }
    pack->file++;
    pack->offset = 0;
    return start_file(pack);
}

static int start_file(PwPack *pack)
{
    const PwPackChunk *chunk = &pack->manifest->chunks[pack->chunk];
    if (pack->file >= chunk->first + chunk->count) return PW_PACK_OK;
    const PwPackFile *file = &pack->manifest->files[pack->file];
    pw_sha256_init(&pack->file_hash);
    if (!pack->in_place[pack->file]) {
        char part[1200], path[1200];
        struct stat st;
        int status = folders(pack, file->path);
        if (status) return status;
        join(pack, file->path, PART, part, sizeof(part));
        join(pack, file->path, "", path, sizeof(path));
        if (lstat(path, &st) == 0 && !S_ISREG(st.st_mode)) return fail(pack, PW_PACK_PATH, EEXIST, file->path);
        pack->fd = open(part, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0644);
        if (pack->fd < 0) return fail(pack, errno == ELOOP ? PW_PACK_PATH : PW_PACK_WRITE, errno, file->path);
        /* As the FTP deploy leaves every file (rwxrwxrwx): an eboot.bin without its execute bits does not start
         * (../PS5_RetroArch/evidence/webui-update). */
        if (fchmod(pack->fd, 0777) != 0) return fail(pack, PW_PACK_WRITE, errno, file->path);
    }
    return file->size == 0 ? end_file(pack) : PW_PACK_OK;
}

int pw_pack_chunk_begin(PwPack *pack, size_t chunk)
{
    if (!pack || chunk >= pack->manifest->chunk_count) return PW_PACK_STATE;
    drop_part(pack);
    pack->chunk = (long)chunk;
    pack->file = pack->manifest->chunks[chunk].first;
    pack->offset = pack->fed_chunk = pack->decoded = 0;
    pack->frame_done = 0;
    pw_sha256_init(&pack->chunk_hash);
    ZSTD_DCtx_reset(pack->zstd, ZSTD_reset_session_only);
    const int status = start_file(pack);
    if (status) pack->chunk = -1;
    return status;
}

/* Decoded bytes, to the files they belong to in order. */
static int take(PwPack *pack, const uint8_t *data, size_t size)
{
    const PwPackChunk *chunk = &pack->manifest->chunks[pack->chunk];
    pack->decoded += size;
    if (pack->decoded > chunk->plain) return fail(pack, PW_PACK_DECODE, 0, chunk->name);
    while (size) {
        const PwPackFile *file = &pack->manifest->files[pack->file];
        const size_t piece = file->size - pack->offset < size ? (size_t)(file->size - pack->offset) : size;
        pw_sha256_update(&pack->file_hash, data, piece);
        for (size_t done = 0; pack->fd >= 0 && done < piece;) {
            const ssize_t wrote = write(pack->fd, data + done, piece - done);
            if (wrote < 0 && errno == EINTR) continue;
            if (wrote <= 0) {
                const int error = wrote < 0 ? errno : EIO;
                drop_part(pack);
                return fail(pack, PW_PACK_WRITE, error, file->path);
            }
            done += (size_t)wrote;
            atomic_fetch_add(&pack->written, (uint64_t)wrote);
        }
        pack->offset += piece;
        data += piece;
        size -= piece;
        if (pack->offset == file->size) {
            const int status = end_file(pack);
            if (status) return status;
        }
    }
    return PW_PACK_OK;
}

int pw_pack_chunk_feed(PwPack *pack, const void *data, size_t size)
{
    if (!pack || pack->chunk < 0) return PW_PACK_STATE;
    const PwPackChunk *chunk = &pack->manifest->chunks[pack->chunk];
    pack->fed_chunk += size;
    atomic_fetch_add(&pack->fed, (uint64_t)size);
    if (pack->fed_chunk > chunk->packed) return fail(pack, PW_PACK_CHUNK_SIZE, 0, chunk->name);
    pw_sha256_update(&pack->chunk_hash, data, size);
    ZSTD_inBuffer in = { data, size, 0 };
    while (in.pos < in.size) {
        if (pack->frame_done) return fail(pack, PW_PACK_DECODE, 0, chunk->name);   /* one frame a chunk */
        ZSTD_outBuffer out = { pack->buffer, BUFFER, 0 };
        const size_t left = ZSTD_decompressStream(pack->zstd, &out, &in);
        if (ZSTD_isError(left)) return fail(pack, PW_PACK_DECODE, 0, chunk->name);
        const int status = take(pack, pack->buffer, out.pos);
        if (status) return status;
        pack->frame_done = left == 0;
        /* A full buffer may hold back more output even with the input used up. */
        while (!pack->frame_done && in.pos == in.size && out.pos == out.size) {
            out.pos = 0;
            const size_t more = ZSTD_decompressStream(pack->zstd, &out, &in);
            if (ZSTD_isError(more)) return fail(pack, PW_PACK_DECODE, 0, chunk->name);
            const int again = take(pack, pack->buffer, out.pos);
            if (again) return again;
            pack->frame_done = more == 0;
        }
    }
    return PW_PACK_OK;
}

int pw_pack_chunk_end(PwPack *pack)
{
    if (!pack || pack->chunk < 0) return PW_PACK_STATE;
    const size_t index = (size_t)pack->chunk;
    const PwPackChunk *chunk = &pack->manifest->chunks[index];
    pack->chunk = -1;
    uint8_t digest[32];
    pw_sha256_final(&pack->chunk_hash, digest);
    int status = PW_PACK_OK;
    if (pack->fed_chunk != chunk->packed) status = fail(pack, PW_PACK_CHUNK_SIZE, 0, chunk->name);
    else if (memcmp(digest, chunk->sha256, 32)) status = fail(pack, PW_PACK_CHUNK_HASH, 0, chunk->name);
    else if (!pack->frame_done || pack->decoded != chunk->plain) status = fail(pack, PW_PACK_DECODE, 0, chunk->name);
    if (status) {
        drop_part(pack);
        return status;
    }
    pack->chunk_done[index] = 1;
    atomic_fetch_add(&pack->chunks_done, 1);
    return PW_PACK_OK;
}

static int listed(const PwPackManifest *manifest, const char *path)
{
    if (listed_before(manifest, path)) return 1;
    for (size_t f = manifest->stamp; f < manifest->file_count; f++)
        if (!strcmp(manifest->files[f].path, path)) return 1;
    return 0;
}

int pw_pack_finish(PwPack *pack, const char *const *old, size_t old_count, const char *record)
{
    if (!pack || pack->chunk >= 0) return PW_PACK_STATE;
    const PwPackManifest *manifest = pack->manifest;
    for (size_t c = 0; c < manifest->chunk_count; c++)
        if (!pack->chunk_done[c]) return fail(pack, PW_PACK_STATE, 0, manifest->chunks[c].name);
    char path[1200], part[1200];
    for (size_t f = 0; old && f < old_count; f++) {
        struct stat st;
        if (!old[f] || !plain_path(old[f], manifest->title) || listed(manifest, old[f]) || !join(pack, old[f], "", path, sizeof(path)) ||
            lstat(path, &st) != 0 || S_ISDIR(st.st_mode))
            continue;
        if (unlink(path) != 0 && errno != ENOENT) return fail(pack, PW_PACK_WRITE, errno, old[f]);
    }
    /* shortcut: folders the old runtime alone used are left, empty; remove them when an update ever renames one. */
    for (size_t f = 0; f < manifest->file_count; f++)
        if (f < manifest->stamp && join(pack, manifest->files[f].path, PART, part, sizeof(part))) unlink(part);
    if (record) {
        if (!plain_path(record, manifest->title) || folders(pack, record) || !join(pack, record, PART, part, sizeof(part)) ||
            !join(pack, record, "", path, sizeof(path)))
            return pack->error ? PW_PACK_WRITE : fail(pack, PW_PACK_PATH, EINVAL, record);
        /* The parsed text has a NUL where each line ended (next_line): the record gets its newlines back. */
        char *text = malloc(manifest->text_size);
        const int fd = text ? open(part, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0644) : -1;
        int ok = fd >= 0;
        for (size_t i = 0; text && i < manifest->text_size; i++) text[i] = manifest->text[i] ? manifest->text[i] : '\n';
        for (size_t done = 0; ok && done < manifest->text_size;) {
            const ssize_t wrote = write(fd, text + done, manifest->text_size - done);
            ok = wrote > 0;
            done += ok ? (size_t)wrote : 0;
        }
        free(text);
        ok = ok && fsync(fd) == 0;
        if (fd >= 0 && close(fd) != 0) ok = 0;
        if (!ok || rename(part, path) != 0) {
            const int error = errno;
            unlink(part);
            return fail(pack, PW_PACK_WRITE, error, record);
        }
    }
    for (size_t f = manifest->stamp; f < manifest->file_count; f++) {   /* the stamps, last and in their order */
        const PwPackFile *stamp = &manifest->files[f];
        join(pack, stamp->path, PART, part, sizeof(part));
        join(pack, stamp->path, "", path, sizeof(path));
        if (!pack->ready[f] || rename(part, path) != 0)
            return fail(pack, pack->ready[f] ? PW_PACK_WRITE : PW_PACK_STATE, errno, stamp->path);
        pack->ready[f] = 0;
        pack->in_place[f] = 1;
    }
    return PW_PACK_OK;
}

void pw_pack_progress(const PwPack *pack, PwPackProgress *out)
{
    memset(out, 0, sizeof(*out));
    if (!pack) return;
    out->written = atomic_load(&pack->written);
    out->fed = atomic_load(&pack->fed);
    out->files_written = atomic_load(&pack->files_written);
    out->chunks_done = atomic_load(&pack->chunks_done);
    out->to_write = pack->to_write;
    out->to_feed = pack->to_feed;
    out->files_to_write = pack->files_to_write;
    out->chunks_needed = pack->chunks_needed;
}

int pw_pack_errno(const PwPack *pack) { return pack ? pack->error : 0; }
const char *pw_pack_failed_path(const PwPack *pack) { return pack ? pack->failed : ""; }

void pw_pack_close(PwPack *pack)
{
    if (!pack) return;
    drop_part(pack);
    for (size_t f = pack->manifest->stamp; f < pack->manifest->file_count; f++) {
        char part[1200];
        if (pack->ready[f] && join(pack, pack->manifest->files[f].path, PART, part, sizeof(part))) unlink(part);
    }
    ZSTD_freeDCtx(pack->zstd);
    free(pack->buffer);
    free(pack->in_place);
    free(pack->ready);
    free(pack->chunk_done);
    free(pack);
}
