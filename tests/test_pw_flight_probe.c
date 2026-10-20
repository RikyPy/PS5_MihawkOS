/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The flight recorder's process reader (src/pw_flight_probe.h): a read that blocks stops only the worker, and the
 * caller learns which pid it is in. alarm() fails the test if anything waits for good. */
#include "../src/pw_flight_probe.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int gate[2];   /* pid 7's read blocks until a byte arrives here */

static uint64_t now_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}
static int fake_read(void *context, int pid, PwFlightRow *row)
{
    assert(context == gate && row->pid == pid);
    if (pid == 9) return EACCES;
    if (pid == 7) {
        char byte;
        assert(read(gate[0], &byte, 1) == 1);
    }
    row->state = 2;
    row->runtime_us = (uint64_t)pid * 1000;
    snprintf(row->name, sizeof(row->name), "p%d", pid);
    return 0;
}
/* take() until the worker hands a round over. */
static unsigned rows_of(PwFlightProbe *probe, PwFlightRow *rows, unsigned capacity)
{
    int busy_pid;
    uint64_t busy_ms;
    for (;;) {
        const unsigned taken = pw_flight_probe_take(probe, rows, capacity, &busy_pid, &busy_ms);
        if (taken) return taken;
        usleep(1000);
    }
}

int main(void)
{
    alarm(20);
    static PwFlightProbe probe;
    PwFlightRow rows[PW_FLIGHT_PIDS];
    int busy_pid = -1;
    uint64_t busy_ms = 1;
    assert(!pipe(gate));
    assert(pw_flight_probe_start(&probe, NULL, gate, now_ms) == EINVAL);
    assert(pw_flight_probe_start(NULL, fake_read, gate, now_ms) == EINVAL);
    assert(!pw_flight_probe_start(&probe, fake_read, gate, now_ms));

    /* Nothing before the first round, and asking for nothing is refused. */
    assert(pw_flight_probe_take(&probe, rows, PW_FLIGHT_PIDS, &busy_pid, &busy_ms) == 0 && busy_pid == 0 && !busy_ms);
    assert(pw_flight_probe_ask(&probe, NULL, 1) == EINVAL);
    const int pids[] = {3, 7};
    assert(pw_flight_probe_ask(&probe, pids, 0) == EINVAL);

    /* The worker reads 3, then blocks in 7: every take returns at once and says 7, for a time that does not go back,
     * and a new round cannot be asked. */
    assert(!pw_flight_probe_ask(&probe, pids, 2));
    while (busy_pid != 7) {
        assert(pw_flight_probe_take(&probe, rows, PW_FLIGHT_PIDS, &busy_pid, &busy_ms) == 0);
        usleep(1000);
    }
    uint64_t before = busy_ms;
    for (int i = 0; i < 3; ++i) {
        usleep(20000);
        assert(pw_flight_probe_take(&probe, rows, PW_FLIGHT_PIDS, &busy_pid, &busy_ms) == 0);
        assert(busy_pid == 7 && busy_ms >= before);
        before = busy_ms;
    }
    assert(before >= 40);
    assert(pw_flight_probe_ask(&probe, pids, 2) == EBUSY);

    /* Released: the round comes out whole, one row per pid, in order, and only once. */
    assert(write(gate[1], "x", 1) == 1);
    assert(rows_of(&probe, rows, PW_FLIGHT_PIDS) == 2);
    assert(rows[0].pid == 3 && !rows[0].error && rows[0].state == 2 && !strcmp(rows[0].name, "p3"));
    assert(rows[1].pid == 7 && !rows[1].error && rows[1].runtime_us == 7000);
    assert(pw_flight_probe_take(&probe, rows, PW_FLIGHT_PIDS, &busy_pid, &busy_ms) == 0 && busy_pid == 0);

    /* A read that fails gives a row with its errno only; a short capacity takes the first rows. */
    const int failing[] = {9, 3};
    assert(!pw_flight_probe_ask(&probe, failing, 2));
    assert(rows_of(&probe, rows, 1) == 1);
    assert(rows[0].pid == 9 && rows[0].error == EACCES && rows[0].state == 0 && !rows[0].name[0]);

    puts("pw_flight_probe: ok");
    return 0;
}
