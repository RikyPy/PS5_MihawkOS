/* SPDX-License-Identifier: GPL-3.0-or-later
 * FreeBSD interface-name APIs missing from Sony's libc exports. Use the real
 * getifaddrs snapshot and AF_LINK indices; never invent an adapter. The caller
 * owns the zero-terminated array and names until if_freenameindex. */
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <stdlib.h>
#include <string.h>

void if_freenameindex(struct if_nameindex *names)
{
    if (!names) return;
    for (size_t i = 0; names[i].if_index; ++i) free(names[i].if_name);
    free(names);
}

struct if_nameindex *if_nameindex(void)
{
    struct ifaddrs *addresses = NULL, *a;
    struct if_nameindex *names;
    size_t count = 0, used = 0;
    if (getifaddrs(&addresses)) return NULL;
    for (a = addresses; a; a = a->ifa_next)
        if (a->ifa_addr && a->ifa_addr->sa_family == AF_LINK && a->ifa_name) ++count;
    names = calloc(count + 1, sizeof(*names));
    if (!names) { freeifaddrs(addresses); return NULL; }
    for (a = addresses; a; a = a->ifa_next) {
        if (!a->ifa_addr || a->ifa_addr->sa_family != AF_LINK || !a->ifa_name) continue;
        unsigned index = ((struct sockaddr_dl *)a->ifa_addr)->sdl_index;
        if (!index) continue;
        size_t i;
        for (i = 0; i < used && names[i].if_index != index; ++i) {}
        if (i != used) continue;
        names[used].if_name = strdup(a->ifa_name);
        if (!names[used].if_name) {
            freeifaddrs(addresses); if_freenameindex(names); errno = ENOMEM; return NULL;
        }
        names[used++].if_index = index;
    }
    freeifaddrs(addresses);
    return names;
}

unsigned int if_nametoindex(const char *name)
{
    unsigned index = 0;
    struct if_nameindex *names = if_nameindex();
    if (!names) return 0;
    for (size_t i = 0; names[i].if_index; ++i)
        if (!strcmp(name, names[i].if_name)) { index = names[i].if_index; break; }
    if_freenameindex(names);
    if (!index) errno = ENXIO;
    return index;
}

char *if_indextoname(unsigned int index, char *name)
{
    struct if_nameindex *names = if_nameindex();
    char *found = NULL;
    if (!names) return NULL;
    for (size_t i = 0; names[i].if_index; ++i)
        if (names[i].if_index == index) {
            strncpy(name, names[i].if_name, IF_NAMESIZE - 1);
            name[IF_NAMESIZE - 1] = 0; found = name; break;
        }
    if_freenameindex(names);
    if (!found) errno = ENXIO;
    return found;
}
