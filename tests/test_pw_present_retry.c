/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The asynchronous present's retry of a refused window frame (src/pw_present_retry.h).
 * - Taken on the third send again.
 * - A newer frame arriving mid-burst: superseded at once.
 * - Refused throughout: dropped after 500 ms of pauses, never lost.
 * - A failure other than ESTALE: reported as lost, before and during a burst. */
#include "../src/pw_present_retry.h"
#include <assert.h>
#include <stdio.h>

struct owner {
    int answers[300];   /* the owner's answer to each send again */
    unsigned sends, newer_after, newer_asks;
    unsigned paused_us;
};
static int send(void *opaque) { struct owner *o = opaque; return o->answers[o->sends++]; }
static int newer(void *opaque) { struct owner *o = opaque; return ++o->newer_asks > o->newer_after; }
static void pause(void *opaque, unsigned us) { ((struct owner *)opaque)->paused_us += us; }

int main(void)
{
    /* Taken at once: no burst, newer() never asked. */
    struct owner a = {{0}, 0, 1000, 0, 0};
    PwPresentBurst b = pw_present_retry_run(0, send, newer, pause, &a);
    assert(b.outcome == PW_PRESENT_TAKEN && b.tries == 0 && a.newer_asks == 0 && a.paused_us == 0);

    /* Refused, refused again twice, taken on the third send again. */
    struct owner c = {{ESTALE, ESTALE, 0}, 0, 1000, 0, 0};
    b = pw_present_retry_run(ESTALE, send, newer, pause, &c);
    assert(b.outcome == PW_PRESENT_TAKEN && b.tries == 3 && c.sends == 3 && b.waited_us == 3 * PW_PRESENT_RETRY_STEP_US);
    assert(c.paused_us == b.waited_us);

    /* A newer frame queued after the second refusal: superseded, no third send. */
    struct owner d = {{ESTALE, ESTALE, ESTALE}, 0, 2, 0, 0};
    b = pw_present_retry_run(ESTALE, send, newer, pause, &d);
    assert(b.outcome == PW_PRESENT_SUPERSEDED && b.tries == 2 && d.sends == 2);

    /* Refused throughout: dropped after the limit of pauses, 250 sends again, never lost. */
    struct owner e = {{0}, 0, 100000, 0, 0};
    for (unsigned i = 0; i < 300; ++i) e.answers[i] = ESTALE;
    b = pw_present_retry_run(ESTALE, send, newer, pause, &e);
    assert(b.outcome == PW_PRESENT_DROPPED && b.waited_us == PW_PRESENT_RETRY_LIMIT_US &&
           b.tries == PW_PRESENT_RETRY_LIMIT_US / PW_PRESENT_RETRY_STEP_US);

    /* A failure other than ESTALE: lost, at once and mid-burst, as before the retry. */
    struct owner f = {{0}, 0, 1000, 0, 0};
    b = pw_present_retry_run(EIO, send, newer, pause, &f);
    assert(b.outcome == PW_PRESENT_LOST && b.tries == 0 && f.newer_asks == 0);
    struct owner g = {{ESTALE, EPIPE}, 0, 1000, 0, 0};
    b = pw_present_retry_run(ESTALE, send, newer, pause, &g);
    assert(b.outcome == PW_PRESENT_LOST && b.tries == 2);

    /* The names the burst's log line uses. */
    assert(pw_present_outcome_name(PW_PRESENT_DROPPED)[0] == 'd' && pw_present_outcome_name(PW_PRESENT_LOST)[0] == 'l');
    puts("test_pw_present_retry: ok");
    return 0;
}
