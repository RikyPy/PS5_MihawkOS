/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * The decoder against the real CPU (docs/IA32_DESIGN.md: "where LLVM and the CPU disagree, the CPU
 * decides"). Built with -m32 by tools/ia32-cpu-decode-check.sh, with src/pw_ia32_decode.c.
 *
 * Corpus: every opcode of the four maps × 12 prefix sets × every ModRM × 2 SIB bytes, followed by
 * zero bytes (so a displacement is 0 and memory operands land in the data window the registers
 * point into). Each encoding is decoded by us and run single-stepped on the CPU in forked workers.
 *
 * Disagreements, grouped by kind, map, opcode, ModRM.reg/mod and prefix set:
 *   LENGTH        the CPU completed a non-branch instruction of another length than ours
 *   CPU_EXECUTES  the CPU executed what we call undefined (MALFORMED)
 *   CPU_UD        we decode a form and the CPU raised #UD (UD2, whose row says UD, is expected;
 *                 a feature this CPU lacks shows here, reported, never silently dropped)
 *   LOCK_UD       LOCK on a form the table calls lockable, with a memory destination: the CPU said #UD
 *   LOCK_RUNS     LOCK where the table says it is #UD (not lockable, or a register destination): it ran
 *   NOFORM_RUNS   the CPU executed a well-formed encoding we have no form for (coverage)
 *   HANG          a worker went silent for 5 s on it (killed; the sweep resumes after it)
 * A fault other than #UD (#GP, #PF, #DE) says nothing about length and is only counted.
 *
 * Output: argv[1] gets one JSON line per group; stdout one JSON summary. Exit status 0.
 */
#include "ia32_cpu_core.h"
#include "../src/pw_ia32_decode.h"
#include <poll.h>
#include <stdlib.h>
#include <sys/wait.h>

enum { KIND_LENGTH, KIND_CPU_EXECUTES, KIND_CPU_UD, KIND_LOCK_UD, KIND_LOCK_RUNS, KIND_NOFORM_RUNS, KIND_HANG, KINDS,
       WORKER_BATCH = 4096, SILENCE_MS = 5000 };
static const char *const kind_names[KINDS] = { "LENGTH", "CPU_EXECUTES", "CPU_UD", "LOCK_UD", "LOCK_RUNS", "NOFORM_RUNS",
                                               "HANG" };

typedef struct Encoding {
    uint8_t bytes[16], length, set;
} Encoding;

/* What a worker sends back for one encoding. */
typedef struct Record {
    uint32_t index;
    int8_t kind;            /* -1: agreement or uninformative fault */
    int8_t ours_result, ours_length, cpu_signal;
    int16_t cpu_si_code, cpu_delta;
    uint8_t map, opcode, reg, mod3;
} Record;

static const uint8_t prefix_sets[][3] = {
    { 0 }, { 0x66 }, { 0x67 }, { 0xf3 }, { 0xf2 }, { 0xf0 }, { 0x64 }, { 0x26 },
    { 0x66, 0x67 }, { 0x66, 0xf3 }, { 0xf2, 0x66 }, { 0x2e },
};
static const uint8_t prefix_lengths[] = { 0, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 1 };

static Encoding *corpus;
static size_t corpus_count, corpus_capacity;

static void corpus_add(const uint8_t *bytes, size_t n, unsigned set)
{
    if (corpus_count == corpus_capacity) {
        corpus_capacity = corpus_capacity ? corpus_capacity * 2 : 1 << 20;
        corpus = realloc(corpus, corpus_capacity * sizeof(*corpus));
    }
    Encoding *e = &corpus[corpus_count++];
    memset(e, 0, sizeof(*e));
    memcpy(e->bytes, bytes, n);
    e->length = 15;           /* zeros follow: the decoder and the CPU both see 15 bytes */
    e->set = (uint8_t)set;
}

static void build_corpus(void)
{
    static const uint8_t escapes[4][2] = { { 0 }, { 0x0f }, { 0x0f, 0x38 }, { 0x0f, 0x3a } };
    static const uint8_t escape_lengths[4] = { 0, 1, 2, 2 };
    static const uint8_t sibs[] = { 0x00, 0x24 };
    for (unsigned map = 0; map < 4; map++)
        for (unsigned op = 0; op < 256; op++) {
            const char shape = pw_ia32_shape(map, op);
            if (map == 0 && (shape == 'P' || shape == '0')) continue;
            if (map == 1 && (shape == '8' || shape == 'A')) continue;
            const int modrm = strchr("mBZfF3QVvx", shape) != NULL;
            for (unsigned set = 0; set < sizeof(prefix_lengths); set++) {
                uint8_t b[16];
                size_t n = 0;
                int address16 = 0;
                for (unsigned i = 0; i < prefix_lengths[set]; i++) {
                    b[n++] = prefix_sets[set][i];
                    if (prefix_sets[set][i] == 0x67) address16 = 1;
                }
                memcpy(b + n, escapes[map], escape_lengths[map]);
                n += escape_lengths[map];
                b[n++] = (uint8_t)op;
                if (!modrm) { corpus_add(b, n, set); continue; }
                for (unsigned m = 0; m < 256; m++) {
                    b[n] = (uint8_t)m;
                    if (!address16 && (m >> 6) != 3 && (m & 7) == 4)
                        for (size_t s = 0; s < sizeof(sibs); s++) { b[n + 1] = sibs[s]; corpus_add(b, n + 2, set); }
                    else
                        corpus_add(b, n + 1, set);
                }
            }
        }
}

static int is_branch(const PwIa32Inst *inst)
{
    return inst->form && (inst->form->attributes & PW_IA32_ATTR_BRANCH);
}

static Record judge(uint32_t index)
{
    const Encoding *e = &corpus[index];
    PwIa32Inst inst;
    const int ours = pw_ia32_decode(e->bytes, e->length, &inst);
    CpuResult cpu;
    cpu_run(e->bytes, inst.length && ours != PW_ERR_MALFORMED ? inst.length : e->length, &cpu);
    Record r = { index, -1, (int8_t)ours, (int8_t)inst.length, (int8_t)cpu.signal, (int16_t)cpu.si_code,
                 (int16_t)cpu.delta, inst.map, inst.opcode, inst.has_modrm ? inst.reg : 8,
                 inst.has_modrm && inst.mod == 3 };
    if (cpu.refused) return r;
    /* Completed: the trap after it, or the trap it is (INT3, INT1), leaves EIP after the instruction.
     * A trace trap with EIP unmoved is one iteration of a REP string instruction: no length there. */
    if (cpu_completed(&cpu) && cpu.delta == 0) return r;
    const int completed = cpu_completed(&cpu) || (cpu.signal == SIGTRAP && cpu.delta > 0);
    const int legacy = inst.encoding == PW_IA32_ENC_LEGACY;
    if (completed && ours == PW_ERR_MALFORMED) r.kind = KIND_CPU_EXECUTES;
    else if (completed && legacy && (ours == PW_OK || ours == PW_ERR_UNSUPPORTED) && !is_branch(&inst) &&
             cpu.delta != inst.length) r.kind = KIND_LENGTH;
    else if (ours == PW_OK && inst.lock && (cpu.signal == SIGILL || completed)) {
        const int lockable = (inst.form->attributes & PW_IA32_ATTR_LOCK) && inst.has_modrm && inst.mod != 3;
        if (cpu.signal == SIGILL && lockable) r.kind = KIND_LOCK_UD;
        else if (completed && !lockable) r.kind = KIND_LOCK_RUNS;
    }
    else if (cpu.signal == SIGILL && ours == PW_OK && !(inst.form->attributes & PW_IA32_ATTR_UD)) r.kind = KIND_CPU_UD;
    else if (completed && ours == PW_ERR_UNSUPPORTED && legacy) r.kind = KIND_NOFORM_RUNS;
    return r;
}

/* Groups, as in tools/ia32_decode_oracle.c. */
typedef struct Group {
    uint32_t key, count;
    Record example;
} Group;
enum { GROUP_SLOTS = 1u << 17 };
static Group *groups;

static void group_add(const Record *r)
{
    const uint32_t key = 1u + ((((((uint32_t)r->kind * 4 + r->map) * 256u + r->opcode) * 9u + r->reg) * 2u + r->mod3) * 16u +
                               corpus[r->index].set);
    for (uint32_t i = (key * 2654435761u) & (GROUP_SLOTS - 1);; i = (i + 1) & (GROUP_SLOTS - 1)) {
        if (groups[i].key == key) { groups[i].count++; return; }
        if (!groups[i].key) { groups[i].key = key; groups[i].count = 1; groups[i].example = *r; return; }
    }
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <groups.jsonl>\n", argv[0]);
        return 2;
    }
    build_corpus();
    /* IA32_CHECK_LIMIT=N runs only the first N encodings; IA32_CHECK_TRACE=1 names each on stderr
     * before it runs (to find one that hangs a worker). */
    if (getenv("IA32_CHECK_LIMIT") && (size_t)atol(getenv("IA32_CHECK_LIMIT")) < corpus_count)
        corpus_count = (size_t)atol(getenv("IA32_CHECK_LIMIT"));
    const int trace = getenv("IA32_CHECK_TRACE") != NULL;
    if (getenv("IA32_CHECK_SHOW")) {    /* IA32_CHECK_SHOW=N prints encoding N and stops */
        const size_t n = (size_t)atol(getenv("IA32_CHECK_SHOW"));
        for (unsigned k = 0; n < corpus_count && k < 15; k++) printf("%02x ", corpus[n].bytes[k]);
        printf("\n");
        return 0;
    }
    groups = calloc(GROUP_SLOTS, sizeof(Group));
    uint64_t kinds[KINDS] = { 0 }, lost = 0, refused_count = 0;
    for (size_t first = 0; first < corpus_count;) {
        const size_t last = first + WORKER_BATCH < corpus_count ? first + WORKER_BATCH : corpus_count;
        int pipefd[2];
        if (pipe(pipefd)) return 2;
        const pid_t pid = fork();
        if (pid == 0) {
            close(pipefd[0]);
            cpu_worker_setup();
            for (size_t i = first; i < last; i++) {
                if (trace) fprintf(stderr, "run %zu\n", i);
                const Record r = judge((uint32_t)i);
                if (write(pipefd[1], &r, sizeof(r)) != (ssize_t)sizeof(r)) _exit(1);
            }
            _exit(0);
        }
        close(pipefd[1]);
        size_t received = 0;
        int silent = 0;
        for (;;) {
            struct pollfd p = { pipefd[0], POLLIN, 0 };
            if (poll(&p, 1, SILENCE_MS) == 0) { silent = 1; break; }
            Record r;
            if (read(pipefd[0], &r, sizeof(r)) != (ssize_t)sizeof(r)) break;
            received++;
            if (r.kind >= 0) { kinds[r.kind]++; group_add(&r); }
        }
        if (silent) kill(pid, SIGKILL);
        close(pipefd[0]);
        waitpid(pid, NULL, 0);
        size_t next = first + received;
        if (next < last) {           /* the worker hung or died on this one: report it, go on after it */
            Record hung = { (uint32_t)next, KIND_HANG, 0, 0, silent ? SIGKILL : 0, 0, 0, 0, 0, 0, 0 };
            PwIa32Inst inst;
            pw_ia32_decode(corpus[next].bytes, corpus[next].length, &inst);
            hung.map = inst.map; hung.opcode = inst.opcode; hung.reg = inst.has_modrm ? inst.reg : 8;
            hung.mod3 = inst.has_modrm && inst.mod == 3;
            kinds[KIND_HANG]++;
            group_add(&hung);
            lost++;
            next++;
        }
        first = next;
    }
    (void)refused_count;
    static const char *const maps[] = { "PRI", "0F", "0F38", "0F3A" };
    FILE *out = fopen(argv[1], "w");
    unsigned group_counts[KINDS] = { 0 };
    for (uint32_t i = 0; i < GROUP_SLOTS; i++) {
        const Group *g = &groups[i];
        if (!g->key) continue;
        const Record *r = &g->example;
        const Encoding *e = &corpus[r->index];
        group_counts[r->kind]++;
        fprintf(out, "{\"kind\":\"%s\",\"map\":\"%s\",\"opcode\":\"%02X\",\"reg\":%d,\"mod3\":%u,\"prefixes\":\"",
                kind_names[r->kind], maps[r->map & 3], r->opcode, r->reg == 8 ? -1 : r->reg, r->mod3);
        for (unsigned k = 0; k < prefix_lengths[e->set]; k++) fprintf(out, "%02x", prefix_sets[e->set][k]);
        fprintf(out, "\",\"count\":%u,\"bytes\":\"", g->count);
        for (unsigned k = 0; k < 12; k++) fprintf(out, "%02x", e->bytes[k]);
        fprintf(out, "\",\"ours_result\":%d,\"ours_length\":%d,\"cpu_signal\":%d,\"cpu_si_code\":%d,\"cpu_delta\":%d}\n",
                r->ours_result, r->ours_length, r->cpu_signal, r->cpu_si_code, r->cpu_delta);
    }
    fclose(out);
    printf("{\"encodings\":%zu,\"lost_to_dead_workers\":%llu", corpus_count, (unsigned long long)lost);
    for (unsigned k = 0; k < KINDS; k++)
        printf(",\"%s\":{\"encodings\":%llu,\"groups\":%u}", kind_names[k], (unsigned long long)kinds[k], group_counts[k]);
    printf("}\n");
    return 0;
}
