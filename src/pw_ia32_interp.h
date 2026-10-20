/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_IA32_INTERP_H
#define PW_IA32_INTERP_H
#include "pw_ia32_decode.h"

/* The IA-32 interpreter (docs/IA32_DESIGN.md, M2): one instruction at a time over a PwIa32State,
 * decoded by pw_ia32_decode and executed by one small function per operation, chosen through the
 * table's mnemonic. It is the exact fallback where native execution cannot start, and the tool
 * that single-steps a guest for a trace. tools/ia32-exec-check.sh compares it with the CPU.
 *
 * Precise exceptions: every memory operand of an instruction is translated (through
 * PwIa32Memory) before anything is written, so an instruction that faults leaves the state as it
 * was. A REP string instruction faults between iterations, with ECX/ESI/EDI counting the ones
 * done, as the CPU does.
 *
 * This slice runs the general-purpose integer instructions. An instruction it does not run yet
 * (x87, MMX, SSE, segment loads, CPUID...) returns PW_ERR_UNSUPPORTED with the state unchanged:
 * that is the interpreter's gap, never the guest's fault. */

/* EFLAGS bits. */
enum {
    PW_IA32_EFLAGS_CF = 1u << 0, PW_IA32_EFLAGS_PF = 1u << 2, PW_IA32_EFLAGS_AF = 1u << 4,
    PW_IA32_EFLAGS_ZF = 1u << 6, PW_IA32_EFLAGS_SF = 1u << 7, PW_IA32_EFLAGS_TF = 1u << 8,
    PW_IA32_EFLAGS_IF = 1u << 9, PW_IA32_EFLAGS_DF = 1u << 10, PW_IA32_EFLAGS_OF = 1u << 11,
    PW_IA32_EFLAGS_AC = 1u << 18, PW_IA32_EFLAGS_ID = 1u << 21,
    PW_IA32_EFLAGS_ARITH = 0x8d5u,      /* CF PF AF ZF SF OF */
};

/* Exception vectors an instruction can raise. */
enum {
    PW_IA32_EXC_NONE = -1, PW_IA32_EXC_DE = 0, PW_IA32_EXC_DB = 1, PW_IA32_EXC_BP = 3, PW_IA32_EXC_OF = 4,
    PW_IA32_EXC_BR = 5, PW_IA32_EXC_UD = 6, PW_IA32_EXC_GP = 13, PW_IA32_EXC_PF = 14,
    PW_IA32_EXC_INT = 256,  /* INT n: the vector is in PwIa32Event.code */
};

typedef struct PwIa32State {
    uint32_t gpr[8];        /* EAX ECX EDX EBX ESP EBP ESI EDI */
    uint32_t eip, eflags;
    uint16_t selector[6];   /* ES CS SS DS FS GS */
    uint32_t base[6];       /* their bases: FS's is the TEB, the others 0 in a flat process */
    /* x87, MMX, SSE and MXCSR as an FXSAVE image: the layout Wine's 32-bit context carries. */
    uint8_t fx[512] __attribute__((aligned(16)));
} PwIa32State;

/* Guest memory: a host pointer to `bytes` bytes at linear `address`, writable when `write`, or
 * NULL when the access would fault. */
typedef struct PwIa32Memory {
    uint8_t *(*translate)(void *opaque, uint32_t address, uint32_t bytes, int write);
    void *opaque;
} PwIa32Memory;

typedef struct PwIa32Event {
    int vector;             /* PW_IA32_EXC_*, PW_IA32_EXC_NONE when the instruction completed */
    uint32_t code;          /* the error code, or INT's vector */
    uint32_t address;       /* #PF: the linear address that faulted */
} PwIa32Event;

/* Executes the instruction at state->eip. PW_OK with event->vector NONE: done, EIP advanced.
 * PW_OK with a vector: the instruction raised it; the state is as the CPU leaves it for that
 * exception (EIP at the instruction for a fault, after it for INT3/INT n/INTO).
 * PW_ERR_UNSUPPORTED: not run by this interpreter yet; the state is unchanged.
 * PW_ERR_PRECONDITION: a null argument. */
int pw_ia32_step(PwIa32State *state, const PwIa32Memory *memory, PwIa32Event *event);

#endif
