/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_SHADER_CACHE_H
#define PW_SHADER_CACHE_H
/* Configure upstream caches by game and runtime identity. An unavailable cache
 * disables all backends and returns errno; disabled by request returns zero. */
int pw_shader_cache_configure(const char *root, const char *profile,
                              const char *version_file, int enabled);
#endif
