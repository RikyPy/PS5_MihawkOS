/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_HTTPS_H
#define PW_HTTPS_H

/* One HTTP or HTTPS GET with the title's own TLS (docs/SELF_INSTALL.md, phase 6). An elevated title's sceSsl
 * rejects every public certificate (0x8095f00c, evidence/probe-install-2026-10-18-02,
 * ps5-native-app-boilerplate/docs/CURL.md), so this is Mbed TLS 3.6.7 (tools/setup-mbedtls.sh) under a small
 * HTTP/1.1 client:
 * - certificates and the hostname are always checked, against the roots the caller gives (the console's
 *   CA_LIST.cer, or a pinned bundle);
 * - 15 s to connect, 30 s without a byte;
 * - up to 5 redirects (GitHub's release downloads redirect once);
 * - Content-Length, chunked and read-to-close bodies, handed to the sink as they come, more than `limit` refused.
 * A failure is typed, for one sentence on the screen. Name lookups are the caller's: the title's go through
 * sceNetResolver, because getaddrinfo is a null import there. Portable C over BSD sockets; host-tested by
 * tests/test_pw_https.py against the PC's own TLS servers. One PwHttps is used by one thread at a time. */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    PW_HTTPS_OK = 0,
    PW_HTTPS_URL = -1,          /* not an http:// or https:// URL this client takes */
    PW_HTTPS_DNS = -2,          /* the name did not resolve */
    PW_HTTPS_CONNECT = -3,      /* no connection (refused, unreachable, or 15 s without one) */
    PW_HTTPS_TLS = -4,          /* the TLS handshake failed for a reason other than the certificate */
    PW_HTTPS_UNTRUSTED = -5,    /* the certificate does not chain to a trusted root */
    PW_HTTPS_CLOCK = -6,        /* the certificate is expired or not yet valid: often a console clock that is wrong */
    PW_HTTPS_HOSTNAME = -7,     /* the certificate is for another name */
    PW_HTTPS_STATUS = -8,       /* the server answered, but not 200 */
    PW_HTTPS_PROTOCOL = -9,     /* a malformed HTTP answer */
    PW_HTTPS_TIMEOUT = -10,     /* 30 s without a byte */
    PW_HTTPS_LIMIT = -11,       /* more bytes than the caller allows */
    PW_HTTPS_STOPPED = -12,     /* the sink asked to stop */
    PW_HTTPS_MEMORY = -13,
    PW_HTTPS_REDIRECTS = -14,   /* more than 5 redirects */
    PW_HTTPS_ROOTS = -15,       /* the trusted roots could not be read */
    PW_HTTPS_IO = -16,          /* the connection broke */
};

const char *pw_https_status_text(int status);

typedef struct PwHttpsResult {
    int status;                 /* PW_HTTPS_* */
    int http;                   /* the last HTTP status, 0 before one */
    uint32_t verify_flags;      /* Mbed TLS's MBEDTLS_X509_BADCERT_* after a refused certificate */
    int tls_error;              /* Mbed TLS's code after a TLS failure */
    int sys_errno;
    int redirects;
    uint64_t bytes;             /* body bytes handed to the sink */
    char host[256];             /* the host the body came from */
} PwHttpsResult;

/* 0 and the address, or nonzero: the caller's name lookup. */
typedef int (*pw_https_resolve_fn)(const char *host, uint8_t address[4]);
/* 0 to go on, nonzero to stop. */
typedef int (*pw_https_sink_fn)(void *context, const void *data, size_t size);

typedef struct PwHttps PwHttps;

/* The trusted roots: a PEM file (`size` 0) or PEM bytes. PW_HTTPS_OK, PW_HTTPS_ROOTS or PW_HTTPS_MEMORY. */
int pw_https_create(PwHttps **out, const char *roots, size_t size);
int pw_https_get(PwHttps *https, const char *url, uint64_t limit, pw_https_resolve_fn resolve, pw_https_sink_fn sink,
                 void *context, PwHttpsResult *result);
void pw_https_destroy(PwHttps *https);

/* Where the entropy came from, after the first TLS connection: "urandom", "arandom" or "none". */
const char *pw_https_entropy_source(void);

#ifdef __cplusplus
}
#endif

#endif
