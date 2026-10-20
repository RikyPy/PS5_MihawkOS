/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_PRESENT_RETRY_H
#define PW_PRESENT_RETRY_H
#include <errno.h>
/* The asynchronous present's answer to a window frame the owner refused for now (graphics patch 0024's WSI, the
 * presenter thread). Host test: tests/test_pw_present_retry.c.
 *
 * The owner answers ESTALE when it cannot place a window's frame yet: the window, or its generation, is not in its
 * snapshot. A program presenting synchronously gets the failure back and presents again; perf-01 and perf-02 show 3-5
 * refusals, then the frame taken. Behind the asynchronous present the failure never reached the program, and the
 * presenter used to take it for a lost device. So a presenter into another process's window that presents once
 * (gpu-window32's into-owner, Chromium's shape) never showed its only frame (gate-2026-10-18-defaults-01).
 *
 * Now the presenter sends the same image again, every PW_PRESENT_RETRY_STEP_US, while nothing newer is queued, for at
 * most PW_PRESENT_RETRY_LIMIT_US of pauses. Then the frame is dropped like one never shown, never the device lost. A
 * failure other than ESTALE is reported as before.
 *
 * It runs on the presenter thread alone. Each send is its own call on the client's present link (no lock held across a
 * pause), and the queue thread never waits on a burst. A newer frame ends the burst at once, so a queue thread that
 * wants an image back gets it by presenting the next one. One line per burst: "gpu-wsi present refused tries= waited_ms=
 * outcome=". */
enum { PW_PRESENT_RETRY_STEP_US = 2000, PW_PRESENT_RETRY_LIMIT_US = 500000 };

typedef enum { PW_PRESENT_TAKEN = 0, PW_PRESENT_AGAIN, PW_PRESENT_DROPPED, PW_PRESENT_SUPERSEDED, PW_PRESENT_LOST } PwPresentStep;

/* One step: status, the owner's answer to the last send (0 taken, ESTALE refused for now, anything else a failure);
 * waited_us, the pauses so far; newer, whether a newer frame is queued or the swapchain is stopping. */
static inline PwPresentStep pw_present_retry_step(int status, unsigned waited_us, int newer)
{
    if (!status) return PW_PRESENT_TAKEN;
    if (status != ESTALE) return PW_PRESENT_LOST;
    if (newer) return PW_PRESENT_SUPERSEDED;
    if (waited_us >= PW_PRESENT_RETRY_LIMIT_US) return PW_PRESENT_DROPPED;
    return PW_PRESENT_AGAIN;
}

static inline const char *pw_present_outcome_name(PwPresentStep step)
{
    switch (step) {
    case PW_PRESENT_TAKEN: return "taken";
    case PW_PRESENT_DROPPED: return "dropped";
    case PW_PRESENT_SUPERSEDED: return "superseded";
    case PW_PRESENT_LOST: return "lost";
    default: return "again";
    }
}

typedef struct PwPresentBurst { unsigned tries, waited_us; PwPresentStep outcome; } PwPresentBurst;

/* A burst after a refused send: newer() is asked only while refused, pause() waits, send() sends the same image again
 * and returns the owner's answer. Returns how many sends again, the pauses and how it ended. */
static inline PwPresentBurst pw_present_retry_run(int status, int (*send)(void *), int (*newer)(void *),
                                                  void (*pause)(void *, unsigned), void *context)
{
    PwPresentBurst burst = {0, 0, PW_PRESENT_TAKEN};
    for (;;) {
        const PwPresentStep step =
            pw_present_retry_step(status, burst.waited_us, status == ESTALE ? newer(context) : 0);
        if (step != PW_PRESENT_AGAIN) {
            burst.outcome = step;
            return burst;
        }
        pause(context, PW_PRESENT_RETRY_STEP_US);
        burst.waited_us += PW_PRESENT_RETRY_STEP_US;
        ++burst.tries;
        status = send(context);
    }
}
#endif
