/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pw_desktop_session.h"
#include <string.h>

int pw_desktop_session_entry(const char *program)
{
    const char *entry = PW_DESKTOP_SESSION_ENTRY;
    if (!program) return 0;
    for (; *program && *entry; ++program, ++entry) {
        char a = *program, b = *entry;
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return 0;
    }
    return !*program && !*entry;
}

int pw_desktop_session_argv(const char **argv, size_t max, const char *loader, const char *const *arguments,
                            size_t count)
{
    if (!argv || !loader || (count && !arguments) || max < 4 || count > max - 4) return -1;
    size_t used = 0;
    argv[used++] = loader;
    argv[used++] = PW_DESKTOP_SESSION_SHELL;
    argv[used++] = PW_DESKTOP_SESSION_DESKTOP;
    for (size_t i = 0; i < count; ++i) argv[used++] = arguments[i];
    argv[used] = NULL;
    return (int)used;
}

void pw_desktop_session_begin(PwDesktopSession *session, uint64_t now_ms)
{
    memset(session, 0, sizeof(*session));
    session->phase = PW_SESSION_SHELL;
    session->phase_ms = now_ms;
    session->desktop_since_ms = now_ms;
}

static void enter(PwDesktopSession *session, PwSessionPhase phase, uint64_t now_ms)
{
    session->phase = phase;
    session->phase_ms = now_ms;
}

PwSessionAction pw_desktop_session_tick(PwDesktopSession *session, const PwSessionState *state)
{
    const uint64_t now = state->now_ms;
    if (session->phase == PW_SESSION_ENDED) return PW_SESSION_NONE;
    /* How long game mode, or its absence, has lasted without a break. */
    if (state->game_mode) {
        if (!session->game_since_ms) session->game_since_ms = now ? now : 1;
        session->desktop_since_ms = 0;
        session->game_pid = state->game_pid;
    } else {
        session->game_since_ms = 0;
        session->hold = 0;
        if (!session->desktop_since_ms) session->desktop_since_ms = now ? now : 1;
    }
    switch (session->phase) {
    case PW_SESSION_STARTING:
        return PW_SESSION_NONE;      /* the caller is starting the shell and reports with started() */
    case PW_SESSION_SHELL:
        /* With the shell running, a stop ends the session through the shell's own exit, as before. */
        if (!state->stopping && !session->hold && state->game_mode &&
            now - session->game_since_ms >= PW_SESSION_UNLOAD_MS) {
            enter(session, PW_SESSION_UNLOADING, now);
            ++session->unloads;
            return PW_SESSION_UNLOAD;
        }
        return PW_SESSION_NONE;
    case PW_SESSION_UNLOADING:
        if (!state->shell_present) {
            enter(session, PW_SESSION_UNLOADED, now);
            break;
        }
        if (now - session->phase_ms > PW_SESSION_UNLOAD_LIMIT_MS) {
            /* The shell did not go: it stays, and is not asked again until game mode has had a break. */
            enter(session, PW_SESSION_SHELL, now);
            session->hold = 1;
            ++session->kept;
        }
        return PW_SESSION_NONE;
    default:
        break;
    }
    /* PW_SESSION_UNLOADED */
    if (state->stopping) {
        if (state->game_alive) return PW_SESSION_NONE;
        session->end_status = 0;
        enter(session, PW_SESSION_ENDED, now);
        return PW_SESSION_END;
    }
    if (session->failed_starts >= PW_SESSION_START_ATTEMPTS && !state->game_alive) {
        /* Nothing runs and the shell cannot come back: the session ends rather than show an empty screen. */
        session->end_status = 1;
        enter(session, PW_SESSION_ENDED, now);
        return PW_SESSION_END;
    }
    if (session->retry_ms && now < session->retry_ms) return PW_SESSION_NONE;
    if (!state->game_alive || (!state->game_mode && now - session->desktop_since_ms >= PW_SESSION_RESTORE_MS)) {
        enter(session, PW_SESSION_STARTING, now);
        return PW_SESSION_START;
    }
    return PW_SESSION_NONE;
}

void pw_desktop_session_started(PwDesktopSession *session, int ok, uint64_t now_ms)
{
    if (session->phase != PW_SESSION_STARTING) return;
    if (ok) {
        enter(session, PW_SESSION_SHELL, now_ms);
        session->game_since_ms = 0;
        session->retry_ms = 0;
        session->failed_starts = 0;
        ++session->starts;
    } else {
        enter(session, PW_SESSION_UNLOADED, now_ms);
        session->retry_ms = now_ms + PW_SESSION_RETRY_MS;
        ++session->failed_starts;
    }
}

const char *pw_desktop_session_phase_name(PwSessionPhase phase)
{
    static const char *const names[] = {"shell", "unloading", "unloaded", "starting", "ended"};
    return phase <= PW_SESSION_ENDED ? names[phase] : "unknown";
}
