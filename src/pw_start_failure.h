/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_START_FAILURE_H
#define PW_START_FAILURE_H
/*
 * Why a helper did not reach READY: what its own log last said it was doing.
 *
 * A helper can die inside the kernel's module loader (the pager error at a fake-signed module's text; errno 92,
 * EPROTO, at the title) before Wine can report anything. It still leaves its log,
 * /data/ps5proton/logs/sessions/0/wine-child-<pid>.log (native/proton_wine_child.c):
 *   "wine-child: firmware status=N version=0x13400000"   once, first
 *   "wine-child: loading ntdll path=..."                 before ntdll's load, then "wine-child: ntdll loaded" (or
 *                                                        "ntdll load failed") after it
 *   "pw_wine_dl: loading <path>"                         before each later module's load (wine/ps5/pw_wine_dl.c), then
 *                                                        "pw_wine_dl: loaded|refused <path> handle=..." after it
 * A load whose "loading" line has nothing after it from the same writer is the one the helper died in. Any line that
 * ends a load clears the module, so a helper that died elsewhere names none. The title reads the file when a spawn
 * fails (native/proton_wine.cpp) and names the module and firmware in its notice instead of a bare errno.
 *
 * Portable: the parser reads a buffer, the reader a file; never aborts.
 */
#include <stddef.h>

enum { PW_START_FAILURE_MODULE = 128 };

typedef struct PwStartFailure {
    char module[PW_START_FAILURE_MODULE]; /* the module a load started for and never ended, "" when none */
    unsigned firmware;                    /* the console's version word, 0 when the log has none */
} PwStartFailure;

/* Scans bytes of text, line by line. 0 when it found an unfinished load or the firmware line; -1 when neither. */
int pw_start_failure_parse(const char *text, size_t bytes, PwStartFailure *out);

/* The same for the file at path. -1 also when it cannot be read. */
int pw_start_failure_read(const char *path, PwStartFailure *out);

/* The firmware word as "13.40" (its two high bytes are BCD digits). 0 for no version or a buffer under 6 bytes. */
int pw_start_failure_firmware(unsigned version, char *text, size_t size);

/* The notice: names the module and the firmware when known, and keeps the errno. */
void pw_start_failure_describe(const PwStartFailure *failure, int error, char *text, size_t size);

#endif
