/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pw_start_failure.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* One log line, NUL-terminated. A marker counts anywhere in it: ntdll's fprintf reaches the log in pieces, so another
 * writer's text can come before a line written whole (gate-2026-10-19-splice-01). */
static void scan_line(const char *line, PwStartFailure *out)
{
    const char *at;
    if ((at = strstr(line, "wine-child: firmware ")) && (at = strstr(at, "version="))) {
        out->firmware = (unsigned)strtoul(at + 8, NULL, 16);
    } else if (strstr(line, "wine-child: loading ntdll")) {
        strcpy(out->module, "ntdll.prx");
    } else if ((at = strstr(line, "pw_wine_dl: loading "))) {
        const char *name = at + 20, *slash = strrchr(name, '/');
        if (slash) name = slash + 1;
        snprintf(out->module, sizeof(out->module), "%.*s", (int)strcspn(name, "\r\n"), name);
    } else if (strstr(line, "wine-child: ntdll load") || strstr(line, "pw_wine_dl: ")) {
        out->module[0] = 0;   /* that load ended: loaded, refused or reported */
    }
}

int pw_start_failure_parse(const char *text, size_t bytes, PwStartFailure *out)
{
    char line[1024];
    if (!text || !out) return -1;
    memset(out, 0, sizeof(*out));
    for (size_t at = 0; at < bytes;) {
        size_t end = at;
        while (end < bytes && text[end] != '\n') end++;
        const size_t kept = end - at < sizeof(line) ? end - at : sizeof(line) - 1;
        memcpy(line, text + at, kept);
        line[kept] = 0;
        scan_line(line, out);
        at = end + 1;
    }
    return out->module[0] || out->firmware ? 0 : -1;
}

int pw_start_failure_read(const char *path, PwStartFailure *out)
{
    char line[1024];
    FILE *file;
    if (!path || !out) return -1;
    memset(out, 0, sizeof(*out));
    if (!(file = fopen(path, "r"))) return -1;
    while (fgets(line, sizeof(line), file)) scan_line(line, out);
    fclose(file);
    return out->module[0] || out->firmware ? 0 : -1;
}

int pw_start_failure_firmware(unsigned version, char *text, size_t size)
{
    if (!text || size < 6 || !version) return 0;
    return snprintf(text, size, "%02x.%02x", (version >> 24) & 0xffu, (version >> 16) & 0xffu) > 0;
}

void pw_start_failure_describe(const PwStartFailure *failure, int error, char *text, size_t size)
{
    char firmware[8];
    const int named = failure && failure->module[0];
    const int dated = failure && pw_start_failure_firmware(failure->firmware, firmware, sizeof(firmware));
    if (!text || !size) return;
    if (named && dated)
        snprintf(text, size, "A program could not start (errno %d): its helper stopped while loading %s on firmware %s.",
                 error, failure->module, firmware);
    else if (named)
        snprintf(text, size, "A program could not start (errno %d): its helper stopped while loading %s.", error,
                 failure->module);
    else if (dated)
        snprintf(text, size, "A program could not start (errno %d) on firmware %s.", error, firmware);
    else
        snprintf(text, size, "A program could not start (errno %d).", error);
}
