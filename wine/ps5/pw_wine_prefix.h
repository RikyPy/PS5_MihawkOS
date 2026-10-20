/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_WINE_PREFIX_H
#define PW_WINE_PREFIX_H
/* Install the staged, runtime-owned Windows files after wineboot has made the
 * prefix and before the initial client's imports/font discovery. `source` is
 * the bundle's windows directory; `target` is the prefix's existing windows
 * directory. Each leaf folder carries .pw-proton-version, written last by the
 * existing atomic file synchronizer. Other files/settings/saves are untouched.
 * Missing input, an incomplete wineboot or a copy failure returns -1/errno;
 * success returns the number of files copied (0 on a current prefix). */
int pw_wine_prefix_prepare(const char *source, const char *target);
#endif
