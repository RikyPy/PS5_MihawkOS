/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pw_ia32_interp.h"
#include <stdatomic.h>
#include <string.h>

/* One instruction in flight. Handlers translate every memory operand first (operand_get), then
 * compute, then write: a fault raised by a translation leaves the state as it was. */
typedef struct Ctx {
    PwIa32State *s;
    const PwIa32Memory *m;
    PwIa32Event *e;
    PwIa32Inst inst;
    uint32_t next_eip;      /* EIP after this instruction */
    int partial;            /* a REP iteration ran before the fault: keep its progress */
} Ctx;

typedef int (*Handler)(Ctx *c);

enum { RAISED = 1 };        /* a handler's "an event was raised" (state as the event requires) */

/* ---------------------------------------------------------------- events */

static int raise_fault(Ctx *c, int vector, uint32_t code)
{
    c->e->vector = vector;
    c->e->code = code;
    return RAISED;
}

static int raise_page_fault(Ctx *c, uint32_t address)
{
    c->e->address = address;
    return raise_fault(c, PW_IA32_EXC_PF, 0);
}

/* A trap: the instruction completed, EIP is after it. */
static int raise_trap(Ctx *c, int vector, uint32_t code)
{
    c->s->eip = c->next_eip;
    return raise_fault(c, vector, code);
}

/* ---------------------------------------------------------------- registers and flags */

static uint32_t size_mask(unsigned bits)
{
    return bits == 32 ? 0xffffffffu : (1u << bits) - 1;
}

static uint32_t sign_bit(unsigned bits)
{
    return 1u << (bits - 1);
}

static uint32_t sign_extend(uint32_t value, unsigned bits)
{
    if (bits == 8) return (uint32_t)(int32_t)(int8_t)value;
    if (bits == 16) return (uint32_t)(int32_t)(int16_t)value;
    return value;
}

static uint32_t reg_read(const Ctx *c, unsigned number, unsigned bits)
{
    if (bits == 8) return number < 4 ? c->s->gpr[number] & 0xff : (c->s->gpr[number - 4] >> 8) & 0xff;
    return c->s->gpr[number & 7] & size_mask(bits);
}

static void reg_write(Ctx *c, unsigned number, unsigned bits, uint32_t value)
{
    if (bits == 8) {
        if (number < 4) c->s->gpr[number] = (c->s->gpr[number] & ~0xffu) | (value & 0xff);
        else c->s->gpr[number - 4] = (c->s->gpr[number - 4] & ~0xff00u) | ((value & 0xff) << 8);
    } else if (bits == 16) {
        c->s->gpr[number & 7] = (c->s->gpr[number & 7] & ~0xffffu) | (value & 0xffff);
    } else {
        c->s->gpr[number & 7] = value;
    }
}

static int flag(const Ctx *c, uint32_t bit)
{
    return (c->s->eflags & bit) != 0;
}

static void flags_set(Ctx *c, uint32_t value, uint32_t which)
{
    c->s->eflags = (c->s->eflags & ~which) | (value & which);
}

static uint32_t parity_flag(uint32_t value)
{
    value &= 0xff;
    value ^= value >> 4;
    value ^= value >> 2;
    value ^= value >> 1;
    return (value & 1) ? 0 : PW_IA32_EFLAGS_PF;
}

/* ZF, SF and PF of a result. */
static uint32_t flags_szp(uint32_t result, unsigned bits)
{
    uint32_t f = parity_flag(result);
    if (!(result & size_mask(bits))) f |= PW_IA32_EFLAGS_ZF;
    if (result & sign_bit(bits)) f |= PW_IA32_EFLAGS_SF;
    return f;
}

static uint32_t flags_add(uint32_t a, uint32_t b, uint32_t carry, unsigned bits, uint32_t *result)
{
    const uint32_t m = size_mask(bits);
    const uint64_t sum = (uint64_t)(a & m) + (b & m) + carry;
    const uint32_t r = (uint32_t)sum & m;
    uint32_t f = flags_szp(r, bits);
    if (sum > m) f |= PW_IA32_EFLAGS_CF;
    if ((a ^ b ^ r) & 0x10) f |= PW_IA32_EFLAGS_AF;
    if ((a ^ r) & (b ^ r) & sign_bit(bits)) f |= PW_IA32_EFLAGS_OF;
    *result = r;
    return f;
}

static uint32_t flags_sub(uint32_t a, uint32_t b, uint32_t borrow, unsigned bits, uint32_t *result)
{
    const uint32_t m = size_mask(bits);
    const uint32_t r = (a - b - borrow) & m;
    uint32_t f = flags_szp(r, bits);
    if ((uint64_t)(a & m) < (uint64_t)(b & m) + borrow) f |= PW_IA32_EFLAGS_CF;
    if ((a ^ b ^ r) & 0x10) f |= PW_IA32_EFLAGS_AF;
    if ((a ^ b) & (a ^ r) & sign_bit(bits)) f |= PW_IA32_EFLAGS_OF;
    *result = r;
    return f;
}

/* AND/OR/XOR/TEST: CF and OF cleared, AF left 0 (undefined). */
static uint32_t flags_logic(uint32_t result, unsigned bits)
{
    return flags_szp(result, bits);
}

/* The condition of a Jcc/SETcc/CMOVcc: the low nibble of its opcode. */
static int condition(const Ctx *c, unsigned cc)
{
    int r;
    switch ((cc >> 1) & 7) {
    case 0: r = flag(c, PW_IA32_EFLAGS_OF); break;
    case 1: r = flag(c, PW_IA32_EFLAGS_CF); break;
    case 2: r = flag(c, PW_IA32_EFLAGS_ZF); break;
    case 3: r = flag(c, PW_IA32_EFLAGS_CF) || flag(c, PW_IA32_EFLAGS_ZF); break;
    case 4: r = flag(c, PW_IA32_EFLAGS_SF); break;
    case 5: r = flag(c, PW_IA32_EFLAGS_PF); break;
    case 6: r = flag(c, PW_IA32_EFLAGS_SF) != flag(c, PW_IA32_EFLAGS_OF); break;
    default: r = flag(c, PW_IA32_EFLAGS_ZF) || flag(c, PW_IA32_EFLAGS_SF) != flag(c, PW_IA32_EFLAGS_OF); break;
    }
    return (cc & 1) ? !r : r;
}

/* ---------------------------------------------------------------- memory */

static unsigned data_segment(const Ctx *c)
{
    return c->inst.segment != PW_IA32_SEG_NONE ? c->inst.segment : c->inst.default_segment;
}

/* The offset a ModRM memory operand (or moffs) names, wrapped to the address size. */
static uint32_t effective_offset(const Ctx *c)
{
    const PwIa32Inst *i = &c->inst;
    uint32_t offset = (uint32_t)i->disp;
    if (i->base != PW_IA32_NO_REG) offset += c->s->gpr[i->base];
    if (i->index != PW_IA32_NO_REG) offset += c->s->gpr[i->index] * i->scale;
    return i->address_size == 16 ? offset & 0xffff : offset;
}

/* A host pointer to `bytes` at segment:offset, or a #PF raised (and NULL). */
static uint8_t *memory_at(Ctx *c, unsigned segment, uint32_t offset, unsigned bytes, int write)
{
    const uint32_t linear = c->s->base[segment] + offset;
    uint8_t *p = c->m->translate(c->m->opaque, linear, bytes, write);
    if (!p) raise_page_fault(c, linear);
    return p;
}

static uint32_t load(const uint8_t *p, unsigned bits)
{
    uint32_t v = 0;
    memcpy(&v, p, bits / 8);
    return v;
}

static void store(uint8_t *p, unsigned bits, uint32_t value)
{
    memcpy(p, &value, bits / 8);
}

static int push(Ctx *c, unsigned bits, uint32_t value)
{
    const uint32_t esp = c->s->gpr[PW_IA32_ESP] - bits / 8;
    uint8_t *p = memory_at(c, PW_IA32_SEG_SS, esp, bits / 8, 1);
    if (!p) return RAISED;
    store(p, bits, value);
    c->s->gpr[PW_IA32_ESP] = esp;
    return 0;
}

/* Reads the top of the stack without moving ESP (callers move it once nothing else can fault). */
static int peek(Ctx *c, unsigned bits, uint32_t depth, uint32_t *value)
{
    const uint8_t *p = memory_at(c, PW_IA32_SEG_SS, c->s->gpr[PW_IA32_ESP] + depth, bits / 8, 0);
    if (!p) return RAISED;
    *value = load(p, bits);
    return 0;
}

/* ---------------------------------------------------------------- operands */

enum { OPND_REG, OPND_MEM, OPND_IMM };

typedef struct Opnd {
    unsigned type, bits, reg;
    uint8_t *ptr;
    uint32_t imm;
} Opnd;

static unsigned operand_size(const Ctx *c)
{
    return c->inst.operand_size;
}

/* Resolves operand i of the form. Memory is translated now, writable when `write`. Returns 0, or
 * RAISED (#PF), or -1 for an operand kind this slice does not run. */
static int operand_get(Ctx *c, unsigned i, int write, Opnd *o)
{
    const PwIa32Inst *in = &c->inst;
    const unsigned kind = in->form->operands[i];
    const unsigned v = operand_size(c);
    memset(o, 0, sizeof(*o));
    unsigned bits = 0;
    switch (kind) {
    case PW_IA32_OP_Eb: bits = 8; break;
    case PW_IA32_OP_Ew: bits = 16; break;
    case PW_IA32_OP_Ed: bits = 32; break;
    case PW_IA32_OP_Ev: bits = v; break;
    case PW_IA32_OP_Gb: *o = (Opnd){ OPND_REG, 8, in->reg, NULL, 0 }; return 0;
    case PW_IA32_OP_Gw: *o = (Opnd){ OPND_REG, 16, in->reg, NULL, 0 }; return 0;
    case PW_IA32_OP_Gd: *o = (Opnd){ OPND_REG, 32, in->reg, NULL, 0 }; return 0;
    case PW_IA32_OP_Gv: *o = (Opnd){ OPND_REG, v, in->reg, NULL, 0 }; return 0;
    case PW_IA32_OP_Zb: *o = (Opnd){ OPND_REG, 8, in->opcode & 7u, NULL, 0 }; return 0;
    case PW_IA32_OP_Zv: *o = (Opnd){ OPND_REG, v, in->opcode & 7u, NULL, 0 }; return 0;
    case PW_IA32_OP_AL: *o = (Opnd){ OPND_REG, 8, PW_IA32_EAX, NULL, 0 }; return 0;
    case PW_IA32_OP_AX: *o = (Opnd){ OPND_REG, 16, PW_IA32_EAX, NULL, 0 }; return 0;
    case PW_IA32_OP_eAX: *o = (Opnd){ OPND_REG, v, PW_IA32_EAX, NULL, 0 }; return 0;
    case PW_IA32_OP_CL: *o = (Opnd){ OPND_REG, 8, PW_IA32_ECX, NULL, 0 }; return 0;
    case PW_IA32_OP_DX: *o = (Opnd){ OPND_REG, 16, PW_IA32_EDX, NULL, 0 }; return 0;
    case PW_IA32_OP_Ib: case PW_IA32_OP_Ibu: *o = (Opnd){ OPND_IMM, 8, 0, NULL, in->imm & 0xff }; return 0;
    case PW_IA32_OP_Ibs: *o = (Opnd){ OPND_IMM, v, 0, NULL, sign_extend(in->imm, 8) & size_mask(v) }; return 0;
    case PW_IA32_OP_Ib2: *o = (Opnd){ OPND_IMM, 8, 0, NULL, in->imm2 & 0xff }; return 0;
    case PW_IA32_OP_Iw: case PW_IA32_OP_Iws: *o = (Opnd){ OPND_IMM, 16, 0, NULL, in->imm & 0xffff }; return 0;
    case PW_IA32_OP_Iz: *o = (Opnd){ OPND_IMM, v, 0, NULL, in->imm & size_mask(v) }; return 0;
    case PW_IA32_OP_ONE: *o = (Opnd){ OPND_IMM, 8, 0, NULL, 1 }; return 0;
    case PW_IA32_OP_Ob: case PW_IA32_OP_Ov: {
        bits = kind == PW_IA32_OP_Ob ? 8 : v;
        uint8_t *p = memory_at(c, data_segment(c), effective_offset(c), bits / 8, write);
        if (!p) return RAISED;
        *o = (Opnd){ OPND_MEM, bits, 0, p, 0 };
        return 0;
    }
    default:
        return -1;
    }
    if (in->mod == 3) {
        *o = (Opnd){ OPND_REG, bits, in->rm, NULL, 0 };
        return 0;
    }
    uint8_t *p = memory_at(c, data_segment(c), effective_offset(c), bits / 8, write);
    if (!p) return RAISED;
    *o = (Opnd){ OPND_MEM, bits, 0, p, 0 };
    return 0;
}

static uint32_t operand_read(const Ctx *c, const Opnd *o)
{
    if (o->type == OPND_REG) return reg_read(c, o->reg, o->bits);
    if (o->type == OPND_MEM) return load(o->ptr, o->bits);
    return o->imm;
}

static void operand_write(Ctx *c, const Opnd *o, uint32_t value)
{
    if (o->type == OPND_REG) reg_write(c, o->reg, o->bits, value);
    else if (o->type == OPND_MEM) store(o->ptr, o->bits, value);
}

/* Operands 0 and 1, the first writable when `write`. 0, RAISED, or -1 (unsupported kind). */
static int operands2(Ctx *c, int write, Opnd *d, Opnd *s)
{
    int r = operand_get(c, 0, write, d);
    if (r) return r;
    return operand_get(c, 1, 0, s);
}

#define GET2(c, write, d, s) do { int r_ = operands2(c, write, d, s); if (r_) return r_; } while (0)
#define GET1(c, i, write, o) do { int r_ = operand_get(c, i, write, o); if (r_) return r_; } while (0)

static int done(Ctx *c)
{
    c->s->eip = c->next_eip;
    return 0;
}

/* ---------------------------------------------------------------- arithmetic and logic */

enum { ALU_ADD, ALU_OR, ALU_ADC, ALU_SBB, ALU_AND, ALU_SUB, ALU_XOR, ALU_CMP, ALU_TEST };

static int alu(Ctx *c, unsigned op)
{
    Opnd d, s;
    GET2(c, op != ALU_CMP && op != ALU_TEST, &d, &s);
    const unsigned bits = d.bits;
    const uint32_t a = operand_read(c, &d), b = operand_read(c, &s);
    const uint32_t cf = flag(c, PW_IA32_EFLAGS_CF);
    uint32_t r = 0, f;
    switch (op) {
    case ALU_ADD: f = flags_add(a, b, 0, bits, &r); break;
    case ALU_ADC: f = flags_add(a, b, cf, bits, &r); break;
    case ALU_SUB: case ALU_CMP: f = flags_sub(a, b, 0, bits, &r); break;
    case ALU_SBB: f = flags_sub(a, b, cf, bits, &r); break;
    case ALU_OR: r = (a | b) & size_mask(bits); f = flags_logic(r, bits); break;
    case ALU_XOR: r = (a ^ b) & size_mask(bits); f = flags_logic(r, bits); break;
    default: r = a & b & size_mask(bits); f = flags_logic(r, bits); break;
    }
    if (op != ALU_CMP && op != ALU_TEST) operand_write(c, &d, r);
    flags_set(c, f, PW_IA32_EFLAGS_ARITH);
    return done(c);
}

static int op_add(Ctx *c) { return alu(c, ALU_ADD); }
static int op_or(Ctx *c) { return alu(c, ALU_OR); }
static int op_adc(Ctx *c) { return alu(c, ALU_ADC); }
static int op_sbb(Ctx *c) { return alu(c, ALU_SBB); }
static int op_and(Ctx *c) { return alu(c, ALU_AND); }
static int op_sub(Ctx *c) { return alu(c, ALU_SUB); }
static int op_xor(Ctx *c) { return alu(c, ALU_XOR); }
static int op_cmp(Ctx *c) { return alu(c, ALU_CMP); }
static int op_test(Ctx *c) { return alu(c, ALU_TEST); }

static int incdec(Ctx *c, int dec)
{
    Opnd d;
    GET1(c, 0, 1, &d);
    uint32_t r;
    const uint32_t a = operand_read(c, &d);
    const uint32_t f = dec ? flags_sub(a, 1, 0, d.bits, &r) : flags_add(a, 1, 0, d.bits, &r);
    operand_write(c, &d, r);
    flags_set(c, f, PW_IA32_EFLAGS_ARITH & ~PW_IA32_EFLAGS_CF);
    return done(c);
}

static int op_inc(Ctx *c) { return incdec(c, 0); }
static int op_dec(Ctx *c) { return incdec(c, 1); }

static int op_not(Ctx *c)
{
    Opnd d;
    GET1(c, 0, 1, &d);
    operand_write(c, &d, ~operand_read(c, &d) & size_mask(d.bits));
    return done(c);
}

static int op_neg(Ctx *c)
{
    Opnd d;
    GET1(c, 0, 1, &d);
    uint32_t r;
    const uint32_t f = flags_sub(0, operand_read(c, &d), 0, d.bits, &r);
    operand_write(c, &d, r);
    flags_set(c, f, PW_IA32_EFLAGS_ARITH);
    return done(c);
}

/* ---------------------------------------------------------------- moves */

static int op_mov(Ctx *c)
{
    const uint8_t *ops = c->inst.form->operands;
    if (ops[0] == PW_IA32_OP_Sw) return PW_ERR_UNSUPPORTED;   /* a segment load */
    if (ops[1] == PW_IA32_OP_Sw) {                              /* MOV r/m, Sreg */
        const uint32_t selector = c->s->selector[c->inst.reg];
        if (c->inst.mod == 3) {
            reg_write(c, c->inst.rm, operand_size(c), selector);
            return done(c);
        }
        uint8_t *p = memory_at(c, data_segment(c), effective_offset(c), 2, 1);
        if (!p) return RAISED;
        store(p, 16, selector);
        return done(c);
    }
    Opnd d, s;
    GET2(c, 1, &d, &s);
    operand_write(c, &d, operand_read(c, &s));
    return done(c);
}

static int extend(Ctx *c, int sign)
{
    Opnd d, s;
    GET2(c, 0, &d, &s);
    uint32_t v = operand_read(c, &s);
    if (sign) v = sign_extend(v, s.bits);
    operand_write(c, &d, v);
    return done(c);
}

static int op_movzx(Ctx *c) { return extend(c, 0); }
static int op_movsx(Ctx *c) { return extend(c, 1); }

static int op_lea(Ctx *c)
{
    reg_write(c, c->inst.reg, operand_size(c), effective_offset(c));
    return done(c);
}

static int op_xchg(Ctx *c)
{
    Opnd d, s;
    int r = operand_get(c, 0, 1, &d);
    if (!r) r = operand_get(c, 1, 1, &s);
    if (r) return r;
    const uint32_t a = operand_read(c, &d), b = operand_read(c, &s);
    operand_write(c, &d, b);
    operand_write(c, &s, a);
    return done(c);
}

static int op_nop(Ctx *c) { return done(c); }

static int op_cbw_cwde(Ctx *c)
{
    const unsigned bits = operand_size(c);
    reg_write(c, PW_IA32_EAX, bits, sign_extend(reg_read(c, PW_IA32_EAX, bits / 2), bits / 2));
    return done(c);
}

static int op_cwd_cdq(Ctx *c)
{
    const unsigned bits = operand_size(c);
    reg_write(c, PW_IA32_EDX, bits, (reg_read(c, PW_IA32_EAX, bits) & sign_bit(bits)) ? 0xffffffffu : 0);
    return done(c);
}

static int op_xlatb(Ctx *c)
{
    const uint32_t offset = (c->s->gpr[PW_IA32_EBX] + reg_read(c, PW_IA32_EAX, 8)) &
                            (c->inst.address_size == 16 ? 0xffffu : 0xffffffffu);
    const uint8_t *p = memory_at(c, data_segment(c), offset, 1, 0);
    if (!p) return RAISED;
    reg_write(c, PW_IA32_EAX, 8, *p);
    return done(c);
}

/* ---------------------------------------------------------------- flags instructions */

static int op_clc(Ctx *c) { flags_set(c, 0, PW_IA32_EFLAGS_CF); return done(c); }
static int op_stc(Ctx *c) { flags_set(c, PW_IA32_EFLAGS_CF, PW_IA32_EFLAGS_CF); return done(c); }
static int op_cmc(Ctx *c) { c->s->eflags ^= PW_IA32_EFLAGS_CF; return done(c); }
static int op_cld(Ctx *c) { flags_set(c, 0, PW_IA32_EFLAGS_DF); return done(c); }
static int op_std(Ctx *c) { flags_set(c, PW_IA32_EFLAGS_DF, PW_IA32_EFLAGS_DF); return done(c); }

static int op_lahf(Ctx *c)
{
    reg_write(c, 4, 8, (c->s->eflags & 0xd5u) | 0x02u);   /* AH: SF ZF AF PF CF, bit 1 set */
    return done(c);
}

static int op_sahf(Ctx *c)
{
    flags_set(c, reg_read(c, 4, 8), 0xd5u);
    return done(c);
}

static int op_salc(Ctx *c)
{
    reg_write(c, PW_IA32_EAX, 8, flag(c, PW_IA32_EFLAGS_CF) ? 0xff : 0);
    return done(c);
}

/* ---------------------------------------------------------------- BCD */

static int op_aam(Ctx *c)
{
    const uint32_t base = c->inst.imm & 0xff, al = reg_read(c, PW_IA32_EAX, 8);
    if (!base) return raise_fault(c, PW_IA32_EXC_DE, 0);
    reg_write(c, PW_IA32_EAX, 16, ((al / base) << 8) | (al % base));
    flags_set(c, flags_szp(al % base, 8), PW_IA32_EFLAGS_ARITH);
    return done(c);
}

static int op_aad(Ctx *c)
{
    const uint32_t base = c->inst.imm & 0xff;
    const uint32_t al = (reg_read(c, PW_IA32_EAX, 8) + reg_read(c, 4, 8) * base) & 0xff;
    reg_write(c, PW_IA32_EAX, 16, al);
    flags_set(c, flags_szp(al, 8), PW_IA32_EFLAGS_ARITH);
    return done(c);
}

static int op_daa(Ctx *c)
{
    const uint32_t old_al = reg_read(c, PW_IA32_EAX, 8), old_cf = flag(c, PW_IA32_EFLAGS_CF);
    uint32_t al = old_al, f = 0;
    if ((al & 0xf) > 9 || flag(c, PW_IA32_EFLAGS_AF)) {
        al = (al + 6) & 0xff;
        f |= PW_IA32_EFLAGS_AF;
        if (old_cf || old_al > 0xf9) f |= PW_IA32_EFLAGS_CF;
    }
    if (old_al > 0x99 || old_cf) {
        al = (al + 0x60) & 0xff;
        f |= PW_IA32_EFLAGS_CF;
    } else {
        f &= ~PW_IA32_EFLAGS_CF;
    }
    reg_write(c, PW_IA32_EAX, 8, al);
    flags_set(c, f | flags_szp(al, 8), PW_IA32_EFLAGS_ARITH & ~PW_IA32_EFLAGS_OF);
    return done(c);
}

static int op_das(Ctx *c)
{
    const uint32_t old_al = reg_read(c, PW_IA32_EAX, 8), old_cf = flag(c, PW_IA32_EFLAGS_CF);
    uint32_t al = old_al, f = 0;
    if ((al & 0xf) > 9 || flag(c, PW_IA32_EFLAGS_AF)) {
        al = (al - 6) & 0xff;
        f |= PW_IA32_EFLAGS_AF;
        if (old_cf || old_al < 6) f |= PW_IA32_EFLAGS_CF;
    }
    if (old_al > 0x99 || old_cf) {
        al = (al - 0x60) & 0xff;
        f |= PW_IA32_EFLAGS_CF;
    }
    reg_write(c, PW_IA32_EAX, 8, al);
    flags_set(c, f | flags_szp(al, 8), PW_IA32_EFLAGS_ARITH & ~PW_IA32_EFLAGS_OF);
    return done(c);
}

static int ascii_adjust(Ctx *c, int subtract)
{
    uint32_t ax = reg_read(c, PW_IA32_EAX, 16), f = 0;
    if ((ax & 0xf) > 9 || flag(c, PW_IA32_EFLAGS_AF)) {
        /* AAS: AX - 6 (the borrow reaches AH), then AH - 1; AAA: AX + 106h. */
        ax = subtract ? ((ax - 6) & 0xffff) - 0x100 : ax + 0x106;
        f = PW_IA32_EFLAGS_AF | PW_IA32_EFLAGS_CF;
    }
    reg_write(c, PW_IA32_EAX, 16, ax & 0xff0f);
    flags_set(c, f, PW_IA32_EFLAGS_AF | PW_IA32_EFLAGS_CF);
    return done(c);
}

static int op_aaa(Ctx *c) { return ascii_adjust(c, 0); }
static int op_aas(Ctx *c) { return ascii_adjust(c, 1); }

/* ---------------------------------------------------------------- shifts and rotates */

enum { SH_ROL, SH_ROR, SH_RCL, SH_RCR, SH_SHL, SH_SHR, SH_SAL, SH_SAR };

static int shift(Ctx *c, unsigned op)
{
    Opnd d, s;
    GET2(c, 1, &d, &s);
    const unsigned bits = d.bits;
    const uint32_t m = size_mask(bits), msb = sign_bit(bits);
    const uint32_t a = operand_read(c, &d) & m;
    const unsigned count = operand_read(c, &s) & 0x1f;
    if (!count) return done(c);      /* a masked count of 0 changes nothing, flags included */
    uint32_t r = a, f = c->s->eflags;
    unsigned cf = flag(c, PW_IA32_EFLAGS_CF);
    switch (op) {
    case SH_ROL: {
        const unsigned n = count % bits;
        r = n ? ((a << n) | (a >> (bits - n))) & m : a;
        cf = r & 1;
        f = (f & ~(PW_IA32_EFLAGS_CF | PW_IA32_EFLAGS_OF)) | cf |
            ((((r & msb) != 0) ^ cf) ? PW_IA32_EFLAGS_OF : 0);
        break;
    }
    case SH_ROR: {
        const unsigned n = count % bits;
        r = n ? ((a >> n) | (a << (bits - n))) & m : a;
        cf = (r & msb) != 0;
        f = (f & ~(PW_IA32_EFLAGS_CF | PW_IA32_EFLAGS_OF)) | cf |
            ((((r & msb) != 0) ^ ((r & (msb >> 1)) != 0)) ? PW_IA32_EFLAGS_OF : 0);
        break;
    }
    case SH_RCL: case SH_RCR: {
        const unsigned n = count % (bits + 1);
        const unsigned of_rcr = ((a & msb) != 0) ^ cf;
        for (unsigned k = 0; k < n; k++) {
            if (op == SH_RCL) {
                const unsigned out = (r & msb) != 0;
                r = ((r << 1) | cf) & m;
                cf = out;
            } else {
                const unsigned out = r & 1;
                r = (r >> 1) | (cf ? msb : 0);
                cf = out;
            }
        }
        const unsigned of = op == SH_RCL ? (((r & msb) != 0) ^ cf) : of_rcr;
        f = (f & ~(PW_IA32_EFLAGS_CF | PW_IA32_EFLAGS_OF)) | cf | (of ? PW_IA32_EFLAGS_OF : 0);
        break;
    }
    case SH_SHL: case SH_SAL:
        r = count < bits ? (a << count) & m : 0;
        cf = count <= bits ? (a >> (bits - count)) & 1 : 0;
        f = (f & ~PW_IA32_EFLAGS_ARITH) | flags_szp(r, bits) | cf |
            ((((r & msb) != 0) ^ cf) ? PW_IA32_EFLAGS_OF : 0);
        break;
    case SH_SHR:
        r = count < bits ? a >> count : 0;
        cf = count <= bits ? (a >> (count - 1)) & 1 : 0;
        f = (f & ~PW_IA32_EFLAGS_ARITH) | flags_szp(r, bits) | cf | ((a & msb) ? PW_IA32_EFLAGS_OF : 0);
        break;
    default: {  /* SAR */
        const int32_t sa = (int32_t)sign_extend(a, bits);
        const unsigned n = count < bits ? count : bits - 1;
        r = (uint32_t)(sa >> n) & m;
        cf = count < bits ? ((uint32_t)(sa >> (count - 1)) & 1) : ((a & msb) != 0);
        f = (f & ~PW_IA32_EFLAGS_ARITH) | flags_szp(r, bits) | cf;
        break;
    }
    }
    operand_write(c, &d, r);
    c->s->eflags = f;
    return done(c);
}

static int op_rol(Ctx *c) { return shift(c, SH_ROL); }
static int op_ror(Ctx *c) { return shift(c, SH_ROR); }
static int op_rcl(Ctx *c) { return shift(c, SH_RCL); }
static int op_rcr(Ctx *c) { return shift(c, SH_RCR); }
static int op_shl(Ctx *c) { return shift(c, SH_SHL); }
static int op_shr(Ctx *c) { return shift(c, SH_SHR); }
static int op_sal(Ctx *c) { return shift(c, SH_SAL); }
static int op_sar(Ctx *c) { return shift(c, SH_SAR); }

static int double_shift(Ctx *c, int left)
{
    Opnd d, s, n;
    int r = operands2(c, 1, &d, &s);
    if (!r) r = operand_get(c, 2, 0, &n);
    if (r) return r;
    const unsigned bits = d.bits;
    const unsigned count = operand_read(c, &n) & 0x1f;
    if (!count) return done(c);
    const uint64_t a = operand_read(c, &d), b = operand_read(c, &s);
    uint32_t result, cf;
    if (left) {
        const uint64_t wide = (a << bits) | b;               /* a:b, shifted left */
        result = (uint32_t)((wide << count) >> bits) & size_mask(bits);
        cf = count <= bits ? (uint32_t)(a >> (bits - count)) & 1 : 0;   /* past the size: undefined */
    } else {
        const uint64_t wide = (b << bits) | a;               /* b:a, shifted right */
        result = (uint32_t)(wide >> count) & size_mask(bits);
        cf = (uint32_t)(wide >> (count - 1)) & 1;
    }
    const uint32_t of = ((result ^ (uint32_t)a) & sign_bit(bits)) ? PW_IA32_EFLAGS_OF : 0;
    operand_write(c, &d, result);
    flags_set(c, flags_szp(result, bits) | cf | of, PW_IA32_EFLAGS_ARITH);
    return done(c);
}

static int op_shld(Ctx *c) { return double_shift(c, 1); }
static int op_shrd(Ctx *c) { return double_shift(c, 0); }

/* ---------------------------------------------------------------- multiply and divide */

static int op_mul(Ctx *c)
{
    Opnd s;
    GET1(c, 0, 0, &s);
    const unsigned bits = s.bits;
    const uint64_t p = (uint64_t)reg_read(c, PW_IA32_EAX, bits) * operand_read(c, &s);
    if (bits == 8) {
        reg_write(c, PW_IA32_EAX, 16, (uint32_t)p);
    } else {
        reg_write(c, PW_IA32_EAX, bits, (uint32_t)p);
        reg_write(c, PW_IA32_EDX, bits, (uint32_t)(p >> bits));
    }
    const uint32_t over = (p >> bits) ? PW_IA32_EFLAGS_CF | PW_IA32_EFLAGS_OF : 0;
    flags_set(c, over, PW_IA32_EFLAGS_CF | PW_IA32_EFLAGS_OF);
    return done(c);
}

/* The overflow of a signed product truncated to `bits`. */
static uint32_t imul_overflow(int64_t product, unsigned bits)
{
    return (int64_t)(int32_t)sign_extend((uint32_t)product & size_mask(bits), bits) != product
               ? PW_IA32_EFLAGS_CF | PW_IA32_EFLAGS_OF : 0;
}

static int op_imul(Ctx *c)
{
    const uint8_t *ops = c->inst.form->operands;
    if (ops[1] == PW_IA32_OP_NONE) {                  /* the one-operand form */
        Opnd s;
        GET1(c, 0, 0, &s);
        const unsigned bits = s.bits;
        const int64_t p = (int64_t)(int32_t)sign_extend(reg_read(c, PW_IA32_EAX, bits), bits) *
                          (int32_t)sign_extend(operand_read(c, &s), bits);
        if (bits == 8) {
            reg_write(c, PW_IA32_EAX, 16, (uint32_t)p);
        } else {
            reg_write(c, PW_IA32_EAX, bits, (uint32_t)p);
            reg_write(c, PW_IA32_EDX, bits, (uint32_t)((uint64_t)p >> bits));
        }
        flags_set(c, imul_overflow(p, bits), PW_IA32_EFLAGS_CF | PW_IA32_EFLAGS_OF);
        return done(c);
    }
    Opnd d, a, b;
    int r = operand_get(c, 0, 0, &d);
    if (!r) r = operand_get(c, 1, 0, &a);
    if (!r) r = ops[2] != PW_IA32_OP_NONE ? operand_get(c, 2, 0, &b) : (b = d, 0);
    if (r) return r;
    const unsigned bits = d.bits;
    const int64_t p = (int64_t)(int32_t)sign_extend(operand_read(c, &a), bits) *
                      (int32_t)sign_extend(operand_read(c, &b) & size_mask(bits), bits);
    operand_write(c, &d, (uint32_t)p);
    flags_set(c, imul_overflow(p, bits), PW_IA32_EFLAGS_CF | PW_IA32_EFLAGS_OF);
    return done(c);
}

static int divide(Ctx *c, int is_signed)
{
    Opnd s;
    GET1(c, 0, 0, &s);
    const unsigned bits = s.bits;
    const uint32_t divisor = operand_read(c, &s);
    if (!divisor) return raise_fault(c, PW_IA32_EXC_DE, 0);
    const uint64_t dividend = bits == 8 ? reg_read(c, PW_IA32_EAX, 16)
                                        : ((uint64_t)reg_read(c, PW_IA32_EDX, bits) << bits) | reg_read(c, PW_IA32_EAX, bits);
    uint32_t quotient, remainder;
    if (is_signed) {
        const int64_t n = bits == 32 ? (int64_t)dividend
                                     : (int64_t)(dividend << (64 - 2 * bits)) >> (64 - 2 * bits);
        const int64_t dv = (int32_t)sign_extend(divisor, bits);
        if (n == INT64_MIN && dv == -1) return raise_fault(c, PW_IA32_EXC_DE, 0);
        const int64_t q = n / dv;
        if (q > (int64_t)(size_mask(bits) >> 1) || q < -(int64_t)sign_bit(bits)) return raise_fault(c, PW_IA32_EXC_DE, 0);
        quotient = (uint32_t)q;
        remainder = (uint32_t)(n % dv);
    } else {
        const uint64_t q = dividend / divisor;
        if (q > size_mask(bits)) return raise_fault(c, PW_IA32_EXC_DE, 0);
        quotient = (uint32_t)q;
        remainder = (uint32_t)(dividend % divisor);
    }
    if (bits == 8) {
        reg_write(c, PW_IA32_EAX, 16, (quotient & 0xff) | ((remainder & 0xff) << 8));
    } else {
        reg_write(c, PW_IA32_EAX, bits, quotient);
        reg_write(c, PW_IA32_EDX, bits, remainder);
    }
    return done(c);
}

static int op_div(Ctx *c) { return divide(c, 0); }
static int op_idiv(Ctx *c) { return divide(c, 1); }

/* ---------------------------------------------------------------- bits */

enum { BT_TEST, BT_SET, BT_RESET, BT_COMPLEMENT };

static int bit_test(Ctx *c, unsigned op)
{
    const PwIa32Inst *in = &c->inst;
    const unsigned bits = operand_size(c);
    Opnd s;
    GET1(c, 1, 0, &s);
    int32_t offset = (int32_t)operand_read(c, &s);
    uint32_t displacement = 0;
    if (s.type == OPND_IMM) offset &= (int32_t)bits - 1;
    else if (in->mod != 3) {                      /* a register offset reaches beyond the operand */
        offset = (int32_t)sign_extend((uint32_t)offset, bits);
        displacement = (uint32_t)((offset >> (bits == 32 ? 5 : 4)) * (int32_t)(bits / 8));
        offset &= (int32_t)bits - 1;
    } else {
        offset &= (int32_t)bits - 1;
    }
    Opnd d;
    if (in->mod == 3) {
        d = (Opnd){ OPND_REG, bits, in->rm, NULL, 0 };
    } else {
        uint8_t *p = memory_at(c, data_segment(c), effective_offset(c) + displacement, bits / 8, op != BT_TEST);
        if (!p) return RAISED;
        d = (Opnd){ OPND_MEM, bits, 0, p, 0 };
    }
    const uint32_t v = operand_read(c, &d), mask = 1u << offset;
    if (op == BT_SET) operand_write(c, &d, v | mask);
    else if (op == BT_RESET) operand_write(c, &d, v & ~mask);
    else if (op == BT_COMPLEMENT) operand_write(c, &d, v ^ mask);
    flags_set(c, (v & mask) ? PW_IA32_EFLAGS_CF : 0, PW_IA32_EFLAGS_CF);
    return done(c);
}

static int op_bt(Ctx *c) { return bit_test(c, BT_TEST); }
static int op_bts(Ctx *c) { return bit_test(c, BT_SET); }
static int op_btr(Ctx *c) { return bit_test(c, BT_RESET); }
static int op_btc(Ctx *c) { return bit_test(c, BT_COMPLEMENT); }

static int bit_scan(Ctx *c, int reverse)
{
    Opnd d, s;
    GET2(c, 0, &d, &s);
    const uint32_t v = operand_read(c, &s);
    if (!v) {                                   /* the destination keeps its value */
        flags_set(c, PW_IA32_EFLAGS_ZF, PW_IA32_EFLAGS_ZF);
        return done(c);
    }
    operand_write(c, &d, reverse ? 31u - (uint32_t)__builtin_clz(v) : (uint32_t)__builtin_ctz(v));
    flags_set(c, 0, PW_IA32_EFLAGS_ZF);
    return done(c);
}

static int op_bsf(Ctx *c) { return bit_scan(c, 0); }
static int op_bsr(Ctx *c) { return bit_scan(c, 1); }

static int bit_count(Ctx *c, unsigned which)   /* 0 tzcnt, 1 lzcnt, 2 popcnt */
{
    Opnd d, s;
    GET2(c, 0, &d, &s);
    const unsigned bits = d.bits;
    const uint32_t v = operand_read(c, &s) & size_mask(bits);
    uint32_t r;
    if (which == 2) r = (uint32_t)__builtin_popcount(v);
    else if (!v) r = bits;
    else r = which == 0 ? (uint32_t)__builtin_ctz(v) : (uint32_t)__builtin_clz(v) - (32 - bits);
    operand_write(c, &d, r);
    if (which == 2) flags_set(c, v ? 0 : PW_IA32_EFLAGS_ZF, PW_IA32_EFLAGS_ARITH);
    else flags_set(c, (v ? 0 : PW_IA32_EFLAGS_CF) | (r ? 0 : PW_IA32_EFLAGS_ZF), PW_IA32_EFLAGS_CF | PW_IA32_EFLAGS_ZF);
    return done(c);
}

static int op_tzcnt(Ctx *c) { return bit_count(c, 0); }
static int op_lzcnt(Ctx *c) { return bit_count(c, 1); }
static int op_popcnt(Ctx *c) { return bit_count(c, 2); }

static int op_bswap(Ctx *c)
{
    if (operand_size(c) != 32) return PW_ERR_UNSUPPORTED;     /* BSWAP r16 is undefined */
    const unsigned reg = c->inst.opcode & 7;
    c->s->gpr[reg] = __builtin_bswap32(c->s->gpr[reg]);
    return done(c);
}

/* ---------------------------------------------------------------- exchange-and-operate */

static int op_xadd(Ctx *c)
{
    Opnd d, s;
    GET2(c, 1, &d, &s);
    uint32_t r;
    const uint32_t a = operand_read(c, &d), b = operand_read(c, &s);
    const uint32_t f = flags_add(a, b, 0, d.bits, &r);
    operand_write(c, &s, a);
    operand_write(c, &d, r);
    flags_set(c, f, PW_IA32_EFLAGS_ARITH);
    return done(c);
}

static int op_cmpxchg(Ctx *c)
{
    Opnd d, s;
    GET2(c, 1, &d, &s);
    uint32_t r;
    const unsigned bits = d.bits;
    const uint32_t accumulator = reg_read(c, PW_IA32_EAX, bits), current = operand_read(c, &d);
    const uint32_t f = flags_sub(accumulator, current, 0, bits, &r);
    if (accumulator == current) operand_write(c, &d, operand_read(c, &s));
    else reg_write(c, PW_IA32_EAX, bits, current);
    flags_set(c, f, PW_IA32_EFLAGS_ARITH);
    return done(c);
}

static int op_cmpxchg8b(Ctx *c)
{
    uint8_t *p = memory_at(c, data_segment(c), effective_offset(c), 8, 1);
    if (!p) return RAISED;
    const uint32_t lo = load(p, 32), hi = load(p + 4, 32);
    if (lo == c->s->gpr[PW_IA32_EAX] && hi == c->s->gpr[PW_IA32_EDX]) {
        store(p, 32, c->s->gpr[PW_IA32_EBX]);
        store(p + 4, 32, c->s->gpr[PW_IA32_ECX]);
        flags_set(c, PW_IA32_EFLAGS_ZF, PW_IA32_EFLAGS_ZF);
    } else {
        c->s->gpr[PW_IA32_EAX] = lo;
        c->s->gpr[PW_IA32_EDX] = hi;
        flags_set(c, 0, PW_IA32_EFLAGS_ZF);
    }
    return done(c);
}

/* ---------------------------------------------------------------- conditions */

static int op_setcc(Ctx *c)
{
    Opnd d;
    GET1(c, 0, 1, &d);
    operand_write(c, &d, (uint32_t)condition(c, c->inst.opcode & 0xf));
    return done(c);
}

static int op_cmovcc(Ctx *c)
{
    Opnd d, s;
    GET2(c, 0, &d, &s);                          /* the source is read (and may fault) either way */
    if (condition(c, c->inst.opcode & 0xf)) operand_write(c, &d, operand_read(c, &s));
    return done(c);
}

/* ---------------------------------------------------------------- the stack */

static int op_push(Ctx *c)
{
    const unsigned kind = c->inst.form->operands[0];
    if (kind >= PW_IA32_OP_ES && kind <= PW_IA32_OP_GS) return PW_ERR_UNSUPPORTED;
    Opnd s;
    GET1(c, 0, 0, &s);
    const unsigned bits = operand_size(c);
    const uint32_t v = s.type == OPND_IMM ? sign_extend(s.imm, s.bits) : operand_read(c, &s);
    if (push(c, bits, v)) return RAISED;
    return done(c);
}

static int op_pop(Ctx *c)
{
    const unsigned kind = c->inst.form->operands[0];
    if (kind >= PW_IA32_OP_ES && kind <= PW_IA32_OP_GS) return PW_ERR_UNSUPPORTED;
    const unsigned bits = operand_size(c);
    uint32_t v;
    if (peek(c, bits, 0, &v)) return RAISED;
    if (kind == PW_IA32_OP_Zv || c->inst.mod == 3) {
        c->s->gpr[PW_IA32_ESP] += bits / 8;
        reg_write(c, kind == PW_IA32_OP_Zv ? (c->inst.opcode & 7u) : c->inst.rm, bits, v);
        return done(c);
    }
    /* POP to memory: the address is computed with ESP already incremented. */
    const uint32_t esp = c->s->gpr[PW_IA32_ESP];
    c->s->gpr[PW_IA32_ESP] = esp + bits / 8;
    uint8_t *p = memory_at(c, data_segment(c), effective_offset(c), bits / 8, 1);
    if (!p) {
        c->s->gpr[PW_IA32_ESP] = esp;
        return RAISED;
    }
    store(p, bits, v);
    return done(c);
}

static int op_pusha(Ctx *c)
{
    const unsigned bits = operand_size(c), n = bits / 8;
    const uint32_t esp = c->s->gpr[PW_IA32_ESP];
    uint8_t *p = memory_at(c, PW_IA32_SEG_SS, esp - 8 * n, 8 * n, 1);
    if (!p) return RAISED;
    for (unsigned i = 0; i < 8; i++) store(p + (7 - i) * n, bits, i == PW_IA32_ESP ? esp : c->s->gpr[i]);
    c->s->gpr[PW_IA32_ESP] = esp - 8 * n;
    return done(c);
}

static int op_popa(Ctx *c)
{
    const unsigned bits = operand_size(c), n = bits / 8;
    const uint32_t esp = c->s->gpr[PW_IA32_ESP];
    const uint8_t *p = memory_at(c, PW_IA32_SEG_SS, esp, 8 * n, 0);
    if (!p) return RAISED;
    for (unsigned i = 0; i < 8; i++)
        if (i != PW_IA32_ESP) reg_write(c, i, bits, load(p + (7 - i) * n, bits));
    c->s->gpr[PW_IA32_ESP] = esp + 8 * n;
    return done(c);
}

enum { EFLAGS_USER_WRITABLE = 0x244dd5u };  /* CF PF AF ZF SF TF DF OF NT AC ID: what POPF changes at CPL 3 */

static int op_pushf(Ctx *c)
{
    if (push(c, operand_size(c), c->s->eflags & 0x00fcffffu)) return RAISED;   /* VM and RF read as 0 */
    return done(c);
}

static int op_popf(Ctx *c)
{
    const unsigned bits = operand_size(c);
    uint32_t v;
    if (peek(c, bits, 0, &v)) return RAISED;
    c->s->gpr[PW_IA32_ESP] += bits / 8;
    flags_set(c, v, EFLAGS_USER_WRITABLE & size_mask(bits));
    return done(c);
}

static int op_leave(Ctx *c)
{
    const unsigned bits = operand_size(c);
    const uint32_t ebp = c->s->gpr[PW_IA32_EBP];
    const uint8_t *p = memory_at(c, PW_IA32_SEG_SS, ebp, bits / 8, 0);
    if (!p) return RAISED;
    c->s->gpr[PW_IA32_ESP] = ebp + bits / 8;
    reg_write(c, PW_IA32_EBP, bits, load(p, bits));
    return done(c);
}

static int op_enter(Ctx *c)
{
    if (c->inst.imm2 & 0x1f) return PW_ERR_UNSUPPORTED;      /* nested frames: not in this slice */
    const unsigned bits = operand_size(c);
    const uint32_t frame = c->s->gpr[PW_IA32_ESP] - bits / 8;
    const uint32_t esp = frame - (c->inst.imm & 0xffff);
    uint8_t *p = memory_at(c, PW_IA32_SEG_SS, frame, bits / 8, 1);
    if (!p) return RAISED;
    /* As the CPU: EBP is pushed, then the new top is checked; a fault there leaves the push. */
    store(p, bits, c->s->gpr[PW_IA32_EBP]);
    if (!memory_at(c, PW_IA32_SEG_SS, esp, 1, 1)) return RAISED;
    reg_write(c, PW_IA32_EBP, bits, frame);
    c->s->gpr[PW_IA32_ESP] = esp;
    return done(c);
}

/* ---------------------------------------------------------------- control transfer */

static uint32_t relative_target(const Ctx *c)
{
    const int32_t rel = c->inst.imm_size == 1 ? (int8_t)c->inst.imm
                      : c->inst.imm_size == 2 ? (int16_t)c->inst.imm : (int32_t)c->inst.imm;
    const uint32_t target = c->next_eip + (uint32_t)rel;
    return operand_size(c) == 16 ? target & 0xffff : target;
}

static int jump_to(Ctx *c, uint32_t target)
{
    c->s->eip = target;
    return 0;
}

static int op_jcc(Ctx *c)
{
    return condition(c, c->inst.opcode & 0xf) ? jump_to(c, relative_target(c)) : done(c);
}

static int op_jmp(Ctx *c)
{
    if (c->inst.form->operands[0] == PW_IA32_OP_Jb || c->inst.form->operands[0] == PW_IA32_OP_Jz)
        return jump_to(c, relative_target(c));
    if (c->inst.form->operands[0] != PW_IA32_OP_Ev) return PW_ERR_UNSUPPORTED;   /* far */
    Opnd s;
    GET1(c, 0, 0, &s);
    return jump_to(c, operand_read(c, &s));
}

static int op_call(Ctx *c)
{
    const unsigned bits = operand_size(c);
    uint32_t target;
    if (c->inst.form->operands[0] == PW_IA32_OP_Jz) {
        target = relative_target(c);
    } else if (c->inst.form->operands[0] == PW_IA32_OP_Ev) {
        Opnd s;
        GET1(c, 0, 0, &s);
        target = operand_read(c, &s);
    } else {
        return PW_ERR_UNSUPPORTED;                                                  /* far */
    }
    if (push(c, bits, c->next_eip)) return RAISED;
    return jump_to(c, target);
}

static int op_ret(Ctx *c)
{
    const unsigned bits = operand_size(c);
    uint32_t target;
    if (peek(c, bits, 0, &target)) return RAISED;
    const uint32_t release = c->inst.form->operands[0] == PW_IA32_OP_Iw ? (c->inst.imm & 0xffff) : 0;
    c->s->gpr[PW_IA32_ESP] += bits / 8 + release;
    return jump_to(c, target);
}

static int loop_branch(Ctx *c, int zf_needed, int zf_value)
{
    const unsigned abits = c->inst.address_size;
    const uint32_t count = (reg_read(c, PW_IA32_ECX, abits) - 1) & size_mask(abits);
    reg_write(c, PW_IA32_ECX, abits, count);
    const int taken = count != 0 && (!zf_needed || flag(c, PW_IA32_EFLAGS_ZF) == zf_value);
    return taken ? jump_to(c, relative_target(c)) : done(c);
}

static int op_loop(Ctx *c) { return loop_branch(c, 0, 0); }
static int op_loope(Ctx *c) { return loop_branch(c, 1, 1); }
static int op_loopne(Ctx *c) { return loop_branch(c, 1, 0); }

static int op_jecxz(Ctx *c)
{
    return reg_read(c, PW_IA32_ECX, c->inst.address_size) == 0 ? jump_to(c, relative_target(c)) : done(c);
}

/* ---------------------------------------------------------------- interrupts and faults */

static int op_int3(Ctx *c) { return raise_trap(c, PW_IA32_EXC_BP, 0); }
static int op_int1(Ctx *c) { return raise_trap(c, PW_IA32_EXC_DB, 0); }
static int op_int(Ctx *c) { return raise_fault(c, PW_IA32_EXC_INT, c->inst.imm & 0xff); }

static int op_into(Ctx *c)
{
    return flag(c, PW_IA32_EFLAGS_OF) ? raise_trap(c, PW_IA32_EXC_OF, 0) : done(c);
}

static int op_ud(Ctx *c) { return raise_fault(c, PW_IA32_EXC_UD, 0); }
static int op_privileged(Ctx *c) { return raise_fault(c, PW_IA32_EXC_GP, 0); }

static int op_clflush(Ctx *c)
{
    if (!memory_at(c, data_segment(c), effective_offset(c), 1, 0)) return RAISED;
    return done(c);
}

/* ---------------------------------------------------------------- strings */

enum { STR_MOVS, STR_STOS, STR_LODS, STR_CMPS, STR_SCAS };

/* One element of a string instruction; 0, or RAISED (the state shows the elements done). */
static int string_element(Ctx *c, unsigned op, unsigned bits, uint32_t *flags_out)
{
    const unsigned abits = c->inst.address_size, n = bits / 8;
    const uint32_t amask = size_mask(abits);
    const uint32_t esi = c->s->gpr[PW_IA32_ESI] & amask, edi = c->s->gpr[PW_IA32_EDI] & amask;
    const uint8_t *src = NULL;
    uint8_t *dst = NULL;
    if (op == STR_MOVS || op == STR_LODS || op == STR_CMPS)
        if (!(src = memory_at(c, data_segment(c), esi, n, 0))) return RAISED;
    if (op == STR_MOVS || op == STR_STOS || op == STR_CMPS || op == STR_SCAS)
        if (!(dst = memory_at(c, PW_IA32_SEG_ES, edi, n, op == STR_MOVS || op == STR_STOS))) return RAISED;
    uint32_t r;
    switch (op) {
    case STR_MOVS: store(dst, bits, load(src, bits)); break;
    case STR_STOS: store(dst, bits, reg_read(c, PW_IA32_EAX, bits)); break;
    case STR_LODS: reg_write(c, PW_IA32_EAX, bits, load(src, bits)); break;
    case STR_CMPS: *flags_out = flags_sub(load(src, bits), load(dst, bits), 0, bits, &r); break;
    default: *flags_out = flags_sub(reg_read(c, PW_IA32_EAX, bits), load(dst, bits), 0, bits, &r); break;
    }
    const uint32_t step = flag(c, PW_IA32_EFLAGS_DF) ? (uint32_t)0 - n : n;
    if (src) reg_write(c, PW_IA32_ESI, abits, esi + step);
    if (dst) reg_write(c, PW_IA32_EDI, abits, edi + step);
    return 0;
}

static int string_op(Ctx *c, unsigned op, unsigned bits)
{
    const int compares = op == STR_CMPS || op == STR_SCAS;
    uint32_t f = 0;
    if (!c->inst.rep) {
        if (string_element(c, op, bits, &f)) return RAISED;
        if (compares) flags_set(c, f, PW_IA32_EFLAGS_ARITH);
        return done(c);
    }
    const unsigned abits = c->inst.address_size;
    while (reg_read(c, PW_IA32_ECX, abits)) {
        if (string_element(c, op, bits, &f)) {
            c->partial = 1;
            return RAISED;
        }
        reg_write(c, PW_IA32_ECX, abits, reg_read(c, PW_IA32_ECX, abits) - 1);
        if (compares) {
            flags_set(c, f, PW_IA32_EFLAGS_ARITH);
            const int zf = flag(c, PW_IA32_EFLAGS_ZF);
            if ((c->inst.rep == 0xf3 && !zf) || (c->inst.rep == 0xf2 && zf)) break;
        }
    }
    return done(c);
}

static unsigned string_bits(const Ctx *c)
{
    const unsigned kind = c->inst.form->operands[0] == PW_IA32_OP_AL || c->inst.form->operands[0] == PW_IA32_OP_eAX
                              ? c->inst.form->operands[1] : c->inst.form->operands[0];
    return kind == PW_IA32_OP_Xb || kind == PW_IA32_OP_Yb ? 8 : operand_size(c);
}

static int op_movs(Ctx *c) { return c->inst.map == PW_IA32_MAP_PRI ? string_op(c, STR_MOVS, string_bits(c)) : PW_ERR_UNSUPPORTED; }
static int op_stos(Ctx *c) { return string_op(c, STR_STOS, string_bits(c)); }
static int op_lods(Ctx *c) { return string_op(c, STR_LODS, string_bits(c)); }
static int op_cmps(Ctx *c) { return c->inst.map == PW_IA32_MAP_PRI ? string_op(c, STR_CMPS, string_bits(c)) : PW_ERR_UNSUPPORTED; }
static int op_scas(Ctx *c) { return string_op(c, STR_SCAS, string_bits(c)); }

/* ---------------------------------------------------------------- dispatch */

typedef struct Operation {
    const char *mnemonic;
    Handler run;
} Operation;

/* The mnemonics this slice runs. Everything else in the table returns PW_ERR_UNSUPPORTED. */
static const Operation operations[] = {
    { "add", op_add }, { "or", op_or }, { "adc", op_adc }, { "sbb", op_sbb }, { "and", op_and }, { "sub", op_sub },
    { "xor", op_xor }, { "cmp", op_cmp }, { "test", op_test }, { "inc", op_inc }, { "dec", op_dec },
    { "not", op_not }, { "neg", op_neg }, { "mov", op_mov }, { "movzx", op_movzx }, { "movsx", op_movsx },
    { "lea", op_lea }, { "xchg", op_xchg }, { "nop", op_nop }, { "pause", op_nop }, { "endbr32", op_nop },
    { "prefetchnta", op_nop }, { "prefetcht0", op_nop }, { "prefetcht1", op_nop }, { "prefetcht2", op_nop },
    { "prefetch", op_nop }, { "prefetchw", op_nop }, { "lfence", op_nop }, { "mfence", op_nop }, { "sfence", op_nop },
    { "clflush", op_clflush },
    { "cbw", op_cbw_cwde }, { "cwde", op_cbw_cwde }, { "cwd", op_cwd_cdq }, { "cdq", op_cwd_cdq },
    { "xlatb", op_xlatb }, { "clc", op_clc }, { "stc", op_stc }, { "cmc", op_cmc }, { "cld", op_cld },
    { "std", op_std }, { "lahf", op_lahf }, { "sahf", op_sahf }, { "salc", op_salc },
    { "aam", op_aam }, { "aad", op_aad }, { "daa", op_daa }, { "das", op_das }, { "aaa", op_aaa }, { "aas", op_aas },
    { "rol", op_rol }, { "ror", op_ror }, { "rcl", op_rcl }, { "rcr", op_rcr }, { "shl", op_shl }, { "shr", op_shr },
    { "sal", op_sal }, { "sar", op_sar }, { "shld", op_shld }, { "shrd", op_shrd },
    { "mul", op_mul }, { "imul", op_imul }, { "div", op_div }, { "idiv", op_idiv },
    { "bt", op_bt }, { "bts", op_bts }, { "btr", op_btr }, { "btc", op_btc }, { "bsf", op_bsf }, { "bsr", op_bsr },
    { "tzcnt", op_tzcnt }, { "lzcnt", op_lzcnt }, { "popcnt", op_popcnt }, { "bswap", op_bswap },
    { "xadd", op_xadd }, { "cmpxchg", op_cmpxchg }, { "cmpxchg8b", op_cmpxchg8b },
    { "seto", op_setcc }, { "setno", op_setcc }, { "setb", op_setcc }, { "setae", op_setcc }, { "sete", op_setcc },
    { "setne", op_setcc }, { "setbe", op_setcc }, { "seta", op_setcc }, { "sets", op_setcc }, { "setns", op_setcc },
    { "setp", op_setcc }, { "setnp", op_setcc }, { "setl", op_setcc }, { "setge", op_setcc }, { "setle", op_setcc },
    { "setg", op_setcc },
    { "cmovo", op_cmovcc }, { "cmovno", op_cmovcc }, { "cmovb", op_cmovcc }, { "cmovae", op_cmovcc },
    { "cmove", op_cmovcc }, { "cmovne", op_cmovcc }, { "cmovbe", op_cmovcc }, { "cmova", op_cmovcc },
    { "cmovs", op_cmovcc }, { "cmovns", op_cmovcc }, { "cmovp", op_cmovcc }, { "cmovnp", op_cmovcc },
    { "cmovl", op_cmovcc }, { "cmovge", op_cmovcc }, { "cmovle", op_cmovcc }, { "cmovg", op_cmovcc },
    { "jo", op_jcc }, { "jno", op_jcc }, { "jb", op_jcc }, { "jae", op_jcc }, { "je", op_jcc }, { "jne", op_jcc },
    { "jbe", op_jcc }, { "ja", op_jcc }, { "js", op_jcc }, { "jns", op_jcc }, { "jp", op_jcc }, { "jnp", op_jcc },
    { "jl", op_jcc }, { "jge", op_jcc }, { "jle", op_jcc }, { "jg", op_jcc },
    { "jmp", op_jmp }, { "call", op_call }, { "ret", op_ret }, { "loop", op_loop }, { "loope", op_loope },
    { "loopne", op_loopne }, { "jecxz", op_jecxz }, { "jcxz", op_jecxz },
    { "push", op_push }, { "pop", op_pop }, { "pushal", op_pusha }, { "pushaw", op_pusha }, { "popal", op_popa },
    { "popaw", op_popa }, { "pushfd", op_pushf }, { "pushf", op_pushf }, { "popfd", op_popf }, { "popf", op_popf },
    { "leave", op_leave }, { "enter", op_enter },
    { "movsb", op_movs }, { "movsw", op_movs }, { "movsd", op_movs }, { "stosb", op_stos }, { "stosw", op_stos },
    { "stosd", op_stos }, { "lodsb", op_lods }, { "lodsw", op_lods }, { "lodsd", op_lods }, { "cmpsb", op_cmps },
    { "cmpsw", op_cmps }, { "cmpsd", op_cmps }, { "scasb", op_scas }, { "scasw", op_scas }, { "scasd", op_scas },
    { "int3", op_int3 }, { "int1", op_int1 }, { "int", op_int }, { "into", op_into }, { "ud2", op_ud },
    { "hlt", op_privileged }, { "cli", op_privileged }, { "sti", op_privileged }, { "in", op_privileged },
    { "out", op_privileged }, { "insb", op_privileged }, { "insw", op_privileged }, { "insd", op_privileged },
    { "outsb", op_privileged }, { "outsw", op_privileged }, { "outsd", op_privileged },
};

/* The handler of each form, found once by its mnemonic (0: not yet built, 1: building, 2: ready). */
static Handler form_handlers[2048];
static atomic_int handlers_state;

static void handlers_build(void)
{
    for (size_t i = 0; i < pw_ia32_form_count() && i < sizeof(form_handlers) / sizeof(form_handlers[0]); i++) {
        const PwIa32Form *form = pw_ia32_form_at(i);
        for (size_t k = 0; k < sizeof(operations) / sizeof(operations[0]); k++)
            if (!strcmp(form->mnemonic, operations[k].mnemonic)) form_handlers[i] = operations[k].run;
    }
}

static Handler handler_for(const PwIa32Form *form)
{
    if (atomic_load_explicit(&handlers_state, memory_order_acquire) != 2) {
        int expected = 0;
        if (atomic_compare_exchange_strong(&handlers_state, &expected, 1)) {
            handlers_build();
            atomic_store_explicit(&handlers_state, 2, memory_order_release);
        } else {
            while (atomic_load_explicit(&handlers_state, memory_order_acquire) != 2) {}
        }
    }
    const size_t index = (size_t)(form - pw_ia32_form_at(0));   /* the table is one array */
    return index < sizeof(form_handlers) / sizeof(form_handlers[0]) ? form_handlers[index] : NULL;
}

/* Fetches up to 15 bytes at EIP: as many as are readable, one page boundary at a time. */
static size_t fetch(Ctx *c, uint8_t *bytes)
{
    const uint32_t linear = c->s->base[PW_IA32_SEG_CS] + c->s->eip;
    size_t n = 0;
    while (n < PW_IA32_MAX_LENGTH) {
        const uint8_t *p = c->m->translate(c->m->opaque, linear + (uint32_t)n, 1, 0);
        if (!p) break;
        bytes[n++] = *p;
    }
    return n;
}

int pw_ia32_step(PwIa32State *state, const PwIa32Memory *memory, PwIa32Event *event)
{
    if (!state || !memory || !memory->translate || !event) return PW_ERR_PRECONDITION;
    Ctx c = { state, memory, event, { 0 }, 0, 0 };
    event->vector = PW_IA32_EXC_NONE;
    event->code = event->address = 0;
    uint8_t bytes[PW_IA32_MAX_LENGTH];
    const size_t n = fetch(&c, bytes);
    const int decoded = pw_ia32_decode(bytes, n, &c.inst);
    if (decoded == PW_ERR_TRUNCATED) {
        raise_page_fault(&c, state->base[PW_IA32_SEG_CS] + state->eip + (uint32_t)n);
        return PW_OK;
    }
    if (decoded == PW_ERR_MALFORMED) {
        raise_fault(&c, PW_IA32_EXC_UD, 0);
        return PW_OK;
    }
    if (decoded != PW_OK) return PW_ERR_UNSUPPORTED;
    const PwIa32Form *form = c.inst.form;
    if (c.inst.lock && !((form->attributes & PW_IA32_ATTR_LOCK) && c.inst.has_modrm && c.inst.mod != 3)) {
        raise_fault(&c, PW_IA32_EXC_UD, 0);
        return PW_OK;
    }
    const Handler run = handler_for(form);
    if (!run) return PW_ERR_UNSUPPORTED;
    c.next_eip = state->eip + c.inst.length;   /* only a 16-bit control transfer truncates EIP */
    PwIa32State before = *state;
    const int result = run(&c);
    if (result == RAISED) {
        /* A fault leaves the state as it was, except a REP string's completed iterations. */
        if (!c.partial && event->vector != PW_IA32_EXC_BP && event->vector != PW_IA32_EXC_DB &&
            event->vector != PW_IA32_EXC_OF) {
            const PwIa32Event raised = *event;
            *state = before;
            *event = raised;
        }
        return PW_OK;
    }
    if (result != 0) {
        *state = before;
        event->vector = PW_IA32_EXC_NONE;
        return PW_ERR_UNSUPPORTED;
    }
    return PW_OK;
}
