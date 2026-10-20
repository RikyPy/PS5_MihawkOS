/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/pw_pack.h's rules, against chunks the PC's zstd makes as tools/pack-runtime.py does (zstd -19). */
#include "../src/pw_pack.h"
#include "../src/pw_sha256.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct { const char *path; size_t size; uint64_t mtime; unsigned seed; } Spec;
typedef struct { uint8_t *data; size_t size; } Blob;

static char work[256];
static const char *kind = "runtime";   /* the next make()'s pack kind, and how many of its last files are stamps */
static size_t stamp_count = 1;

static uint8_t *content(const Spec *spec)
{
    uint8_t *data = malloc(spec->size + 1);
    uint32_t state = spec->seed * 2654435761u + 1;
    for (size_t i = 0; i < spec->size; i++) {
        state = state * 1103515245u + 12345u;
        data[i] = (uint8_t)(i % 97 < 60 ? 'a' + i % 7 : state >> 24);   /* half compressible */
    }
    return data;
}

static void hex_of(const void *data, size_t size, char hex[65])
{
    PwSha256 context;
    uint8_t digest[32];
    pw_sha256_init(&context);
    pw_sha256_update(&context, data, size);
    pw_sha256_final(&context, digest);
    pw_sha256_hex(digest, hex);
}

static Blob compress(const uint8_t *data, size_t size, const char *options)
{
    char in[300], out[300], command[800];
    snprintf(in, sizeof(in), "%s/plain", work);
    snprintf(out, sizeof(out), "%s/packed", work);
    FILE *file = fopen(in, "wb");
    assert(file && fwrite(data, 1, size, file) == size && fclose(file) == 0);
    snprintf(command, sizeof(command), "zstd -q -f %s -o %s %s", options, out, in);
    assert(system(command) == 0);
    Blob blob = { 0 };
    file = fopen(out, "rb");
    assert(file);
    fseek(file, 0, SEEK_END);
    blob.size = (size_t)ftell(file);
    rewind(file);
    blob.data = malloc(blob.size);
    assert(fread(blob.data, 1, blob.size, file) == blob.size);
    fclose(file);
    return blob;
}

/* A manifest and its chunks: `cuts` says how many files each chunk takes. */
typedef struct { char *text; Blob chunks[8]; size_t count; } Pack;

static Pack make(const Spec *files, size_t count, const size_t *cuts, const char *options)
{
    Pack pack = { calloc(1, 1 << 20), { { 0 } }, 0 };
    char *t = pack.text;
    t += sprintf(t, "mihawkos-%s-pack 1\nzstd test -19\n", kind);
    for (size_t s = count - stamp_count; s < count; s++) t += sprintf(t, "stamp %s\n", files[s].path);
    for (size_t first = 0; first < count; first += cuts[pack.count++]) {
        size_t plain = 0;
        for (size_t f = first; f < first + cuts[pack.count]; f++) plain += files[f].size;
        uint8_t *joined = malloc(plain + 1), *at = joined;
        for (size_t f = first; f < first + cuts[pack.count]; f++) {
            uint8_t *data = content(&files[f]);
            memcpy(at, data, files[f].size);
            at += files[f].size;
            free(data);
        }
        Blob blob = compress(joined, plain, options);
        char hex[65];
        hex_of(blob.data, blob.size, hex);
        t += sprintf(t, "chunk %s %zu %zu %zu\n", hex, blob.size, plain, cuts[pack.count]);
        for (size_t f = first; f < first + cuts[pack.count]; f++) {
            uint8_t *data = content(&files[f]);
            hex_of(data, files[f].size, hex);
            free(data);
            t += sprintf(t, "file %s %zu %llu %s\n", hex, files[f].size, (unsigned long long)files[f].mtime, files[f].path);
        }
        pack.chunks[pack.count] = blob;
        free(joined);
    }
    return pack;
}

static void parse(const Pack *pack, PwPackManifest *manifest)
{
    assert(pw_pack_parse(pack->text, strlen(pack->text), manifest) == PW_PACK_OK);
}

/* Every needed chunk, fed in pieces of `piece` bytes. */
static int install(const PwPackManifest *manifest, const Pack *pack, const char *root, size_t piece,
                   const PwPackManifest *old, PwPackProgress *progress)
{
    PwPack *engine;
    int status = pw_pack_open(&engine, manifest, root);
    for (size_t c = 0; !status && c < manifest->chunk_count; c++) {
        if (!pw_pack_chunk_needed(engine, c)) continue;
        status = pw_pack_chunk_begin(engine, c);
        for (size_t done = 0; !status && done < pack->chunks[c].size; done += piece)
            status = pw_pack_chunk_feed(engine, pack->chunks[c].data + done,
                                        pack->chunks[c].size - done < piece ? pack->chunks[c].size - done : piece);
        if (!status) status = pw_pack_chunk_end(engine);
    }
    const char *paths[16] = { 0 };
    for (size_t f = 0; old && f < old->file_count; f++) paths[f] = old->files[f].path;
    paths[old ? old->file_count : 0] = "runtime/../outside";   /* never a manifest path: left alone */
    if (!status) status = pw_pack_finish(engine, paths, old ? old->file_count + 1 : 1, "runtime/ge11-7/runtime-pack.txt");
    if (progress) pw_pack_progress(engine, progress);
    pw_pack_close(engine);
    return status;
}

static int holds(const char *root, const Spec *spec)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", root, spec->path);
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    uint8_t *want = content(spec), *got = malloc(spec->size + 1);
    const size_t read = fread(got, 1, spec->size + 1, file);
    fclose(file);
    const int same = read == spec->size && !memcmp(want, got, spec->size);
    free(want);
    free(got);
    return same;
}

static int exists(const char *root, const char *relative)
{
    char path[512];
    struct stat st;
    snprintf(path, sizeof(path), "%s/%s", root, relative);
    return lstat(path, &st) == 0;
}

static void refuse(const char *text)
{
    PwPackManifest manifest;
    assert(pw_pack_parse(text, strlen(text), &manifest) == PW_PACK_MANIFEST);
}

static void free_pack(Pack *pack)
{
    for (size_t c = 0; c < pack->count; c++) free(pack->chunks[c].data);
    free(pack->text);
}

int main(void)
{
    snprintf(work, sizeof(work), "/tmp/test_pw_pack.%d", (int)getpid());
    assert(mkdir(work, 0777) == 0);
    char root[300];
    snprintf(root, sizeof(root), "%s/data", work);
    assert(mkdir(root, 0777) == 0);

    const Spec v1[] = {
        { "run/proton-payload-child.elf", 3000, 0, 1 },
        { "runtime/ge11-7/empty", 0, 0, 2 },
        { "runtime/ge11-7/lib/big.so", 9u << 20, 0, 3 },        /* more than the 4 MiB buffer: several fills */
        { "runtime/ge11-7/lib/small.dll", 777, 0, 4 },
        { "runtime/ge11-7/share/wine/wine.inf", 5000, 1700000000, 5 },
        { "runtime/ge11-7/runtime-stamp", 65, 0, 6 },
    };
    const size_t v1_cuts[] = { 2, 3, 1 };
    Pack p1 = make(v1, 6, v1_cuts, "-19");
    PwPackManifest m1;
    parse(&p1, &m1);
    assert(m1.file_count == 6 && m1.chunk_count == 3 && m1.stamp == 5 && m1.largest == 9u << 20);
    assert(pw_pack_space_needed(&m1, 100) == m1.plain + m1.largest + 100);

    /* A fresh install, in odd pieces; the stamp, the record and wine.inf's time. */
    PwPackProgress progress;
    assert(install(&m1, &p1, root, 4093, NULL, &progress) == PW_PACK_OK);
    for (size_t f = 0; f < 6; f++) assert(holds(root, &v1[f]));
    assert(progress.files_written == 6 && progress.written == m1.plain && progress.chunks_done == 3);
    assert(exists(root, "runtime/ge11-7/runtime-pack.txt") && !exists(root, "runtime/ge11-7/lib/big.so.mihawkos-part"));
    {   /* the record is the manifest, byte for byte: the next install's old list */
        char record[512], *copy = calloc(1, strlen(p1.text) + 2);
        snprintf(record, sizeof(record), "%s/runtime/ge11-7/runtime-pack.txt", root);
        FILE *file = fopen(record, "rb");
        assert(file && fread(copy, 1, strlen(p1.text) + 1, file) == strlen(p1.text) && !strcmp(copy, p1.text));
        fclose(file);
        free(copy);
    }
    char path[512];
    struct stat st;
    snprintf(path, sizeof(path), "%s/runtime/ge11-7/share/wine/wine.inf", root);
    assert(stat(path, &st) == 0 && st.st_mtime == 1700000000 && (st.st_mode & 0777) == 0777);

    /* Again: only the stamp's chunk is needed (the stamp is removed at open), nothing else is written. */
    assert(install(&m1, &p1, root, 1 << 20, NULL, &progress) == PW_PACK_OK);
    assert(progress.chunks_needed == 1 && progress.files_to_write == 1 && progress.written == 65);

    /* The stamp goes first: an open alone leaves the runtime without one. */
    PwPack *engine;
    assert(pw_pack_open(&engine, &m1, root) == PW_PACK_OK && !exists(root, v1[5].path));
    pw_pack_close(engine);

    /* An update: small.dll changes, wine.inf goes, a new file comes; big.so's chunk is not needed. */
    const Spec v2[] = {
        { "run/proton-payload-child.elf", 3000, 0, 1 },
        { "runtime/ge11-7/empty", 0, 0, 2 },
        { "runtime/ge11-7/lib/big.so", 9u << 20, 0, 3 },
        { "runtime/ge11-7/lib/new.dll", 1234, 0, 7 },
        { "runtime/ge11-7/lib/small.dll", 777, 0, 8 },
        { "runtime/ge11-7/runtime-stamp", 65, 0, 9 },
    };
    const size_t v2_cuts[] = { 2, 1, 2, 1 };
    Pack p2 = make(v2, 6, v2_cuts, "-19");
    PwPackManifest m2;
    parse(&p2, &m2);
    snprintf(path, sizeof(path), "%s/outside", root);
    FILE *outside = fopen(path, "w");
    assert(outside && fclose(outside) == 0);
    assert(install(&m2, &p2, root, 333, &m1, &progress) == PW_PACK_OK);
    assert(exists(root, "outside"));
    assert(progress.chunks_needed == 2 && progress.files_to_write == 3);
    for (size_t f = 0; f < 6; f++) assert(holds(root, &v2[f]));
    assert(!exists(root, "runtime/ge11-7/share/wine/wine.inf"));

    /* A damaged chunk: refused, nothing renamed, no part left, the stamp absent; then the right one finishes. */
    p2.chunks[3].data[p2.chunks[3].size / 2] ^= 0x40;
    p2.chunks[2].data[p2.chunks[2].size - 3] ^= 0x01;
    const int damaged = install(&m2, &p2, root, 4096, NULL, NULL);
    assert(damaged == PW_PACK_DECODE || damaged == PW_PACK_CHUNK_HASH || damaged == PW_PACK_FILE_HASH);
    assert(!exists(root, v2[5].path) && !exists(root, "runtime/ge11-7/lib/new.dll.mihawkos-part"));
    assert(holds(root, &v2[3]) && holds(root, &v2[4]));
    p2.chunks[3].data[p2.chunks[3].size / 2] ^= 0x40;
    p2.chunks[2].data[p2.chunks[2].size - 3] ^= 0x01;

    /* Cut short and overlong chunks. */
    assert(pw_pack_open(&engine, &m2, root) == PW_PACK_OK);
    assert(pw_pack_chunk_begin(engine, 3) == PW_PACK_OK);
    assert(pw_pack_chunk_feed(engine, p2.chunks[3].data, p2.chunks[3].size - 1) == PW_PACK_OK);
    assert(pw_pack_chunk_end(engine) == PW_PACK_CHUNK_SIZE);
    assert(pw_pack_chunk_begin(engine, 3) == PW_PACK_OK);
    assert(pw_pack_chunk_feed(engine, p2.chunks[3].data, p2.chunks[3].size) == PW_PACK_OK);
    assert(pw_pack_chunk_feed(engine, "x", 1) == PW_PACK_CHUNK_SIZE);
    assert(pw_pack_finish(engine, NULL, 0, NULL) == PW_PACK_STATE);   /* the stamp's chunk is not in */
    pw_pack_close(engine);
    assert(!exists(root, "runtime/ge11-7/runtime-stamp.mihawkos-part"));

    /* A manifest whose file digest is not the chunk's bytes: the file is refused, not renamed. */
    Pack lie = make(v2, 6, v2_cuts, "-19");
    char *digest = strstr(lie.text, "file ") + 5;
    for (char *line = strstr(lie.text, "runtime/ge11-7/lib/new.dll"); line > lie.text && line[-1] != '\n'; line--)
        digest = line - 1;
    digest = strstr(digest, "file ") + 5;
    digest[0] = digest[0] == '0' ? '1' : '0';
    PwPackManifest m_lie;
    parse(&lie, &m_lie);
    snprintf(path, sizeof(path), "%s/runtime/ge11-7/lib/new.dll", root);
    assert(unlink(path) == 0);
    assert(install(&m_lie, &lie, root, 4096, NULL, NULL) == PW_PACK_FILE_HASH && !exists(root, "runtime/ge11-7/lib/new.dll"));
    pw_pack_manifest_free(&m_lie);
    free_pack(&lie);

    /* Links and folders in the way. */
    snprintf(path, sizeof(path), "%s/runtime/ge11-7/lib/new.dll", root);
    assert(symlink("/etc/passwd", path) == 0);
    assert(install(&m2, &p2, root, 4096, NULL, NULL) == PW_PACK_PATH);
    assert(unlink(path) == 0 && mkdir(path, 0777) == 0);
    assert(install(&m2, &p2, root, 4096, NULL, NULL) == PW_PACK_PATH);
    assert(rmdir(path) == 0);
    char lib[512], moved[512];
    snprintf(lib, sizeof(lib), "%s/runtime/ge11-7/lib", root);
    snprintf(moved, sizeof(moved), "%s/elsewhere", work);
    assert(rename(lib, moved) == 0 && symlink(moved, lib) == 0);
    assert(install(&m2, &p2, root, 4096, NULL, NULL) == PW_PACK_PATH);
    assert(unlink(lib) == 0 && rename(moved, lib) == 0);
    assert(install(&m2, &p2, root, 4096, NULL, NULL) == PW_PACK_OK);
    for (size_t f = 0; f < 6; f++) assert(holds(root, &v2[f]));

    /* A window larger than 8 MiB is refused. */
    const Spec wide[] = { { "runtime/ge11-7/wide", 20u << 20, 0, 11 }, { "runtime/ge11-7/runtime-stamp", 1, 0, 12 } };
    const size_t wide_cuts[] = { 1, 1 };
    Pack pw = make(wide, 2, wide_cuts, "-19 --long=25");
    PwPackManifest mw;
    parse(&pw, &mw);
    assert(install(&mw, &pw, root, 1 << 20, NULL, NULL) == PW_PACK_DECODE);
    pw_pack_manifest_free(&mw);
    free_pack(&pw);

    /* A title pack (the updater's): paths anywhere under its root, eboot.bin then param.json last. */
    char title_root[300];
    snprintf(title_root, sizeof(title_root), "%s/title", work);
    assert(mkdir(title_root, 0777) == 0);
    const Spec title[] = {
        { "assets/font.ttf", 70000, 0, 21 },
        { "licenses/zstd/LICENSE", 1500, 0, 22 },
        { "runtime-pack.txt", 4000, 0, 23 },
        { "eboot.bin", 2u << 20, 0, 24 },
        { "sce_sys/param.json", 900, 0, 25 },
    };
    const size_t title_cuts[] = { 3, 2 };
    kind = "title", stamp_count = 2;
    Pack pt = make(title, 5, title_cuts, "-19");
    kind = "runtime", stamp_count = 1;
    PwPackManifest mt;
    parse(&pt, &mt);
    assert(mt.title && mt.stamp == 3);
    assert(pw_pack_open(&engine, &mt, title_root) == PW_PACK_OK);
    for (size_t c = 0; c < mt.chunk_count; c++) {
        assert(pw_pack_chunk_begin(engine, c) == PW_PACK_OK);
        assert(pw_pack_chunk_feed(engine, pt.chunks[c].data, pt.chunks[c].size) == PW_PACK_OK);
        assert(pw_pack_chunk_end(engine) == PW_PACK_OK);
    }
    /* Until finish, neither stamp is in place: no half title for ShadowMountPlus to register. */
    assert(!exists(title_root, "eboot.bin") && !exists(title_root, "sce_sys/param.json") && holds(title_root, &title[0]));
    assert(pw_pack_finish(engine, NULL, 0, NULL) == PW_PACK_OK);
    pw_pack_close(engine);
    for (size_t f = 0; f < 5; f++) assert(holds(title_root, &title[f]));
    /* An update: the stamps go first again, the rest stays. */
    assert(pw_pack_open(&engine, &mt, title_root) == PW_PACK_OK && !exists(title_root, "eboot.bin") &&
           !exists(title_root, "sce_sys/param.json") && holds(title_root, &title[0]));
    pw_pack_close(engine);
    pw_pack_manifest_free(&mt);
    /* The same paths are refused in a runtime pack. */
    Pack pr = make(title, 5, title_cuts, "-19");
    assert(pw_pack_parse(pr.text, strlen(pr.text), &mt) == PW_PACK_MANIFEST);
    free_pack(&pr);
    free_pack(&pt);

    /* Manifests tools/pack-runtime.py never writes. */
    const char *head = "mihawkos-runtime-pack 1\nzstd 1.5.7 -19\nstamp runtime/s\n";
    const char *z64 = "0000000000000000000000000000000000000000000000000000000000000000";
    char text[2048];
    snprintf(text, sizeof(text), "%schunk %s 10 3 2\nfile %s 1 0 runtime/a\nfile %s 2 0 runtime/s\n", head, z64, z64, z64);
    PwPackManifest ok;
    assert(pw_pack_parse(text, strlen(text), &ok) == PW_PACK_OK);
    pw_pack_manifest_free(&ok);
    text[strlen(text) - 1] = 0;
    refuse(text);                                                                       /* no last newline */
    const char *bad_paths[] = { "/runtime/a", "runtime/../a", "runtime//a", "runtime/a/", "prefixes/a", "runtime/./a",
                                "runtime/a b", "runtime/a.mihawkos-part", "runtime/a\\b", "run" };
    for (size_t i = 0; i < sizeof(bad_paths) / sizeof(bad_paths[0]); i++) {
        snprintf(text, sizeof(text), "%schunk %s 10 3 2\nfile %s 1 0 %s\nfile %s 2 0 runtime/s\n", head, z64, z64,
                 bad_paths[i], z64);
        refuse(text);
    }
    snprintf(text, sizeof(text), "%schunk %s 10 4 2\nfile %s 1 0 runtime/a\nfile %s 2 0 runtime/s\n", head, z64, z64, z64);
    refuse(text);                                                                       /* plain differs */
    snprintf(text, sizeof(text), "%schunk %s 10 3 2\nfile %s 1 0 runtime/b\nfile %s 1 0 runtime/a\nchunk %s 9 2 1\n"
             "file %s 2 0 runtime/s\n", head, z64, z64, z64, z64, z64);
    refuse(text);                                                                       /* out of order, count */
    snprintf(text, sizeof(text), "%schunk %s 10 3 2\nfile %s 2 0 runtime/s\nfile %s 1 0 runtime/z\n", head, z64, z64, z64);
    refuse(text);                                                                       /* stamp not last */
    snprintf(text, sizeof(text), "%schunk %s 10 3 2\nfile %s 1 0 runtime/a\n", head, z64, z64);
    refuse(text);                                                                       /* chunk short of files */
    const char *two = "mihawkos-title-pack 1\nzstd 1.5.7 -19\nstamp e\nstamp p\n";
    snprintf(text, sizeof(text), "%schunk %s 10 6 3\nfile %s 1 0 a\nfile %s 2 0 e\nfile %s 3 0 p\n", two, z64, z64, z64, z64);
    PwPackManifest okay;
    assert(pw_pack_parse(text, strlen(text), &okay) == PW_PACK_OK && okay.stamp == 1);
    pw_pack_manifest_free(&okay);
    snprintf(text, sizeof(text), "%schunk %s 10 6 3\nfile %s 1 0 a\nfile %s 3 0 p\nfile %s 2 0 e\n", two, z64, z64, z64, z64);
    refuse(text);                                                                       /* stamps out of order */
    snprintf(text, sizeof(text), "%schunk %s 10 6 3\nfile %s 1 0 e\nfile %s 2 0 e\nfile %s 3 0 p\n", two, z64, z64, z64, z64);
    refuse(text);                                                                       /* a stamp twice */
    snprintf(text, sizeof(text), "mihawkos-title-pack 1\nzstd x\nstamp p\nstamp p\nchunk %s 2 2 2\nfile %s 1 0 p\n"
             "file %s 1 0 p\n", z64, z64, z64);
    refuse(text);                                                                       /* a stamp line twice */
    snprintf(text, sizeof(text), "mihawkos-title-pack 1\nzstd x\nstamp a\nstamp b\nstamp c\nstamp d\nstamp e\nstamp f\n"
             "stamp g\nstamp h\nstamp i\nchunk %s 1 1 1\nfile %s 1 0 a\n", z64, z64);
    refuse(text);                                                                       /* too many stamps */
    snprintf(text, sizeof(text), "mihawkos-runtime-pack 2\nzstd x\nstamp runtime/s\nchunk %s 1 1 1\nfile %s 1 0 runtime/s\n", z64, z64);
    refuse(text);                                                                       /* another format */
    snprintf(text, sizeof(text), "%schunk %s 10 3 2\nfile %s 1 0 runtime/a\nfile %s 99999999999999999999 0 runtime/s\n",
             head, z64, z64, z64);
    refuse(text);                                                                       /* a number that wraps */

    pw_pack_manifest_free(&m1);
    pw_pack_manifest_free(&m2);
    free_pack(&p1);
    free_pack(&p2);
    char command[400];
    snprintf(command, sizeof(command), "rm -rf %s", work);
    assert(system(command) == 0);
    puts("pw_pack: ok");
    return 0;
}
