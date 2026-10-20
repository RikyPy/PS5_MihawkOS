/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_WINE_WALLPAPER_H
#define PW_WINE_WALLPAPER_H
#include <stdint.h>
/* Load an uncompressed 24/32-bit BMP into an already allocated desktop.
 * This is the native compositor's background when no explorer owns a desktop
 * surface. No window pixels are altered. On failure the destination is cleared.
 * Inputs are bounded to 4096 pixels per axis; decoded rows use at most 16 KiB.
 * The default asset is supplied locally, not redistributed with the source.
 */
int pw_wine_wallpaper_load(const char *path, uint32_t *pixels, unsigned width, unsigned height);
#endif
