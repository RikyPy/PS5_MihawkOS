/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_ENVIRONMENT_H
#define PW_ENVIRONMENT_H
#include <stddef.h>
/* Explicit application overrides, in caller order. No host-environment scan.
 * UTF-8 NAME=VALUE entries; empty values and '=' inside values are preserved.
 * The packed block ends with two NULs. Validation failures leave output alone.
 * Input entries must not alias output. Wine owns Unicode/case-insensitive
 * overlay semantics; this module owns only the bounded transport format. */
int pw_environment_pack(char *out,size_t capacity,const char *const *entries,
                        size_t count,size_t *bytes);
int pw_environment_validate(const char *block,size_t bytes);
#endif
