/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The IA-32 interpreter on small fixed programs (docs/IA32_DESIGN.md, M2). The CPU oracle
 * (tools/ia32-exec-check.sh) compares it with the real CPU from random states; this test pins down
 * what an oracle run cannot show at a glance: whole programs, precise faults, REP progress at a
 * fault, and that an instruction it does not run leaves the state untouched. */
#include "../src/pw_ia32_interp.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

enum { MEMORY_BASE = 0x10000, MEMORY_BYTES = 0x4000 };
static uint8_t memory[MEMORY_BYTES];

static uint8_t *translate(void *opaque, uint32_t address, uint32_t bytes, int write)
{
    (void)opaque; (void)write;
    if (address < MEMORY_BASE || address + bytes > MEMORY_BASE + MEMORY_BYTES || address + bytes < address) return NULL;
    return memory + (address - MEMORY_BASE);
}

static const PwIa32Memory guest = { translate, NULL };

static void load(PwIa32State *s, const char *hex)
{
    memset(memory, 0, sizeof(memory));
    memset(s, 0, sizeof(*s));
    for (size_t i = 0; hex[2 * i] && hex[2 * i + 1]; i++) {
        unsigned v;
        sscanf(hex + 2 * i, "%2x", &v);
        memory[i] = (uint8_t)v;
    }
    s->eip = MEMORY_BASE;
    s->eflags = 0x202;
    s->gpr[PW_IA32_ESP] = MEMORY_BASE + MEMORY_BYTES;
}

/* Runs until an event or `limit` steps; returns the event's vector. */
static int run(PwIa32State *s, unsigned limit)
{
    PwIa32Event e;
    for (unsigned i = 0; i < limit; i++) {
        assert(pw_ia32_step(s, &guest, &e) == PW_OK);
        if (e.vector != PW_IA32_EXC_NONE) return e.vector;
    }
    return PW_IA32_EXC_NONE;
}

int main(void)
{
    PwIa32State s;
    /* mov ecx, 10; xor eax, eax; again: add eax, ecx; loop again; push eax; pop ebx; int3 */
    load(&s, "b90a00000031c001c8e2fc505bcc");
    assert(run(&s, 100) == PW_IA32_EXC_BP);
    assert(s.gpr[PW_IA32_EAX] == 55 && s.gpr[PW_IA32_EBX] == 55 && s.gpr[PW_IA32_ECX] == 0);
    assert(s.eip == MEMORY_BASE + 14 && s.gpr[PW_IA32_ESP] == MEMORY_BASE + MEMORY_BYTES);

    /* call +0; pop eax: EAX is the return address */
    load(&s, "e80000000058cc");
    assert(run(&s, 10) == PW_IA32_EXC_BP && s.gpr[PW_IA32_EAX] == MEMORY_BASE + 5);

    /* A load from an unmapped address faults with the state unchanged. */
    load(&s, "a100000000");
    PwIa32State before = s;
    PwIa32Event e;
    assert(pw_ia32_step(&s, &guest, &e) == PW_OK && e.vector == PW_IA32_EXC_PF && e.address == 0);
    assert(!memcmp(&s, &before, sizeof(s)));

    /* rep movsb across the end of memory: the bytes before the fault are copied and counted. */
    load(&s, "f3a4");
    s.gpr[PW_IA32_ESI] = MEMORY_BASE + 0x100;
    s.gpr[PW_IA32_EDI] = MEMORY_BASE + MEMORY_BYTES - 3;
    s.gpr[PW_IA32_ECX] = 10;
    memset(memory + 0x100, 0xab, 16);
    assert(pw_ia32_step(&s, &guest, &e) == PW_OK && e.vector == PW_IA32_EXC_PF);
    assert(s.gpr[PW_IA32_ECX] == 7 && s.eip == MEMORY_BASE && memory[MEMORY_BYTES - 1] == 0xab);

    /* div by zero: #DE, nothing changed. */
    load(&s, "31c9f7f1");
    assert(run(&s, 5) == PW_IA32_EXC_DE && s.eip == MEMORY_BASE + 2);

    /* LOCK on a register destination: #UD. */
    load(&s, "f001c0");
    assert(run(&s, 1) == PW_IA32_EXC_UD);

    /* An x87 instruction is not run by this slice: PW_ERR_UNSUPPORTED, the state untouched. */
    load(&s, "d9e8");
    before = s;
    assert(pw_ia32_step(&s, &guest, &e) == PW_ERR_UNSUPPORTED && !memcmp(&s, &before, sizeof(s)));
    assert(pw_ia32_step(NULL, &guest, &e) == PW_ERR_PRECONDITION);
    printf("pw_ia32_interp: all checks passed\n");
    return 0;
}
