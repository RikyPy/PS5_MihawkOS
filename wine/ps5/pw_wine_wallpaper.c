/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pw_wine_wallpaper.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t little(const unsigned char *p, unsigned count)
{
    uint32_t value = 0;
    for (unsigned i = 0; i < count; ++i) value |= (uint32_t)p[i] << (8 * i);
    return value;
}

int pw_wine_wallpaper_load(const char *path, uint32_t *pixels, unsigned width, unsigned height)
{
    unsigned char header[54], *row = NULL;
    unsigned source_width, source_height, bytes, stride;
    uint32_t offset;
    int okay = 0, saved = EINVAL;
    FILE *file = NULL;
    if (!path || !pixels || !width || !height || width > 4096 || height > 4096) {
        errno = EINVAL; return -1;
    }
    memset(pixels, 0, (size_t)width * height * sizeof(*pixels));
    if (!(file = fopen(path, "rb"))) return -1;
    if (fread(header, 1, sizeof(header), file) != sizeof(header) ||
        memcmp(header, "BM", 2) || little(header + 14, 4) != 40 ||
        little(header + 26, 2) != 1 || little(header + 30, 4)) goto done;
    source_width = little(header + 18, 4);
    int32_t signed_height = (int32_t)little(header + 22, 4);
    source_height = signed_height < 0 ? -(int64_t)signed_height : signed_height;
    bytes = little(header + 28, 2) / 8;
    if (!source_width || source_width > 4096 || !source_height || source_height > 4096 ||
        (little(header + 28, 2) != 24 && little(header + 28, 2) != 32)) goto done;
    stride = (source_width * bytes + 3) & ~3u;
    offset = little(header + 10, 4);
    if (offset < sizeof(header) || offset > 1048576) goto done;
    if (!(row = malloc(stride))) { saved = ENOMEM; goto done; }
    /* Fill the desktop, preserving aspect ratio and cropping equally at edges. */
    uint64_t scaled_width = (uint64_t)width * source_height;
    uint64_t scaled_height = (uint64_t)height * source_width;
    unsigned crop_width = source_width, crop_height = source_height;
    if (scaled_width > scaled_height) crop_height = (uint64_t)source_width * height / width;
    else crop_width = (uint64_t)source_height * width / height;
    if (!crop_width || !crop_height) goto done;
    unsigned left = (source_width - crop_width) / 2, top = (source_height - crop_height) / 2;
    unsigned previous = ~0u;
    for (unsigned y = 0; y < height; ++y) {
        unsigned sy = top + (2 * (uint64_t)y + 1) * crop_height / (2 * height);
        if (sy != previous) {
            unsigned disk_y = signed_height < 0 ? sy : source_height - sy - 1;
            if (fseek(file, offset + (long)disk_y * stride, SEEK_SET) ||
                fread(row, 1, stride, file) != stride) goto done;
            previous = sy;
        }
        for (unsigned x = 0; x < width; ++x) {
            unsigned sx = left + (2 * (uint64_t)x + 1) * crop_width / (2 * width);
            pixels[(size_t)y * width + x] = 0xff000000u | little(row + sx * bytes, 3);
        }
    }
    okay = 1;
done:
    free(row);
    fclose(file);
    if (!okay) memset(pixels, 0, (size_t)width * height * sizeof(*pixels));
    errno = okay ? 0 : saved;
    return okay ? 0 : -1;
}
