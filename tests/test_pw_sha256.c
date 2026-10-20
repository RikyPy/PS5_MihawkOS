/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/pw_sha256.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void digest_of(const void *data, size_t size, size_t piece, char hex[65])
{
    PwSha256 context;
    uint8_t digest[32];
    pw_sha256_init(&context);
    for (size_t done = 0; done < size; done += piece)
        pw_sha256_update(&context, (const uint8_t *)data + done, size - done < piece ? size - done : piece);
    pw_sha256_final(&context, digest);
    pw_sha256_hex(digest, hex);
}

int main(void)
{
    char hex[65];
    /* FIPS 180-4's examples. */
    digest_of("abc", 3, 3, hex);
    assert(!strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    const char *two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    digest_of(two, strlen(two), 7, hex);
    assert(!strcmp(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    char *million = malloc(1000000);
    memset(million, 'a', 1000000);
    digest_of(million, 1000000, 4093, hex);
    assert(!strcmp(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
    free(million);
    /* Python's hashlib over (i * 31 + 7) & 255, at every padding edge; each fed whole and in odd pieces. */
    static const struct { size_t size; const char *hex; } known[] = {
        { 0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" },
        { 1, "ca358758f6d27e6cf45272937977a748fd88391db679ceda7dc7bf1f005ee879" },
        { 55, "8aa994584139d128848eeebc4e815639ba5ab6e6e39574195a63ac4f14f7c43b" },
        { 56, "ad574708f75c044c9b85de64cb568ee7711ff4f36448c6242f053ba8f6cc2b63" },
        { 57, "5b46e502092be01b1100193e089fdda95638c12e19a1d24f308eb2c3d3ae849d" },
        { 63, "280ed3e8ff1df845b2e7dfe6ac6cee817bef20e783cc65abc41b818b4d2fe076" },
        { 64, "c6ab9724ade5b6a7a1edfffb12f3aa9181351355af8fd08c919952ad211339dd" },
        { 65, "788367c73c7ddf4c53f65e68cc0d943e6227ab55b0e78ba63ace822b1c6301c0" },
        { 119, "3d610547d68216dedf7435a4fb6260353911f6b3fd3f18805ddb8be285d726fe" },
        { 120, "1f80156a804cb7862ad113e8200e9d74499723e7c7854d5f48776d3148e09656" },
        { 127, "192409cd280e14b743642ad1343fbd3e82d9305de72c078117745a679210cc3d" },
        { 128, "cc548ca2dec1f6fe4f58b2e27aa9c7521607df1130d140b55a4dad0665302356" },
        { 1000, "5097e7d587352f5097062ae679f37bda5802d9f875aba14c8cb4d1a188ada179" },
        { 1048577, "0d09f3eabb5c78e5d435c41e3b3755f1c4d038657f739d0be0083c24e71235df" },
    };
    uint8_t *data = malloc(1048577);
    for (size_t i = 0; i < 1048577; i++) data[i] = (uint8_t)(i * 31 + 7);
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
        for (size_t piece = 1; piece <= 129; piece += 64) {
            digest_of(data, known[i].size, known[i].size > 100000 && piece == 1 ? 65536 : piece, hex);
            assert(!strcmp(hex, known[i].hex));
        }
        digest_of(data, known[i].size, known[i].size ? known[i].size : 1, hex);
        assert(!strcmp(hex, known[i].hex));
    }
    free(data);
    puts("pw_sha256: ok");
    return 0;
}
