/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pw_ia32_decode.h"
#include <string.h>

/* Bytes are read only through a Cursor, which refuses the 16th byte (MALFORMED) and any byte past
 * `available` (TRUNCATED). */
typedef struct Cursor {
    const uint8_t *bytes;
    size_t available;
    size_t at;
} Cursor;

static int cursor_peek(const Cursor *c, uint8_t *out)
{
    if (c->at >= PW_IA32_MAX_LENGTH) return PW_ERR_MALFORMED;
    if (c->at >= c->available) return PW_ERR_TRUNCATED;
    *out = c->bytes[c->at];
    return PW_OK;
}

static int cursor_take(Cursor *c, uint8_t *out)
{
    int result = cursor_peek(c, out);
    if (result == PW_OK) c->at++;
    return result;
}

/* size bytes, little-endian, zero-extended. */
static int cursor_take_le(Cursor *c, unsigned size, uint64_t *out)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < size; i++) {
        uint8_t b;
        int result = cursor_take(c, &b);
        if (result != PW_OK) return result;
        value |= (uint64_t)b << (8 * i);
    }
    *out = value;
    return PW_OK;
}

static int32_t sign_extend(uint64_t value, unsigned size)
{
    if (size == 1) return (int8_t)value;
    if (size == 2) return (int16_t)value;
    return (int32_t)value;
}

static void prefix_record(PwIa32Inst *inst, uint8_t b)
{
    switch (b) {
    case 0xf0: inst->lock = 1; break;
    case 0xf2: case 0xf3: inst->rep = b; break;
    case 0x26: case 0x2e: case 0x36: case 0x3e:
        /* Long mode: null prefixes (AMD APM vol. 3, 1.2.4), so an FS/GS before them stays. */
        if (inst->mode != PW_IA32_MODE_64 || (inst->segment != PW_IA32_SEG_FS && inst->segment != PW_IA32_SEG_GS))
            inst->segment = b == 0x26 ? PW_IA32_SEG_ES : b == 0x2e ? PW_IA32_SEG_CS :
                            b == 0x36 ? PW_IA32_SEG_SS : PW_IA32_SEG_DS;
        break;
    case 0x64: inst->segment = PW_IA32_SEG_FS; break;
    case 0x65: inst->segment = PW_IA32_SEG_GS; break;
    case 0x66: inst->has_66 = 1; break;
    case 0x67: inst->has_67 = 1; break;
    default: break;
    }
}

static int decode_modrm(Cursor *c, PwIa32Inst *inst, unsigned rex);

/* Legacy prefixes, and in long mode REX: only a REX right before the opcode takes effect. */
static int decode_prefixes(Cursor *c, PwIa32Inst *inst)
{
    for (;;) {
        uint8_t b;
        int result = cursor_peek(c, &b);
        if (result != PW_OK) return result;
        if (inst->mode == PW_IA32_MODE_64 && (b & 0xf0) == 0x40) {
            inst->rex = b;
        } else if (pw_ia32_shape(PW_IA32_MAP_PRI, b) == 'P') {
            prefix_record(inst, b);
            inst->rex = 0;
            inst->rep_last = b == 0xf2 || b == 0xf3;
        } else {
            return PW_OK;
        }
        inst->prefixes++;
        c->at++;
    }
}

/* The opcode, through the 0F, 0F 38 and 0F 3A escapes. */
static int decode_opcode(Cursor *c, PwIa32Inst *inst)
{
    uint8_t b;
    int result = cursor_take(c, &b);
    if (result != PW_OK) return result;
    inst->map = PW_IA32_MAP_PRI;
    inst->opcode = b;
    inst->opcode_at = (uint8_t)(c->at - 1);
    inst->shape = pw_ia32_shape(PW_IA32_MAP_PRI, b);
    if (inst->shape != '0') return PW_OK;
    if ((result = cursor_take(c, &b)) != PW_OK) return result;
    inst->map = PW_IA32_MAP_0F;
    inst->opcode = b;
    inst->opcode_at = (uint8_t)(c->at - 1);
    inst->shape = pw_ia32_shape(PW_IA32_MAP_0F, b);
    if (inst->shape != '8' && inst->shape != 'A') return PW_OK;
    inst->map = inst->shape == '8' ? PW_IA32_MAP_0F38 : PW_IA32_MAP_0F3A;
    if ((result = cursor_take(c, &b)) != PW_OK) return result;
    inst->opcode = b;
    inst->opcode_at = (uint8_t)(c->at - 1);
    inst->shape = pw_ia32_shape(inst->map, b);
    return PW_OK;
}

/* VEX (C4/C5), EVEX (62) and XOP (8F): the payload names a map, and the map says whether an imm8
 * (or XOP's imm32) follows. In 32-bit mode only the length is decoded, and the payload's first two
 * bits are 1, which is what told them from LES/LDS/BOUND/POP; in long mode VEX finds the table's
 * VEX rows (form_find), and EVEX and XOP still have none. */
static int vex_map_shape(unsigned encoding, unsigned map, unsigned opcode, char *shape, uint8_t *map_out)
{
    if (encoding == PW_IA32_ENC_XOP) {
        if (map < 8 || map > 10) return PW_ERR_MALFORMED;
        *shape = map == 8 ? 'B' : map == 9 ? 'm' : 'Z';   /* map A takes an imm32 */
        *map_out = (uint8_t)map;
        return PW_OK;
    }
    if (map < 1 || map > 3) return PW_ERR_MALFORMED;
    *map_out = (uint8_t)map;
    if (map == PW_IA32_MAP_0F38) *shape = 'm';
    else if (map == PW_IA32_MAP_0F3A) *shape = 'B';
    else if (opcode == 0x77) *shape = '.';                  /* VZEROUPPER/VZEROALL */
    else *shape = pw_ia32_shape(PW_IA32_MAP_0F, opcode) == 'B' ? 'B' : 'm';
    return PW_OK;
}

static const PwIa32Form *form_find(const PwIa32Inst *inst, int *excluded);
static uint8_t default_segment(const PwIa32Inst *inst);

static int decode_vex(Cursor *c, PwIa32Inst *inst)
{
    const unsigned payload = inst->encoding == PW_IA32_ENC_EVEX ? 3 : inst->opcode == 0xc5 ? 1 : 2;
    uint8_t p[3] = { 0, 0, 0 };
    for (unsigned i = 0; i < payload; i++) {
        int result = cursor_take(c, &p[i]);
        if (result != PW_OK) return result;
    }
    unsigned map = inst->opcode == 0xc5 ? PW_IA32_MAP_0F : p[0] & (inst->encoding == PW_IA32_ENC_EVEX ? 0x07 : 0x1f);
    uint8_t opcode;
    int result = cursor_take(c, &opcode);
    if (result != PW_OK) return result;
    if (inst->has_66 || inst->rep || inst->lock || inst->rex) return PW_ERR_MALFORMED;   /* #UD before VEX/EVEX/XOP */
    /* R X B W vvvv L pp: C5 has R vvvv L pp; C4/8F have R X B map, then W vvvv L pp; 62 has R X B R' map,
     * then W vvvv 1 pp, then z L'L b V' aaa. R X B and vvvv are inverted. */
    const int two = inst->opcode == 0xc5;
    const unsigned first = (uint8_t)~p[0], last = two ? p[0] : p[1];
    unsigned rex = (first >> 5) & 4;                                               /* R */
    if (!two) rex |= ((first >> 5) & 3) | ((p[1] >> 4) & 8u);                      /* X B, W */
    inst->opcode = opcode;
    inst->opcode_at = (uint8_t)(c->at - 1);
    inst->vex_w = (rex >> 3) & 1;
    inst->vex_vvvv = (uint8_t)~last >> 3 & 15;
    inst->vex_l = inst->encoding == PW_IA32_ENC_EVEX ? (p[2] >> 5) & 1 : (last >> 2) & 1;
    inst->vex_pp = last & 3;
    if (inst->mode != PW_IA32_MODE_64) {
        rex = 0;   /* R X B W and vvvv's top bit say nothing in 32-bit mode */
        inst->vex_w = 0;
        inst->vex_vvvv &= 7;
    }
    if ((result = vex_map_shape(inst->encoding, map, opcode, &inst->shape, &inst->map)) != PW_OK) return result;
    if (inst->shape != '.' && (result = decode_modrm(c, inst, rex)) != PW_OK) return result;
    inst->imm_size = inst->shape == 'B' ? 1 : inst->shape == 'Z' ? 4 : 0;
    if ((result = cursor_take_le(c, inst->imm_size, &inst->imm)) != PW_OK) return result;
    if (inst->mode != PW_IA32_MODE_64 || inst->encoding != PW_IA32_ENC_VEX) return PW_ERR_UNSUPPORTED;
    inst->default_segment = default_segment(inst);
    int excluded = 0;
    inst->form = form_find(inst, &excluded);
    if (!inst->form) return PW_ERR_UNSUPPORTED;
    inst->operand_size = inst->vex_w ? 64 : 32;
    return PW_OK;
}

/* C4/C5, 62 and 8F begin VEX, EVEX and XOP when the next byte could not be their legacy ModRM; in
 * long mode LES, LDS and BOUND do not exist, so C4/C5 and 62 always do. */
static int decode_escape_shapes(Cursor *c, PwIa32Inst *inst)
{
    if (inst->shape != 'V' && inst->shape != 'v' && inst->shape != 'x') return PW_OK;
    uint8_t next;
    int result = cursor_peek(c, &next);
    if (result != PW_OK) return result;
    const int any = inst->mode == PW_IA32_MODE_64;
    if (inst->shape == 'V' && (any || (next & 0xc0) == 0xc0)) inst->encoding = PW_IA32_ENC_VEX;
    else if (inst->shape == 'v' && (any || (next & 0xc0) == 0xc0)) inst->encoding = PW_IA32_ENC_EVEX;
    else if (inst->shape == 'x' && (next & 0x1f) >= 8) inst->encoding = PW_IA32_ENC_XOP;
    if (inst->encoding != PW_IA32_ENC_LEGACY) return decode_vex(c, inst);
    inst->shape = 'm';
    return PW_OK;
}

static int shape_has_modrm(char shape)
{
    return shape == 'm' || shape == 'B' || shape == 'Z' || shape == 'f' || shape == 'F' || shape == '3' ||
           shape == 'Q';
}

/* 16-bit addressing: rm names a base and an index, and mod 0 rm 6 is a bare disp16. */
static void address16(PwIa32Inst *inst)
{
    static const uint8_t bases[8] = { PW_IA32_EBX, PW_IA32_EBX, PW_IA32_EBP, PW_IA32_EBP,
                                      PW_IA32_ESI, PW_IA32_EDI, PW_IA32_EBP, PW_IA32_EBX };
    static const uint8_t indexes[8] = { PW_IA32_ESI, PW_IA32_EDI, PW_IA32_ESI, PW_IA32_EDI,
                                        PW_IA32_NO_REG, PW_IA32_NO_REG, PW_IA32_NO_REG, PW_IA32_NO_REG };
    inst->base = bases[inst->rm];
    inst->index = indexes[inst->rm];
    inst->disp_size = inst->mod == 1 ? 1 : inst->mod == 2 ? 2 : 0;
    if (inst->mod == 0 && inst->rm == 6) {
        inst->base = PW_IA32_NO_REG;
        inst->disp_size = 2;
    }
}

/* 32- and 64-bit addressing: rm 4 is a SIB byte, mod 0 rm 5 a bare disp32 (RIP-relative in long
 * mode), SIB base 5 with mod 0 a disp32 without base. The low three bits decide, whatever REX.B
 * adds (r12 needs a SIB, r13 a displacement); SIB index 4 is none, unless REX.X makes it r12. */
static int address32(Cursor *c, PwIa32Inst *inst, unsigned rex)
{
    inst->disp_size = inst->mod == 1 ? 1 : inst->mod == 2 ? 4 : 0;
    if ((inst->rm & 7) != 4) {
        inst->base = inst->rm;
        if (inst->mod == 0 && (inst->rm & 7) == 5) {
            inst->base = PW_IA32_NO_REG;
            inst->disp_size = 4;
            inst->rip_relative = inst->mode == PW_IA32_MODE_64;
        }
        return PW_OK;
    }
    int result = cursor_take(c, &inst->sib);
    if (result != PW_OK) return result;
    inst->has_sib = 1;
    inst->scale = (uint8_t)(1u << (inst->sib >> 6));
    inst->index = (uint8_t)(((inst->sib >> 3) & 7) | ((rex & 2) << 2));
    if (inst->index == PW_IA32_ESP) inst->index = PW_IA32_NO_REG;
    inst->base = (uint8_t)((inst->sib & 7) | ((rex & 1) << 3));
    if (inst->mod == 0 && (inst->sib & 7) == PW_IA32_EBP) {
        inst->base = PW_IA32_NO_REG;
        inst->disp_size = 4;
    }
    return PW_OK;
}

/* rex: the REX bits (W R X B) that extend reg, rm, index and base: REX's own, or VEX's. */
static int decode_modrm(Cursor *c, PwIa32Inst *inst, unsigned rex)
{
    uint8_t modrm;
    inst->modrm_at = (uint8_t)c->at;
    int result = cursor_take(c, &modrm);
    if (result != PW_OK) return result;
    inst->has_modrm = 1;
    inst->mod = modrm >> 6;
    inst->reg = (uint8_t)(((modrm >> 3) & 7) | ((rex & 4) << 1));
    inst->rm = (uint8_t)((modrm & 7) | ((rex & 1) << 3));
    if (inst->mod == 3) return PW_OK;
    if (inst->address_size == 16) address16(inst);
    else if ((result = address32(c, inst, rex)) != PW_OK) return result;
    uint64_t disp = 0;
    if ((result = cursor_take_le(c, inst->disp_size, &disp)) != PW_OK) return result;
    inst->disp = sign_extend(disp, inst->disp_size);
    return PW_OK;
}

/* The mandatory prefix an SSE-style form would see: the last F2/F3, else 66, else none; VEX.pp. */
static unsigned mandatory_prefix(const PwIa32Inst *inst)
{
    if (inst->encoding != PW_IA32_ENC_LEGACY) {
        static const uint8_t pp[4] = { PW_IA32_PFX_NP, PW_IA32_PFX_P66, PW_IA32_PFX_PF3, PW_IA32_PFX_PF2 };
        return pp[inst->vex_pp & 3];
    }
    if (inst->rep == 0xf3) return PW_IA32_PFX_PF3;
    if (inst->rep == 0xf2) return PW_IA32_PFX_PF2;
    if (inst->has_66) return PW_IA32_PFX_P66;
    return PW_IA32_PFX_NP;
}

static unsigned immediate_size(const PwIa32Inst *inst, char shape)
{
    const unsigned z = inst->has_66 && !(inst->rex & 8) ? 2 : 4;   /* REX.W wins over 66 */
    switch (shape) {
    case 'b': case 'B': case '3': return 1;
    case 'w': return 2;
    case 'z': case 'Z':   /* MOV r64, imm64 (B8-BF with REX.W) is the one 8-byte immediate */
        return (inst->rex & 8) && inst->map == PW_IA32_MAP_PRI && (inst->opcode & 0xf8) == 0xb8 ? 8 : z;
    case 'o': return inst->address_size / 8;
    case 'p': return z;          /* the offset; the selector follows as imm2 */
    case 'e': return 2;          /* ENTER: imm16, then imm8 as imm2 */
    case 'f': return (inst->reg & 7) < 2 ? 1 : 0;   /* TEST's /0 and /1, whatever REX.R adds */
    case 'F': return (inst->reg & 7) < 2 ? z : 0;
    case 'Q': return mandatory_prefix(inst) == PW_IA32_PFX_P66 || mandatory_prefix(inst) == PW_IA32_PFX_PF2 ? 1 : 0;
    default: return 0;
    }
}

static unsigned second_immediate_size(const PwIa32Inst *inst, char shape)
{
    if (shape == 'p') return 2;
    if (shape == 'e') return 1;
    if (shape == 'Q') return immediate_size(inst, shape);
    return 0;
}

static int decode_immediates(Cursor *c, PwIa32Inst *inst)
{
    inst->imm_size = (uint8_t)immediate_size(inst, inst->shape);
    inst->imm2_size = (uint8_t)second_immediate_size(inst, inst->shape);
    if (inst->shape == 'o') {       /* moffs is an address: a displacement with no base */
        uint64_t moffs = 0;
        int result = cursor_take_le(c, inst->imm_size, &moffs);
        if (result != PW_OK) return result;
        inst->disp_size = inst->imm_size;
        inst->disp = (int32_t)moffs;
        inst->imm = moffs;
        return PW_OK;
    }
    int result = cursor_take_le(c, inst->imm_size, &inst->imm);
    if (result != PW_OK) return result;
    uint64_t imm2 = 0;
    result = cursor_take_le(c, inst->imm2_size, &imm2);
    inst->imm2 = (uint32_t)imm2;
    return result;
}

/* The operand size a form would have: 66 (unless it is the form's mandatory prefix; NOREP keeps it)
 * makes it 16; in long mode REX.W makes it 64, and so does D64 without a 66. VEX: 64 with W. */
static unsigned form_operand_size(const PwIa32Form *form, const PwIa32Inst *inst)
{
    if (inst->encoding != PW_IA32_ENC_LEGACY) return inst->vex_w ? 64 : 32;
    const int narrow = inst->has_66 && form->prefix != PW_IA32_PFX_P66;
    if (inst->mode != PW_IA32_MODE_64) return narrow ? 16 : 32;
    if (inst->rex & 8) return 64;
    if (narrow) return 16;
    return form->attributes & PW_IA32_ATTR_D64 ? 64 : 32;
}

/* Whether a row may describe the instruction at all, before its fields are compared: its mode
 * (NO64, X64) and encoding (VEX, VEXONLY with VEX.L, VEX.W and NOV), and long mode's two exceptions: 90
 * with REX.B is XCHG r8 (the OPREG row), and a near branch with 66 has no form (AMD and Intel
 * disagree on it). */
static int form_allowed(const PwIa32Form *form, const PwIa32Inst *inst)
{
    const uint32_t a = form->attributes;
    if (inst->mode == PW_IA32_MODE_64 ? (a & PW_IA32_ATTR_NO64) : (a & PW_IA32_ATTR_X64)) return 0;
    if (inst->encoding == PW_IA32_ENC_LEGACY) {
        if (a & PW_IA32_ATTR_VEXONLY) return 0;
    } else {
        if (!(a & (PW_IA32_ATTR_VEX | PW_IA32_ATTR_VEXONLY))) return 0;
        if ((a & PW_IA32_ATTR_L0) && inst->vex_l) return 0;
        if ((a & PW_IA32_ATTR_L1) && !inst->vex_l) return 0;
        if ((a & PW_IA32_ATTR_W0) && inst->vex_w) return 0;
        if ((a & PW_IA32_ATTR_W1) && !inst->vex_w) return 0;
        if ((a & PW_IA32_ATTR_NOV) && inst->vex_vvvv) return 0;
    }
    if (inst->mode != PW_IA32_MODE_64) return 1;
    if (inst->map == PW_IA32_MAP_PRI && inst->opcode == 0x90 && (inst->rex & 1) && !(a & PW_IA32_ATTR_OPREG)) return 0;
    if ((a & PW_IA32_ATTR_BRANCH) && (a & PW_IA32_ATTR_D64) && inst->has_66) return 0;
    return 1;
}

static int form_matches(const PwIa32Form *form, const PwIa32Inst *inst)
{
    if (form->reg != PW_IA32_SEL_X && (!inst->has_modrm || (inst->reg & 7) != form->reg)) return 0;
    if (form->mod == PW_IA32_MOD_MEM && (!inst->has_modrm || inst->mod == 3)) return 0;
    if (form->mod == PW_IA32_MOD_REG && (!inst->has_modrm || inst->mod != 3)) return 0;
    if (form->rm != PW_IA32_SEL_X && (!inst->has_modrm || inst->mod != 3 || (inst->rm & 7) != form->rm)) return 0;
    if (form->prefix == PW_IA32_PFX_NOREP) {
        if (inst->rep) return 0;
    } else if (form->prefix != PW_IA32_PFX_ANY && form->prefix != mandatory_prefix(inst)) {
        return 0;
    }
    const unsigned operand_size = form_operand_size(form, inst);
    switch (form->size) {
    case PW_IA32_SIZE_O16: return operand_size == 16;
    case PW_IA32_SIZE_O32: return operand_size == 32;
    case PW_IA32_SIZE_O64: return operand_size == 64;
    case PW_IA32_SIZE_A16: return inst->address_size == 16;
    case PW_IA32_SIZE_A32: return inst->address_size == 32;
    case PW_IA32_SIZE_A64: return inst->address_size == 64;
    default: return 1;
    }
}

/* Operand values no form allows: a segment register number above GS, and CS as a destination
 * (MOV CS is #UD on the CPU). REX.R does not reach a segment register. */
static int operands_valid(const PwIa32Form *form, const PwIa32Inst *inst)
{
    for (unsigned i = 0; i < 3; i++)
        if (form->operands[i] == PW_IA32_OP_Sw && (inst->reg & 7) > PW_IA32_SEG_GS) return 0;
    if (form->operands[0] == PW_IA32_OP_Sw && (inst->reg & 7) == PW_IA32_SEG_CS) return 0;
    return 1;
}

/* The first row that matches; *excluded says a row would have matched but for its mode (NO64 in
 * long mode: the CPU raises #UD). */
static const PwIa32Form *form_find(const PwIa32Inst *inst, int *excluded)
{
    size_t count = 0;
    const uint16_t *rows = pw_ia32_forms_for(inst->map, inst->opcode, &count);
    for (size_t i = 0; i < count; i++) {
        const PwIa32Form *form = pw_ia32_form_at(rows[i]);
        if (!form_matches(form, inst)) continue;
        if (form_allowed(form, inst)) return form;
        if (inst->mode == PW_IA32_MODE_64 && (form->attributes & PW_IA32_ATTR_NO64)) *excluded = 1;
    }
    return NULL;
}

static uint8_t default_segment(const PwIa32Inst *inst)
{
    if (inst->base == PW_IA32_EBP || inst->base == PW_IA32_ESP) return PW_IA32_SEG_SS;
    return PW_IA32_SEG_DS;
}

/* Without a form: REX.W, else 66, picks the operand size. */
static uint8_t plain_operand_size(const PwIa32Inst *inst)
{
    return inst->rex & 8 ? 64 : inst->has_66 ? 16 : 32;
}

static int decode_steps(Cursor *c, PwIa32Inst *inst)
{
    int result = decode_prefixes(c, inst);
    if (result != PW_OK) return result;
    if (inst->mode == PW_IA32_MODE_64) inst->address_size = inst->has_67 ? 32 : 64;
    else inst->address_size = inst->has_67 ? 16 : 32;
    if ((result = decode_opcode(c, inst)) != PW_OK) return result;
    result = decode_escape_shapes(c, inst);
    if (result != PW_OK || inst->encoding != PW_IA32_ENC_LEGACY) return result;
    if (inst->shape == '-') return PW_ERR_MALFORMED;
    if (shape_has_modrm(inst->shape) && (result = decode_modrm(c, inst, inst->rex)) != PW_OK) return result;
    if ((result = decode_immediates(c, inst)) != PW_OK) return result;
    inst->default_segment = default_segment(inst);
    int excluded = 0;
    inst->form = form_find(inst, &excluded);
    if (!inst->form) return excluded ? PW_ERR_MALFORMED : PW_ERR_UNSUPPORTED;
    if (!operands_valid(inst->form, inst)) {
        inst->form = NULL;
        return PW_ERR_MALFORMED;
    }
    inst->operand_size = (uint8_t)form_operand_size(inst->form, inst);
    return PW_OK;
}

int pw_ia32_decode(const uint8_t *bytes, size_t available, PwIa32Inst *inst)
{
    return pw_ia32_decode_mode(bytes, available, PW_IA32_MODE_32, inst);
}

int pw_ia32_decode_mode(const uint8_t *bytes, size_t available, unsigned mode, PwIa32Inst *inst)
{
    if (!bytes || !inst || (mode != PW_IA32_MODE_32 && mode != PW_IA32_MODE_64)) return PW_ERR_PRECONDITION;
    memset(inst, 0, sizeof(*inst));
    inst->mode = (uint8_t)mode;
    inst->opcode_at = inst->modrm_at = PW_IA32_NO_OFFSET;
    inst->segment = PW_IA32_SEG_NONE;
    inst->base = inst->index = PW_IA32_NO_REG;
    inst->scale = 1;
    inst->operand_size = 32;
    inst->address_size = mode == PW_IA32_MODE_64 ? 64 : 32;
    Cursor c = { bytes, available, 0 };
    const int result = decode_steps(&c, inst);
    inst->length = (uint8_t)c.at;
    if (result == PW_ERR_UNSUPPORTED && inst->encoding == PW_IA32_ENC_LEGACY) inst->operand_size = plain_operand_size(inst);
    return result;
}

/* Bytes of memory an operand names, or 0 when it cannot be memory; *sized is 0 for M (no size). */
static unsigned memory_bytes(unsigned operand, const PwIa32Inst *inst, int *sized)
{
    const unsigned vector = inst->encoding != PW_IA32_ENC_LEGACY && inst->vex_l ? 32 : 16;
    *sized = 1;
    switch (operand) {
    case PW_IA32_OP_Eb: case PW_IA32_OP_Mb: case PW_IA32_OP_Ebd: case PW_IA32_OP_Wb: return 1;
    case PW_IA32_OP_Ew: case PW_IA32_OP_Mw: case PW_IA32_OP_Ww: case PW_IA32_OP_Ewd: case PW_IA32_OP_Evw: return 2;
    case PW_IA32_OP_Ed: case PW_IA32_OP_Md: case PW_IA32_OP_Wd: case PW_IA32_OP_Qd: return 4;
    case PW_IA32_OP_Ev: return inst->operand_size / 8u;
    case PW_IA32_OP_Mq: case PW_IA32_OP_Wq: case PW_IA32_OP_Qq: return 8;
    case PW_IA32_OP_Mt: return 10;
    case PW_IA32_OP_Mx: case PW_IA32_OP_Wx: return vector;
    case PW_IA32_OP_Wdq: return 16;
    case PW_IA32_OP_Ma: return inst->operand_size / 4u;        /* BOUND's two bounds */
    case PW_IA32_OP_Mp: return 2 + inst->operand_size / 8u;    /* offset, then selector */
    case PW_IA32_OP_M: *sized = 0; return 1;
    default: return 0;
    }
}

int pw_ia32_access(const PwIa32Inst *inst, PwIa32Access *access)
{
    if (!inst || !access || !inst->form) return PW_ERR_PRECONDITION;
    const PwIa32Form *form = inst->form;
    const uint32_t a = form->attributes;
    access->kind = PW_IA32_ACCESS_NONE;
    access->exact = 0;
    access->bytes = 0;
    if (a & (PW_IA32_ATTR_STACK | PW_IA32_ATTR_STR | PW_IA32_ATTR_IMPL)) return PW_ERR_UNSUPPORTED;
    for (unsigned i = 0; i < 3; i++)
        if (form->operands[i] == PW_IA32_OP_Ob || form->operands[i] == PW_IA32_OP_Ov) return PW_ERR_UNSUPPORTED;
    if (!inst->has_modrm || inst->mod == 3) return PW_OK;
    if (a & PW_IA32_ATTR_ADDR) {
        access->kind = PW_IA32_ACCESS_ADDRESS;
        return PW_OK;
    }
    for (unsigned i = 0; i < 3; i++) {
        int sized = 1;
        unsigned bytes = memory_bytes(form->operands[i], inst, &sized);
        if (!bytes) continue;
        if (!sized) return PW_ERR_UNSUPPORTED;
        if ((a & PW_IA32_ATTR_VLH) && inst->encoding != PW_IA32_ENC_LEGACY && inst->vex_l) bytes *= 2;
        if (i) access->kind = PW_IA32_ACCESS_READ;
        else if (a & PW_IA32_ATTR_MRO) access->kind = PW_IA32_ACCESS_READ;
        else if (a & (PW_IA32_ATTR_LOCK | PW_IA32_ATTR_O1)) access->kind = PW_IA32_ACCESS_RW;
        else access->kind = PW_IA32_ACCESS_WRITE;
        access->exact = !(a & PW_IA32_ATTR_MASK);
        access->bytes = (uint16_t)bytes;
        return PW_OK;
    }
    return PW_ERR_UNSUPPORTED;   /* a ModRM memory operand no operand of the row names: a table error */
}
