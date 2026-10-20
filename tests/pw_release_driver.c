/* SPDX-License-Identifier: GPL-3.0-or-later */
/* tests/test_sign_release.py's view of src/pw_release.c: pw_release_driver MANIFEST PUBLIC-HEX [RUNNING-VERSION]
 * prints "ok <version> <base> <title> <runtime> <newer>" or "status <n>". */
#include "../src/pw_release.h"

#include <stdio.h>

int main(int argc, char **argv)
{
    static char text[1 << 16];
    uint8_t key[32];
    FILE *file = argc >= 3 ? fopen(argv[1], "rb") : NULL;
    if (!file || !pw_release_hex32(argv[2], key)) return 2;
    const size_t size = fread(text, 1, sizeof(text), file);
    fclose(file);
    PwRelease release;
    const int status = pw_release_parse(text, size, key, &release);
    if (status != PW_RELEASE_OK) {
        printf("status %d\n", status);
        return 0;
    }
    printf("ok %s %s %s %s %d\n", release.version, release.base, release.title, release.runtime,
           argc > 3 ? pw_release_version_compare(release.version, argv[3]) > 0 : -1);
    return 0;
}
