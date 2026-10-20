/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/pw_elfldr.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    PwElfUpload request;
    const char elf[] = "owned binary\0\r\n--ProtonUpload0--\r\nProtonUpload1\0\xff";
    assert(!pw_elfldr_request(&request, "lapy-root-daemon.elf", "", elf, sizeof(elf)));
    char *head = request.head;
    assert(request.head_size == strlen(head));
    assert(!strncmp(head, "POST /elfldr HTTP/1.1\r\n", 23));
    /* Binary embedded delimiters must not terminate the upload: scan beyond NUL. */
    assert(strstr(head, "boundary=ProtonUpload2\r\n"));
    assert(strstr(head, "name=\"pipe\"\r\n\r\n0\r\n"));
    assert(strstr(head, "filename=\"lapy-root-daemon.elf\"\r\n\r\n"));
    const char *body = strstr(head, "\r\n\r\n") + 4;
    const char *length = strstr(head, "Content-Length: ");
    assert(length && length < body);
    assert(strtoul(length + 16, NULL, 10) == strlen(body) + sizeof(elf) + request.tail_size);
    assert(!strcmp(head + request.head_size - 4, "\r\n\r\n"));
    assert(!strcmp(request.tail, "\r\n--ProtonUpload2--\r\n"));
    assert(request.tail_size == strlen(request.tail));
    assert(!pw_elfldr_request(&request, "child.elf", "pp0123", "ELF", 3));
    assert(strstr(request.head, "\r\n\r\npp0123\r\n"));
    assert(strstr(request.head, "boundary=ProtonUpload0\r\n"));
    assert(pw_elfldr_request(&request, "a\"b", "", elf, sizeof(elf)));
    assert(pw_elfldr_request(&request, "x.elf", "one\r\ntwo", elf, sizeof(elf)));
    assert(pw_elfldr_request(&request, "", "", elf, sizeof(elf)));
    assert(pw_elfldr_request(&request, "x.elf", "", NULL, 1));
    assert(pw_elfldr_request(&request, "x.elf", "", elf, 0));
    assert(pw_elfldr_request(&request, "x.elf", "", elf, (4u << 20) + 1));
    char oversized[1024]; memset(oversized, 'x', sizeof(oversized)); oversized[1023] = 0;
    assert(pw_elfldr_request(&request, "x.elf", oversized, elf, sizeof(elf)));
    assert(pw_elfldr_accepted("HTTP/1.1 200 OK\r\n"));
    assert(pw_elfldr_accepted("HTTP/1.0 200 OK\r\n"));
    assert(!pw_elfldr_accepted("HTTP/1.1 500 Internal Server Error\r\n"));
    assert(!pw_elfldr_accepted("HTTP/1.1 2000\r\n"));
    assert(!pw_elfldr_accepted(""));
    puts("pw_elfldr: ok");
    return 0;
}
