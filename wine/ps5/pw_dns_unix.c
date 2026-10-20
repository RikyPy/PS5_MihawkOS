/* SPDX-License-Identifier: LGPL-2.1-or-later
 * DNSAPI Unix contract: the title supplies actual configured servers; c-ares
 * performs ordinary DNS packet queries. Wine retains parsing and Windows API
 * behavior. No synthesized records, addresses, search suffixes or TTLs.
 * Each query owns a channel and polls at most 20 seconds before cancellation.
 * Server overrides follow upstream's per-thread resolver state. Both call tables
 * use the same implementation; the WoW64 adapters are from pinned Wine libresolv.c.
 */
#include "config.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <poll.h>
#include <time.h>
#include <arpa/inet.h>
#include <ares.h>
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"
#include "winbase.h"
#include "windns.h"
#define USE_WS_PREFIX
#include "ws2def.h"
#include "ws2ipdef.h"
#include "dnsapi.h"
#include "pw_wine_process.h"

static pthread_once_t resolver_once = PTHREAD_ONCE_INIT;
static pthread_key_t server_key;
static int resolver_status;
static void initialize(void)
{
    resolver_status = ares_library_init(ARES_LIB_INIT_ALL);
    if (!resolver_status && pthread_key_create(&server_key, free)) resolver_status = ARES_ENOMEM;
}
static DNS_STATUS status_from_ares(int status)
{
    switch (status) {
    case ARES_SUCCESS: return ERROR_SUCCESS;
    case ARES_ENODATA: case ARES_ENOTFOUND: return DNS_ERROR_RCODE_NAME_ERROR;
    case ARES_EFORMERR: case ARES_EBADRESP: return DNS_ERROR_BAD_PACKET;
    case ARES_ESERVFAIL: return DNS_ERROR_RCODE_SERVER_FAILURE;
    case ARES_ENOTIMP: return DNS_ERROR_RCODE_NOT_IMPLEMENTED;
    case ARES_EREFUSED: return DNS_ERROR_RCODE_REFUSED;
    case ARES_ETIMEOUT: return ERROR_TIMEOUT;
    case ARES_ENOMEM: return ERROR_NOT_ENOUGH_MEMORY;
    case ARES_ENOSERVER: return DNS_ERROR_NO_DNS_SERVERS;
    case ARES_EBADNAME: case ARES_EBADQUERY: return ERROR_INVALID_PARAMETER;
    default: return DNS_ERROR_RCODE_SERVER_FAILURE;
    }
}
static NTSTATUS resolv_get_searchlist(void *args)
{
    const struct get_searchlist_params *p = args;
    DWORD available = *p->len;
    *p->len = sizeof(WCHAR);
    if (!p->list) return ERROR_SUCCESS;
    if (available < sizeof(WCHAR)) return ERROR_MORE_DATA;
    *p->list = 0; /* No configured search suffix is exposed by Sony's DNS API. */
    return ERROR_SUCCESS;
}
static NTSTATUS resolv_get_serverlist(void *args)
{
    const struct get_serverlist_params *p = args;
    struct pw_dns_config config;
    DWORD count = 0, available = *p->len;
    if (p->family != WS_AF_UNSPEC && p->family != WS_AF_INET && p->family != WS_AF_INET6)
        return ERROR_INVALID_PARAMETER;
    if (pw_wine_dns_config(&config)) return DNS_ERROR_NO_DNS_SERVERS;
    for (unsigned i = 0; i < config.count; ++i)
        if (p->family == WS_AF_UNSPEC || (config.servers[i].family == 4 ? WS_AF_INET : WS_AF_INET6) == p->family) ++count;
    if (!count) return DNS_ERROR_NO_DNS_SERVERS;
    DWORD needed = FIELD_OFFSET(DNS_ADDR_ARRAY, AddrArray[count]);
    *p->len = needed;
    if (!p->addrs) return ERROR_SUCCESS;
    if (available < needed) return ERROR_MORE_DATA;
    memset(p->addrs, 0, needed);
    p->addrs->AddrCount = p->addrs->MaxCount = count;
    count = 0;
    for (unsigned i = 0; i < config.count; ++i) {
        const struct pw_dns_server *server = &config.servers[i];
        if (p->family != WS_AF_UNSPEC && (server->family == 4 ? WS_AF_INET : WS_AF_INET6) != p->family) continue;
        DNS_ADDR *addr = &p->addrs->AddrArray[count++];
        if (server->family == 4) {
            SOCKADDR_IN *sa = (SOCKADDR_IN *)addr->MaxSa;
            sa->sin_family = WS_AF_INET;
            memcpy(&sa->sin_addr, server->address, 4);
            addr->Data.DnsAddrUserDword[0] = sizeof(*sa);
        } else {
            SOCKADDR_IN6 *sa = (SOCKADDR_IN6 *)addr->MaxSa;
            sa->sin6_family = WS_AF_INET6;
            memcpy(&sa->sin6_addr, server->address, 16);
            addr->Data.DnsAddrUserDword[0] = sizeof(*sa);
        }
    }
    return ERROR_SUCCESS;
}
static NTSTATUS resolv_set_serverlist(void *args)
{
    const IP4_ARRAY *addrs = args;
    pthread_once(&resolver_once, initialize);
    if (resolver_status) return status_from_ares(resolver_status);
    if (!addrs || !addrs->AddrCount) return ERROR_SUCCESS;
    if (addrs->AddrCount > 4) return ERROR_INVALID_PARAMETER;
    struct pw_dns_config *config = calloc(1, sizeof(*config));
    if (!config) return ERROR_NOT_ENOUGH_MEMORY;
    config->count = addrs->AddrCount;
    for (unsigned i = 0; i < config->count; ++i) {
        config->servers[i].family = 4;
        memcpy(config->servers[i].address, &addrs->AddrArray[i], 4);
    }
    void *old = pthread_getspecific(server_key);
    if (pthread_setspecific(server_key, config)) { free(config); return ERROR_NOT_ENOUGH_MEMORY; }
    free(old); return ERROR_SUCCESS;
}
struct answer { int done; DNS_STATUS status; unsigned char *buffer; DWORD capacity, length; };
static void query_done(void *opaque, int status, int timeouts, unsigned char *packet, int length)
{
    struct answer *answer = opaque;
    (void)timeouts;
    answer->done = 1; answer->status = status_from_ares(status);
    if (status || length < 0) return;
    answer->length = length;
    if (answer->capacity < (DWORD)length) answer->status = ERROR_MORE_DATA;
    else memcpy(answer->buffer, packet, length);
}
static long long milliseconds(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}
static NTSTATUS resolv_query(void *args)
{
    const struct query_params *p = args;
    struct pw_dns_config config, *override;
    struct ares_addr_port_node servers[4] = {{0}};
    struct in_addr ipv4[4];
    struct ares_options options = {0};
    ares_channel_t *channel = NULL;
    pthread_once(&resolver_once, initialize);
    if (resolver_status) return status_from_ares(resolver_status);
    override = pthread_getspecific(server_key);
    if (override) config = *override;
    else if (pw_wine_dns_config(&config)) return DNS_ERROR_NO_DNS_SERVERS;
    if (!config.count) return DNS_ERROR_NO_DNS_SERVERS;
    options.flags = ARES_FLAG_NOSEARCH;
    if (p->options & DNS_QUERY_USE_TCP_ONLY) options.flags |= ARES_FLAG_USEVC;
    if (p->options & DNS_QUERY_ACCEPT_TRUNCATED_RESPONSE) options.flags |= ARES_FLAG_IGNTC;
    if (p->options & DNS_QUERY_NO_RECURSION) options.flags |= ARES_FLAG_NORECURSE;
    options.timeout = 3000; options.tries = 2;
    options.servers = ipv4;
    for (unsigned i = 0; i < config.count; ++i) {
        servers[i].family = config.servers[i].family == 4 ? AF_INET : AF_INET6;
        memcpy(&servers[i].addr, config.servers[i].address, config.servers[i].family == 4 ? 4 : 16);
        servers[i].next = i + 1 < config.count ? &servers[i+1] : NULL;
        if (config.servers[i].family == 4) memcpy(&ipv4[options.nservers++], config.servers[i].address, 4);
    }
    int mask = ARES_OPT_FLAGS | ARES_OPT_TIMEOUTMS | ARES_OPT_TRIES;
    if (options.nservers) mask |= ARES_OPT_SERVERS;
    int status = ares_init_options(&channel, &options, mask);
    if (status) return status_from_ares(status);
    /* Replace any platform defaults before issuing a query, including IPv6-only networks. */
    status = ares_set_servers_ports(channel, servers);
    if (status) { ares_destroy(channel); return status_from_ares(status); }
    struct answer answer = {0, ERROR_TIMEOUT, p->buf, *p->len, 0};
    long long deadline = milliseconds() + 20000;
    ares_query(channel, p->name, 1, p->type, query_done, &answer);
    while (!answer.done && milliseconds() < deadline) {
        ares_socket_t sockets[ARES_GETSOCK_MAXNUM];
        struct pollfd fds[ARES_GETSOCK_MAXNUM];
        /* c-ares' public bit macros shift signed 1 into bit 31 at slot 15.
         * Interpret this fixed 16-read/16-write mask as unsigned instead. */
        uint32_t bits = (uint32_t)ares_getsock(channel, sockets, ARES_GETSOCK_MAXNUM);
        int count = 0;
        for (unsigned i = 0; i < ARES_GETSOCK_MAXNUM; ++i) {
            short events = ((bits & (1u << i)) ? POLLIN : 0) | ((bits & (1u << (i + ARES_GETSOCK_MAXNUM))) ? POLLOUT : 0);
            if (events) fds[count++] = (struct pollfd){sockets[i], events, 0};
        }
        struct timeval maximum = {0, 100000}, timeout;
        struct timeval *wait = ares_timeout(channel, &maximum, &timeout);
        int ready = poll(fds, count, (int)(wait->tv_sec * 1000 + (wait->tv_usec + 999) / 1000));
        if (ready <= 0) ares_process_fd(channel, ARES_SOCKET_BAD, ARES_SOCKET_BAD);
        else for (int i = 0; i < count; ++i)
            if (fds[i].revents) ares_process_fd(channel,
                fds[i].revents & (POLLIN|POLLERR|POLLHUP) ? fds[i].fd : ARES_SOCKET_BAD,
                fds[i].revents & POLLOUT ? fds[i].fd : ARES_SOCKET_BAD);
    }
    if (!answer.done) { ares_cancel(channel); answer.status = ERROR_TIMEOUT; }
    ares_destroy(channel);
    if (!answer.status || answer.status == ERROR_MORE_DATA) *p->len = answer.length;
    return answer.status;
}
const unixlib_entry_t __wine_unix_call_funcs[] = {
    resolv_get_searchlist, resolv_get_serverlist, resolv_set_serverlist, resolv_query
};
C_ASSERT(ARRAYSIZE(__wine_unix_call_funcs) == unix_funcs_count);

/* WoW64 marshaling: Wine, Copyright 2021 Hans Leidekker for CodeWeavers. */
#ifdef _WIN64

typedef ULONG PTR32;

static NTSTATUS wow64_resolv_get_searchlist( void *args )
{
    struct
    {
        PTR32  list;
        PTR32  len;
    } const *params32 = args;

    struct get_searchlist_params params =
    {
        ULongToPtr(params32->list),
        ULongToPtr(params32->len)
    };

    return resolv_get_searchlist( &params );
}

static NTSTATUS wow64_resolv_get_serverlist( void *args )
{
    struct
    {
        USHORT family;
        PTR32  addrs;
        PTR32  len;
    } const *params32 = args;

    struct get_serverlist_params params =
    {
        params32->family,
        ULongToPtr(params32->addrs),
        ULongToPtr(params32->len)
    };

    return resolv_get_serverlist( &params );
}

static NTSTATUS wow64_resolv_query( void *args )
{
    struct
    {
        PTR32  name;
        WORD   type;
        DWORD  options;
        PTR32  buf;
        PTR32  len;
    } const *params32 = args;

    struct query_params params =
    {
        ULongToPtr(params32->name),
        params32->type,
        params32->options,
        ULongToPtr(params32->buf),
        ULongToPtr(params32->len)
    };

    return resolv_query( &params );
}

const unixlib_entry_t __wine_unix_call_wow64_funcs[] =
{
    wow64_resolv_get_searchlist,
    wow64_resolv_get_serverlist,
    resolv_set_serverlist,
    wow64_resolv_query,
};

C_ASSERT( ARRAYSIZE(__wine_unix_call_wow64_funcs) == unix_funcs_count );

#endif  /* _WIN64 */

