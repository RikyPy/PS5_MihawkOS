/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pw_https.h"

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#if defined(__FreeBSD__) || defined(__PROSPERO__)
#include <sys/sysctl.h>
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define CONNECT_MS 15000
#define IDLE_S 30
#define MAX_REDIRECTS 5
#define HEAD_MAX 16384

struct PwHttps {
    mbedtls_x509_crt roots;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
};

/* ---- entropy: Mbed TLS asks here (MBEDTLS_ENTROPY_HARDWARE_ALT, platform/tls/pw_mbedtls_config.h) ---- */

static const char *entropy_source = "none";

int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen);
int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen)
{
    (void)data;
    *olen = 0;
    const int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        const ssize_t got = read(fd, output, len);
        close(fd);
        if (got > 0) {
            *olen = (size_t)got;
            entropy_source = "urandom";
            return 0;
        }
    }
#if defined(KERN_ARND)
    int name[2] = { CTL_KERN, KERN_ARND };
    size_t size = len;
    if (sysctl(name, 2, output, &size, NULL, 0) == 0 && size > 0) {
        *olen = size;
        entropy_source = "arandom";
        return 0;
    }
#endif
    return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
}

const char *pw_https_entropy_source(void) { return entropy_source; }

const char *pw_https_status_text(int status)
{
    switch (status) {
    case PW_HTTPS_OK: return "done";
    case PW_HTTPS_URL: return "the address is not one this console can download from";
    case PW_HTTPS_DNS: return "the server's name could not be found: check the network";
    case PW_HTTPS_CONNECT: return "the server could not be reached: check the network";
    case PW_HTTPS_TLS: return "the secure connection failed";
    case PW_HTTPS_UNTRUSTED: return "the server's certificate is not trusted";
    case PW_HTTPS_CLOCK: return "the server's certificate is not valid now: check the console's date and time";
    case PW_HTTPS_HOSTNAME: return "the server's certificate is for another name";
    case PW_HTTPS_STATUS: return "the server refused the download";
    case PW_HTTPS_PROTOCOL: return "the server's answer was malformed";
    case PW_HTTPS_TIMEOUT: return "the server stopped answering";
    case PW_HTTPS_LIMIT: return "the server sent more than expected";
    case PW_HTTPS_STOPPED: return "stopped";
    case PW_HTTPS_MEMORY: return "not enough memory";
    case PW_HTTPS_REDIRECTS: return "the server redirected too many times";
    case PW_HTTPS_ROOTS: return "the trusted certificates could not be read";
    case PW_HTTPS_IO: return "the connection broke";
    }
    return "unknown error";
}

int pw_https_create(PwHttps **out, const char *roots, size_t size)
{
    *out = NULL;
    PwHttps *https = calloc(1, sizeof(*https));
    if (!https) return PW_HTTPS_MEMORY;
    mbedtls_x509_crt_init(&https->roots);
    mbedtls_entropy_init(&https->entropy);
    mbedtls_ctr_drbg_init(&https->drbg);
    /* TLS 1.3 runs on PSA's crypto. */
    if (psa_crypto_init() != PSA_SUCCESS ||
        mbedtls_ctr_drbg_seed(&https->drbg, mbedtls_entropy_func, &https->entropy, (const unsigned char *)"MihawkOS", 8)) {
        pw_https_destroy(https);
        return PW_HTTPS_TLS;
    }
    int parsed;
    if (size) {
        char *copy = malloc(size + 1);   /* PEM is parsed as a string */
        if (!copy) {
            pw_https_destroy(https);
            return PW_HTTPS_MEMORY;
        }
        memcpy(copy, roots, size);
        copy[size] = 0;
        parsed = mbedtls_x509_crt_parse(&https->roots, (const unsigned char *)copy, size + 1);
        free(copy);
    } else {
        parsed = mbedtls_x509_crt_parse_file(&https->roots, roots);
    }
    /* A positive count is the certificates that did not parse; the rest are kept. */
    if (parsed < 0 || !https->roots.raw.p) {
        pw_https_destroy(https);
        return PW_HTTPS_ROOTS;
    }
    *out = https;
    return PW_HTTPS_OK;
}

void pw_https_destroy(PwHttps *https)
{
    if (!https) return;
    mbedtls_x509_crt_free(&https->roots);
    mbedtls_ctr_drbg_free(&https->drbg);
    mbedtls_entropy_free(&https->entropy);
    free(https);
}

/* ---- one connection ---- */

typedef struct Connection {
    int fd, tls, timed_out;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config config;
    char buffer[HEAD_MAX];
    size_t at, end;
} Connection;

static int tls_send(void *context, const unsigned char *data, size_t size)
{
    Connection *c = context;
    const ssize_t sent = send(c->fd, data, size, MSG_NOSIGNAL);
    if (sent >= 0) return (int)sent;
    if (errno == EAGAIN || errno == EWOULDBLOCK) c->timed_out = 1;
    return errno == EINTR ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}

static int tls_recv(void *context, unsigned char *data, size_t size)
{
    Connection *c = context;
    const ssize_t got = recv(c->fd, data, size, 0);
    if (got >= 0) return (int)got;
    if (errno == EAGAIN || errno == EWOULDBLOCK) c->timed_out = 1;
    return errno == EINTR ? MBEDTLS_ERR_SSL_WANT_READ : MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}

/* Bytes in, 0 at the end of the stream, or a PW_HTTPS_* failure. */
static int raw_read(Connection *c, void *data, size_t size, PwHttpsResult *result)
{
    for (;;) {
        int got;
        if (c->tls) {
            got = mbedtls_ssl_read(&c->ssl, data, size);
            if (got == MBEDTLS_ERR_SSL_WANT_READ || got == MBEDTLS_ERR_SSL_WANT_WRITE ||
                got == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)
                continue;
            if (got == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return 0;
        } else {
            got = (int)recv(c->fd, data, size, 0);
            if (got < 0 && errno == EINTR) continue;
            if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) c->timed_out = 1;
        }
        if (got >= 0) return got;
        result->tls_error = c->tls ? got : 0;
        result->sys_errno = errno;
        return c->timed_out ? PW_HTTPS_TIMEOUT : PW_HTTPS_IO;
    }
}

static int write_all(Connection *c, const char *data, size_t size, PwHttpsResult *result)
{
    while (size) {
        const int sent = c->tls ? mbedtls_ssl_write(&c->ssl, (const unsigned char *)data, size)
                                : (int)send(c->fd, data, size, MSG_NOSIGNAL);
        if (c->tls && (sent == MBEDTLS_ERR_SSL_WANT_READ || sent == MBEDTLS_ERR_SSL_WANT_WRITE)) continue;
        if (sent <= 0) {
            result->tls_error = c->tls ? sent : 0;
            result->sys_errno = errno;
            return c->timed_out ? PW_HTTPS_TIMEOUT : PW_HTTPS_IO;
        }
        data += sent;
        size -= (size_t)sent;
    }
    return PW_HTTPS_OK;
}

/* Buffered: the next byte, or a negative PW_HTTPS_* (PW_HTTPS_PROTOCOL at a stream that ended early). */
static int next_byte(Connection *c, PwHttpsResult *result)
{
    if (c->at == c->end) {
        const int got = raw_read(c, c->buffer, sizeof(c->buffer), result);
        if (got <= 0) return got ? got : PW_HTTPS_PROTOCOL;
        c->at = 0, c->end = (size_t)got;
    }
    return (unsigned char)c->buffer[c->at++];
}

/* One line without its CRLF, at most `capacity` - 1 bytes. */
static int read_line(Connection *c, char *line, size_t capacity, PwHttpsResult *result)
{
    size_t used = 0;
    for (;;) {
        const int byte = next_byte(c, result);
        if (byte < 0) return byte;
        if (byte == '\n') break;
        if (used + 1 >= capacity) return PW_HTTPS_PROTOCOL;
        line[used++] = (char)byte;
    }
    if (used && line[used - 1] == '\r') used--;
    line[used] = 0;
    return PW_HTTPS_OK;
}

/* Up to `want` body bytes to the sink: buffered ones first, then straight from the connection. `until_end` reads to
 * the stream's end instead. */
static int body(Connection *c, uint64_t want, int until_end, uint64_t limit, pw_https_sink_fn sink, void *context,
                PwHttpsResult *result)
{
    while (until_end || want) {
        if (c->at == c->end) {
            const int got = raw_read(c, c->buffer, sizeof(c->buffer), result);
            if (got < 0) return got;
            if (got == 0) return until_end ? PW_HTTPS_OK : PW_HTTPS_PROTOCOL;
            c->at = 0, c->end = (size_t)got;
        }
        size_t take = c->end - c->at;
        if (!until_end && take > want) take = (size_t)want;
        if (result->bytes + take > limit) return PW_HTTPS_LIMIT;
        if (sink(context, c->buffer + c->at, take)) return PW_HTTPS_STOPPED;
        c->at += take;
        result->bytes += take;
        if (!until_end) want -= take;
    }
    return PW_HTTPS_OK;
}

typedef struct Url {
    int tls;
    char host[256];
    unsigned port;
    const char *path;
} Url;

static int parse_url(const char *url, Url *out)
{
    const char *rest;
    if (!strncmp(url, "https://", 8)) out->tls = 1, out->port = 443, rest = url + 8;
    else if (!strncmp(url, "http://", 7)) out->tls = 0, out->port = 80, rest = url + 7;
    else return PW_HTTPS_URL;
    const size_t authority = strcspn(rest, "/?#");
    const char *colon = memchr(rest, ':', authority);
    const size_t host = colon ? (size_t)(colon - rest) : authority;
    if (!host || host >= sizeof(out->host) || memchr(rest, '@', authority) || memchr(rest, '[', authority))
        return PW_HTTPS_URL;
    memcpy(out->host, rest, host);
    out->host[host] = 0;
    if (colon) {
        char *end;
        const unsigned long port = strtoul(colon + 1, &end, 10);
        if (end != rest + authority || !port || port > 65535) return PW_HTTPS_URL;
        out->port = (unsigned)port;
    }
    out->path = rest[authority] == '/' ? rest + authority : "/";
    for (const char *p = out->path; *p; p++)
        if ((unsigned char)*p <= 0x20 || *p == 0x7f) return PW_HTTPS_URL;
    return PW_HTTPS_OK;
}

static int open_connection(PwHttps *https, const Url *url, pw_https_resolve_fn resolve, Connection *c,
                           PwHttpsResult *result)
{
    struct sockaddr_in address = { 0 };
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)url->port);
    if (inet_pton(AF_INET, url->host, &address.sin_addr) != 1) {
        uint8_t found[4];
        if (!resolve || resolve(url->host, found)) return PW_HTTPS_DNS;
        memcpy(&address.sin_addr, found, 4);
    }
    c->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (c->fd < 0) return result->sys_errno = errno, PW_HTTPS_CONNECT;
    const int flags = fcntl(c->fd, F_GETFL);
    fcntl(c->fd, F_SETFL, flags | O_NONBLOCK);
    if (connect(c->fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        if (errno != EINPROGRESS) return result->sys_errno = errno, PW_HTTPS_CONNECT;
        struct pollfd wait = { c->fd, POLLOUT, 0 };
        int error = 0;
        socklen_t length = sizeof(error);
        if (poll(&wait, 1, CONNECT_MS) != 1 || getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &error, &length) || error)
            return result->sys_errno = error ? error : ETIMEDOUT, PW_HTTPS_CONNECT;
    }
    fcntl(c->fd, F_SETFL, flags);
    struct timeval idle = { IDLE_S, 0 };
    setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &idle, sizeof(idle));
    setsockopt(c->fd, SOL_SOCKET, SO_SNDTIMEO, &idle, sizeof(idle));
    if (!url->tls) return PW_HTTPS_OK;
    c->tls = 1;
    mbedtls_ssl_init(&c->ssl);
    mbedtls_ssl_config_init(&c->config);
    if (mbedtls_ssl_config_defaults(&c->config, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT))
        return PW_HTTPS_TLS;
    mbedtls_ssl_conf_authmode(&c->config, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&c->config, &https->roots, NULL);
    mbedtls_ssl_conf_rng(&c->config, mbedtls_ctr_drbg_random, &https->drbg);
    if (mbedtls_ssl_setup(&c->ssl, &c->config) || mbedtls_ssl_set_hostname(&c->ssl, url->host)) return PW_HTTPS_TLS;
    mbedtls_ssl_set_bio(&c->ssl, c, tls_send, tls_recv, NULL);
    int shaken;
    while ((shaken = mbedtls_ssl_handshake(&c->ssl)) == MBEDTLS_ERR_SSL_WANT_READ ||
           shaken == MBEDTLS_ERR_SSL_WANT_WRITE) {
    }
    if (!shaken) return PW_HTTPS_OK;
    result->tls_error = shaken;
    result->verify_flags = mbedtls_ssl_get_verify_result(&c->ssl);
    if (c->timed_out) return PW_HTTPS_TIMEOUT;
    const uint32_t flags_seen = result->verify_flags == (uint32_t)-1 ? 0 : result->verify_flags;
    if (flags_seen & (MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_FUTURE)) return PW_HTTPS_CLOCK;
    if (flags_seen & MBEDTLS_X509_BADCERT_CN_MISMATCH) return PW_HTTPS_HOSTNAME;
    if (flags_seen) return PW_HTTPS_UNTRUSTED;
    return PW_HTTPS_TLS;
}

static void close_connection(Connection *c)
{
    if (c->tls) {
        mbedtls_ssl_close_notify(&c->ssl);
        mbedtls_ssl_free(&c->ssl);
        mbedtls_ssl_config_free(&c->config);
    }
    if (c->fd >= 0) close(c->fd);
}

/* The header's value when `line` is that header ("Name: value"), else NULL. */
static const char *header(const char *line, const char *name)
{
    const size_t length = strlen(name);
    if (strncasecmp(line, name, length) || line[length] != ':') return NULL;
    line += length + 1;
    while (*line == ' ' || *line == '\t') line++;
    return line;
}

int pw_https_get(PwHttps *https, const char *url, uint64_t limit, pw_https_resolve_fn resolve, pw_https_sink_fn sink,
                 void *context, PwHttpsResult *result)
{
    memset(result, 0, sizeof(*result));
    if (!https || !url || !sink) return result->status = PW_HTTPS_URL;
    char current[2048], location[2048];
    if (snprintf(current, sizeof(current), "%s", url) >= (int)sizeof(current)) return result->status = PW_HTTPS_URL;
    for (;;) {
        Url target;
        int status = parse_url(current, &target);
        if (status) return result->status = status;
        snprintf(result->host, sizeof(result->host), "%s", target.host);
        Connection *c = calloc(1, sizeof(*c));
        if (!c) return result->status = PW_HTTPS_MEMORY;
        c->fd = -1;
        status = open_connection(https, &target, resolve, c, result);
        char request[2560];
        const int head = snprintf(request, sizeof(request),
                                  "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: MihawkOS/1\r\nAccept-Encoding: identity\r\n"
                                  "Connection: close\r\n\r\n", target.path, target.host);
        if (!status && (head <= 0 || head >= (int)sizeof(request))) status = PW_HTTPS_URL;
        if (!status) status = write_all(c, request, (size_t)head, result);
        char line[HEAD_MAX];
        if (!status) status = read_line(c, line, sizeof(line), result);
        int code = 0, chunked = 0, have_length = 0;
        uint64_t length = 0;
        location[0] = 0;
        if (!status && (sscanf(line, "HTTP/1.%*d %d", &code) != 1 || code < 100 || code > 999)) status = PW_HTTPS_PROTOCOL;
        result->http = code;
        while (!status && !(status = read_line(c, line, sizeof(line), result)) && line[0]) {
            const char *value;
            if ((value = header(line, "Content-Length"))) {
                char *end;
                length = strtoull(value, &end, 10);
                have_length = end != value && !*end;
                if (!have_length) status = PW_HTTPS_PROTOCOL;
            } else if ((value = header(line, "Transfer-Encoding"))) {
                chunked = strstr(value, "chunked") != NULL;
            } else if ((value = header(line, "Location"))) {
                snprintf(location, sizeof(location), "%s", value);
            }
        }
        const int redirect = code == 301 || code == 302 || code == 303 || code == 307 || code == 308;
        if (!status && redirect) {
            close_connection(c);
            free(c);
            if (++result->redirects > MAX_REDIRECTS) return result->status = PW_HTTPS_REDIRECTS;
            if (!location[0]) return result->status = PW_HTTPS_PROTOCOL;
            if (location[0] == '/') {   /* the same server */
                char base[400];
                snprintf(base, sizeof(base), "%s://%s:%u", target.tls ? "https" : "http", target.host, target.port);
                if (snprintf(current, sizeof(current), "%s%s", base, location) >= (int)sizeof(current))
                    return result->status = PW_HTTPS_URL;
            } else {
                snprintf(current, sizeof(current), "%s", location);
            }
            continue;
        }
        if (!status && code != 200) status = PW_HTTPS_STATUS;
        if (!status && have_length && length > limit) status = PW_HTTPS_LIMIT;
        if (!status && chunked) {
            for (;;) {
                if ((status = read_line(c, line, sizeof(line), result))) break;
                char *end;
                const uint64_t size = strtoull(line, &end, 16);
                if (end == line || (*end && *end != ';')) {
                    status = PW_HTTPS_PROTOCOL;
                    break;
                }
                if (!size) {   /* the trailers, to their empty line */
                    while (!(status = read_line(c, line, sizeof(line), result)) && line[0]) {
                    }
                    break;
                }
                if ((status = body(c, size, 0, limit, sink, context, result)) ||
                    (status = read_line(c, line, sizeof(line), result)))
                    break;
                if (line[0]) {
                    status = PW_HTTPS_PROTOCOL;
                    break;
                }
            }
        } else if (!status) {
            status = body(c, length, !have_length, limit, sink, context, result);
        }
        close_connection(c);
        free(c);
        return result->status = status;
    }
}
