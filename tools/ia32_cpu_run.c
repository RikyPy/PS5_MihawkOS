/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * The real CPU, in 32-bit mode, as an oracle (docs/IA32_DESIGN.md, M2). Built with -m32 by
 * tools/ia32-cpu-run.sh; reads one instruction encoding per line (hex) on stdin and writes one
 * JSON object per line on stdout: what the CPU did with it.
 *
 * Each encoding runs single-stepped: a trampoline loads the registers, sets EFLAGS.TF and jumps to
 * the instruction; the first trap arrives before it, the second after it, and the signal handler
 * records the state and returns to the harness. So for every instruction that completes, the
 * CPU's own length is EIP after minus EIP before (or the branch target, for a branch), and a fault
 * is reported by its signal with EIP still at the instruction.
 *
 * Registers start pointing into a 64 KiB data window, so a ModRM without displacement reaches
 * mapped memory. Encodings run in forked workers, 256 to a worker: a wild write can corrupt only
 * a worker, and a worker that dies takes one line with it, reported as "worker-died".
 *
 * Refused, never run: INT 80h, SYSENTER and SYSCALL (random registers must not reach Linux).
 *
 * Output: {"bytes":"..","event":"step|SIGILL|SIGSEGV|SIGBUS|SIGFPE|SIGTRAP|refused|worker-died",
 *          "si_code":N,"delta":N,"eip":"..","eflags":"..","regs":[eax..edi]}
 * delta is EIP after the event minus the instruction's address.
 */
#include "ia32_cpu_core.h"
#include <stdlib.h>
#include <sys/wait.h>

static const char *event_name(int sig)
{
    switch (sig) {
    case SIGTRAP: return "SIGTRAP";
    case SIGILL: return "SIGILL";
    case SIGSEGV: return "SIGSEGV";
    case SIGBUS: return "SIGBUS";
    case SIGFPE: return "SIGFPE";
    default: return "other";
    }
}

static void run_one(const uint8_t *bytes, size_t n, char *out, size_t capacity)
{
    char hex[64] = "";
    for (size_t i = 0; i < n && i < 30; i++) snprintf(hex + 2 * i, sizeof(hex) - 2 * i, "%02x", bytes[i]);
    CpuResult r;
    cpu_run(bytes, n, &r);
    if (r.refused) {
        snprintf(out, capacity, "{\"bytes\":\"%s\",\"event\":\"refused\"}", hex);
        return;
    }
    const int step = cpu_completed(&r);
    int at = snprintf(out, capacity, "{\"bytes\":\"%s\",\"event\":\"%s\",\"si_code\":%d,\"delta\":%d,\"eip\":\"%08x\","
                      "\"eflags\":\"%08x\",\"regs\":[", hex, step ? "step" : event_name(r.signal), r.si_code,
                      (int)r.delta, (uint32_t)(CODE_AT + r.delta), r.eflags);
    for (int i = 0; i < 8 && at < (int)capacity; i++)
        at += snprintf(out + at, capacity - (size_t)at, "%s\"%08x\"", i ? "," : "", r.regs[i]);
    snprintf(out + at, capacity - (size_t)at, "]}");
}

static size_t parse_hex(const char *line, uint8_t *out, size_t capacity)
{
    size_t n = 0;
    unsigned value;
    int used;
    while (n < capacity && sscanf(line, " %2x%n", &value, &used) == 1) {
        out[n++] = (uint8_t)value;
        line += used;
    }
    return n;
}

/* One worker: runs up to BATCH lines and writes their results to the pipe, one per line. */
static void run_batch(char lines[][128], int count, int fd)
{
    cpu_worker_setup();
    for (int i = 0; i < count; i++) {
        uint8_t bytes[32];
        const size_t n = parse_hex(lines[i], bytes, 15);
        char out[512];
        run_one(bytes, n, out, sizeof(out));
        dprintf(fd, "%s\n", out);
    }
    _exit(0);
}

int main(void)
{
    static char lines[BATCH][128];
    for (;;) {
        int count = 0;
        while (count < BATCH && fgets(lines[count], sizeof(lines[count]), stdin)) {
            lines[count][strcspn(lines[count], "\r\n")] = 0;
            if (lines[count][0]) count++;
        }
        if (!count) break;
        int pipefd[2];
        if (pipe(pipefd)) return 2;
        const pid_t pid = fork();
        if (pid == 0) {
            close(pipefd[0]);
            run_batch(lines, count, pipefd[1]);
        }
        close(pipefd[1]);
        FILE *results = fdopen(pipefd[0], "r");
        char out[512];
        int done = 0;
        while (done < count && fgets(out, sizeof(out), results)) {
            fputs(out, stdout);
            done++;
        }
        fclose(results);
        waitpid(pid, NULL, 0);
        for (int i = done; i < count; i++) printf("{\"bytes\":\"%s\",\"event\":\"worker-died\"}\n", lines[i]);
        fflush(stdout);
        if (feof(stdin)) break;
    }
    return 0;
}
