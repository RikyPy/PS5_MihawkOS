/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/pw_pe_arch.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* A minimal image: DOS header, PE header at 0x80, an optional header with 16 directories, one section
 * (.text at RVA 0x2000, file offset 0x400) holding the CLR header at RVA 0x2008 when there is one. */
struct Image { unsigned char bytes[0x600]; size_t size; };

static void put16(unsigned char *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void put32(unsigned char *p, unsigned v) { put16(p, v & 0xffff); put16(p + 2, v >> 16); }

static struct Image image(unsigned machine, int pe32_plus, int clr, unsigned clr_flags)
{
    struct Image out;
    memset(&out, 0, sizeof(out));
    out.size = sizeof(out.bytes);
    unsigned char *b = out.bytes;
    b[0] = 'M'; b[1] = 'Z';
    put32(b + 0x3c, 0x80);
    memcpy(b + 0x80, "PE\0\0", 4);
    put16(b + 0x84, machine);
    put16(b + 0x86, 1);                                 /* one section */
    const unsigned optional_size = pe32_plus ? 240 : 224;
    put16(b + 0x94, optional_size);
    unsigned char *optional = b + 0x98;
    put16(optional, pe32_plus ? 0x20b : 0x10b);
    const unsigned count_at = pe32_plus ? 108 : 92;
    put32(optional + count_at, 16);
    if (clr) {
        put32(optional + count_at + 4 + 8 * 14, 0x2008);
        put32(optional + count_at + 4 + 8 * 14 + 4, 72);
    }
    unsigned char *section = optional + optional_size;
    memcpy(section, ".text", 5);
    put32(section + 8, 0x200);                          /* virtual size */
    put32(section + 12, 0x2000);                        /* virtual address */
    put32(section + 16, 0x200);                         /* raw size */
    put32(section + 20, 0x400);                         /* raw offset */
    if (clr) {
        put32(b + 0x408, 72);                           /* cb */
        put32(b + 0x408 + 16, clr_flags);
    }
    return out;
}

static int read_image(void *context, uint64_t offset, void *into, size_t size)
{
    const struct Image *image = context;
    if (offset > image->size || size > image->size - offset) return -1;
    memcpy(into, image->bytes + offset, size);
    return 0;
}

static enum PwPeArch arch_of(struct Image image, struct PwPeInfo *info)
{
    return pw_pe_arch(read_image, &image, info);
}

int main(void)
{
    struct PwPeInfo info;

    /* A native x86-64 program, and a .NET one built for x64. */
    assert(arch_of(image(0x8664, 1, 0, 0), &info) == PW_PE_X64 && info.machine == 0x8664);
    assert(arch_of(image(0x8664, 1, 1, 1), &info) == PW_PE_X64);
    /* A native 32-bit program. */
    assert(arch_of(image(0x14c, 0, 0, 0), &info) == PW_PE_X86 && !info.clr);
    /* An FNA game's two installations: IL-only and 32BITREQUIRED (flags 3). */
    assert(arch_of(image(0x14c, 0, 1, 3), &info) == PW_PE_X86 && info.clr && info.clr_flags == 3);
    /* "Prefer 32-bit" sets 32BITREQUIRED too: it runs as 32-bit on 64-bit Windows. */
    assert(arch_of(image(0x14c, 0, 1, 0x20003), &info) == PW_PE_X86);
    /* Any CPU: IL-only, no 32-bit requirement; Wine runs it as x86-64. */
    assert(arch_of(image(0x14c, 0, 1, 1), &info) == PW_PE_ANYCPU && info.clr_flags == 1);
    /* Mixed-mode .NET (not IL-only) on an i386 header carries native 32-bit code. */
    assert(arch_of(image(0x14c, 0, 1, 0), &info) == PW_PE_X86);
    /* Characteristics and ImageBase: an old installer linked at 0x400000 with its relocations stripped can load only
     * there, where a helper's own executable sits; relocatable, elsewhere, or 64-bit, it can. */
    struct Image fixed = image(0x14c, 0, 0, 0);
    put16(fixed.bytes + 0x96, 0x0103);                  /* RELOCS_STRIPPED | EXECUTABLE_IMAGE | 32BIT_MACHINE */
    put32(fixed.bytes + 0x98 + 28, 0x400000);
    assert(arch_of(fixed, &info) == PW_PE_X86 && info.characteristics == 0x0103 && info.image_base == 0x400000);
    assert(pw_pe_arch_fixed_base32(&info));
    struct Image relocatable = fixed;
    put16(relocatable.bytes + 0x96, 0x0102);
    assert(arch_of(relocatable, &info) == PW_PE_X86 && !pw_pe_arch_fixed_base32(&info));
    struct Image elsewhere = fixed;
    put32(elsewhere.bytes + 0x98 + 28, 0x10000000);
    assert(arch_of(elsewhere, &info) == PW_PE_X86 && !pw_pe_arch_fixed_base32(&info));
    struct Image wide = image(0x8664, 1, 0, 0);
    put16(wide.bytes + 0x96, 0x0023);
    put32(wide.bytes + 0x98 + 24, 0x400000);
    put32(wide.bytes + 0x98 + 28, 0x1);
    assert(arch_of(wide, &info) == PW_PE_X64 && info.image_base == 0x100400000ull && !pw_pe_arch_fixed_base32(&info));
    /* IL-only .NET on an i386 header runs as x86-64: never fixed-base 32-bit, whatever its header says. */
    struct Image il = image(0x14c, 0, 1, 1);
    put16(il.bytes + 0x96, 0x0103);
    put32(il.bytes + 0x98 + 28, 0x400000);
    assert(arch_of(il, &info) == PW_PE_ANYCPU && !pw_pe_arch_fixed_base32(&info));
    /* ARM64. */
    assert(arch_of(image(0xaa64, 1, 0, 0), &info) == PW_PE_OTHER);

    /* Not images: no MZ, a bad PE signature, a header past the end of the file. */
    struct Image script = image(0x8664, 1, 0, 0);
    memcpy(script.bytes, "#!", 2);
    assert(arch_of(script, &info) == PW_PE_NOT_PE && info.machine == 0);
    struct Image signature = image(0x8664, 1, 0, 0);
    signature.bytes[0x81] = 'X';
    assert(arch_of(signature, &info) == PW_PE_NOT_PE);
    struct Image truncated = image(0x8664, 1, 0, 0);
    truncated.size = 0x82;
    assert(arch_of(truncated, &info) == PW_PE_NOT_PE);
    /* A CLR directory that points outside every section: no flags to read, so 32-bit. */
    struct Image stray = image(0x14c, 0, 1, 1);
    put32(stray.bytes + 0x98 + 92 + 4 + 8 * 14, 0x9000);
    assert(arch_of(stray, &info) == PW_PE_X86 && !info.clr);

    /* The file reader, and a missing file. */
    char path[] = "/tmp/pw-pe-arch-XXXXXX";
    const int fd = mkstemp(path);
    assert(fd >= 0);
    struct Image anycpu = image(0x14c, 0, 1, 1);
    assert(write(fd, anycpu.bytes, anycpu.size) == (ssize_t)anycpu.size);
    close(fd);
    assert(pw_pe_arch_file(path, &info) == PW_PE_ANYCPU);
    unlink(path);
    assert(pw_pe_arch_file(path, &info) == PW_PE_UNREADABLE);

    assert(pw_pe_arch_runs_on_x64(PW_PE_X64) && pw_pe_arch_runs_on_x64(PW_PE_ANYCPU));
    assert(!pw_pe_arch_runs_on_x64(PW_PE_X86) && !pw_pe_arch_runs_on_x64(PW_PE_OTHER));
    assert(!strcmp(pw_pe_arch_name(PW_PE_X86), "32-bit x86"));
    puts("pw_pe_arch: ok");
    return 0;
}
