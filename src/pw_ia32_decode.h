/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_IA32_DECODE_H
#define PW_IA32_DECODE_H
#include "../include/prospero_win.h"
#include "pw_ia32_isa.h"

/* The x86 decoder: bytes to one PwIa32Inst, in 32-bit protected mode (the default operand and
 * address size are 32 bits) or in 64-bit long mode (docs/MMU4K.md, section 4.1). The only x86 parser
 * in the repository; it knows nothing the table (src/pw_ia32_isa.def) does not say. It never reads
 * past `available`, whatever the bytes.
 *
 * Results:
 *   PW_OK               a form of the table; inst->form is set
 *   PW_ERR_UNSUPPORTED  well formed, length known, but no form: not in the table yet, EVEX/XOP, VEX in
 *                       32-bit mode (their map, opcode, ModRM and immediate are decoded; inst->map is
 *                       8-10 for XOP), or a near branch with 66 in long mode (AMD makes it 16-bit and
 *                       Intel ignores the 66: the length is AMD's, the console's)
 *   PW_ERR_TRUNCATED    the encoding needs more than `available` bytes
 *   PW_ERR_MALFORMED    not an instruction: an undefined opcode, invalid in the mode (NO64), or longer
 *                       than 15 bytes
 *   PW_ERR_PRECONDITION a null argument, or a mode that is neither 32 nor 64
 * On every result but PW_ERR_PRECONDITION, *inst holds what was decoded up to the point of the
 * verdict.
 *
 * Long mode, as the console's Zen 2 runs it: 40-4F are REX, the last prefix (a REX followed by
 * another prefix is ignored); C4/C5 are always VEX and 62 always EVEX; mod 00 rm 101 is
 * RIP-relative; stack and near-branch forms default to 64-bit operands (D64); MOV r64, imm64 and
 * moffs64 carry 8 bytes; NO64 opcodes are MALFORMED. 32-bit results are what they always were:
 * pw_ia32_decode is pw_ia32_decode_mode(..., PW_IA32_MODE_32, ...). */

enum { PW_IA32_MAX_LENGTH = 15 };
enum { PW_IA32_MODE_32 = 32, PW_IA32_MODE_64 = 64 };
enum { PW_IA32_NO_OFFSET = 0xff };

/* Segment registers, in their encoding order (Sreg). */
enum { PW_IA32_SEG_ES, PW_IA32_SEG_CS, PW_IA32_SEG_SS, PW_IA32_SEG_DS, PW_IA32_SEG_FS, PW_IA32_SEG_GS,
       PW_IA32_SEG_NONE = 0xff };
enum { PW_IA32_ENC_LEGACY, PW_IA32_ENC_VEX, PW_IA32_ENC_EVEX, PW_IA32_ENC_XOP };
/* General registers, in their encoding order; 8-15 are r8-r15 in long mode. */
enum { PW_IA32_EAX, PW_IA32_ECX, PW_IA32_EDX, PW_IA32_EBX, PW_IA32_ESP, PW_IA32_EBP, PW_IA32_ESI, PW_IA32_EDI,
       PW_IA32_NO_REG = 0xff };
/* VEX.pp: the mandatory prefix the payload stands for. */
enum { PW_IA32_PP_NONE, PW_IA32_PP_66, PW_IA32_PP_F3, PW_IA32_PP_F2 };

typedef struct PwIa32Inst {
    const PwIa32Form *form;   /* the matching row, or NULL */
    uint8_t mode;             /* PW_IA32_MODE_32 or PW_IA32_MODE_64 */
    uint8_t length;           /* bytes, prefixes included */
    uint8_t encoding;         /* PW_IA32_ENC_* */
    uint8_t map, opcode;      /* PW_IA32_MAP_*, and the opcode byte in it */
    char shape;               /* the grid code of map/opcode */
    uint8_t prefixes;         /* prefix bytes (REX included) before the opcode, or before the VEX/EVEX/XOP escape */
    uint8_t rex;              /* long mode: the REX byte that took effect, or 0 */
    uint8_t opcode_at;        /* offset of the opcode byte (after 0F/0F 38/0F 3A or the VEX payload) */
    uint8_t modrm_at;         /* offset of the ModRM byte, or PW_IA32_NO_OFFSET */
    uint8_t has_66, has_67;
    uint8_t lock;
    uint8_t rep;              /* 0, 0xF2 or 0xF3: the last of them */
    uint8_t rep_last;         /* that F2/F3 is the byte just before the opcode (a REX between them aside) */
    uint8_t segment;          /* the last segment override, or PW_IA32_SEG_NONE; in long mode ES/CS/SS/DS are null
                               * prefixes, so an FS or GS before them stays (AMD APM vol. 3, 1.2.4) */
    uint8_t operand_size;     /* 16, 32 or 64, after the form's mandatory prefix is taken out; VEX: 64 with W */
    uint8_t address_size;     /* 16 or 32; long mode: 32 or 64 */
    uint8_t has_modrm, mod;
    uint8_t reg, rm;          /* ModRM.reg and .rm, extended to 0-15 by REX.R/B (VEX R/B) in long mode */
    /* A memory operand (has_modrm and mod != 3), or moffs: base and index are PW_IA32_* register
     * numbers (0-15 in long mode) or PW_IA32_NO_REG; with 16-bit addressing they name BX, BP, SI and DI
     * by those numbers. RIP-relative: base and index PW_IA32_NO_REG, rip_relative 1, disp from the
     * next instruction. */
    uint8_t has_sib, sib;
    uint8_t base, index, scale;    /* scale 1, 2, 4 or 8 */
    uint8_t rip_relative;
    uint8_t default_segment;       /* SS for an EBP/ESP (BP) base, else DS */
    uint8_t disp_size;             /* 0, 1, 2 or 4; moffs: its size, with the whole value in imm */
    int32_t disp;                  /* sign-extended */
    uint8_t imm_size, imm2_size;   /* bytes of the first and second immediates */
    uint64_t imm;                  /* as encoded, zero-extended (imm64 and moffs64 in long mode) */
    uint32_t imm2;                 /* for ptr16:32, imm is the offset and imm2 the selector */
    /* VEX/EVEX/XOP: L, W, vvvv (un-inverted, 0-15) and pp (PW_IA32_PP_*); 0 otherwise. */
    uint8_t vex_l, vex_w, vex_vvvv, vex_pp;
} PwIa32Inst;

int pw_ia32_decode(const uint8_t *bytes, size_t available, PwIa32Inst *inst);
int pw_ia32_decode_mode(const uint8_t *bytes, size_t available, unsigned mode, PwIa32Inst *inst);

/* How an instruction's ModRM memory operand is accessed (docs/MMU4K.md, section 5): the engine
 * stores through an alias only what it can size, so a size here is never smaller than the access.
 * bytes is exact when exact is 1, else an upper bound (a masked store). */
enum { PW_IA32_ACCESS_NONE, PW_IA32_ACCESS_ADDRESS, PW_IA32_ACCESS_READ, PW_IA32_ACCESS_WRITE,
       PW_IA32_ACCESS_RW };
typedef struct PwIa32Access {
    uint8_t kind, exact;
    uint16_t bytes;
} PwIa32Access;
/* NONE: no ModRM memory operand (mod 3, or none at all). ADDRESS: the address is only computed
 * (LEA, NOP Ev, PREFETCH), bytes 0. READ, WRITE, RW: the .def's rule (operand 1 or 2 is read;
 * operand 0 is read with MRO, read and written with LOCK or O1, else written).
 * PW_OK; PW_ERR_UNSUPPORTED when the form also touches memory ModRM does not name (STACK, STR, IMPL,
 * moffs) or its memory operand has no size (M); PW_ERR_PRECONDITION for a null argument or no form. */
int pw_ia32_access(const PwIa32Inst *inst, PwIa32Access *access);

#endif
