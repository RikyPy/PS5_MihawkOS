/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * IA-32 bytes as decoded instructions, through the repository's own decoder and printer
 * (docs/IA32_DESIGN.md): an agent reads this, never raw bytes. Run it through tools/ia32-dump.sh.
 *
 *   ia32_dump [--64] [--address A] [--offset N] [--count N] FILE   bytes of FILE from offset N
 *   ia32_dump [--64] [--address A] --hex "8b 44 88 10 ..."         bytes given in hex
 *
 * --64 decodes long mode (docs/MMU4K.md); a VEX form there prints its row's name and memory operand,
 * since the printer spells no VEX register.
 *
 * One line per instruction: address, bytes, text; a branch also shows its absolute target. An
 * encoding without a form prints "(no form)", or "(vex)", "(evex)", "(xop)", with its length; an
 * undefined one "(bad)" and one byte, so the dump always moves on.
 */
#include "../src/pw_ia32_decode.h"
#include "../src/pw_ia32_print.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t parse_hex(const char *text, uint8_t *out, size_t capacity)
{
    size_t n = 0;
    while (*text && n < capacity) {
        while (*text == ' ' || *text == ',') text++;
        unsigned value;
        int used = 0;
        if (sscanf(text, "%2x%n", &value, &used) != 1) break;
        out[n++] = (uint8_t)value;
        text += used;
    }
    return n;
}

static unsigned mode = PW_IA32_MODE_32;

static int branch_target(const PwIa32Inst *inst, uint64_t address, uint64_t *target)
{
    const PwIa32Form *form = inst->form;
    for (unsigned i = 0; i < 3; i++) {
        if (form->operands[i] != PW_IA32_OP_Jb && form->operands[i] != PW_IA32_OP_Jz) continue;
        const int32_t rel = inst->imm_size == 1 ? (int8_t)inst->imm : inst->imm_size == 2 ? (int16_t)inst->imm
                                                                                         : (int32_t)inst->imm;
        *target = address + inst->length + (uint64_t)(int64_t)rel;
        if (inst->operand_size == 16) *target &= 0xffff;
        else if (mode != PW_IA32_MODE_64) *target &= 0xffffffffu;
        return 1;
    }
    return 0;
}

static void dump(const uint8_t *bytes, size_t size, uint64_t address)
{
    size_t at = 0;
    while (at < size) {
        PwIa32Inst inst;
        const int result = pw_ia32_decode_mode(bytes + at, size - at, mode, &inst);
        size_t length = inst.length;
        char text[160];
        if (result == PW_OK && inst.encoding == PW_IA32_ENC_VEX) {
            char memory[96] = "";
            PwIa32Access access;
            if (inst.has_modrm && inst.mod != 3 && pw_ia32_access(&inst, &access) == PW_OK)
                pw_ia32_print_memory(&inst, access.exact ? access.bytes * 8u : 0, memory, sizeof(memory));
            snprintf(text, sizeof(text), "(vex %s)%s%s", inst.form->name, memory[0] ? " " : "", memory);
        } else if (result == PW_OK) {
            pw_ia32_print(&inst, text, sizeof(text));
            for (char *t = text; *t; t++) if (*t == '\t') *t = ' ';
        } else if (result == PW_ERR_UNSUPPORTED && length) {
            static const char *const kinds[] = { "(no form)", "(vex)", "(evex)", "(xop)" };
            snprintf(text, sizeof(text), "%s", kinds[inst.encoding & 3]);
        } else if (result == PW_ERR_TRUNCATED) {
            snprintf(text, sizeof(text), "(truncated)");
            length = size - at;
        } else {
            snprintf(text, sizeof(text), "(bad)");
            length = 1;
        }
        if (mode == PW_IA32_MODE_64) printf("%016llx  ", (unsigned long long)(address + at));
        else printf("%08x  ", (uint32_t)(address + at));
        for (size_t i = 0; i < 8; i++) {
            if (i < length) printf("%02x ", bytes[at + i]);
            else printf("   ");
        }
        printf("%s %s", length > 8 ? "+" : " ", text);
        uint64_t target;
        if (result == PW_OK && branch_target(&inst, address + at, &target))
            printf(mode == PW_IA32_MODE_64 ? "    ; -> %016llx" : "    ; -> %08llx", (unsigned long long)target);
        printf("\n");
        at += length;
    }
}

int main(int argc, char **argv)
{
    uint64_t address = 0;
    size_t offset = 0, count = 256;
    const char *hex = NULL, *path = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--64")) mode = PW_IA32_MODE_64;
        else if (!strcmp(argv[i], "--address") && i + 1 < argc) address = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--offset") && i + 1 < argc) offset = strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--count") && i + 1 < argc) count = strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--hex") && i + 1 < argc) hex = argv[++i];
        else if (argv[i][0] != '-' && !path) path = argv[i];
        else {
            fprintf(stderr, "usage: %s [--64] [--address A] [--offset N] [--count N] (FILE | --hex BYTES)\n", argv[0]);
            return 2;
        }
    }
    static uint8_t bytes[1 << 20];
    size_t size = 0;
    if (hex) {
        size = parse_hex(hex, bytes, sizeof(bytes));
    } else if (path) {
        FILE *file = fopen(path, "rb");
        if (!file || fseek(file, (long)offset, SEEK_SET)) {
            perror(path);
            return 2;
        }
        size = fread(bytes, 1, count < sizeof(bytes) ? count : sizeof(bytes), file);
        fclose(file);
    } else {
        fprintf(stderr, "%s: give a FILE or --hex\n", argv[0]);
        return 2;
    }
    dump(bytes, size, address);
    return 0;
}
