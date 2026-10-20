/* SPDX-License-Identifier: GPL-3.0-or-later */
/* tests/test_pw_https.py's client: one pw_https_get, every name resolved to 127.0.0.1, the body hashed.
 *   pw_https_driver ROOTS.pem URL LIMIT  ->  status=.. http=.. bytes=.. sha256=.. verify=.. redirects=.. host=.. */
#include "../src/pw_https.h"
#include "../src/pw_sha256.h"

#include <stdio.h>
#include <stdlib.h>

static int loopback(const char *host, uint8_t address[4])
{
    (void)host;
    address[0] = 127, address[1] = 0, address[2] = 0, address[3] = 1;
    return 0;
}

static int hash(void *context, const void *data, size_t size)
{
    pw_sha256_update(context, data, size);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 4) return 2;
    PwHttps *https;
    const int created = pw_https_create(&https, argv[1], 0);
    if (created) {
        printf("status=%d\n", created);
        return 0;
    }
    PwSha256 sha;
    pw_sha256_init(&sha);
    PwHttpsResult result;
    pw_https_get(https, argv[2], strtoull(argv[3], NULL, 10), loopback, hash, &sha, &result);
    uint8_t digest[32];
    char hex[65];
    pw_sha256_final(&sha, digest);
    pw_sha256_hex(digest, hex);
    printf("status=%d http=%d bytes=%llu sha256=%s verify=%x redirects=%d host=%s entropy=%s text=%s\n", result.status,
           result.http, (unsigned long long)result.bytes, hex, result.verify_flags, result.redirects, result.host,
           pw_https_entropy_source(), pw_https_status_text(result.status));
    pw_https_destroy(https);
    return 0;
}
