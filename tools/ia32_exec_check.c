/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * The interpreter against the real CPU (docs/IA32_DESIGN.md, M2). Built with -m32 by
 * tools/ia32-exec-check.sh, with the decoder and src/pw_ia32_interp.c.
 *
 * For every form of the table, CASES encodings are generated (random ModRM, SIB, displacement and
 * immediates; 66 now and then; REP and LOCK where the form takes them), each from a random state:
 * registers pointing into the 64 KiB data window or random, random arithmetic flags and DF,
 * random window bytes. Each runs once natively, single-stepped, and once through the interpreter
 * over a copy of the window, and the two are compared:
 *
 *   EVENT   one completed and the other faulted, or they faulted differently
 *   EIP     both completed, at different EIPs
 *   REGS    the general registers differ
 *   FLAGS   EFLAGS differ outside the bits the table leaves undefined
 *   MEMORY  the windows differ
 *
 * One JSON line per form with any mismatch (counts and the first example of each kind) goes to
 * argv[1]; stdout gets the summary. Forms the interpreter does not run yet are counted, not
 * compared. Exit status 0.
 */
#include "ia32_cpu_core.h"
#include "../src/pw_ia32_interp.h"
#include <poll.h>
#include <stdlib.h>
#include <sys/wait.h>

enum { CASES = 400, KIND_EVENT, KIND_EIP, KIND_REGS, KIND_FLAGS, KIND_MEMORY };
enum { RESERVED = 0x40000000u, RESERVED_BYTES = 0x40000000u };   /* PROT_NONE in every worker */
static const char *const kind_names[] = { "EVENT", "EIP", "REGS", "FLAGS", "MEMORY" };

static uint8_t interp_window[DATA_BYTES];
static uint8_t initial_window[DATA_BYTES];

static uint64_t rng_state;
static uint32_t rnd(void)
{
    rng_state ^= rng_state << 13; rng_state ^= rng_state >> 7; rng_state ^= rng_state << 17;
    return (uint32_t)(rng_state >> 16);
}

/* The interpreter's memory: the window copy, and the code page for fetching. */
static uint8_t *translate(void *opaque, uint32_t address, uint32_t bytes, int write)
{
    (void)opaque;
    if (address >= DATA && address + bytes <= DATA + DATA_BYTES && address + bytes > address)
        return interp_window + (address - DATA);
    if (!write && address >= CODE && address + bytes <= CODE + CODE_BYTES) return (uint8_t *)(uintptr_t)address;
    return NULL;
}

/* An encoding of `form`, or 0 when this draw does not decode back to it. */
static size_t build(const PwIa32Form *form, uint8_t *b, PwIa32Inst *inst)
{
    size_t n = 0;
    if (form->size == PW_IA32_SIZE_O16) b[n++] = 0x66;
    else if (form->size == PW_IA32_SIZE_A16) b[n++] = 0x67;
    else if (form->size == PW_IA32_SIZE_ANY && (form->prefix == PW_IA32_PFX_ANY || form->prefix == PW_IA32_PFX_NOREP) &&
             rnd() % 4 == 0) b[n++] = 0x66;
    if (form->prefix == PW_IA32_PFX_P66) b[n++] = 0x66;
    if (form->prefix == PW_IA32_PFX_PF3) b[n++] = 0xf3;
    if (form->prefix == PW_IA32_PFX_PF2) b[n++] = 0xf2;
    if ((form->attributes & (PW_IA32_ATTR_REP | PW_IA32_ATTR_REPCC)) && rnd() % 2)
        b[n++] = (form->attributes & PW_IA32_ATTR_REPCC) && rnd() % 2 ? 0xf2 : 0xf3;
    if ((form->attributes & PW_IA32_ATTR_LOCK) && rnd() % 4 == 0) b[n++] = 0xf0;
    if (form->map == PW_IA32_MAP_0F) b[n++] = 0x0f;
    if (form->map == PW_IA32_MAP_0F38) { b[n++] = 0x0f; b[n++] = 0x38; }
    if (form->map == PW_IA32_MAP_0F3A) { b[n++] = 0x0f; b[n++] = 0x3a; }
    b[n++] = (uint8_t)(form->opcode + ((form->attributes & PW_IA32_ATTR_OPREG) ? rnd() % 8 : 0));
    const char shape = pw_ia32_shape(form->map, form->opcode);
    if (strchr("mBZfF3Q", shape)) {
        const unsigned reg = form->reg != PW_IA32_SEL_X ? form->reg : rnd() % 8;
        unsigned mod = form->mod == PW_IA32_MOD_REG ? 3 : form->mod == PW_IA32_MOD_MEM ? rnd() % 3 : rnd() % 4;
        const unsigned rm = form->rm != PW_IA32_SEL_X ? form->rm : rnd() % 8;
        b[n++] = (uint8_t)((mod << 6) | (reg << 3) | rm);
        if (mod != 3 && rm == 4) b[n++] = (uint8_t)rnd();
        if (mod == 0 && (rm == 5 || (rm == 4 && (b[n - 1] & 7) == 5))) {   /* disp32 without a base */
            const uint32_t at = DATA + 0x1000 + rnd() % 0xc000;
            memcpy(b + n, &at, 4); n += 4;
        } else if (mod == 1) {
            b[n++] = (uint8_t)(rnd() % 128);
        } else if (mod == 2) {
            const uint32_t d = rnd() % 0x400;
            memcpy(b + n, &d, 4); n += 4;
        }
    }
    for (unsigned k = 0; k < 8; k++) b[n + k] = (uint8_t)rnd();
    if (pw_ia32_decode(b, n + 8, inst) != PW_OK || inst->form != form) return 0;
    return inst->length;
}

/* EFLAGS bits worth comparing for this instruction. */
static uint32_t flags_compared(const PwIa32Inst *inst, const uint32_t regs[8])
{
    const PwIa32Form *form = inst->form;
    uint32_t mask = PW_IA32_EFLAGS_ARITH | PW_IA32_EFLAGS_DF;
    mask &= ~(uint32_t)form->undefined;
    if (form->attributes & PW_IA32_ATTR_O1) {
        unsigned count;
        const unsigned last = form->operands[2] != PW_IA32_OP_NONE ? form->operands[2] : form->operands[1];
        if (last == PW_IA32_OP_CL) count = regs[PW_IA32_ECX] & 0x1f;
        else if (last == PW_IA32_OP_ONE) count = 1;
        else count = inst->imm & 0x1f;
        if (count != 1) mask &= ~PW_IA32_EFLAGS_OF;
        unsigned bits = inst->operand_size;
        if (form->operands[0] == PW_IA32_OP_Eb) bits = 8;
        if (count > bits) mask &= ~(PW_IA32_EFLAGS_CF | PW_IA32_EFLAGS_OF);
    }
    return mask;
}

/* Results with the shift of a 16-bit SHLD/SHRD past 16 bits are undefined: nothing to compare. */
static int undefined_result(const PwIa32Inst *inst, const uint32_t regs[8])
{
    const char *m = inst->form->mnemonic;
    if ((!strcmp(m, "shld") || !strcmp(m, "shrd")) && inst->operand_size == 16) {
        const unsigned count = inst->form->operands[2] == PW_IA32_OP_CL ? regs[PW_IA32_ECX] & 0x1f : inst->imm & 0x1f;
        return count > 16;
    }
    return 0;
}

/* The CPU's signal and the interpreter's event agree on what happened. */
static int events_match(const CpuResult *cpu, int completed, const PwIa32Event *e)
{
    if (completed) return e->vector == PW_IA32_EXC_NONE;
    switch (cpu->signal) {
    case SIGSEGV: case SIGBUS:
        return e->vector == PW_IA32_EXC_PF || e->vector == PW_IA32_EXC_GP || e->vector == PW_IA32_EXC_INT ||
               e->vector == PW_IA32_EXC_OF;
    case SIGFPE: return e->vector == PW_IA32_EXC_DE;
    case SIGILL: return e->vector == PW_IA32_EXC_UD;
    case SIGTRAP: return e->vector == (cpu->si_code == 128 ? PW_IA32_EXC_BP : PW_IA32_EXC_DB);
    default: return 0;
    }
}

typedef struct Report {
    uint32_t form, cases, compared, unsupported, skipped, mismatches[5];
    char example[5][1024];
} Report;

static void describe(char *out, size_t cap, const uint8_t *b, size_t n, const uint32_t regs[8], uint32_t eflags,
                     const CpuResult *cpu, const PwIa32State *s, const PwIa32Event *e)
{
    int at = snprintf(out, cap, "\"bytes\":\"");
    for (size_t i = 0; i < n; i++) at += snprintf(out + at, cap - (size_t)at, "%02x", b[i]);
    at += snprintf(out + at, cap - (size_t)at, "\",\"eflags_in\":\"%08x\",\"regs_in\":[", eflags);
    for (int i = 0; i < 8; i++) at += snprintf(out + at, cap - (size_t)at, "%s\"%08x\"", i ? "," : "", regs[i]);
    at += snprintf(out + at, cap - (size_t)at, "],\"cpu\":{\"signal\":%d,\"si_code\":%d,\"delta\":%d,\"eflags\":\"%08x\",\"regs\":[",
                   cpu->signal, cpu->si_code, cpu->delta, cpu->eflags);
    for (int i = 0; i < 8; i++) at += snprintf(out + at, cap - (size_t)at, "%s\"%08x\"", i ? "," : "", cpu->regs[i]);
    at += snprintf(out + at, cap - (size_t)at, "]},\"ours\":{\"vector\":%d,\"delta\":%d,\"eflags\":\"%08x\",\"regs\":[",
                   e->vector, (int)(s->eip - CODE_AT), s->eflags);
    for (int i = 0; i < 8; i++) at += snprintf(out + at, cap - (size_t)at, "%s\"%08x\"", i ? "," : "", s->gpr[i]);
    snprintf(out + at, cap - (size_t)at, "]}");
}

static void check_form(uint32_t index, Report *rep)
{
    const PwIa32Form *form = pw_ia32_form_at(index);
    memset(rep, 0, sizeof(*rep));
    rep->form = index;
    for (unsigned k = 0; k < CASES; k++) {
        uint8_t b[32];
        PwIa32Inst inst;
        const size_t n = build(form, b, &inst);
        if (!n) continue;
        rep->cases++;
        uint32_t regs[8];
        /* Into the window, or into the reserved range nothing is mapped at (so a stray address
         * faults on the CPU as it does in the interpreter). */
        for (int i = 0; i < 8; i++) regs[i] = rnd() % 5 < 3 ? DATA + 0x2000 + rnd() % 0x8000 : RESERVED + rnd() % RESERVED_BYTES;
        regs[PW_IA32_ESP] = DATA_STACK - 4 * (rnd() % 64);
        if (form->attributes & (PW_IA32_ATTR_REP | PW_IA32_ATTR_REPCC)) regs[PW_IA32_ECX] = rnd() % 24;
        if (rnd() % 2) regs[PW_IA32_ECX] = (regs[PW_IA32_ECX] & ~0xffu) | (rnd() % 40);
        const uint32_t eflags = 0x202 | (rnd() & (PW_IA32_EFLAGS_ARITH | (rnd() % 4 == 0 ? PW_IA32_EFLAGS_DF : 0)));
        for (unsigned i = 0; i < DATA_BYTES; i += 4) { const uint32_t v = rnd(); memcpy(initial_window + i, &v, 4); }
        /* The interpreter, over a copy of the window. */
        memcpy(interp_window, initial_window, DATA_BYTES);
        PwIa32State s;
        memset(&s, 0, sizeof(s));
        memcpy(s.gpr, regs, sizeof(regs));
        s.eip = CODE_AT;
        s.eflags = eflags;
        s.selector[0] = g_es; s.selector[1] = g_cs; s.selector[2] = g_ss; s.selector[3] = g_ds;
        s.selector[4] = g_fs; s.selector[5] = g_gs;
        memset((void *)CODE, 0xcc, CODE_BYTES);
        memcpy((void *)CODE_AT, b, n);
        const PwIa32Memory memory = { translate, NULL };
        PwIa32Event e;
        if (pw_ia32_step(&s, &memory, &e) == PW_ERR_UNSUPPORTED) {
            rep->unsupported++;
            continue;
        }
        if (undefined_result(&inst, regs)) { rep->skipped++; continue; }
        /* The CPU, over the window itself. */
        memcpy((void *)DATA, initial_window, DATA_BYTES);
        CpuResult cpu;
        cpu_run_state(b, n, regs, eflags, &cpu);
        if (cpu.refused || cpu.signal == SIGALRM) { rep->skipped++; continue; }
        const int completed = cpu_completed(&cpu);
        /* An address outside the window and the reserved range may be mapped in this process (the
         * harness's own memory): the CPU reaches it, the interpreter cannot. Not a comparison. */
        if (cpu.signal != SIGSEGV && e.vector == PW_IA32_EXC_PF && !(e.address >= RESERVED && e.address - RESERVED < RESERVED_BYTES) &&
            !(e.address >= DATA && e.address < DATA + DATA_BYTES)) { rep->skipped++; continue; }
        /* The oracle single-steps the CPU, so a PUSHF there pushes TF: the interpreter's image gets it too. */
        if (completed && e.vector == PW_IA32_EXC_NONE && (!strcmp(form->mnemonic, "pushf") || !strcmp(form->mnemonic, "pushfd"))) {
            const uint32_t at = s.gpr[PW_IA32_ESP];
            if (at >= DATA && at + 2 <= DATA + DATA_BYTES) interp_window[at - DATA + 1] |= 0x01;
        }
        rep->compared++;
        int kind = -1;
        if (!events_match(&cpu, completed, &e)) kind = KIND_EVENT;
        else if (completed && (uint32_t)(CODE_AT + cpu.delta) != s.eip) kind = KIND_EIP;
        else if (memcmp(cpu.regs, s.gpr, sizeof(s.gpr))) kind = KIND_REGS;
        else if (((cpu.eflags ^ s.eflags) & flags_compared(&inst, regs))) kind = KIND_FLAGS;
        else if (memcmp((void *)DATA, interp_window, DATA_BYTES)) kind = KIND_MEMORY;
        if (kind < 0) continue;
        const unsigned slot = (unsigned)(kind - KIND_EVENT);
        if (!rep->mismatches[slot]++)
            describe(rep->example[slot], sizeof(rep->example[slot]), b, n, regs, eflags, &cpu, &s, &e);
    }
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <mismatches.jsonl>\n", argv[0]);
        return 2;
    }
    const size_t forms = pw_ia32_form_count();
    FILE *out = fopen(argv[1], "w");
    uint64_t cases = 0, compared = 0, unsupported = 0, skipped = 0, mismatched_forms = 0, ran_forms = 0;
    uint64_t kinds[5] = { 0 };
    for (uint32_t index = 0; index < forms; index++) {
        int pipefd[2];
        if (pipe(pipefd)) return 2;
        const pid_t pid = fork();
        if (pid == 0) {
            close(pipefd[0]);
            cpu_worker_setup();
            g_follow_rep = 1;
            if (mmap((void *)RESERVED, RESERVED_BYTES, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0) == MAP_FAILED)
                _exit(3);
            rng_state = 0x9e3779b97f4a7c15ull ^ ((uint64_t)index * 0x100000001b3ull);
            static Report rep;
            check_form(index, &rep);
            if (write(pipefd[1], &rep, sizeof(rep)) != (ssize_t)sizeof(rep)) _exit(1);
            _exit(0);
        }
        close(pipefd[1]);
        static Report rep;
        size_t got = 0;
        while (got < sizeof(rep)) {
            struct pollfd p = { pipefd[0], POLLIN, 0 };
            if (poll(&p, 1, 120000) == 0) { kill(pid, SIGKILL); break; }   /* a hung form: reported, skipped */
            const ssize_t r = read(pipefd[0], (char *)&rep + got, sizeof(rep) - got);
            if (r <= 0) break;
            got += (size_t)r;
        }
        close(pipefd[0]);
        waitpid(pid, NULL, 0);
        if (got != sizeof(rep)) {
            fprintf(out, "{\"form\":\"%s\",\"worker\":\"died or hung\"}\n", pw_ia32_form_at(index)->name);
            fflush(out);
            continue;
        }
        cases += rep.cases; compared += rep.compared; unsupported += rep.unsupported; skipped += rep.skipped;
        if (rep.compared) ran_forms++;
        uint32_t any = 0;
        for (int k = 0; k < 5; k++) { kinds[k] += rep.mismatches[k]; any += rep.mismatches[k]; }
        if (!any) continue;
        mismatched_forms++;
        fprintf(out, "{\"form\":\"%s\",\"compared\":%u", pw_ia32_form_at(index)->name, rep.compared);
        for (int k = 0; k < 5; k++)
            if (rep.mismatches[k]) fprintf(out, ",\"%s\":{\"count\":%u,%s}", kind_names[k], rep.mismatches[k], rep.example[k]);
        fprintf(out, "}\n");
        fflush(out);
    }
    fclose(out);
    printf("{\"forms\":%zu,\"forms_compared\":%llu,\"forms_with_mismatches\":%llu,\"cases\":%llu,\"compared\":%llu,"
           "\"unsupported\":%llu,\"skipped\":%llu", forms, (unsigned long long)ran_forms,
           (unsigned long long)mismatched_forms, (unsigned long long)cases, (unsigned long long)compared,
           (unsigned long long)unsupported, (unsigned long long)skipped);
    for (int k = 0; k < 5; k++) printf(",\"%s\":%llu", kind_names[k], (unsigned long long)kinds[k]);
    printf("}\n");
    return 0;
}
