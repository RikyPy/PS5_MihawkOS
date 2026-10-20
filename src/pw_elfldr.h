/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A request to websrv's payload launcher (POST /elfldr on the console's loopback, port 8080), which
 * starts an ELF as its own process through elfldr. The Wine side sends its helper processes this way
 * (wine/ps5/pw_payload_spawn.h); the title sends the resident Lapy daemon when no daemon answered its
 * /data request, so the library is there on every launch, not only after a host tool ran.
 *
 * The request is the HTTP head and the form fields (args, pipe=0, then the ELF part's header), the
 * ELF's bytes, and the closing boundary. pipe=0: the process does not hold the connection.
 */
#ifndef PW_ELFLDR_H
#define PW_ELFLDR_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PW_ELFLDR_PORT 8080

/* One encoder for title, Wine and payload uploads. It chooses a boundary
 * absent from the actual binary, so even a payload containing this encoder
 * cannot truncate itself in the receiver's multipart parser. No global state. */
typedef struct {
    char head[1536], tail[64];
    size_t head_size, tail_size;
} PwElfUpload;
/* Zero on success, -1 for malformed/oversized fields or unusable payload. */
int pw_elfldr_request(PwElfUpload *request, const char *name, const char *args,
                      const void *elf, size_t elf_size);

/* 1 when `reply` begins with an HTTP 200 status line. */
int pw_elfldr_accepted(const char *reply);

#ifdef __cplusplus
}
#endif

#endif
