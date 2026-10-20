/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pw_ia32_print.h"

/* Text goes out only through a Writer, which never writes past capacity and remembers overflow. */
typedef struct Writer {
    char *out;
    size_t capacity, at;
    int overflow;
} Writer;

static void put_char(Writer *w, char ch)
{
    if (w->at + 1 < w->capacity) w->out[w->at++] = ch;
    else w->overflow = 1;
}

static void put_text(Writer *w, const char *text)
{
    while (*text) put_char(w, *text++);
}

static void put_unsigned(Writer *w, uint64_t value)
{
    char digits[20];
    unsigned n = 0;
    do { digits[n++] = (char)('0' + value % 10); value /= 10; } while (value);
    while (n) put_char(w, digits[--n]);
}

static void put_signed(Writer *w, int64_t value)
{
    if (value < 0) {
        put_char(w, '-');
        put_unsigned(w, (uint64_t)0 - (uint64_t)value);
    } else {
        put_unsigned(w, (uint64_t)value);
    }
}

static const char *const reg8[8] = { "al", "cl", "dl", "bl", "ah", "ch", "dh", "bh" };
static const char *const reg8_rex[16] = { "al", "cl", "dl", "bl", "spl", "bpl", "sil", "dil",
                                          "r8b", "r9b", "r10b", "r11b", "r12b", "r13b", "r14b", "r15b" };
static const char *const reg16[16] = { "ax", "cx", "dx", "bx", "sp", "bp", "si", "di",
                                       "r8w", "r9w", "r10w", "r11w", "r12w", "r13w", "r14w", "r15w" };
static const char *const reg32[16] = { "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi",
                                       "r8d", "r9d", "r10d", "r11d", "r12d", "r13d", "r14d", "r15d" };
static const char *const reg64[16] = { "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
                                       "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15" };
static const char *const sreg[8] = { "es", "cs", "ss", "ds", "fs", "gs", "?", "?" };

/* A general register by number (0-15) and width; with any REX, byte registers 4-7 are SPL-DIL. */
static const char *register_name(const PwIa32Inst *inst, unsigned number, unsigned bits)
{
    number &= 15;
    if (bits == 8) return inst->rex ? reg8_rex[number] : reg8[number & 7];
    if (bits == 16) return reg16[number];
    if (bits == 64) return reg64[number];
    return reg32[number];
}

/* Bits of the data an operand names: 8, 16 or 32, or 0 for memory printed without a size. */
static unsigned operand_bits(unsigned kind, const PwIa32Inst *inst)
{
    switch (kind) {
    case PW_IA32_OP_Eb: case PW_IA32_OP_Gb: case PW_IA32_OP_Zb: case PW_IA32_OP_Ob: case PW_IA32_OP_Mb:
    case PW_IA32_OP_Xb: case PW_IA32_OP_Yb: case PW_IA32_OP_AL: case PW_IA32_OP_CL: case PW_IA32_OP_Wb:
        return 8;
    case PW_IA32_OP_Ew: case PW_IA32_OP_Gw: case PW_IA32_OP_DX: case PW_IA32_OP_Mw: case PW_IA32_OP_AX:
        return 16;
    case PW_IA32_OP_Ed: case PW_IA32_OP_Gd: case PW_IA32_OP_Md:
        return 32;
    case PW_IA32_OP_Mq:
        return 64;
    case PW_IA32_OP_Mt:
        return 80;
    case PW_IA32_OP_Mx: case PW_IA32_OP_Wdq:
        return 128;
    case PW_IA32_OP_Ma:
        return inst->operand_size;
    case PW_IA32_OP_M: case PW_IA32_OP_Mp:
        return 0;
    default:
        return inst->operand_size;
    }
}

static const char *size_word(unsigned bits)
{
    switch (bits) {
    case 8: return "byte ptr ";
    case 16: return "word ptr ";
    case 32: return "dword ptr ";
    case 64: return "qword ptr ";
    case 80: return "tbyte ptr ";
    case 128: return "xmmword ptr ";
    case 256: return "ymmword ptr ";
    default: return "";
    }
}

static void put_segment(Writer *w, unsigned segment)
{
    if (segment == PW_IA32_SEG_NONE) return;
    put_text(w, sreg[segment & 7]);
    put_char(w, ':');
}

/* LLVM names the SIB byte's absent index "eiz" ("riz" with 64-bit addressing) unless the base is
 * ESP (RSP, R12) at scale 1, or, in long mode, there is no base at scale 1. */
static int sib_shows_eiz(const PwIa32Inst *inst)
{
    if (!inst->has_sib || inst->index != PW_IA32_NO_REG) return 0;
    if (inst->scale != 1) return 1;
    if (inst->base == PW_IA32_NO_REG) return inst->mode != PW_IA32_MODE_64;
    return (inst->base & 7) != PW_IA32_ESP;
}

/* [base + scale*index + disp], as LLVM spells it; [rip + disp] (eip with 67) in long mode. */
static void put_address(Writer *w, const PwIa32Inst *inst)
{
    const unsigned bits = inst->address_size;
    const int has_base = inst->base != PW_IA32_NO_REG;
    const int eiz = sib_shows_eiz(inst);
    const int has_index = inst->index != PW_IA32_NO_REG || eiz;
    put_char(w, '[');
    if (inst->rip_relative) put_text(w, bits == 64 ? "rip" : "eip");
    if (has_base) put_text(w, register_name(inst, inst->base, bits));
    if (has_index) {
        if (has_base) put_text(w, " + ");
        if (inst->scale != 1 || !has_base) {
            put_unsigned(w, inst->scale);
            put_char(w, '*');
        }
        put_text(w, eiz ? (bits == 64 ? "riz" : "eiz") : register_name(inst, inst->index, bits));
    }
    if (inst->rip_relative) {
        if (inst->disp > 0) put_text(w, " + ");
        else if (inst->disp < 0) put_text(w, " - ");
        if (inst->disp) put_unsigned(w, inst->disp > 0 ? (uint32_t)inst->disp : (uint32_t)0 - (uint32_t)inst->disp);
    } else if (!has_base && !has_index) {
        put_signed(w, inst->disp);
    } else if (inst->disp > 0) {
        put_text(w, " + ");
        put_unsigned(w, (uint32_t)inst->disp);
    } else if (inst->disp < 0) {
        put_text(w, " - ");
        put_unsigned(w, (uint32_t)0 - (uint32_t)inst->disp);
    }
    put_char(w, ']');
}

static void put_memory(Writer *w, const PwIa32Inst *inst, unsigned bits)
{
    put_text(w, size_word(bits));
    put_segment(w, inst->segment);
    put_address(w, inst);
}

/* An r/m operand: a register of `bits` when mod is 3, else memory of `memory_bits`. */
static void put_rm(Writer *w, const PwIa32Inst *inst, unsigned bits, unsigned memory_bits)
{
    if (inst->mod == 3) put_text(w, register_name(inst, inst->rm, bits));
    else put_memory(w, inst, memory_bits);
}

/* moffs: an absolute address, printed unsigned; LLVM prints a 64-bit one signed. */
static void put_moffs(Writer *w, const PwIa32Inst *inst, unsigned bits)
{
    put_text(w, size_word(bits));
    put_segment(w, inst->segment);
    put_char(w, '[');
    if (inst->address_size == 64) put_signed(w, (int64_t)inst->imm);
    else put_unsigned(w, inst->imm);
    put_char(w, ']');
}

/* A string operand: DS:[eSI] (DS can be overridden) or ES:[eDI] (it cannot). */
static void put_string(Writer *w, const PwIa32Inst *inst, unsigned bits, int destination)
{
    put_text(w, size_word(bits));
    if (destination) put_segment(w, PW_IA32_SEG_ES);
    else put_segment(w, inst->segment);
    put_char(w, '[');
    put_text(w, register_name(inst, destination ? PW_IA32_EDI : PW_IA32_ESI, inst->address_size));
    put_char(w, ']');
}

/* The relative target: the displacement after the instruction. */
static void put_relative(Writer *w, const PwIa32Inst *inst)
{
    put_signed(w, inst->imm_size == 1 ? (int8_t)inst->imm : inst->imm_size == 2 ? (int16_t)inst->imm
                                                                                  : (int32_t)inst->imm);
}

/* A 16-bit immediate as LLVM prints it: unsigned in a 16-bit operation, signed in a 32-bit one. */
static void put_imm16(Writer *w, const PwIa32Inst *inst, uint32_t value)
{
    if (inst->operand_size == 16) put_unsigned(w, value & 0xffff);
    else put_signed(w, (int16_t)value);
}

/* A vector register: "xmm3" (xmm8-15 with REX), "mm3" (REX does not reach MMX). */
static void put_vector(Writer *w, const char *bank, unsigned number)
{
    put_text(w, bank);
    put_unsigned(w, number & (bank[0] == 'x' ? 15u : 7u));
}

/* A vector operand in ModRM.rm: the register when mod is 3, else memory of `memory_bits`. */
static void put_vector_rm(Writer *w, const PwIa32Inst *inst, const char *bank, unsigned memory_bits)
{
    if (inst->mod == 3) put_vector(w, bank, inst->rm);
    else put_memory(w, inst, memory_bits);
}

/* One operand; returns 0 when the operand prints nothing (the shift count 1). */
static int put_operand(Writer *w, const PwIa32Inst *inst, unsigned kind)
{
    const unsigned bits = operand_bits(kind, inst);
    switch (kind) {
    case PW_IA32_OP_Eb: case PW_IA32_OP_Ew: case PW_IA32_OP_Ed: case PW_IA32_OP_Ev:
        put_rm(w, inst, bits, bits); return 1;
    case PW_IA32_OP_Evw:
        put_rm(w, inst, inst->operand_size, 16); return 1;
    case PW_IA32_OP_Gb: case PW_IA32_OP_Gw: case PW_IA32_OP_Gd: case PW_IA32_OP_Gv:
        put_text(w, register_name(inst, inst->reg, bits)); return 1;
    case PW_IA32_OP_By:
        put_text(w, register_name(inst, inst->vex_vvvv, bits)); return 1;
    case PW_IA32_OP_Ib: case PW_IA32_OP_Ibs:
        put_signed(w, (int8_t)inst->imm); return 1;
    case PW_IA32_OP_Iz:   /* sign-extended at operand size 64, and LLVM prints it so */
        if (inst->operand_size == 64) put_signed(w, (int32_t)inst->imm);
        else put_unsigned(w, inst->imm);
        return 1;
    case PW_IA32_OP_Iv:
        put_signed(w, (int64_t)inst->imm); return 1;
    case PW_IA32_OP_Ibu:
        put_unsigned(w, inst->imm); return 1;
    case PW_IA32_OP_Iw:
        put_imm16(w, inst, inst->imm); return 1;
    case PW_IA32_OP_Iws:
        put_signed(w, (int16_t)inst->imm); return 1;
    case PW_IA32_OP_Ib2:
        put_signed(w, (int8_t)inst->imm2); return 1;
    case PW_IA32_OP_Jb: case PW_IA32_OP_Jz:
        put_relative(w, inst); return 1;
    case PW_IA32_OP_Ob: case PW_IA32_OP_Ov:
        put_moffs(w, inst, bits); return 1;
    case PW_IA32_OP_M: case PW_IA32_OP_Mb: case PW_IA32_OP_Mw: case PW_IA32_OP_Md: case PW_IA32_OP_Mq:
    case PW_IA32_OP_Mt: case PW_IA32_OP_Mx: case PW_IA32_OP_Ma: case PW_IA32_OP_Mp:
        put_memory(w, inst, bits); return 1;
    case PW_IA32_OP_Ap:
        put_imm16(w, inst, inst->imm2);
        put_text(w, ", ");
        put_unsigned(w, inst->imm);
        return 1;
    case PW_IA32_OP_Sw:
        put_text(w, sreg[inst->reg & 7]); return 1;
    case PW_IA32_OP_Zb: case PW_IA32_OP_Zv:
        put_text(w, register_name(inst, (inst->opcode & 7u) | (inst->rex & 1u) << 3, bits)); return 1;
    case PW_IA32_OP_AL: put_text(w, "al"); return 1;
    case PW_IA32_OP_AX: put_text(w, "ax"); return 1;
    case PW_IA32_OP_ST0: put_text(w, "st"); return 1;
    case PW_IA32_OP_STi:
        put_text(w, "st(");
        put_unsigned(w, inst->rm & 7u);
        put_char(w, ')');
        return 1;
    case PW_IA32_OP_CL: put_text(w, "cl"); return 1;
    case PW_IA32_OP_DX: put_text(w, "dx"); return 1;
    case PW_IA32_OP_eAX: put_text(w, register_name(inst, PW_IA32_EAX, bits)); return 1;
    case PW_IA32_OP_ONE: return 0;
    case PW_IA32_OP_ES: put_text(w, "es"); return 1;
    case PW_IA32_OP_CS: put_text(w, "cs"); return 1;
    case PW_IA32_OP_SS: put_text(w, "ss"); return 1;
    case PW_IA32_OP_DS: put_text(w, "ds"); return 1;
    case PW_IA32_OP_FS: put_text(w, "fs"); return 1;
    case PW_IA32_OP_GS: put_text(w, "gs"); return 1;
    case PW_IA32_OP_Xb: case PW_IA32_OP_Xv:
        put_string(w, inst, bits, 0); return 1;
    case PW_IA32_OP_Yb: case PW_IA32_OP_Yv:
        put_string(w, inst, bits, 1); return 1;
    case PW_IA32_OP_Vx: put_vector(w, "xmm", inst->reg); return 1;
    case PW_IA32_OP_Wx: put_vector_rm(w, inst, "xmm", 128); return 1;
    case PW_IA32_OP_Wq: put_vector_rm(w, inst, "xmm", 64); return 1;
    case PW_IA32_OP_Wd: put_vector_rm(w, inst, "xmm", 32); return 1;
    case PW_IA32_OP_Ww: put_vector_rm(w, inst, "xmm", 16); return 1;
    case PW_IA32_OP_Wb: put_vector_rm(w, inst, "xmm", 8); return 1;
    case PW_IA32_OP_Wdq: put_vector_rm(w, inst, "xmm", 128); return 1;
    case PW_IA32_OP_Ux: put_vector(w, "xmm", inst->rm); return 1;
    case PW_IA32_OP_Pq: put_vector(w, "mm", inst->reg); return 1;
    case PW_IA32_OP_Qq: put_vector_rm(w, inst, "mm", 64); return 1;
    case PW_IA32_OP_Qd: put_vector_rm(w, inst, "mm", 32); return 1;
    case PW_IA32_OP_Nq: put_vector(w, "mm", inst->rm); return 1;
    case PW_IA32_OP_Ebd: put_rm(w, inst, 32, 8); return 1;
    case PW_IA32_OP_Ewd: put_rm(w, inst, 32, 16); return 1;
    case PW_IA32_OP_XMM0: put_text(w, "xmm0"); return 1;
    default:
        put_text(w, "?"); return 1;
    }
}

/* A comparison predicate below 8 goes into the mnemonic (cmpltps), and the imm8 is not printed. */
static int predicate_in_mnemonic(Writer *w, const PwIa32Inst *inst)
{
    static const char *const predicates[8] = { "eq", "lt", "le", "unord", "neq", "nlt", "nle", "ord" };
    if (!(inst->form->attributes & PW_IA32_ATTR_CMPPRED) || inst->imm >= 8) return 0;
    put_text(w, "cmp");
    put_text(w, predicates[inst->imm]);
    put_text(w, inst->form->mnemonic + 3);
    return 1;
}

/* Whether the printed operands show the address size: a memory operand with registers in it, a
 * string operand, moffs (its own opcode in LLVM), or a mnemonic chosen by address size. */
static int address_size_shown(const PwIa32Inst *inst)
{
    const PwIa32Form *form = inst->form;
    if (form->size == PW_IA32_SIZE_A16 || form->size == PW_IA32_SIZE_A32) return 1;
    for (unsigned i = 0; i < 3; i++) {
        const unsigned kind = form->operands[i];
        if (kind == PW_IA32_OP_Xb || kind == PW_IA32_OP_Xv || kind == PW_IA32_OP_Yb || kind == PW_IA32_OP_Yv ||
            kind == PW_IA32_OP_Ob || kind == PW_IA32_OP_Ov)
            return 1;
    }
    return inst->has_modrm && inst->mod != 3 &&
           (inst->base != PW_IA32_NO_REG || inst->index != PW_IA32_NO_REG || inst->rip_relative || sib_shows_eiz(inst));
}

/* The prefixes LLVM prints: addr16 (addr32 in long mode) when nothing else shows that 67 was there, lock, and F2/F3
 * before a one-byte-map form they do not select (XACQUIRE/XRELEASE where the form says so and the
 * prefix is next to the opcode). */
static void put_prefixes(Writer *w, const PwIa32Inst *inst)
{
    const PwIa32Form *form = inst->form;
    if (inst->has_67 && !address_size_shown(inst)) put_text(w, inst->mode == PW_IA32_MODE_64 ? "addr32\t" : "addr16\t");
    if (inst->lock) put_text(w, "lock\t");
    if (!inst->rep || form->prefix != PW_IA32_PFX_ANY) return;
    if (inst->rep == 0xf3 && inst->rep_last && (form->attributes & PW_IA32_ATTR_XREL)) put_text(w, "xrelease\t");
    else if (inst->rep == 0xf2 && inst->rep_last && (form->attributes & PW_IA32_ATTR_XACQ)) put_text(w, "xacquire\t");
    else if (inst->map == PW_IA32_MAP_PRI) put_text(w, inst->rep == 0xf3 ? "rep\t" : "repne\t");
}

int pw_ia32_print(const PwIa32Inst *inst, char *out, size_t capacity)
{
    if (!inst || !inst->form || !out || !capacity) return PW_ERR_PRECONDITION;
    if (inst->encoding != PW_IA32_ENC_LEGACY) {
        out[0] = 0;
        return PW_ERR_UNSUPPORTED;
    }
    Writer w = { out, capacity, 0, 0 };
    put_prefixes(&w, inst);
    const int predicate = predicate_in_mnemonic(&w, inst);
    if (!predicate) put_text(&w, inst->form->mnemonic);
    const char *separator = "\t";
    for (unsigned i = 0; i < 3 && inst->form->operands[i] != PW_IA32_OP_NONE; i++) {
        if (predicate && inst->form->operands[i] == PW_IA32_OP_Ibu) continue;
        Writer probe = w;   /* the separator only goes out when the operand prints something */
        put_text(&probe, separator);
        if (put_operand(&probe, inst, inst->form->operands[i])) {
            w = probe;
            separator = ", ";
        }
    }
    out[w.at] = 0;
    return w.overflow ? PW_ERR_LIMIT : PW_OK;
}

int pw_ia32_print_memory(const PwIa32Inst *inst, unsigned bits, char *out, size_t capacity)
{
    if (!inst || !out || !capacity || !inst->has_modrm || inst->mod == 3) return PW_ERR_PRECONDITION;
    Writer w = { out, capacity, 0, 0 };
    put_memory(&w, inst, bits);
    out[w.at] = 0;
    return w.overflow ? PW_ERR_LIMIT : PW_OK;
}
