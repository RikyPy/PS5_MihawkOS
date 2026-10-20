/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Which CPU a Windows executable needs, from its PE headers. A 32-bit program runs under WoW64 only
 * on a runtime that ships the i386 modules (docs/IA32_DESIGN.md); on 2026-10-06, on a runtime without
 * them, the first launch of a 32-bit-required .NET game (an FNA game) ended with the console
 * restarting. The launcher reads the header first and refuses such a game where it cannot run.
 *
 * The machine field alone is not the answer. A .NET program built for "Any CPU" carries an i386
 * header but runs as a 64-bit process (its CLR header is IL-only and does not require 32 bits);
 * Wine loads it as x86-64. Only the CLR flags tell it apart from a 32-bit-required one.
 */
#ifndef PW_PE_ARCH_H
#define PW_PE_ARCH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum PwPeArch {
    PW_PE_UNREADABLE,   /* the file could not be read */
    PW_PE_NOT_PE,       /* not a PE image (a script, a truncated or damaged file) */
    PW_PE_X64,          /* an x86-64 image */
    PW_PE_ANYCPU,       /* an i386-headed .NET image that is IL-only and may run as x86-64 */
    PW_PE_X86,          /* a 32-bit x86 image: native, or .NET that requires 32 bits */
    PW_PE_OTHER,        /* another machine (ARM64, ...) */
};

struct PwPeInfo {
    enum PwPeArch arch;
    uint16_t machine;   /* IMAGE_FILE_HEADER.Machine; 0 when there is no PE header */
    int clr;            /* 1: the image has a CLR (.NET) header */
    uint32_t clr_flags; /* IMAGE_COR20_HEADER.Flags when clr */
    uint16_t characteristics; /* IMAGE_FILE_HEADER.Characteristics (IMAGE_FILE_RELOCS_STRIPPED is 0x0001) */
    uint64_t image_base;      /* the optional header's ImageBase: where the image was linked to load */
};

/* Read bytes [offset, offset + size) of the image into `into`; 0 when all of them were read. */
typedef int (*PwPeRead)(void *context, uint64_t offset, void *into, size_t size);

enum PwPeArch pw_pe_arch(PwPeRead read, void *context, struct PwPeInfo *info);
enum PwPeArch pw_pe_arch_file(const char *path, struct PwPeInfo *info);

/* 1 when the x86-64 runtime can run an image of this kind. */
int pw_pe_arch_runs_on_x64(enum PwPeArch arch);
/* "x86-64", "32-bit x86", ...; for a log line. */
const char *pw_pe_arch_name(enum PwPeArch arch);
/* 1 for a 32-bit image that can load only at 0x400000 (its relocations stripped): an old installer's. Only its helper
 * gives the carrier's image there back (wine/ps5/pw_wine_process.c, PW_CHILD_FIXED_BASE); every other helper keeps it,
 * since a relocatable 32-bit program placed at 0x400000 lost its audio and controller drivers
 * (free-0x400000-control-2026-10-19-01; docs/IA32_DESIGN.md). */
int pw_pe_arch_fixed_base32(const struct PwPeInfo *info);

#ifdef __cplusplus
}
#endif

#endif
