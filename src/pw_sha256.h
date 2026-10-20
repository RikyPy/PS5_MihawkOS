/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_SHA256_H
#define PW_SHA256_H

/* SHA-256 (FIPS 180-4) for the runtime install (src/pw_pack.h), which hashes every file as it streams. Written here
 * rather than vendored: the console's libraries carry no implementation (the payload SDK has FreeBSD's sha256.h and
 * no code behind it). tests/test_pw_sha256.c checks it against FIPS 180-4's examples and Python's hashlib.
 * Portable, no allocation. */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PwSha256 {
    uint32_t state[8];
    uint64_t bytes;
    uint8_t block[64];
} PwSha256;

void pw_sha256_init(PwSha256 *context);
void pw_sha256_update(PwSha256 *context, const void *data, size_t size);
void pw_sha256_final(PwSha256 *context, uint8_t digest[32]);
/* The digest as 64 lowercase hex digits and a NUL. */
void pw_sha256_hex(const uint8_t digest[32], char hex[65]);

#ifdef __cplusplus
}
#endif

#endif
