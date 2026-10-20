/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * The decoder oracle: src/pw_ia32_decode.c and src/pw_ia32_print.c against LLVM's disassembler
 * (libLLVM's C API) over a generated corpus, in 32-bit mode or (--64) in long mode. Run it through
 * tools/ia32-decode-oracle.sh.
 *
 * Corpus: every opcode of every map (primary, 0F, 0F 38, 0F 3A) × the mode's prefix sets (12; 25 in
 * long mode, with REX) × every ModRM byte (when the opcode has one) × 10 SIB bytes (when the ModRM
 * has one) × 2 tails (the bytes that become displacement and immediates: one positive, one
 * negative); then VEX, EVEX and XOP: every opcode of each of their maps × a few payloads × every
 * ModRM. In 32-bit mode only VEX/EVEX/XOP lengths are compared (there are no forms for them); in
 * long mode VEX is compared as legacy encodings are, with MEMORY in place of TEXT (the printer
 * spells only a VEX form's memory operand), and EVEX/XOP lengths only. Long mode also compares
 * every form's MEMORY: pw_ia32_access's kind and size against LLVM's memory operand.
 *
 * Long mode also checks the table's attributes against LLVM's own description of each instruction
 * (tools/x86_llvm_semantics.cpp, libLLVM's MC layer), for every encoding both decode to one length:
 *   STORE_MISSED  LLVM says it may store; the row says no store (no WRITE/RW, STACK, STR or IMPL)
 *   STORE_EXTRA   the row writes its ModRM memory operand; LLVM says it never stores (a missing MRO)
 *   FLOW          LLVM and the row (BRANCH) disagree on whether it changes RIP
 *   STACK         LLVM and the row (STACK) disagree on whether it uses RSP implicitly
 *
 * Each encoding gets one verdict. Disagreements are grouped by kind, map, opcode and ModRM.reg/mod,
 * so one fix clears a group:
 *   LENGTH     both decode it, to different lengths
 *   TEXT       same length, different text
 *   OURS_ONLY  we decode a form, LLVM rejects the bytes
 *   LLVM_ONLY  we call it undefined (MALFORMED), LLVM decodes it
 *   NOFORM     same length, but we have no form for it yet (coverage, not a failure)
 * A group listed in the differences file (argv[1]) is explained, with its reason, and does not fail.
 *
 * Output: argv[2] gets one JSON line per group (count and one example); stdout gets one JSON
 * object: the counts and the verdict. Exit status 0 on PASS, 1 on FAIL, 2 on a usage error.
 */
#include "../src/pw_ia32_decode.h"
#include "../src/pw_ia32_print.h"
#include <llvm-c/Core.h>
#include <llvm-c/Disassembler.h>
#include <llvm-c/Target.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { KIND_LENGTH, KIND_TEXT, KIND_OURS_ONLY, KIND_LLVM_ONLY, KIND_NOFORM, KIND_MEMORY, KIND_STORE_MISSED,
       KIND_STORE_EXTRA, KIND_FLOW, KIND_STACK, KINDS };
static const char *const kind_names[KINDS] = { "LENGTH", "TEXT", "OURS_ONLY", "LLVM_ONLY", "NOFORM", "MEMORY",
                                               "STORE_MISSED", "STORE_EXTRA", "FLOW", "STACK" };
/* Prefix sets: legacy ones from 0; VEX payloads (long mode) at VEX_SET + pp + 4*L + 8*W. */
enum { TEXT_MAX = 96, NO_REG_SLOT = 8, SETS = 64, VEX_SET = 32 };

/* tools/x86_llvm_semantics.cpp */
enum { X86_SEM_LOAD = 1, X86_SEM_STORE = 2, X86_SEM_FLOW = 4, X86_SEM_STACK = 8 };
int x86_semantics_open(int mode);
int x86_semantics(const uint8_t *bytes, size_t size, unsigned *flags);
static unsigned mode = PW_IA32_MODE_32;

typedef struct Group {
    uint32_t key;         /* 0: empty slot */
    uint32_t count;
    uint8_t bytes[24];
    uint8_t length;
    char ours[TEXT_MAX], llvm[TEXT_MAX];
    int ours_length, llvm_length;
} Group;

/* Groups live in an open-addressing table keyed by kind, map, opcode, ModRM.reg (NO_REG_SLOT
 * without one), mod == 3 and the prefix set, so one example stands for one pattern. */
enum { GROUP_SLOTS = 1u << 18 };
static Group *groups;
static unsigned group_count;
/* Reasons from the differences file: [kind][map][opcode][reg or NO_REG_SLOT][prefix set]. */
static const char *reasons[KINDS][PW_IA32_MAPS][256][9][SETS];
static unsigned current_prefix_set;

static uint32_t group_key(unsigned kind, unsigned map, unsigned opcode, unsigned reg, unsigned mod3, unsigned set)
{
    return 1u + (((((kind * PW_IA32_MAPS + map) * 256u + opcode) * 9u + reg) * 2u + mod3) * SETS + set);
}

static Group *group_slot(uint32_t key)
{
    for (uint32_t i = (key * 2654435761u) & (GROUP_SLOTS - 1);; i = (i + 1) & (GROUP_SLOTS - 1)) {
        if (groups[i].key == key) return &groups[i];
        if (!groups[i].key) {
            if (group_count + 1 >= GROUP_SLOTS) return NULL;
            group_count++;
            groups[i].key = key;
            return &groups[i];
        }
    }
}

typedef struct Totals {
    uint64_t encodings, agree_text, agree_length_noform, agree_invalid, skipped_vex, ours_only_explained;
    uint64_t agree_memory, agree_semantics, skipped_semantics;
    uint64_t kind[KINDS];
} Totals;

/* The corpus. Long mode adds REX alone, after other prefixes (where it counts) and before them
 * (where it is ignored). */
typedef struct PrefixSet {
    uint8_t length, bytes[3];
} PrefixSet;
static const PrefixSet prefix_sets_32[] = {
    { 0, { 0 } }, { 1, { 0x66 } }, { 1, { 0x67 } }, { 1, { 0xf3 } }, { 1, { 0xf2 } }, { 1, { 0xf0 } },
    { 1, { 0x64 } }, { 1, { 0x26 } }, { 2, { 0x66, 0x67 } }, { 2, { 0x66, 0xf3 } }, { 2, { 0xf2, 0x66 } },
    { 1, { 0x2e } },
};
static const PrefixSet prefix_sets_64[] = {
    { 0, { 0 } }, { 1, { 0x66 } }, { 1, { 0x67 } }, { 1, { 0xf3 } }, { 1, { 0xf2 } }, { 1, { 0xf0 } },
    { 1, { 0x64 } }, { 1, { 0x26 } }, { 2, { 0x66, 0x67 } }, { 2, { 0x66, 0xf3 } }, { 2, { 0xf2, 0x66 } },
    { 1, { 0x2e } }, { 1, { 0x48 } }, { 1, { 0x41 } }, { 1, { 0x44 } }, { 1, { 0x42 } }, { 1, { 0x4f } },
    { 1, { 0x40 } }, { 2, { 0x66, 0x48 } }, { 2, { 0x48, 0x66 } }, { 2, { 0xf3, 0x48 } }, { 2, { 0xf2, 0x4c } },
    { 2, { 0xf0, 0x48 } }, { 2, { 0x67, 0x48 } }, { 2, { 0x66, 0x41 } },
};
static const PrefixSet *prefix_sets = prefix_sets_32;
static size_t prefix_set_count = sizeof(prefix_sets_32) / sizeof(prefix_sets_32[0]);
static const uint8_t sibs[] = { 0x00, 0x24, 0x25, 0x65, 0xa0, 0x0c, 0x64, 0xe5, 0x8d, 0x1c };
static const uint8_t tails[2][16] = {
    { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x08, 0x19, 0x2a, 0x3b, 0x4c, 0x5d, 0x6e, 0x7f, 0x01 },
    { 0xf0, 0xff, 0xff, 0xff, 0x80, 0xff, 0x7f, 0x00, 0xfe, 0xff, 0xff, 0xff, 0x10, 0x20, 0x30, 0x40 },
};

static LLVMDisasmContextRef llvm_context;

/* Collapses whitespace runs to one space and trims, so tabs and LLVM's double tabs compare equal. */
static void normalize(char *text)
{
    char *out = text;
    int space = 1;
    for (const char *in = text; *in; in++) {
        if (*in == ' ' || *in == '\t' || *in == '\n') {
            if (!space) *out++ = ' ';
            space = 1;
        } else {
            *out++ = *in;
            space = 0;
        }
    }
    if (out > text && out[-1] == ' ') out--;
    *out = 0;
}

static int is_lone_prefix(const char *text)
{
    static const char *const prefixes[] = { "lock", "rep", "repe", "repne", "xacquire", "xrelease", "data16", "addr16" };
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++)
        if (!strcmp(text, prefixes[i])) return 1;
    return 0;
}

/* LLVM's verdict: the length (0: invalid) and the text. LLVM returns LOCK, and F2/F3 read as
 * XACQUIRE/XRELEASE, as instructions of their own; the rest is decoded and joined to them, as one
 * instruction. LLVM's comments ("# xmm0 = mem[0,0]") are cut off. */
static int llvm_decode(const uint8_t *bytes, size_t size, char *text, size_t capacity)
{
    size_t total = 0;
    text[0] = 0;
    for (int part = 0; part < 4; part++) {
        char piece[256];
        const size_t n = LLVMDisasmInstruction(llvm_context, (uint8_t *)bytes + total, size - total, 0, piece,
                                               sizeof(piece));
        if (!n) return 0;
        piece[strcspn(piece, "#")] = 0;
        normalize(piece);
        if (text[0]) strncat(text, " ", capacity - strlen(text) - 1);
        strncat(text, piece, capacity - strlen(text) - 1);
        total += n;
        if (!is_lone_prefix(piece)) return (int)total;
    }
    return 0;
}

static void group_add(unsigned kind, const PwIa32Inst *inst, const uint8_t *bytes, size_t size, const char *ours,
                      int ours_length, const char *llvm, int llvm_length)
{
    const unsigned reg = inst->has_modrm ? inst->reg & 7u : NO_REG_SLOT;
    Group *g = group_slot(group_key(kind, inst->map, inst->opcode, reg, inst->has_modrm && inst->mod == 3,
                                    current_prefix_set));
    if (!g || g->count++) return;
    memcpy(g->bytes, bytes, size < sizeof(g->bytes) ? size : sizeof(g->bytes));
    g->length = (uint8_t)(size < sizeof(g->bytes) ? size : sizeof(g->bytes));
    snprintf(g->ours, sizeof(g->ours), "%s", ours);
    snprintf(g->llvm, sizeof(g->llvm), "%s", llvm);
    g->ours_length = ours_length;
    g->llvm_length = llvm_length;
}

/* The memory operand of LLVM's text, from its size word (or bracket) to the closing bracket, or "". */
static void llvm_memory(const char *llvm, char *out, size_t capacity)
{
    out[0] = 0;
    const char *open = strchr(llvm, '['), *close = open ? strchr(open, ']') : NULL;
    if (!close) return;
    const char *start = open;
    if (start - llvm >= 3 && start[-1] == ':') start -= 3;                  /* a segment: fs: */
    if (start - llvm >= 5 && !strncmp(start - 5, " ptr ", 5)) {             /* the size word before ptr */
        start -= 5;
        while (start > llvm && start[-1] != ' ') start--;
    }
    snprintf(out, capacity, "%.*s", (int)(close + 1 - start), start);
}

/* pw_ia32_access against LLVM's text: the memory operand, as pw_ia32_print_memory spells it with
 * the access's size (VEX forms, whose rows print nothing else, and in long mode every form: the
 * engine sizes a store by pw_ia32_access, not by the printer). LLVM never sizes a far pointer. */
static int memory_agrees(const PwIa32Inst *inst, const char *llvm, char *ours, size_t capacity)
{
    char theirs[TEXT_MAX];
    llvm_memory(llvm, theirs, sizeof(theirs));
    PwIa32Access access;
    ours[0] = 0;
    if (pw_ia32_access(inst, &access) != PW_OK) return 1;   /* STACK, STR, IMPL: no ModRM size to compare */
    if (access.kind == PW_IA32_ACCESS_NONE) return theirs[0] == 0;
    int far = 0;
    for (unsigned i = 0; i < 3; i++) far |= inst->form->operands[i] == PW_IA32_OP_Mp;
    const unsigned bits = access.kind == PW_IA32_ACCESS_ADDRESS || !access.exact || far ? 0 : access.bytes * 8u;
    pw_ia32_print_memory(inst, bits, ours, capacity);
    const char *compared = theirs;
    if (!bits && strstr(theirs, "ptr ")) compared = strstr(theirs, "ptr ") + 4;
    return !strcmp(ours, compared);
}

/* Long mode: the row's memory, stack and control-flow attributes against LLVM's description: a
 * KIND_*, KINDS when they agree, or SKIPPED. LLVM's MC layer returns LOCK and XACQUIRE/XRELEASE as
 * instructions of their own; those encodings are checked without the prefix only. */
enum { SKIPPED = KINDS + 1 };
static unsigned semantics_disagree(const PwIa32Inst *inst, const uint8_t *bytes, size_t size, unsigned *llvm_flags)
{
    unsigned flags = 0;
    if (x86_semantics(bytes, size, &flags) != inst->length) return SKIPPED;
    *llvm_flags = flags;
    const uint32_t a = inst->form->attributes;
    PwIa32Access access;
    const int sized = pw_ia32_access(inst, &access) == PW_OK;
    const int writes = sized && (access.kind == PW_IA32_ACCESS_WRITE || access.kind == PW_IA32_ACCESS_RW);
    const unsigned first = inst->form->operands[0];
    const int may_store = writes || (a & (PW_IA32_ATTR_STACK | PW_IA32_ATTR_STR | PW_IA32_ATTR_IMPL)) ||
                          first == PW_IA32_OP_Ob || first == PW_IA32_OP_Ov;   /* a moffs store */
    if ((flags & X86_SEM_STORE) && !may_store) return KIND_STORE_MISSED;
    if (writes && !(flags & X86_SEM_STORE)) return KIND_STORE_EXTRA;
    if (!(flags & X86_SEM_FLOW) != !(a & PW_IA32_ATTR_BRANCH)) return KIND_FLOW;
    if (!(flags & X86_SEM_STACK) != !(a & PW_IA32_ATTR_STACK)) return KIND_STACK;
    return KINDS;
}

static void check_one(const uint8_t *bytes, size_t size, Totals *totals)
{
    PwIa32Inst inst;
    char ours[256] = "", llvm[256];
    const int result = pw_ia32_decode_mode(bytes, size, mode, &inst);
    const int llvm_length = llvm_decode(bytes, size, llvm, sizeof(llvm));
    totals->encodings++;
    const int lengths_only = inst.encoding != PW_IA32_ENC_LEGACY &&
                             (mode != PW_IA32_MODE_64 || inst.encoding != PW_IA32_ENC_VEX);
    if (lengths_only) {
        /* No forms: only the length is checked, and a disagreement is a LENGTH group. */
        if (result == PW_ERR_UNSUPPORTED && llvm_length && inst.length != llvm_length) {
            totals->kind[KIND_LENGTH]++;
            group_add(KIND_LENGTH, &inst, bytes, size, "(vex)", inst.length, llvm, llvm_length);
        } else if (result == PW_ERR_UNSUPPORTED && llvm_length) {
            totals->skipped_vex++;
        } else {   /* LLVM knows no such opcode, or both refuse: we decode only lengths here */
            totals->agree_invalid++;
        }
        return;
    }
    const int vex = inst.encoding == PW_IA32_ENC_VEX;
    unsigned flags = 0;
    if (result == PW_OK && !vex) {
        pw_ia32_print(&inst, ours, sizeof(ours));
        normalize(ours);
    }
    unsigned kind = KINDS;
    if (result == PW_OK && !llvm_length) kind = KIND_OURS_ONLY;
    else if (result == PW_ERR_MALFORMED && llvm_length) kind = KIND_LLVM_ONLY;
    else if ((result == PW_OK || result == PW_ERR_UNSUPPORTED) && llvm_length && inst.length != llvm_length)
        kind = KIND_LENGTH;
    else if (result == PW_OK && !vex && strcmp(ours, llvm)) kind = KIND_TEXT;
    else if (result == PW_OK && (vex || mode == PW_IA32_MODE_64) && !memory_agrees(&inst, llvm, ours, sizeof(ours)))
        kind = KIND_MEMORY;
    else if (result == PW_ERR_UNSUPPORTED && llvm_length) kind = KIND_NOFORM;
    else if (result == PW_OK && mode == PW_IA32_MODE_64 &&
             (kind = semantics_disagree(&inst, bytes, size, &flags)) < KINDS) {
        snprintf(ours, sizeof(ours), "%s: llvm%s%s%s%s", inst.form->name, flags & X86_SEM_LOAD ? " load" : "",
                 flags & X86_SEM_STORE ? " store" : "", flags & X86_SEM_FLOW ? " flow" : "",
                 flags & X86_SEM_STACK ? " stack" : "");
    } else if (result == PW_OK) {
        totals->agree_text++;
        if (vex) totals->agree_memory++;
        if (kind == KINDS) totals->agree_semantics++;
        if (kind == SKIPPED) totals->skipped_semantics++;
        kind = KINDS;
    } else {
        totals->agree_invalid++;
    }
    if (kind == KINDS) return;
    totals->kind[kind]++;
    group_add(kind, &inst, bytes, size, result == PW_OK ? ours : result == PW_ERR_UNSUPPORTED ? "(no form)" : "(bad)",
              inst.length, llvm_length ? llvm : "(invalid)", llvm_length);
}

static void emit_tails(uint8_t *buffer, size_t head, Totals *totals)
{
    for (unsigned t = 0; t < 2; t++) {
        memcpy(buffer + head, tails[t], sizeof(tails[t]));
        check_one(buffer, head + sizeof(tails[t]), totals);
    }
}

static void emit_modrm(uint8_t *buffer, size_t head, int address16, Totals *totals)
{
    for (unsigned modrm = 0; modrm < 256; modrm++) {
        buffer[head] = (uint8_t)modrm;
        const int has_sib = !address16 && (modrm >> 6) != 3 && (modrm & 7) == 4;
        if (!has_sib) {
            emit_tails(buffer, head + 1, totals);
            continue;
        }
        for (size_t s = 0; s < sizeof(sibs); s++) {
            buffer[head + 1] = sibs[s];
            emit_tails(buffer, head + 2, totals);
        }
    }
}

static int shape_takes_modrm(char shape)
{
    return strchr("mBZfF3QVvx", shape) != NULL;
}

static void emit_opcode(const uint8_t *escape, size_t escape_length, unsigned map, unsigned opcode, Totals *totals)
{
    const char shape = pw_ia32_shape(map, opcode);
    if (map == PW_IA32_MAP_PRI && (shape == 'P' || shape == '0')) return;
    if (map == PW_IA32_MAP_PRI && mode == PW_IA32_MODE_64 && (opcode & 0xf0) == 0x40) return;   /* REX */
    if (map == PW_IA32_MAP_0F && (shape == '8' || shape == 'A')) return;
    for (size_t p = 0; p < prefix_set_count; p++) {
        uint8_t buffer[48];
        current_prefix_set = (unsigned)p;
        size_t head = 0;
        for (unsigned i = 0; i < prefix_sets[p].length; i++) buffer[head++] = prefix_sets[p].bytes[i];
        int address16 = 0;   /* 16-bit addressing (no SIB): 67 in 32-bit mode only */
        for (unsigned i = 0; i < prefix_sets[p].length; i++)
            if (prefix_sets[p].bytes[i] == 0x67 && mode != PW_IA32_MODE_64) address16 = 1;
        memcpy(buffer + head, escape, escape_length);
        head += escape_length;
        buffer[head++] = (uint8_t)opcode;
        if (shape_takes_modrm(shape)) emit_modrm(buffer, head, address16, totals);
        else emit_tails(buffer, head, totals);
    }
}

/* Long mode's VEX corpus: every map (VEX3) and map 1 (VEX2) × pp × L × W, with R X B all clear and
 * all set, and vvvv unused (0000) or 1001 (with R X B clear): the CPU and LLVM reject a vvvv an
 * instruction does not use, so the second checks the table's NOV both ways. The prefix set records
 * pp, L, W and a used vvvv (".v"). EVEX and XOP keep the 32-bit corpus's payloads (lengths only). */
static void emit_vex_corpus_64(Totals *totals)
{
    for (unsigned op = 0; op < 256; op++) {
        uint8_t buffer[48];
        for (unsigned v = 0; v < 128; v++) {
            const unsigned pp = v & 3, l = (v >> 2) & 1, w = (v >> 3) & 1, rxb = (v >> 4) & 1, two = (v >> 5) & 1;
            const unsigned vvvv = v >> 6 ? 0x30 : 0x78;   /* inverted: 1001, or 0000 */
            if (two && (w || rxb)) continue;   /* VEX2: W 0, no X or B */
            if ((v >> 6) && rxb) continue;
            current_prefix_set = VEX_SET + pp + 4 * l + 8 * w + 16 * (v >> 6);
            for (unsigned map = 1; map <= (two ? 1u : 3u); map++) {
                size_t head = 0;
                if (two) {
                    buffer[head++] = 0xc5;
                    buffer[head++] = (uint8_t)((rxb ? 0 : 0x80) | vvvv | (l << 2) | pp);
                } else {
                    buffer[head++] = 0xc4;
                    buffer[head++] = (uint8_t)((rxb ? 0 : 0xe0) | map);
                    buffer[head++] = (uint8_t)((w << 7) | vvvv | (l << 2) | pp);
                }
                buffer[head++] = (uint8_t)op;
                if (map == 1 && op == 0x77) emit_tails(buffer, head, totals);
                else emit_modrm(buffer, head, 0, totals);
            }
        }
    }
}

/* VEX (C5 two-byte, C4 three-byte), EVEX (62) and XOP (8F) payloads that name each map, with
 * W/L/pp varied; the first payload bits are 1, as 32-bit mode requires. Long mode keeps the EVEX and
 * XOP payloads, and VEX has its own corpus (above). */
static void emit_vex_corpus(Totals *totals)
{
    static const uint8_t vex2[][1] = { { 0xf8 }, { 0xf9 }, { 0xfc }, { 0xfd }, { 0x78 } };
    static const uint8_t vex3[][2] = { { 0xe1, 0x78 }, { 0xe1, 0xfd }, { 0xe2, 0x79 }, { 0xe2, 0xf9 },
                                       { 0xe3, 0x79 }, { 0xe3, 0x7d } };
    static const uint8_t evex[][3] = { { 0xf1, 0x7c, 0x08 }, { 0xf1, 0xfd, 0x48 }, { 0xf2, 0x7d, 0x08 },
                                       { 0xf3, 0x7d, 0x28 } };
    static const uint8_t xop[][2] = { { 0xe8, 0x78 }, { 0xe9, 0x78 }, { 0xea, 0x78 } };
    if (mode == PW_IA32_MODE_64) emit_vex_corpus_64(totals);
    current_prefix_set = 0;
    for (unsigned op = 0; op < 256; op++) {
        uint8_t buffer[48];
        for (size_t v = 0; mode != PW_IA32_MODE_64 && v < sizeof(vex2) / sizeof(vex2[0]); v++) {
            buffer[0] = 0xc5; buffer[1] = vex2[v][0]; buffer[2] = (uint8_t)op;
            emit_modrm(buffer, 3, 0, totals);
        }
        for (size_t v = 0; mode != PW_IA32_MODE_64 && v < sizeof(vex3) / sizeof(vex3[0]); v++) {
            buffer[0] = 0xc4; buffer[1] = vex3[v][0]; buffer[2] = vex3[v][1]; buffer[3] = (uint8_t)op;
            emit_modrm(buffer, 4, 0, totals);
        }
        for (size_t v = 0; v < sizeof(evex) / sizeof(evex[0]); v++) {
            buffer[0] = 0x62; memcpy(buffer + 1, evex[v], 3); buffer[4] = (uint8_t)op;
            emit_modrm(buffer, 5, 0, totals);
        }
        for (size_t v = 0; v < sizeof(xop) / sizeof(xop[0]); v++) {
            buffer[0] = 0x8f; buffer[1] = xop[v][0]; buffer[2] = xop[v][1]; buffer[3] = (uint8_t)op;
            emit_modrm(buffer, 4, 0, totals);
        }
    }
}

/* A prefix set's name: its bytes in hex ("-" for none), or a VEX payload's "vex.<pp>.L<l>.W<w>[.v]". */
static void prefix_set_name(unsigned set, char *text, size_t capacity)
{
    static const char *const pp[4] = { "np", "66", "f3", "f2" };
    if (set >= VEX_SET) {
        snprintf(text, capacity, "vex.%s.L%u.W%u%s", pp[(set - VEX_SET) & 3], ((set - VEX_SET) >> 2) & 1,
                 ((set - VEX_SET) >> 3) & 1, (set - VEX_SET) >> 4 ? ".v" : "");
        return;
    }
    snprintf(text, capacity, "-");
    for (unsigned i = 0; set < prefix_set_count && i < prefix_sets[set].length; i++)
        snprintf(text + 2 * i, capacity - 2 * i, "%02x", prefix_sets[set].bytes[i]);
}

/* The differences file: "<KIND> <MAP>:<op>[-<op>][/<reg>] [p=<prefixes>[,<prefixes>...]] <reason>",
 * '#' comments. p= limits the line to those prefix sets ("-" is no prefix), written as in the
 * groups file (66f3, vex.66.L1.W0, vex.66.L1.W0.v with a vvvv in use). */
static int prefix_set_named(const char *name)
{
    for (unsigned set = 0; set < SETS; set++) {
        if (set >= prefix_set_count && set < VEX_SET) continue;
        char text[24];
        prefix_set_name(set, text, sizeof(text));
        if (!strcmp(name, text)) return (int)set;
    }
    return -1;
}

static void load_differences(const char *path)
{
    FILE *file = fopen(path, "r");
    if (!file) return;
    char line[512];
    static const char *const maps[] = { "PRI", "0F", "0F38", "0F3A" };
    while (fgets(line, sizeof(line), file)) {
        char kind_name[32], where[32], filter[96] = "";
        int consumed = 0;
        if (line[0] == '#' || sscanf(line, "%31s %31s %n", kind_name, where, &consumed) < 2) continue;
        if (!strncmp(line + consumed, "p=", 2)) {
            int more = 0;
            sscanf(line + consumed + 2, "%95s %n", filter, &more);
            consumed += 2 + more;
        }
        unsigned kind = KINDS, map = PW_IA32_MAPS, first = 0, last = 0, reg = NO_REG_SLOT;
        for (unsigned k = 0; k < KINDS; k++) if (!strcmp(kind_name, kind_names[k])) kind = k;
        char map_name[8], rest[24] = "";
        if (kind == KINDS || sscanf(where, "%7[^:]:%x%23s", map_name, &first, rest) < 2) continue;
        last = first;
        const char *r = rest;
        if (*r == '-') { last = (unsigned)strtoul(r + 1, (char **)&r, 16); }
        if (*r == '/') reg = (unsigned)strtoul(r + 1, NULL, 10);
        for (unsigned m = 0; m < PW_IA32_MAPS; m++) if (!strcmp(map_name, maps[m])) map = m;
        if (map == PW_IA32_MAPS || last > 255 || first > last || reg > NO_REG_SLOT) continue;
        unsigned sets[SETS], set_count = 0;
        for (char *name = strtok(filter, ","); name; name = strtok(NULL, ",")) {
            const int set = prefix_set_named(name);
            if (set >= 0 && set_count < SETS) sets[set_count++] = (unsigned)set;
        }
        if (!filter[0]) for (unsigned p = 0; p < SETS; p++) sets[set_count++] = p;
        char *reason = strdup(line + consumed);
        reason[strcspn(reason, "\n")] = 0;
        for (unsigned op = first; op <= last; op++)
            for (unsigned g = 0; g <= NO_REG_SLOT; g++)
                for (unsigned i = 0; i < set_count; i++)
                    if (reg == NO_REG_SLOT || g == reg) reasons[kind][map][op][g][sets[i]] = reason;
    }
    fclose(file);
}

static void json_string(FILE *out, const char *text)
{
    fputc('"', out);
    for (; *text; text++) {
        if (*text == '"' || *text == '\\') fputc('\\', out);
        fputc(*text, out);
    }
    fputc('"', out);
}

static int group_order(const void *a, const void *b)
{
    const uint32_t x = ((const Group *)a)->key, y = ((const Group *)b)->key;
    return (x > y) - (x < y);
}

static unsigned write_groups(FILE *out, unsigned *unexplained)
{
    static const char *const maps[] = { "PRI", "0F", "0F38", "0F3A" };
    qsort(groups, GROUP_SLOTS, sizeof(Group), group_order);
    unsigned written = 0;
    for (unsigned i = 0; i < GROUP_SLOTS; i++) {
        const Group *g = &groups[i];
        if (!g->key) continue;
        uint32_t rest = g->key - 1;
        const unsigned set = rest % SETS; rest /= SETS;
        const unsigned mod3 = rest % 2; rest /= 2;
        const unsigned r = rest % 9; rest /= 9;
        const unsigned op = rest % 256; rest /= 256;
        const unsigned m = rest % PW_IA32_MAPS; rest /= PW_IA32_MAPS;
        const unsigned k = rest;
        const char *reason = reasons[k][m][op][r][set];
        written++;
        if (k != KIND_NOFORM && !reason) unexplained[k]++;
        char set_name[24] = "";
        if (set) prefix_set_name(set, set_name, sizeof(set_name));
        fprintf(out, "{\"kind\":\"%s\",\"map\":\"%s\",\"opcode\":\"%02X\",\"reg\":%d,\"mod3\":%u,\"prefixes\":\"%s",
                kind_names[k], maps[m], op, r == NO_REG_SLOT ? -1 : (int)r, mod3, set_name);
        fprintf(out, "\",\"count\":%u,\"bytes\":\"", g->count);
        for (unsigned b = 0; b < g->length; b++) fprintf(out, "%02x", g->bytes[b]);
        fprintf(out, "\",\"ours_length\":%d,\"llvm_length\":%d,\"ours\":", g->ours_length, g->llvm_length);
        json_string(out, g->ours);
        fputs(",\"llvm\":", out);
        json_string(out, g->llvm);
        if (reason) {
            fputs(",\"explained\":", out);
            json_string(out, reason);
        }
        fputs("}\n", out);
    }
    return written;
}

int main(int argc, char **argv)
{
    if (argc == 4 && !strcmp(argv[1], "--64")) {
        mode = PW_IA32_MODE_64;
        prefix_sets = prefix_sets_64;
        prefix_set_count = sizeof(prefix_sets_64) / sizeof(prefix_sets_64[0]);
        argv++;
        argc--;
    }
    if (argc != 3) {
        fprintf(stderr, "usage: %s [--64] <differences file> <groups.jsonl>\n", argv[0]);
        return 2;
    }
    LLVMInitializeX86TargetInfo();
    LLVMInitializeX86TargetMC();
    LLVMInitializeX86Disassembler();
    llvm_context = LLVMCreateDisasm(mode == PW_IA32_MODE_64 ? "x86_64-unknown-unknown" : "i386-unknown-unknown", NULL,
                                    0, NULL, NULL);
    if (mode == PW_IA32_MODE_64 && x86_semantics_open(PW_IA32_MODE_64)) {
        fprintf(stderr, "LLVM's x86 MC layer is not available\n");
        return 2;
    }
    if (!llvm_context || !LLVMSetDisasmOptions(llvm_context, LLVMDisassembler_Option_AsmPrinterVariant)) {
        fprintf(stderr, "LLVM's x86 disassembler is not available\n");
        return 2;
    }
    groups = calloc(GROUP_SLOTS, sizeof(Group));
    if (!groups) {
        fprintf(stderr, "out of memory\n");
        return 2;
    }
    load_differences(argv[1]);
    Totals totals = { 0 };
    static const uint8_t escape_0f[] = { 0x0f }, escape_0f38[] = { 0x0f, 0x38 }, escape_0f3a[] = { 0x0f, 0x3a };
    for (unsigned op = 0; op < 256; op++) {
        emit_opcode(NULL, 0, PW_IA32_MAP_PRI, op, &totals);
        emit_opcode(escape_0f, 1, PW_IA32_MAP_0F, op, &totals);
        emit_opcode(escape_0f38, 2, PW_IA32_MAP_0F38, op, &totals);
        emit_opcode(escape_0f3a, 2, PW_IA32_MAP_0F3A, op, &totals);
    }
    emit_vex_corpus(&totals);
    FILE *out = fopen(argv[2], "w");
    if (!out) {
        perror(argv[2]);
        return 2;
    }
    unsigned unexplained[KINDS] = { 0 };
    const unsigned written = write_groups(out, unexplained);
    fclose(out);
    unsigned failing = 0;
    for (unsigned k = 0; k < KINDS; k++) failing += unexplained[k];
    printf("{%s\"verdict\":\"%s\",\"encodings\":%llu,\"agree_text\":%llu,\"agree_invalid\":%llu,"
           "\"vex_evex_xop_length_agree\":%llu,\"groups\":%u,\"unexplained_groups\":%u",
           mode == PW_IA32_MODE_64 ? "\"mode\":64," : "",
           failing ? "FAIL" : "PASS", (unsigned long long)totals.encodings, (unsigned long long)totals.agree_text,
           (unsigned long long)totals.agree_invalid, (unsigned long long)totals.skipped_vex, written, failing);
    if (mode == PW_IA32_MODE_64)
        printf(",\"vex_memory_agree\":%llu,\"semantics_agree\":%llu,\"semantics_skipped\":%llu",
               (unsigned long long)totals.agree_memory, (unsigned long long)totals.agree_semantics,
               (unsigned long long)totals.skipped_semantics);
    /* 32-bit mode keeps its five kinds, so its summary reads as it always has. */
    for (unsigned k = 0; k < (mode == PW_IA32_MODE_64 ? KINDS : KIND_NOFORM + 1u); k++)
        printf(",\"%s\":{\"encodings\":%llu,\"unexplained_groups\":%u}", kind_names[k],
               (unsigned long long)totals.kind[k], unexplained[k]);
    unsigned major = 0, minor = 0, patch = 0;
    LLVMGetVersion(&major, &minor, &patch);
    printf(",\"forms\":%zu,\"llvm\":\"%u.%u.%u\"}\n", pw_ia32_form_count(), major, minor, patch);
    LLVMDisasmDispose(llvm_context);
    return failing ? 1 : 0;
}
