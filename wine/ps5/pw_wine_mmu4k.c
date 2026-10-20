/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The 4 KiB MMU's engine (pw_wine_mmu4k.h, docs/MMU4K.md sections 5-7). Built -mgeneral-regs-only -ffreestanding
 * -fno-builtin: the program's vector registers are live while a run executes, so nothing here may touch them, and
 * nothing here calls libc (whose copies use them). Copies are rep movs; everything outside this file is reached
 * through pw_wine_mmu4k_call (ops, the decoder) or the routines of pw_wine_mmu4k_slot.S. */
#include "pw_wine_mmu4k.h"
#include "../../include/prospero_win.h"
#include "../../src/pw_ia32_decode.h"

enum { SIGNAL_ILL = 4 };   /* SIGILL, the same number on FreeBSD and Linux: the exit trap's ud2 */
enum { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI };
enum { NONE = 0xff, SLOTS = PW_WINE_MMU4K_SLOT_BYTES / PW_WINE_MMU4K_SLOT, PAGE = PW_WINE_MMU4K_PAGE,
       HOST = PW_WINE_MMU4K_HOST_PAGE, XSAVE_BYTES = 1024 /* x87/SSE 512, header 64, AVX 256 at 576 */ };
enum { FLAG_ZF = 1u << 6, FLAG_DF = 1u << 10, ENGINE_FLAGS = 0x202 /* IF, and DF, AC, TF clear */ };
static const uint64_t STACK_PATTERN = 0x6b34756d6d70ull;   /* the lowest 64 bytes of every run stack */

/* What a run does with an instruction (docs/MMU4K.md section 6, step 3). */
enum { PLAN_EMPTY, PLAN_FORM, PLAN_VERBATIM, PLAN_MEMORY, PLAN_JMP, PLAN_JCC, PLAN_JMP_RM, PLAN_CALL, PLAN_CALL_RM,
       PLAN_RET, PLAN_LOOP, PLAN_JRCXZ, PLAN_PUSH, PLAN_PUSH_RM, PLAN_POP, PLAN_LEAVE, PLAN_MOVS, PLAN_STOS };

/* A decoded instruction and what to do with it; its slot is slot_memory + its index * PW_WINE_MMU4K_SLOT. */
typedef struct Plan {
    uint64_t rip;
    uint8_t code[PW_IA32_MAX_LENGTH], length;  /* compared with the bytes at rip before every use */
    uint8_t kind, access, exact, size;         /* size: the memory operand's, a push or pop's, a string element's */
    uint8_t scratch, scratch2;                 /* the slot's address register and its stand-in for RSP, or NONE */
    uint8_t base, index, scale, addr32, rip_relative, memory;   /* the ModRM operand: memory or register reg */
    uint8_t reg;                               /* r/m register, PUSH/POP register (or NONE: an immediate), Jcc cc */
    uint8_t variant;                           /* LOOP 0/E/NE, JECXZ 1, a REP string 1 */
    uint16_t op;                               /* map << 8 | opcode, for the census */
    int32_t disp;
    int64_t imm;                               /* branch displacement, pushed immediate, RET's imm16 */
} Plan;

struct PwWineMmu4kRun {
    PwWineMmu4kRegs regs;      /* the program's: pw_wine_mmu4k_exec reads and writes them at fixed offsets */
    uint64_t host_rsp;         /* pw_wine_mmu4k_exec's stack at the slot call, for its recovery */
    uint64_t fault_rcx, fault_rsi, fault_rdi, fault_address;   /* a recovered fault */
    int32_t fault_signal, stack;
    uintptr_t host_page, address, exit_address;   /* the store that started it; a straddle's fault address */
    uint64_t steps, idle, generation;
    int32_t exit, action, progress;
    uint16_t first_op;
    uint32_t held, next;
    PwWineMmu4kPage pages[PW_WINE_MMU4K_PAGES];   /* written host pages, for the whole run (it holds the mutex) */
    uint8_t xsave[XSAVE_BYTES] __attribute__((aligned(64)));
};
_Static_assert(__builtin_offsetof(PwWineMmu4kRun, regs) == 0 &&
               __builtin_offsetof(PwWineMmu4kRun, regs.rflags) == 136 &&
               __builtin_offsetof(PwWineMmu4kRun, host_rsp) == 144, "pw_wine_mmu4k_slot.S's offsets");
_Static_assert(sizeof(PwWineMmu4kRun) <= PW_WINE_MMU4K_STACK / 4, "a run leaves its stack room");
_Static_assert(sizeof(Plan) % 8 == 0, "plans are cleared by words");

/* pw_wine_mmu4k_slot.S. exec: 0, or 1 after a recovered fault (the run's registers as before it). read, write, copy:
 * 0, or 1 after a recovered fault (copy: the run's fault_rcx/rsi/rdi are where it stopped). */
int pw_wine_mmu4k_exec(PwWineMmu4kRun *run, const void *slot);
int pw_wine_mmu4k_read(void *dst, const void *src, size_t bytes);
int pw_wine_mmu4k_write(void *dst, const void *src, size_t bytes);
int pw_wine_mmu4k_copy(void *dst, uintptr_t src, size_t count, size_t element, int stos);
void pw_wine_mmu4k_call(void (*fn)(void *), void *arg, void *xsave_area, uint64_t mask);
extern const char pw_wine_mmu4k_recover[], pw_wine_mmu4k_guarded_fail[];
extern const char pw_wine_mmu4k_guarded_begin[], pw_wine_mmu4k_guarded_end[];

static PwWineMmu4kOps engine_ops;
static uint8_t *slot_memory;
static uint64_t xsave_mask;
static int ready;
static uint32_t stacks_used;   /* bit i: run stack i is taken */
static uint8_t stacks[PW_WINE_MMU4K_STACKS][PW_WINE_MMU4K_STACK] __attribute__((aligned(64)));
static Plan plans[SLOTS];      /* under the virtual mutex, with the slots */
static PwWineMmu4kStats counters;

static void copy_bytes(void *dst, const void *src, size_t n)
{
    __asm__ volatile("rep movsb" : "+D"(dst), "+S"(src), "+c"(n) : : "memory");
}
static void clear_words(void *dst, size_t words)
{
    __asm__ volatile("rep stosq" : "+D"(dst), "+c"(words) : "a"((uint64_t)0) : "memory");
}
static int same_bytes(const uint8_t *a, const uint8_t *b, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        if (a[i] != b[i]) return 0;
    return 1;
}
static void count(uint64_t *counter, uint64_t n) { __atomic_add_fetch(counter, n, __ATOMIC_RELAXED); }

/* ---------------------------------------------------------------- calls out (P2) */
enum { OP_LOCK, OP_UNLOCK, OP_CLASSIFY };
typedef struct OpCall { int op, result; uintptr_t host_page; PwWineMmu4kPage *page; } OpCall;
static void op_call(void *argument)
{
    OpCall *call = argument;
    if (call->op == OP_LOCK) engine_ops.lock(engine_ops.context);
    else if (call->op == OP_UNLOCK) engine_ops.unlock(engine_ops.context);
    else call->result = engine_ops.classify(engine_ops.context, call->host_page, call->page);
}
static int op(PwWineMmu4kRun *run, int which, uintptr_t host_page, PwWineMmu4kPage *page)
{
    OpCall call = { which, 0, host_page, page };
    pw_wine_mmu4k_call(op_call, &call, run->xsave, xsave_mask);
    return call.result;
}

/* ---------------------------------------------------------------- plans and slots (section 7) */
static int is_g(unsigned o) { return o == PW_IA32_OP_Gb || o == PW_IA32_OP_Gw || o == PW_IA32_OP_Gd || o == PW_IA32_OP_Gv; }
static int is_e(unsigned o)
{
    return o == PW_IA32_OP_Eb || o == PW_IA32_OP_Ew || o == PW_IA32_OP_Ed || o == PW_IA32_OP_Ev || o == PW_IA32_OP_Evw ||
           o == PW_IA32_OP_Ebd || o == PW_IA32_OP_Ewd;
}
static int same_name(const char *a, const char *b)
{
    while (*a && *a == *b) a++, b++;
    return *a == *b;
}
static int64_t signed_imm(const PwIa32Inst *inst)
{
    if (inst->imm_size == 1) return (int8_t)inst->imm;
    if (inst->imm_size == 2) return (int16_t)inst->imm;
    if (inst->imm_size == 4) return (int32_t)inst->imm;
    return (int64_t)inst->imm;
}

/* The forms emulated in C, by the table's row names; 0 for any other. */
static uint8_t emulated(Plan *p, const PwIa32Inst *inst)
{
    static const struct { const char *name; uint8_t kind, variant, size; } rows[] = {
        { "jmp_Jb", PLAN_JMP, 0, 0 }, { "jmp_Jz", PLAN_JMP, 0, 0 }, { "call_Jz", PLAN_CALL, 0, 0 },
        { "jmp_Ev", PLAN_JMP_RM, 0, 0 }, { "call_Ev", PLAN_CALL_RM, 0, 0 }, { "ret", PLAN_RET, 0, 0 },
        { "ret_Iw", PLAN_RET, 0, 0 }, { "loop_Jb", PLAN_LOOP, 0, 0 }, { "loope_Jb", PLAN_LOOP, 1, 0 },
        { "loopne_Jb", PLAN_LOOP, 2, 0 }, { "jrcxz_Jb", PLAN_JRCXZ, 0, 0 }, { "jecxz_Jb", PLAN_JRCXZ, 1, 0 },
        { "push_Zv", PLAN_PUSH, 0, 0 }, { "push_Iz", PLAN_PUSH, 0, 0 }, { "push_Ibs", PLAN_PUSH, 0, 0 },
        { "push_Ev", PLAN_PUSH_RM, 0, 0 }, { "pop_Zv", PLAN_POP, 0, 0 }, { "leave", PLAN_LEAVE, 0, 0 },
        { "movsb", PLAN_MOVS, 0, 1 }, { "movsw", PLAN_MOVS, 0, 2 }, { "movsd", PLAN_MOVS, 0, 4 },
        { "movsq", PLAN_MOVS, 0, 8 }, { "stosb", PLAN_STOS, 0, 1 }, { "stosw", PLAN_STOS, 0, 2 },
        { "stosd", PLAN_STOS, 0, 4 }, { "stosq", PLAN_STOS, 0, 8 },
    };
    const PwIa32Form *form = inst->form;
    const unsigned operand = form->operands[0];
    if ((form->attributes & PW_IA32_ATTR_BRANCH) && (operand == PW_IA32_OP_Jb || operand == PW_IA32_OP_Jz) &&
        ((inst->map == PW_IA32_MAP_PRI && (inst->opcode & 0xf0) == 0x70) ||
         (inst->map == PW_IA32_MAP_0F && (inst->opcode & 0xf0) == 0x80))) {
        p->reg = inst->opcode & 15;
        p->imm = signed_imm(inst);
        return PLAN_JCC;
    }
    for (unsigned i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        if (!same_name(form->name, rows[i].name)) continue;
        p->variant = rows[i].variant;
        p->size = rows[i].size ? rows[i].size : (uint8_t)(inst->operand_size / 8);
        p->reg = NONE;
        p->imm = signed_imm(inst);
        if (rows[i].kind == PLAN_RET) p->imm = (int64_t)(inst->imm & 0xffff);
        if (rows[i].kind == PLAN_PUSH || rows[i].kind == PLAN_POP)
            p->reg = operand == PW_IA32_OP_Zv ? (uint8_t)((inst->opcode & 7) | (inst->rex & 1) << 3) : NONE;
        if (rows[i].kind == PLAN_JMP_RM || rows[i].kind == PLAN_CALL_RM || rows[i].kind == PLAN_PUSH_RM)
            p->reg = inst->rm;
        if (rows[i].kind == PLAN_MOVS || rows[i].kind == PLAN_STOS) {
            if (inst->has_67) return PLAN_FORM;   /* 32-bit RSI/RDI */
            p->variant = inst->rep != 0;          /* F2 repeats MOVS/STOS as F3 does */
        }
        if (rows[i].kind == PLAN_LOOP && inst->has_67) return PLAN_FORM;   /* ECX: what its upper half gets is unsaid */
        if (rows[i].kind == PLAN_JMP_RM || rows[i].kind == PLAN_CALL_RM) p->size = 8;
        return rows[i].kind;
    }
    return 0;
}

/* The slot's encoding of a rewritten instruction (section 7): legacy prefixes but 67 and REX, a new REX (or VEX3) with
 * the memory operand as [scratch] (mod 00, no SIB, no displacement) and RSP's operand as scratch2, the immediates,
 * then ret. Its length without the ret. */
static unsigned encode(const PwIa32Inst *inst, const uint8_t *code, uint8_t *out, int memory, unsigned scratch,
                       unsigned scratch2, int rsp_reg, int rsp_rm, int rsp_v)
{
    const unsigned reg = rsp_reg ? scratch2 : inst->reg;
    const unsigned rm = memory ? scratch : rsp_rm ? scratch2 : inst->rm;
    unsigned n = 0;
    for (unsigned i = 0; i < inst->prefixes; i++)
        if ((code[i] & 0xf0) != 0x40 && code[i] != 0x67) out[n++] = code[i];
    if (inst->encoding == PW_IA32_ENC_LEGACY) {
        out[n++] = (uint8_t)(0x40 | (inst->rex & 8) | (reg >> 3) << 2 | rm >> 3);
        for (unsigned i = inst->prefixes; i < inst->modrm_at; i++) out[n++] = code[i];
    } else {
        const unsigned vvvv = rsp_v ? scratch2 : inst->vex_vvvv;
        out[n++] = 0xc4;
        out[n++] = (uint8_t)((~reg >> 3 & 1) << 7 | 1u << 6 | (~rm >> 3 & 1) << 5 | inst->map);
        out[n++] = (uint8_t)(inst->vex_w << 7 | (~vvvv & 15) << 3 | inst->vex_l << 2 | inst->vex_pp);
        out[n++] = inst->opcode;
    }
    out[n++] = (uint8_t)((memory ? 0 : 3u << 6) | (reg & 7) << 3 | (rm & 7));
    for (unsigned i = inst->length - inst->imm_size - inst->imm2_size; i < inst->length; i++) out[n++] = code[i];
    out[n] = 0xc3;
    return n;
}

/* The plan for a decoded instruction with a form; PLAN_FORM when the engine does not run it. */
static uint8_t plan_kind(Plan *p, const PwIa32Inst *inst, uint8_t *slot)
{
    const PwIa32Form *form = inst->form;
    const uint32_t a = form->attributes;
    if (inst->segment == PW_IA32_SEG_FS || inst->segment == PW_IA32_SEG_GS) return PLAN_FORM;
    p->memory = inst->has_modrm && inst->mod != 3;
    /* shortcut: the CPU adds the register's bit offset to the address, past [EA, +size); a run ends at them. Offset
     * the classification by the register if the stepfall top shows them. BT (0F A3) only reads, so it stays. */
    if (p->memory && inst->map == PW_IA32_MAP_0F &&
        (inst->opcode == 0xab || inst->opcode == 0xb3 || inst->opcode == 0xbb))
        return PLAN_FORM;
    p->base = inst->base;
    p->index = inst->index;
    p->scale = inst->scale;
    p->disp = inst->disp;
    p->addr32 = inst->address_size == 32;
    p->rip_relative = inst->rip_relative;
    const uint8_t kind = emulated(p, inst);
    if (kind) return kind;
    if (a & (PW_IA32_ATTR_BRANCH | PW_IA32_ATTR_STACK | PW_IA32_ATTR_PRIV | PW_IA32_ATTR_UD)) return PLAN_FORM;
    if ((a & PW_IA32_ATTR_IMPL) && !(inst->map == PW_IA32_MAP_PRI && inst->opcode == 0xd7)) return PLAN_FORM; /* XLAT */
    PwIa32Access access = { PW_IA32_ACCESS_NONE, 0, 0 };
    if (!(a & (PW_IA32_ATTR_IMPL | PW_IA32_ATTR_STR)) && pw_ia32_access(inst, &access) != PW_OK) return PLAN_FORM;
    /* General registers it names, and which of them is RSP (SPL with a REX; without one, 4 is AH in a byte operand). */
    const int rex = inst->rex || inst->encoding != PW_IA32_ENC_LEGACY;
    unsigned named = 0;
    int rsp_reg = 0, rsp_rm = 0, rsp_v = 0, high_byte = 0;
    for (unsigned i = 0; i < 3; i++) {
        const unsigned o = form->operands[i];
        if (is_g(o)) {
            named |= 1u << inst->reg;
            if (o == PW_IA32_OP_Gb && !rex && inst->reg >= 4) high_byte = 1;
            else if (inst->reg == RSP) rsp_reg = 1;
        }
        if (is_e(o) && inst->has_modrm && inst->mod == 3) {
            named |= 1u << inst->rm;
            if (o == PW_IA32_OP_Eb && !rex && inst->rm >= 4) high_byte = 1;
            else if (inst->rm == RSP) rsp_rm = 1;
        }
        if (o == PW_IA32_OP_By) {
            named |= 1u << inst->vex_vvvv;
            rsp_v |= inst->vex_vvvv == RSP;
        }
        if ((o == PW_IA32_OP_Zb || o == PW_IA32_OP_Zv) && ((inst->opcode & 7) | (inst->rex & 1) << 3) == RSP &&
            (o == PW_IA32_OP_Zv || rex))
            return PLAN_FORM;   /* RSP in the opcode: XCHG, BSWAP, MOV imm */
    }
    if (p->memory) {
        if (inst->base != PW_IA32_NO_REG) named |= 1u << inst->base;
        if (inst->index != PW_IA32_NO_REG) named |= 1u << inst->index;
    }
    const int memory = access.kind != PW_IA32_ACCESS_NONE, rsp = rsp_reg || rsp_rm || rsp_v;
    if (!memory && !rsp) {
        if (inst->length > PW_WINE_MMU4K_SLOT - 1) return PLAN_FORM;
        copy_bytes(slot, p->code, inst->length);
        slot[inst->length] = 0xc3;
        return PLAN_VERBATIM;
    }
    if (high_byte && inst->encoding == PW_IA32_ENC_LEGACY && !inst->rex) return PLAN_FORM; /* AH-BH cannot take a REX */
    static const uint8_t pool[] = { 8, 9, 10, 11, 14, 15 };   /* r12 needs a SIB, r13 a displacement */
    p->scratch = p->scratch2 = NONE;
    for (unsigned i = 0; i < sizeof(pool); i++) {
        if (named & 1u << pool[i]) continue;
        if (memory && p->scratch == NONE) p->scratch = pool[i];
        else if (rsp && p->scratch2 == NONE) p->scratch2 = pool[i];
    }
    if ((memory && p->scratch == NONE) || (rsp && p->scratch2 == NONE)) return PLAN_FORM;
    uint8_t out[PW_WINE_MMU4K_SLOT];
    const unsigned n = encode(inst, p->code, out, memory, p->scratch, p->scratch2, rsp_reg, rsp_rm, rsp_v);
    if (n > PW_IA32_MAX_LENGTH) return PLAN_FORM;
    copy_bytes(slot, out, n + 1);
    p->access = access.kind;
    p->exact = access.exact;
    p->size = (uint8_t)access.bytes;
    return PLAN_MEMORY;
}

typedef struct DecodeCall { Plan *plan; const uint8_t *bytes; size_t available; uint64_t rip; uint8_t *slot; } DecodeCall;
/* Inside pw_wine_mmu4k_call: the decoder is built like the rest of ntdll. A truncated encoding leaves the plan empty. */
static void decode_call(void *argument)
{
    DecodeCall *d = argument;
    Plan *p = d->plan;
    PwIa32Inst inst;
    const int rc = pw_ia32_decode_mode(d->bytes, d->available, PW_IA32_MODE_64, &inst);
    clear_words(p, sizeof(*p) / 8);
    p->rip = d->rip;
    p->op = (uint16_t)(inst.map << 8 | inst.opcode);
    p->scratch = p->scratch2 = p->reg = NONE;
    if (rc == PW_ERR_TRUNCATED || !inst.length) return;
    p->length = inst.length;
    copy_bytes(p->code, d->bytes, inst.length);
    p->kind = rc == PW_OK ? plan_kind(p, &inst, d->slot) : PLAN_FORM;
}

/* ---------------------------------------------------------------- classification (section 5) */
/* host_page's entry in the run's table, classified on a miss; NULL when it is not Wine's. A result of a newer alias
 * generation empties the table first, so no window given back since is ever used. */
static const PwWineMmu4kPage *page_of(PwWineMmu4kRun *run, uintptr_t host_page)
{
    for (uint32_t i = 0; i < run->held; i++)
        if (run->pages[i].host_page == host_page) return &run->pages[i];
    PwWineMmu4kPage page;
    clear_words(&page, sizeof(page) / 8);
    if (op(run, OP_CLASSIFY, host_page, &page)) return NULL;
    page.host_page = host_page;
    if (page.generation != run->generation) {
        run->held = 0;
        run->generation = page.generation;
    }
    const uint32_t at = run->held < PW_WINE_MMU4K_PAGES ? run->held++ : run->next++ % PW_WINE_MMU4K_PAGES;
    run->pages[at] = page;
    return &run->pages[at];
}

/* Where a store of [a, +n) goes: -1 with *target (the program's address, or the alias with *alias 1), or the exit
 * that ends the run before it (protected; straddle with run->exit_address; noalias). */
static int classify_write(PwWineMmu4kRun *run, uint64_t a, uint64_t n, int exact, uint64_t *target, int *alias)
{
    const uint64_t first = a & ~(uint64_t)(PAGE - 1), last = (a + n - 1) & ~(uint64_t)(PAGE - 1);
    for (int attempt = 0; attempt < 2; attempt++) {
        const uint64_t generation = run->generation;
        int kernel = 0, mixed = 0, contiguous = 1;
        intptr_t delta = 0;
        for (uint64_t h = a & ~(uint64_t)(HOST - 1); h <= last; h += HOST) {
            const PwWineMmu4kPage *page = page_of(run, (uintptr_t)h);
            if (!page) return PW_WINE_MMU4K_EXIT_PROTECTED;
            for (uint64_t q = h > first ? h : first; q < h + HOST && q <= last; q += PAGE) {
                if (page->plain & 1u << ((q - h) / PAGE)) continue;
                if (q == first || !exact) return PW_WINE_MMU4K_EXIT_PROTECTED;
                run->exit_address = (uintptr_t)q;
                return PW_WINE_MMU4K_EXIT_STRADDLE;
            }
            if (page->kernel_write) {
                kernel = 1;
                continue;
            }
            if (!page->alias_delta || (mixed && page->alias_delta != delta)) contiguous = 0;
            mixed = 1;
            delta = page->alias_delta;
        }
        if (run->generation != generation) continue;   /* a window went back while classifying: ask again */
        if (!mixed) {
            *target = a;
            *alias = 0;
            return -1;
        }
        if (kernel || !contiguous) return PW_WINE_MMU4K_EXIT_NOALIAS;
        *target = a + (uint64_t)delta;
        *alias = 1;
        return -1;
    }
    return PW_WINE_MMU4K_EXIT_NOALIAS;
}

/* ---------------------------------------------------------------- execution (section 6) */
static uint64_t effective(const PwWineMmu4kRun *run, const Plan *p)
{
    uint64_t a = (uint64_t)(int64_t)p->disp;
    if (p->rip_relative) a += run->regs.rip + p->length;
    if (p->base != PW_IA32_NO_REG) a += run->regs.gpr[p->base];
    if (p->index != PW_IA32_NO_REG) a += run->regs.gpr[p->index] * p->scale;
    return p->addr32 ? (uint32_t)a : a;
}

static int run_slot(PwWineMmu4kRun *run, const Plan *p, uint64_t target)
{
    uint64_t *gpr = run->regs.gpr, saved = 0, saved2 = 0;
    if (p->scratch != NONE) {
        saved = gpr[p->scratch];
        gpr[p->scratch] = target;
    }
    if (p->scratch2 != NONE) {
        saved2 = gpr[p->scratch2];
        gpr[p->scratch2] = gpr[RSP];
    }
    const int fault = pw_wine_mmu4k_exec(run, slot_memory + (size_t)(p - plans) * PW_WINE_MMU4K_SLOT);
    if (p->scratch2 != NONE) {
        if (!fault) gpr[RSP] = gpr[p->scratch2];
        gpr[p->scratch2] = saved2;
    }
    if (p->scratch != NONE) gpr[p->scratch] = saved;
    return fault;
}

static void retire(PwWineMmu4kRun *run, int alias)
{
    run->steps++;
    run->progress = 1;
    run->idle = alias ? 0 : run->idle + 1;
}

/* A push of size bytes: -1 done, or the exit before it. */
static int push(PwWineMmu4kRun *run, uint64_t value, unsigned size, int *alias)
{
    const uint64_t at = run->regs.gpr[RSP] - size;
    uint64_t target;
    const int e = classify_write(run, at, size, 1, &target, alias);
    if (e >= 0) return e;
    if (pw_wine_mmu4k_write((void *)target, &value, size)) return PW_WINE_MMU4K_EXIT_FAULT;
    run->regs.gpr[RSP] = at;
    return -1;
}

static void set_sized(uint64_t *reg, uint64_t value, unsigned size)
{
    *reg = size == 8 ? value : (*reg & ~(uint64_t)0xffff) | (value & 0xffff);
}

static int taken(uint64_t f, unsigned cc)
{
    const int cf = f & 1, pf = f >> 2 & 1, zf = f >> 6 & 1, sf = f >> 7 & 1, of = f >> 11 & 1;
    int t;
    switch (cc >> 1) {
    case 0: t = of; break;
    case 1: t = cf; break;
    case 2: t = zf; break;
    case 3: t = cf | zf; break;
    case 4: t = sf; break;
    case 5: t = pf; break;
    case 6: t = sf != of; break;
    default: t = zf | (sf != of); break;
    }
    return t ^ (cc & 1);
}

/* One chunk of MOVS/STOS: elements up to the next 4 KiB boundary of either operand (at least one), stored by class.
 * -1, or the exit. */
static int string(PwWineMmu4kRun *run, const Plan *p)
{
    uint64_t *gpr = run->regs.gpr;
    const unsigned e = p->size;
    const int movs = p->kind == PLAN_MOVS, rep = p->variant;
    if (run->regs.rflags & FLAG_DF) return PW_WINE_MMU4K_EXIT_FORM;
    if (rep && !gpr[RCX]) {
        run->regs.rip += p->length;
        retire(run, 0);
        return -1;
    }
    uint64_t chunk = (PAGE - (gpr[RDI] & (PAGE - 1))) / e;
    if (movs) {
        const uint64_t room = (PAGE - (gpr[RSI] & (PAGE - 1))) / e;
        if (room < chunk) chunk = room;
    }
    if (!chunk) chunk = 1;
    if (!rep) chunk = 1;
    else if (gpr[RCX] < chunk) chunk = gpr[RCX];
    uint64_t target;
    int alias = 0;
    const int exit = classify_write(run, gpr[RDI], chunk * e, 1, &target, &alias);
    if (exit >= 0) return exit;
    uint64_t source = movs ? gpr[RSI] : gpr[RAX];
    if (movs && alias && gpr[RDI] > gpr[RSI] && gpr[RDI] - gpr[RSI] < chunk * e) {
        /* A forward copy onto its own source: the CPU's fast strings judge overlap by address, so the source goes
         * through the same alias (on the host, 689 of 3,239 such copies came out wrong otherwise, 2026-10-15). */
        if ((gpr[RSI] ^ gpr[RDI]) & ~(uint64_t)(HOST - 1)) return PW_WINE_MMU4K_EXIT_FORM;
        source = target - (gpr[RDI] - gpr[RSI]);
    }
    uint64_t done = chunk;
    const int fault = pw_wine_mmu4k_copy((void *)target, source, chunk, e, !movs);
    if (fault) done = chunk - run->fault_rcx;
    gpr[RDI] += done * e;
    if (movs) gpr[RSI] += done * e;
    if (rep) gpr[RCX] -= done;
    if (done) run->progress = 1;
    if (fault) return PW_WINE_MMU4K_EXIT_FAULT;
    if (!rep || !gpr[RCX]) run->regs.rip += p->length;
    retire(run, alias);
    return -1;
}

/* A branch to a non-canonical target faults (#GP) at the branch, before it pushes or pops: the native retry raises it.
 * A relative branch cannot reach one from user code. */
static int canonical(uint64_t a) { return (uint64_t)((int64_t)(a << 16) >> 16) == a; }

/* One instruction (or one chunk of a REP string): -1, or the exit before it. */
static int step(PwWineMmu4kRun *run, const Plan *p)
{
    PwWineMmu4kRegs *r = &run->regs;
    const uint64_t next = r->rip + p->length;
    uint64_t target = 0, value = 0;
    int alias = 0, e;
    switch (p->kind) {
    case PLAN_VERBATIM:
        if (run_slot(run, p, 0)) return PW_WINE_MMU4K_EXIT_FAULT;
        r->rip = next;
        break;
    case PLAN_MEMORY:
        target = effective(run, p);
        if ((p->access == PW_IA32_ACCESS_WRITE || p->access == PW_IA32_ACCESS_RW) &&
            (e = classify_write(run, target, p->size, p->exact, &target, &alias)) >= 0)
            return e;
        if (run_slot(run, p, target)) return PW_WINE_MMU4K_EXIT_FAULT;
        r->rip = next;
        break;
    case PLAN_JMP: r->rip = next + (uint64_t)p->imm; break;
    case PLAN_JCC: r->rip = taken(r->rflags, p->reg) ? next + (uint64_t)p->imm : next; break;
    case PLAN_JMP_RM:
    case PLAN_CALL_RM:
        if (!p->memory) value = r->gpr[p->reg];
        else if (pw_wine_mmu4k_read(&value, (const void *)effective(run, p), 8)) return PW_WINE_MMU4K_EXIT_FAULT;
        if (!canonical(value)) return PW_WINE_MMU4K_EXIT_FORM;
        if (p->kind == PLAN_CALL_RM && (e = push(run, next, 8, &alias)) >= 0) return e;
        r->rip = value;
        break;
    case PLAN_CALL:
        if ((e = push(run, next, 8, &alias)) >= 0) return e;
        r->rip = next + (uint64_t)p->imm;
        break;
    case PLAN_RET:
        if (pw_wine_mmu4k_read(&value, (const void *)r->gpr[RSP], 8)) return PW_WINE_MMU4K_EXIT_FAULT;
        if (!canonical(value)) return PW_WINE_MMU4K_EXIT_FORM;
        r->gpr[RSP] += 8 + (uint64_t)p->imm;
        r->rip = value;
        break;
    case PLAN_LOOP: {
        const uint64_t c = --r->gpr[RCX];
        const int zf = (r->rflags & FLAG_ZF) != 0;
        r->rip = c && (p->variant == 0 || (p->variant == 1) == zf) ? next + (uint64_t)p->imm : next;
        break;
    }
    case PLAN_JRCXZ:
        r->rip = !(p->variant ? (uint32_t)r->gpr[RCX] : r->gpr[RCX]) ? next + (uint64_t)p->imm : next;
        break;
    case PLAN_PUSH:
    case PLAN_PUSH_RM:
        if (p->kind == PLAN_PUSH) value = p->reg == NONE ? (uint64_t)p->imm : r->gpr[p->reg];
        else if (!p->memory) value = r->gpr[p->reg];
        else if (pw_wine_mmu4k_read(&value, (const void *)effective(run, p), p->size))
            return PW_WINE_MMU4K_EXIT_FAULT;
        if ((e = push(run, value, p->size, &alias)) >= 0) return e;
        r->rip = next;
        break;
    case PLAN_POP:
        if (pw_wine_mmu4k_read(&value, (const void *)r->gpr[RSP], p->size)) return PW_WINE_MMU4K_EXIT_FAULT;
        r->gpr[RSP] += p->size;
        set_sized(&r->gpr[p->reg], value, p->size);
        r->rip = next;
        break;
    case PLAN_LEAVE:
        if (pw_wine_mmu4k_read(&value, (const void *)r->gpr[RBP], p->size)) return PW_WINE_MMU4K_EXIT_FAULT;
        r->gpr[RSP] = r->gpr[RBP] + p->size;
        set_sized(&r->gpr[RBP], value, p->size);
        r->rip = next;
        break;
    case PLAN_MOVS:
    case PLAN_STOS:
        return string(run, p);
    default:
        return PW_WINE_MMU4K_EXIT_FORM;
    }
    retire(run, alias);
    return -1;
}

/* Whether the 4 KiB page holding at may be executed (its host page's exec bit; not Wine's: no). */
static int executable(PwWineMmu4kRun *run, uint64_t at)
{
    const PwWineMmu4kPage *page = page_of(run, (uintptr_t)(at & ~(uint64_t)(HOST - 1)));
    return page && (page->exec >> ((at & (HOST - 1)) / PAGE) & 1);
}

/* The plan for the instruction at RIP, decoded on a miss: -1 with *plan, or the exit (fetch, form). Code in a page that
 * may not be executed ends the run (fetch): the native retry takes the execute fault there. */
static int fetch(PwWineMmu4kRun *run, const Plan **plan)
{
    uint8_t bytes[16];
    const uint64_t rip = run->regs.rip;
    size_t n = PW_IA32_MAX_LENGTH;
    if (!executable(run, rip)) return PW_WINE_MMU4K_EXIT_FETCH;
    if (pw_wine_mmu4k_read(bytes, (const void *)rip, n)) {
        n = PAGE - (rip & (PAGE - 1));
        if (n >= PW_IA32_MAX_LENGTH || pw_wine_mmu4k_read(bytes, (const void *)rip, n))
            return PW_WINE_MMU4K_EXIT_FETCH;
    }
    Plan *p = &plans[(rip ^ rip >> 11) & (SLOTS - 1)];
    if (p->kind == PLAN_EMPTY || p->rip != rip || p->length > n || !same_bytes(p->code, bytes, p->length)) {
        DecodeCall call = { p, bytes, n, rip, slot_memory + (size_t)(p - plans) * PW_WINE_MMU4K_SLOT };
        pw_wine_mmu4k_call(decode_call, &call, run->xsave, xsave_mask);
    }
    if (!run->steps) run->first_op = p->op;
    if (p->kind == PLAN_EMPTY) return PW_WINE_MMU4K_EXIT_FETCH;
    const uint64_t last = rip + p->length - 1;
    if (((last ^ rip) & ~(uint64_t)(PAGE - 1)) && !executable(run, last)) return PW_WINE_MMU4K_EXIT_FETCH;
    if (p->kind == PLAN_FORM) return PW_WINE_MMU4K_EXIT_FORM;
    *plan = p;
    return -1;
}

/* Space-saving top of the instructions that fell back to the step. Under the mutex. */
static void top_note(uint16_t op16)
{
    unsigned at = 4, low = 0;
    for (unsigned i = 0; i < 4 && at == 4; i++)
        if (counters.top_count[i] && counters.top[i] == op16) at = i;
    for (unsigned i = 0; i < 4 && at == 4; i++) {
        if (!counters.top_count[i]) at = i;
        else if (counters.top_count[i] < counters.top_count[low]) low = i;
    }
    if (at == 4) at = low;   /* the least counted gives its place, keeping its count (space-saving) */
    __atomic_store_n(&counters.top[at], op16, __ATOMIC_RELAXED);
    count(&counters.top_count[at], 1);
}

void pw_wine_mmu4k_run(PwWineMmu4kRun *run)
{
    int exit = -1;
    op(run, OP_LOCK, 0, 0);
    while (exit < 0) {
        if (run->steps >= PW_WINE_MMU4K_BUDGET) {
            exit = PW_WINE_MMU4K_EXIT_BUDGET;
            break;
        }
        if (run->idle >= PW_WINE_MMU4K_IDLE) {
            exit = PW_WINE_MMU4K_EXIT_IDLE;
            break;
        }
        /* An unlock and relock here handed nothing over (host: 0 of 50): a thread waits, so the run ends instead. */
        if (run->steps && !(run->steps % PW_WINE_MMU4K_YIELD) && engine_ops.waiters &&
            __atomic_load_n(engine_ops.waiters, __ATOMIC_RELAXED)) {
            exit = PW_WINE_MMU4K_EXIT_YIELD;
            break;
        }
        const Plan *p = 0;
        exit = fetch(run, &p);
        if (exit < 0) exit = step(run, p);
    }
    run->exit = exit;
    if (run->exit == PW_WINE_MMU4K_EXIT_STRADDLE) run->action = PW_WINE_MMU4K_FAULT;
    else if (run->progress) run->action = PW_WINE_MMU4K_RESUME;
    else {
        run->action = PW_WINE_MMU4K_STEP;
        count(&counters.stepfall, 1);
        top_note(run->first_op);
    }
    count(&counters.runs, 1);
    count(&counters.insns, run->steps);
    count(&counters.exits[run->exit], 1);
    op(run, OP_UNLOCK, 0, 0);
}

/* ---------------------------------------------------------------- the glue's side */
int pw_wine_mmu4k_init(const PwWineMmu4kOps *ops, void *slots, size_t slot_bytes, uint64_t mask)
{
    if (!ops || !ops->classify || !ops->lock || !ops->unlock || !slots || slot_bytes < PW_WINE_MMU4K_SLOT_BYTES ||
        (mask & 3) != 3 || (mask & ~(uint64_t)7))
        return PW_ERR_PRECONDITION;
    if (__atomic_load_n(&ready, __ATOMIC_ACQUIRE)) return PW_ERR_STATE;
    engine_ops.context = ops->context;
    engine_ops.classify = ops->classify;
    engine_ops.lock = ops->lock;
    engine_ops.unlock = ops->unlock;
    engine_ops.waiters = ops->waiters;
    slot_memory = slots;
    xsave_mask = mask;
    for (unsigned i = 0; i < PW_WINE_MMU4K_STACKS; i++)
        for (unsigned w = 0; w < 8; w++) ((uint64_t *)stacks[i])[w] = STACK_PATTERN;
    __atomic_store_n(&ready, 1, __ATOMIC_RELEASE);
    return PW_OK;
}

int pw_wine_mmu4k_ready(void) { return __atomic_load_n(&ready, __ATOMIC_ACQUIRE); }

int pw_wine_mmu4k_stack_take(void)
{
    uint32_t used = __atomic_load_n(&stacks_used, __ATOMIC_RELAXED);
    for (;;) {
        if (used == (1u << PW_WINE_MMU4K_STACKS) - 1) {
            count(&counters.stack_full, 1);
            return -1;
        }
        const int i = __builtin_ctz(~used);
        if (__atomic_compare_exchange_n(&stacks_used, &used, used | 1u << i, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            return i;
    }
}

void pw_wine_mmu4k_stack_give(int stack)
{
    if (stack >= 0 && stack < PW_WINE_MMU4K_STACKS) __atomic_and_fetch(&stacks_used, ~(1u << stack), __ATOMIC_RELEASE);
}

static PwWineMmu4kRun *run_of(int stack)
{
    return (PwWineMmu4kRun *)((uintptr_t)(stacks[stack] + PW_WINE_MMU4K_STACK - sizeof(PwWineMmu4kRun)) &
                              ~(uintptr_t)63);
}

PwWineMmu4kRun *pw_wine_mmu4k_enter(int stack, PwWineMmu4kRegs *regs, uintptr_t host_page, uintptr_t address)
{
    if (!pw_wine_mmu4k_ready() || stack < 0 || stack >= PW_WINE_MMU4K_STACKS || !regs ||
        !(__atomic_load_n(&stacks_used, __ATOMIC_RELAXED) & 1u << stack))
        return (PwWineMmu4kRun *)0;
    PwWineMmu4kRun *run = run_of(stack);
    clear_words(run, sizeof(*run) / 8);   /* the xsave header among it: XRSTOR wants its reserved bytes 0 */
    copy_bytes(&run->regs, regs, sizeof(*regs));
    run->stack = stack;
    run->host_page = host_page;
    run->address = address;
    run->exit = -1;
    regs->rip = (uintptr_t)pw_wine_mmu4k_start;
    regs->gpr[RSP] = (uintptr_t)run;   /* 64-byte aligned: the start stub's call sees an aligned stack */
    regs->gpr[RDI] = (uintptr_t)run;
    regs->rflags = ENGINE_FLAGS;
    return run;
}

int pw_wine_mmu4k_signal(PwWineMmu4kRun *run, int signal, uintptr_t fault_address, PwWineMmu4kRegs *regs,
                         uintptr_t *address)
{
    if (!run || !regs || !slot_memory) return PW_WINE_MMU4K_NOT_OURS;
    const uintptr_t rip = regs->rip;
    if (rip == (uintptr_t)pw_wine_mmu4k_exit_trap) {
        if (signal != SIGNAL_ILL || !address || run->exit < 0) return PW_WINE_MMU4K_NOT_OURS;
        uint64_t *bottom = (uint64_t *)stacks[run->stack];
        for (unsigned w = 0; w < 8; w++)
            if (bottom[w] != STACK_PATTERN) {
                count(&counters.stack_damage, 1);
                for (unsigned v = 0; v < 8; v++) bottom[v] = STACK_PATTERN;
                break;
            }
        copy_bytes(regs, &run->regs, sizeof(*regs));
        *address = run->action == PW_WINE_MMU4K_FAULT ? run->exit_address : run->address;
        const int action = run->action;
        pw_wine_mmu4k_stack_give(run->stack);
        return action;
    }
    const int in_slot = rip >= (uintptr_t)slot_memory && rip < (uintptr_t)slot_memory + PW_WINE_MMU4K_SLOT_BYTES;
    if (!in_slot && (rip < (uintptr_t)pw_wine_mmu4k_guarded_begin || rip >= (uintptr_t)pw_wine_mmu4k_guarded_end))
        return PW_WINE_MMU4K_NOT_OURS;
    run->fault_signal = signal;
    run->fault_address = fault_address;
    run->fault_rcx = regs->gpr[RCX];
    run->fault_rsi = regs->gpr[RSI];
    run->fault_rdi = regs->gpr[RDI];
    if (in_slot) {
        regs->rip = (uintptr_t)pw_wine_mmu4k_recover;
        regs->gpr[RSP] = run->host_rsp;
    } else {
        regs->rip = (uintptr_t)pw_wine_mmu4k_guarded_fail;   /* a leaf: its RSP holds the return address */
    }
    regs->rflags = ENGINE_FLAGS;
    return PW_WINE_MMU4K_RECOVER;
}

void pw_wine_mmu4k_stats(PwWineMmu4kStats *stats)
{
    if (!stats) return;
    stats->runs = __atomic_load_n(&counters.runs, __ATOMIC_RELAXED);
    stats->insns = __atomic_load_n(&counters.insns, __ATOMIC_RELAXED);
    for (unsigned i = 0; i < PW_WINE_MMU4K_EXITS; i++)
        stats->exits[i] = __atomic_load_n(&counters.exits[i], __ATOMIC_RELAXED);
    stats->stepfall = __atomic_load_n(&counters.stepfall, __ATOMIC_RELAXED);
    stats->stack_full = __atomic_load_n(&counters.stack_full, __ATOMIC_RELAXED);
    stats->stack_damage = __atomic_load_n(&counters.stack_damage, __ATOMIC_RELAXED);
    for (unsigned i = 0; i < 4; i++) {
        stats->top[i] = __atomic_load_n(&counters.top[i], __ATOMIC_RELAXED);
        stats->top_count[i] = __atomic_load_n(&counters.top_count[i], __ATOMIC_RELAXED);
    }
}

void pw_wine_mmu4k_reset(void)
{
    __atomic_store_n(&ready, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&stacks_used, 0, __ATOMIC_RELEASE);
    clear_words(plans, sizeof(plans) / 8);
    clear_words(&counters, sizeof(counters) / 8);
    slot_memory = 0;
}
