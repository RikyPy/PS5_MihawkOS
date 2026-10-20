/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pw_elfldr.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static int plain(const char *word)
{
    return word && !strpbrk(word, "\"\r\n");
}
static int contains(const unsigned char *bytes, size_t size, const char *word)
{
    size_t count = strlen(word);
    if(size < count) return 0;
    for(size_t i = 0; i <= size - count; ++i)
        if(bytes[i] == (unsigned char)word[0] && !memcmp(bytes + i, word, count)) return 1;
    return 0;
}
int pw_elfldr_request(PwElfUpload *r, const char *name, const char *args,
                      const void *elf, size_t elf_size)
{
    if(!r) return -1;
    memset(r, 0, sizeof(*r));
    if(!plain(name) || !plain(args) || !*name || !elf || !elf_size || elf_size > (4u << 20)) return -1;
    char boundary[32];
    /* At most size+1 attempts: each colliding distinct candidate occupies
     * bytes in the bounded ELF. In practice the first or second passes. */
    for(size_t serial = 0; ; ++serial) {
        snprintf(boundary, sizeof(boundary), "ProtonUpload%zu", serial);
        if(!contains(elf, elf_size, boundary)) break;
        if(serial >= elf_size) return -1;
    }
    char fields[768];
    int fields_size = snprintf(fields, sizeof(fields),
        "--%s\r\nContent-Disposition: form-data; name=\"args\"\r\n\r\n%s\r\n"
        "--%s\r\nContent-Disposition: form-data; name=\"pipe\"\r\n\r\n0\r\n"
        "--%s\r\nContent-Disposition: form-data; name=\"elf\"; filename=\"%s\"\r\n\r\n",
        boundary, args, boundary, boundary, name);
    int tail_size = snprintf(r->tail, sizeof(r->tail), "\r\n--%s--\r\n", boundary);
    if(fields_size <= 0 || (size_t)fields_size >= sizeof(fields) ||
       tail_size <= 0 || (size_t)tail_size >= sizeof(r->tail)) return -1;
    int head_size = snprintf(r->head, sizeof(r->head),
        "POST /elfldr HTTP/1.1\r\nHost: localhost\r\nContent-Type: multipart/form-data; boundary=%s\r\n"
        "Content-Length: %zu\r\nConnection: close\r\n\r\n%s",
        boundary, (size_t)fields_size + elf_size + (size_t)tail_size, fields);
    if(head_size <= 0 || (size_t)head_size >= sizeof(r->head)) return -1;
    r->head_size = head_size; r->tail_size = tail_size;
    return 0;
}
int pw_elfldr_accepted(const char *reply)
{
    return !strncmp(reply, "HTTP/1.", 7) && reply[7] && !strncmp(reply + 8, " 200", 4) &&
           (reply[12] == ' ' || reply[12] == '\r' || reply[12] == '\n' || !reply[12]);
}
