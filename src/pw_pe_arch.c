/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pw_pe_arch.h"

#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#define MACHINE_I386 0x014c
#define MACHINE_AMD64 0x8664
#define OPTIONAL_PE32 0x010b
#define OPTIONAL_PE32_PLUS 0x020b
#define DIRECTORY_CLR 14
#define CLR_ILONLY 0x00000001u
#define CLR_32BIT_REQUIRED 0x00000002u
#define SECTIONS_MAX 96
#define RELOCS_STRIPPED 0x0001

static uint16_t u16(const unsigned char *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t u32(const unsigned char *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static enum PwPeArch done(struct PwPeInfo *info, enum PwPeArch arch)
{
    info->arch = arch;
    return arch;
}

/* The file offset of an RVA, from the section table; 0 when no section holds it. */
static uint64_t rva_offset(PwPeRead read, void *context, uint64_t table, unsigned sections, uint32_t rva)
{
    unsigned char section[40];
    for (unsigned i = 0; i < sections && i < SECTIONS_MAX; i++) {
        if (read(context, table + 40ull * i, section, sizeof(section))) return 0;
        const uint32_t virtual_size = u32(section + 8), address = u32(section + 12);
        const uint32_t raw_size = u32(section + 16), raw = u32(section + 20);
        const uint32_t span = virtual_size > raw_size ? virtual_size : raw_size;
        if (rva >= address && rva - address < span && rva - address < raw_size) return (uint64_t)raw + (rva - address);
    }
    return 0;
}

enum PwPeArch pw_pe_arch(PwPeRead read, void *context, struct PwPeInfo *info)
{
    unsigned char dos[64], header[24], optional[32], directory[8], clr[20];

    memset(info, 0, sizeof(*info));
    if (read(context, 0, dos, sizeof(dos))) return done(info, PW_PE_NOT_PE);
    if (dos[0] != 'M' || dos[1] != 'Z') return done(info, PW_PE_NOT_PE);
    const uint64_t pe = u32(dos + 0x3c);
    if (read(context, pe, header, sizeof(header)) || memcmp(header, "PE\0\0", 4)) return done(info, PW_PE_NOT_PE);
    info->machine = u16(header + 4);
    info->characteristics = u16(header + 22);
    const unsigned sections = u16(header + 6), optional_size = u16(header + 20);
    const uint64_t start = pe + sizeof(header);
    if (read(context, start, optional, sizeof(optional))) return done(info, PW_PE_NOT_PE);
    const uint16_t magic = u16(optional);
    if (magic != OPTIONAL_PE32 && magic != OPTIONAL_PE32_PLUS) return done(info, PW_PE_NOT_PE);
    info->image_base = magic == OPTIONAL_PE32 ? u32(optional + 28)
                                              : u32(optional + 24) | (uint64_t)u32(optional + 28) << 32;

    if (info->machine == MACHINE_AMD64) return done(info, PW_PE_X64);
    if (info->machine != MACHINE_I386) return done(info, PW_PE_OTHER);

    /* An i386 image: x86-64 only if it is .NET, IL-only and does not require 32 bits. */
    const uint64_t count_at = magic == OPTIONAL_PE32 ? 92 : 108, directories = count_at + 4;
    unsigned char count[4];
    if (optional_size < directories + 8ull * (DIRECTORY_CLR + 1) || read(context, start + count_at, count, sizeof(count)) ||
        u32(count) <= DIRECTORY_CLR || read(context, start + directories + 8 * DIRECTORY_CLR, directory, sizeof(directory)))
        return done(info, PW_PE_X86);
    const uint32_t clr_rva = u32(directory), clr_size = u32(directory + 4);
    if (!clr_rva || clr_size < sizeof(clr)) return done(info, PW_PE_X86);
    const uint64_t clr_at = rva_offset(read, context, start + optional_size, sections, clr_rva);
    if (!clr_at || read(context, clr_at, clr, sizeof(clr))) return done(info, PW_PE_X86);
    info->clr = 1;
    info->clr_flags = u32(clr + 16);
    if ((info->clr_flags & CLR_ILONLY) && !(info->clr_flags & CLR_32BIT_REQUIRED)) return done(info, PW_PE_ANYCPU);
    return done(info, PW_PE_X86);
}

static int read_fd(void *context, uint64_t offset, void *into, size_t size)
{
    const int fd = *(const int *)context;
    size_t have = 0;
    while (have < size) {
        const ssize_t count = pread(fd, (char *)into + have, size - have, (off_t)(offset + have));
        if (count <= 0) return -1;
        have += (size_t)count;
    }
    return 0;
}

enum PwPeArch pw_pe_arch_file(const char *path, struct PwPeInfo *info)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        memset(info, 0, sizeof(*info));
        return done(info, PW_PE_UNREADABLE);
    }
    const enum PwPeArch arch = pw_pe_arch(read_fd, &fd, info);
    close(fd);
    return arch;
}

int pw_pe_arch_fixed_base32(const struct PwPeInfo *info)
{
    return info->arch == PW_PE_X86 && (info->characteristics & RELOCS_STRIPPED) && info->image_base == 0x400000;
}

int pw_pe_arch_runs_on_x64(enum PwPeArch arch)
{
    return arch == PW_PE_X64 || arch == PW_PE_ANYCPU;
}

const char *pw_pe_arch_name(enum PwPeArch arch)
{
    switch (arch) {
    case PW_PE_UNREADABLE: return "unreadable";
    case PW_PE_NOT_PE: return "not a PE image";
    case PW_PE_X64: return "x86-64";
    case PW_PE_ANYCPU: return ".NET Any CPU (runs as x86-64)";
    case PW_PE_X86: return "32-bit x86";
    case PW_PE_OTHER: return "another CPU";
    }
    return "unknown";
}
