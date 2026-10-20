/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_PACK_H
#define PW_PACK_H

/* The runtime's install from its chunks (docs/SELF_INSTALL.md, phase 5): what tools/pack-runtime.py made, from a
 * folder or a download, into /data/ps5proton. The manifest the title folder pins (runtime-pack.txt, the format is in
 * tools/pack-runtime.py) is the only authority: a chunk carries file bytes and nothing else, so no path, link,
 * device or size comes from the chunk itself.
 *
 * The rules, each checked by tests/test_pw_pack.c:
 * - a file is hashed as it streams, written to <path>.mihawkos-part, and renamed into place only once its size and
 *   SHA-256 match; it is fsync'd first, so a power cut leaves either the old file or the whole new one;
 * - only manifest paths are written, in a runtime pack only under runtime/ and run/ (a title pack, an
 *   update's, may write anywhere under its root); a folder on the way that is a link, or a target
 *   that is a link, a folder or a device, stops the install (PW_PACK_PATH);
 * - a chunk that decodes to more or fewer bytes than its files hold, or whose bytes differ from its SHA-256, fails;
 * - a file already in place with the manifest's size and SHA-256 is not written again (a title's writes are held to
 *   about 2 MiB/s after a burst of about 1.3 GiB, ../PS5_PayloadSDK/platform/docs/PROBE.md), and a chunk holding
 *   only such files is not needed at all, so an update or a retry reads only what changed;
 * - the stamps (the manifest's "stamp" lines: the runtime's runtime-stamp, which the launcher compares; a title's
 *   eboot.bin then sce_sys/param.json, so ShadowMountPlus never registers half a title) are removed at open and
 *   renamed into place by pw_pack_finish, last and in their order, after the files the old manifest lists and the new one does not have been deleted and the
 *   manifest's own copy written: an install cut short at any point resumes, and is never taken for complete;
 * - every file is installed rwxrwxrwx, as the FTP deploy leaves it (a title's eboot.bin must be executable);
 * - a file's time is set when the manifest gives one (wine.inf's: a prefix updates when it changes).
 * Free space is the caller's to check first, with pw_pack_space_needed.
 *
 * Portable C over POSIX files; zstd's decoder (external/zstd) with its window held to 8 MiB. One PwPack is used by
 * one thread; another may read its progress (pw_pack_progress) while it works. */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    PW_PACK_OK = 0,
    PW_PACK_MANIFEST = -1,     /* the manifest is not one tools/pack-runtime.py wrote */
    PW_PACK_PATH = -2,         /* a link, a device or a file where the install needs a folder, or the reverse */
    PW_PACK_CHUNK_SIZE = -3,   /* a chunk has more or fewer bytes than the manifest says */
    PW_PACK_CHUNK_HASH = -4,   /* a chunk's bytes are not the manifest's (damaged, or from another release) */
    PW_PACK_DECODE = -5,       /* zstd refused a chunk, or it decoded to more or fewer bytes than its files */
    PW_PACK_FILE_HASH = -6,    /* a file's bytes are not the manifest's */
    PW_PACK_WRITE = -7,        /* the storage refused: pw_pack_errno says why (ENOSPC: full) */
    PW_PACK_STATE = -8,        /* a call out of order, or a chunk not yet installed at finish */
    PW_PACK_MEMORY = -9,
};

/* One line for the screen. */
const char *pw_pack_status_text(int status);

/* The most files a manifest may name to be installed last ("stamp" lines). */
#define PW_PACK_MAX_LAST 8

typedef struct PwPackFile {
    const char *path;          /* relative to the install root; points into the manifest's text */
    uint64_t size, mtime;      /* mtime 0: the time it is written */
    uint8_t sha256[32];
} PwPackFile;

typedef struct PwPackChunk {
    uint8_t sha256[32];        /* of its compressed bytes, which also name it: <hex>.zst */
    char name[69];
    uint64_t packed, plain;
    size_t first, count;       /* its files, in order */
} PwPackChunk;

typedef struct PwPackManifest {
    char *text;
    size_t text_size;
    PwPackFile *files;
    size_t file_count;
    PwPackChunk *chunks;
    size_t chunk_count;
    size_t stamp;              /* index of the first stamp: the stamps are the last files, in their order */
    int title;                 /* a title pack: its paths may be anywhere under the root, not only runtime/ and run/ */
    uint64_t plain, packed, largest;
} PwPackManifest;

/* PW_PACK_OK and a manifest to free with pw_pack_manifest_free, or PW_PACK_MANIFEST (nothing to free). */
int pw_pack_parse(const char *text, size_t size, PwPackManifest *out);
void pw_pack_manifest_free(PwPackManifest *manifest);
/* What the storage must have free before an install: every file, once more the largest (its part beside the old
 * file), and `extra` (the first prefix, a margin). */
uint64_t pw_pack_space_needed(const PwPackManifest *manifest, uint64_t extra);

typedef struct PwPackProgress {
    uint64_t written, to_write;           /* file bytes: written this install, and in all it must write */
    uint64_t fed, to_feed;                /* compressed bytes: fed so far, and in the chunks it needs */
    size_t files_written, files_to_write, chunks_done, chunks_needed;
} PwPackProgress;

typedef struct PwPack PwPack;

/* Start an install of `manifest` (kept by the caller until pw_pack_close) into `root`: the stamp is removed and
 * every file already in place is found (read and hashed). */
int pw_pack_open(PwPack **out, const PwPackManifest *manifest, const char *root);
/* 1 when the chunk has a file not yet in place. */
int pw_pack_chunk_needed(const PwPack *pack, size_t chunk);
/* One chunk's compressed bytes, in any pieces, between begin and end. A failure leaves no part file behind and the
 * install can go on with pw_pack_chunk_begin again (the same chunk, from its first byte). */
int pw_pack_chunk_begin(PwPack *pack, size_t chunk);
int pw_pack_chunk_feed(PwPack *pack, const void *data, size_t size);
int pw_pack_chunk_end(PwPack *pack);
/* Once every needed chunk is in: delete the `old` paths (what the runtime held before: the last install's manifest,
 * or the PC deploy's list) this manifest does not have, write this manifest's text to `record` (a path under the
 * root, or NULL), then put the stamp in place. An old path that is not a plain manifest path is left alone. */
int pw_pack_finish(PwPack *pack, const char *const *old, size_t old_count, const char *record);
void pw_pack_progress(const PwPack *pack, PwPackProgress *out);
/* After PW_PACK_WRITE or PW_PACK_PATH: the errno, and the path, relative to the root. */
int pw_pack_errno(const PwPack *pack);
const char *pw_pack_failed_path(const PwPack *pack);
void pw_pack_close(PwPack *pack);

#ifdef __cplusplus
}
#endif

#endif
