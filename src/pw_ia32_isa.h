/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_IA32_ISA_H
#define PW_IA32_ISA_H
#include <stddef.h>
#include <stdint.h>

/* The IA-32 instruction table in C (docs/IA32_DESIGN.md). Everything here comes from
 * src/pw_ia32_isa.def, the one description of the instruction set: its grids say how long an
 * encoding is, its forms say what it is. This header only names the values the .def uses.
 * The same table serves 32-bit protected mode and 64-bit long mode (docs/MMU4K.md, section 4.1):
 * NO64 and X64 rows exist in one mode only, VEX and VEXONLY rows also (or only) describe VEX
 * encodings, which have forms in long mode only.
 *
 * Portable: no allocation, no locking beyond a one-time index build, no libc beyond <stddef.h>
 * and <stdint.h>. */

enum {
    PW_IA32_MAP_PRI = 0,  /* one-byte opcodes */
    PW_IA32_MAP_0F = 1,
    PW_IA32_MAP_0F38 = 2,
    PW_IA32_MAP_0F3A = 3,
    PW_IA32_MAPS = 4,
};

/* A form's mandatory prefix. ANY: 66 is the operand size and F2/F3 select nothing. */
enum { PW_IA32_PFX_ANY, PW_IA32_PFX_NP, PW_IA32_PFX_P66, PW_IA32_PFX_PF2, PW_IA32_PFX_PF3,
       PW_IA32_PFX_NOREP /* as ANY, but F2/F3 make it #UD (no form matches) */ };
/* The size a form's mnemonic is for. O64 and A64 occur in long mode only. */
enum { PW_IA32_SIZE_ANY, PW_IA32_SIZE_O16, PW_IA32_SIZE_O32, PW_IA32_SIZE_A16, PW_IA32_SIZE_A32,
       PW_IA32_SIZE_O64, PW_IA32_SIZE_A64 };
/* ModRM selection: reg and rm are 0-7, or PW_IA32_SEL_X for any. */
enum { PW_IA32_SEL_X = 0xff };
enum { PW_IA32_MOD_ANY, PW_IA32_MOD_MEM, PW_IA32_MOD_REG };

/* Operands, in the Intel SDM's notation (volume 2, appendix A.2). b/w/d: 8/16/32 bits; v: the
 * operand size; z: 16 or 32 bits by operand size. */
enum {
    PW_IA32_OP_NONE = 0,
    PW_IA32_OP_Eb, PW_IA32_OP_Ew, PW_IA32_OP_Ed, PW_IA32_OP_Ev, /* ModRM.rm: register or memory */
    PW_IA32_OP_Evw,                                   /* register of the operand size, or a word in memory (MOV Sreg) */
    PW_IA32_OP_Gb, PW_IA32_OP_Gw, PW_IA32_OP_Gd, PW_IA32_OP_Gv, /* ModRM.reg */
    PW_IA32_OP_Ib,                                    /* imm8, printed signed */
    PW_IA32_OP_Ibs,                                   /* imm8 sign-extended to the operand size */
    PW_IA32_OP_Ibu,                                   /* imm8, printed unsigned (counts, ports, vectors) */
    PW_IA32_OP_Ib2,                                   /* the second immediate, imm8 (ENTER) */
    PW_IA32_OP_Iw,                                    /* imm16: unsigned in a 16-bit operation, signed in a 32-bit one */
    PW_IA32_OP_Iws,                                   /* imm16, printed signed (ENTER's frame size) */
    PW_IA32_OP_Iz,
    PW_IA32_OP_Jb, PW_IA32_OP_Jz,                     /* relative to the next instruction */
    PW_IA32_OP_Ob, PW_IA32_OP_Ov,                     /* moffs: an absolute address in the instruction */
    PW_IA32_OP_M,                                     /* memory, no size (LEA, FXSAVE) */
    PW_IA32_OP_Mb, PW_IA32_OP_Mw, PW_IA32_OP_Md, PW_IA32_OP_Mq,
    PW_IA32_OP_Mt,                                    /* 80 bits: x87 extended and BCD */
    PW_IA32_OP_Mx,                                    /* 128 bits */
    PW_IA32_OP_Ma,                                    /* BOUND's pair */
    PW_IA32_OP_Mp,                                    /* a far pointer in memory */
    PW_IA32_OP_Ap,                                    /* a far pointer in the instruction */
    PW_IA32_OP_Sw,                                    /* a segment register in ModRM.reg */
    PW_IA32_OP_Zb, PW_IA32_OP_Zv,                     /* a register in the opcode's low three bits */
    PW_IA32_OP_AL, PW_IA32_OP_AX, PW_IA32_OP_eAX, PW_IA32_OP_CL, PW_IA32_OP_DX,
    PW_IA32_OP_ST0, PW_IA32_OP_STi,                   /* x87: the stack top, and ST(ModRM.rm) */
    PW_IA32_OP_Vx,                                    /* XMM in ModRM.reg */
    PW_IA32_OP_Wx, PW_IA32_OP_Wq, PW_IA32_OP_Wd, PW_IA32_OP_Ww, /* XMM in ModRM.rm, or memory of 128/64/32/16 bits */
    PW_IA32_OP_Ux,                                    /* XMM in ModRM.rm, register only */
    PW_IA32_OP_Pq,                                    /* MMX in ModRM.reg */
    PW_IA32_OP_Qq, PW_IA32_OP_Qd,                     /* MMX in ModRM.rm, or memory of 64/32 bits */
    PW_IA32_OP_Nq,                                    /* MMX in ModRM.rm, register only */
    PW_IA32_OP_Ebd, PW_IA32_OP_Ewd,                   /* a 32-bit register, or a byte/word in memory */
    PW_IA32_OP_XMM0,                                  /* the implicit XMM0 (BLENDV, SHA256RNDS2) */
    PW_IA32_OP_ONE,                                   /* the constant 1 of a shift; not printed */
    PW_IA32_OP_ES, PW_IA32_OP_CS, PW_IA32_OP_SS, PW_IA32_OP_DS, PW_IA32_OP_FS, PW_IA32_OP_GS,
    PW_IA32_OP_Xb, PW_IA32_OP_Xv,                     /* DS:[eSI] */
    PW_IA32_OP_Yb, PW_IA32_OP_Yv,                     /* ES:[eDI] */
    PW_IA32_OP_Iv,                                    /* an immediate of the operand size: imm64 with REX.W (MOVABS) */
    PW_IA32_OP_By,                                    /* a general register of the operand size in VEX.vvvv (BMI) */
    PW_IA32_OP_Wb,                                    /* XMM in ModRM.rm, or memory of 8 bits (VPBROADCASTB) */
    PW_IA32_OP_Wdq,                                   /* XMM in ModRM.rm, or memory of 128 bits whatever VEX.L */
    PW_IA32_OPERANDS
};

/* Arithmetic flags, at their EFLAGS positions. */
enum {
    PW_IA32_F_C = 1u << 0, PW_IA32_F_P = 1u << 2, PW_IA32_F_A = 1u << 4, PW_IA32_F_Z = 1u << 6,
    PW_IA32_F_S = 1u << 7, PW_IA32_F_D = 1u << 10, PW_IA32_F_O = 1u << 11,
};

enum {
    PW_IA32_ATTR_LOCK = 1u << 0,    /* LOCK is valid with a memory destination */
    PW_IA32_ATTR_REP = 1u << 1,     /* F3 repeats it */
    PW_IA32_ATTR_REPCC = 1u << 2,   /* F3/F2 repeat it while ZF is 1/0 */
    PW_IA32_ATTR_BRANCH = 1u << 3,  /* changes EIP other than to the next instruction */
    PW_IA32_ATTR_OPREG = 1u << 4,   /* the opcode's low three bits name a register */
    PW_IA32_ATTR_O1 = 1u << 5,      /* OF is defined only for a count of 1 */
    PW_IA32_ATTR_PRIV = 1u << 6,    /* faults at CPL 3 (or with IOPL below 3) */
    PW_IA32_ATTR_XACQ = 1u << 7,    /* F2 before it is XACQUIRE */
    PW_IA32_ATTR_XREL = 1u << 8,    /* F3 before it is XRELEASE */
    PW_IA32_ATTR_CMPPRED = 1u << 9, /* its imm8, when below 8, is a predicate named in the mnemonic */
    PW_IA32_ATTR_UD = 1u << 10,     /* always raises #UD (UD2) */
    /* How memory is touched (pw_ia32_access, docs/MMU4K.md section 4.1). */
    PW_IA32_ATTR_MRO = 1u << 11,    /* operand 0 in memory is only read (CMP, TEST, BT, PUSH Ev, x87 loads) */
    PW_IA32_ATTR_ADDR = 1u << 12,   /* the ModRM memory operand is an address only, never accessed (LEA, NOPs) */
    PW_IA32_ATTR_STACK = 1u << 13,  /* uses [rSP] and rSP implicitly (PUSH, POP, CALL, RET, ENTER, LEAVE) */
    PW_IA32_ATTR_STR = 1u << 14,    /* uses [rSI] and/or [rDI] implicitly (string instructions) */
    PW_IA32_ATTR_IMPL = 1u << 15,   /* other memory ModRM does not size: XLAT, MASKMOV, XSAVE, FXSAVE, FNSAVE... */
    PW_IA32_ATTR_MASK = 1u << 16,   /* a masked store: its size is an upper bound, never exact */
    /* Which modes and encodings a row describes. */
    PW_IA32_ATTR_NO64 = 1u << 17,   /* invalid in long mode (#UD there) */
    PW_IA32_ATTR_X64 = 1u << 18,    /* long mode only */
    PW_IA32_ATTR_D64 = 1u << 19,    /* in long mode the operand size defaults to 64 (stack and near branches) */
    PW_IA32_ATTR_VEX = 1u << 20,    /* also the VEX form: VEX.pp as its mandatory prefix, VEX.L widening x operands */
    PW_IA32_ATTR_VEXONLY = 1u << 21, /* a VEX form only */
    PW_IA32_ATTR_L0 = 1u << 22,     /* as VEX, only with VEX.L = 0 */
    PW_IA32_ATTR_L1 = 1u << 23,     /* as VEX, only with VEX.L = 1 */
    PW_IA32_ATTR_W0 = 1u << 24,     /* as VEX, only with VEX.W = 0 */
    PW_IA32_ATTR_W1 = 1u << 25,     /* as VEX, only with VEX.W = 1 */
    PW_IA32_ATTR_VLH = 1u << 26,    /* as VEX with L = 1, the memory operand doubles (half-vector sources) */
    PW_IA32_ATTR_NOV = 1u << 27,    /* as VEX, leaves VEX.vvvv unused: anything but 1111b there is #UD */
};

typedef struct PwIa32Form {
    const char *name;       /* the row's name: unique, and a C identifier */
    const char *mnemonic;   /* LLVM's Intel-syntax spelling */
    uint8_t map, opcode;
    uint8_t reg, mod, rm;   /* ModRM selection (PW_IA32_SEL_X, PW_IA32_MOD_*) */
    uint8_t prefix, size;   /* PW_IA32_PFX_*, PW_IA32_SIZE_* */
    uint8_t operands[3];    /* PW_IA32_OP_* */
    uint16_t reads, writes, undefined; /* PW_IA32_F_* */
    uint32_t attributes;    /* PW_IA32_ATTR_* */
} PwIa32Form;

/* What follows an opcode, as the .def's grid codes ('.', 'm', 'b', ... '-'). */
char pw_ia32_shape(unsigned map, unsigned opcode);

size_t pw_ia32_form_count(void);
/* Row index of the table, or NULL past the end. */
const PwIa32Form *pw_ia32_form_at(size_t index);
/* The rows that can describe map/opcode, in table order: *count of them at the returned array
 * of row indices. OPREG rows are listed under each of their eight opcodes. */
const uint16_t *pw_ia32_forms_for(unsigned map, unsigned opcode, size_t *count);

#endif
