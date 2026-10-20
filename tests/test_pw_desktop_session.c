/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The title's desktop session supervisor (src/pw_desktop_session.h): entry, Explorer's argv, and game mode step 3,
 * tick by tick at the title's 100 ms cadence. */
#include "../src/pw_desktop_session.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static PwDesktopSession s;
static PwSessionState st;
/* Advance to `ms`, ticking every 100 ms with the current state; the first action other than NONE, and when. */
static PwSessionAction run_until(uint64_t ms, uint64_t *when)
{
    while (st.now_ms < ms) {
        st.now_ms += 100;
        const PwSessionAction action = pw_desktop_session_tick(&s, &st);
        if (action != PW_SESSION_NONE) { if (when) *when = st.now_ms; return action; }
    }
    return PW_SESSION_NONE;
}

int main(void)
{
    /* The entry and the shell's argv. */
    assert(pw_desktop_session_entry("C:\\windows\\system32\\mihawk.exe"));
    assert(pw_desktop_session_entry("c:\\WINDOWS\\System32\\MIHAWK.EXE"));
    assert(!pw_desktop_session_entry("C:\\windows\\system32\\mihawk.exe2") && !pw_desktop_session_entry("mihawk.exe"));
    assert(!pw_desktop_session_entry(NULL) && !pw_desktop_session_entry("C:\\windows\\system32\\explorer.exe"));
    const char *argv[8], *probe[] = {"C:\\shell64-probe.exe", "--capture=Z:\\x.ppm"};
    assert(pw_desktop_session_argv(argv, 8, "wine", probe, 2) == 5);
    assert(!strcmp(argv[1], PW_DESKTOP_SESSION_SHELL) && !strcmp(argv[2], "/desktop=Default,1920x1080"));
    assert(!strcmp(argv[3], probe[0]) && !strcmp(argv[4], probe[1]) && !argv[5]);
    assert(pw_desktop_session_argv(argv, 8, "wine", NULL, 0) == 3 && !argv[3]);   /* a restart runs nothing */
    assert(pw_desktop_session_argv(argv, 5, "wine", probe, 2) == -1);

    /* A full-screen game: the shell is unloaded after 2 s of unbroken game mode, restarted 1.5 s after it ends. */
    uint64_t when = 0;
    pw_desktop_session_begin(&s, 1000);
    st = (PwSessionState){.now_ms = 1000, .shell_present = 1, .game_alive = 1};
    assert(run_until(5000, NULL) == PW_SESSION_NONE);                 /* a desktop without a game */
    st.game_mode = 1; st.game_pid = 42;
    assert(run_until(5000 + 1900, NULL) == PW_SESSION_NONE);          /* not yet 2 s */
    st.game_mode = 0; run_until(st.now_ms + 100, NULL);               /* a break: the 2 s start again */
    st.game_mode = 1;
    assert(run_until(20000, &when) == PW_SESSION_UNLOAD && when == 7100 + 2000 && s.phase == PW_SESSION_UNLOADING);
    assert(run_until(when + 500, NULL) == PW_SESSION_NONE && s.phase == PW_SESSION_UNLOADING);   /* still going */
    st.shell_present = 0;
    assert(run_until(st.now_ms + 100, NULL) == PW_SESSION_NONE && s.phase == PW_SESSION_UNLOADED);
    assert(run_until(st.now_ms + 60000, NULL) == PW_SESSION_NONE);    /* a long game keeps the desktop away */
    const uint64_t left = st.now_ms;
    st.game_mode = 0;                                                 /* windowed, minimized or closed */
    assert(run_until(left + 1300, NULL) == PW_SESSION_NONE);
    assert(run_until(left + 5000, &when) == PW_SESSION_START && when == left + 100 + PW_SESSION_RESTORE_MS);
    assert(s.phase == PW_SESSION_STARTING);
    assert(run_until(when + 3000, NULL) == PW_SESSION_NONE);          /* the start runs off the input loop */
    pw_desktop_session_started(&s, 1, when); st.shell_present = 1;
    assert(s.phase == PW_SESSION_SHELL && s.unloads == 1 && s.starts == 1);
    /* The same game back in game mode: unloaded again after its own 2 s. */
    st.game_mode = 1;
    assert(run_until(when + 10000, &when) == PW_SESSION_UNLOAD && s.unloads == 2);

    /* A start that fails while the game runs is tried again, never ending the game's session. */
    st.shell_present = 0; run_until(st.now_ms + 100, NULL);
    st.game_mode = 0;
    assert(run_until(st.now_ms + 5000, &when) == PW_SESSION_START);
    pw_desktop_session_started(&s, 0, when);
    assert(s.phase == PW_SESSION_UNLOADED && s.failed_starts == 1);
    assert(run_until(when + PW_SESSION_RETRY_MS - 100, NULL) == PW_SESSION_NONE);
    for (int i = 0; i < 5; ++i) {                                     /* more failures than ending would take */
        uint64_t again = 0;
        assert(run_until(st.now_ms + PW_SESSION_RETRY_MS + 200, &again) == PW_SESSION_START);
        pw_desktop_session_started(&s, 0, again);
    }
    assert(s.phase == PW_SESSION_UNLOADED && s.failed_starts == 6);  /* the game still runs: no end */
    /* Once the game is gone, failed starts end the session (status 1) rather than leave an empty screen. */
    st.game_alive = 0;
    assert(run_until(st.now_ms + 200, NULL) == PW_SESSION_END && s.phase == PW_SESSION_ENDED && s.end_status == 1);
    assert(run_until(st.now_ms + 5000, NULL) == PW_SESSION_NONE);

    /* The game's process is gone: the shell comes back at once, without waiting out the 1.5 s; a success resets. */
    pw_desktop_session_begin(&s, 0);
    st = (PwSessionState){.now_ms = 0, .shell_present = 1, .game_alive = 1, .game_mode = 1, .game_pid = 9};
    assert(run_until(10000, NULL) == PW_SESSION_UNLOAD);
    st.shell_present = 0; run_until(st.now_ms + 100, NULL);
    st.game_alive = 0;
    assert(run_until(st.now_ms + 1000, &when) == PW_SESSION_START);
    pw_desktop_session_started(&s, 0, when);
    assert(run_until(when + PW_SESSION_RETRY_MS + 200, &when) == PW_SESSION_START);   /* game gone, 1 failure */
    pw_desktop_session_started(&s, 1, when);
    assert(s.phase == PW_SESSION_SHELL && s.failed_starts == 0 && s.retry_ms == 0);
    pw_desktop_session_started(&s, 0, when);                          /* a stray report changes nothing */
    assert(s.phase == PW_SESSION_SHELL);

    /* A stop with the shell unloaded: no restart; the session ends once the game is gone. */
    pw_desktop_session_begin(&s, 0);
    st = (PwSessionState){.now_ms = 0, .shell_present = 1, .game_alive = 1, .game_mode = 1, .game_pid = 7};
    assert(run_until(10000, NULL) == PW_SESSION_UNLOAD);
    st.shell_present = 0; run_until(st.now_ms + 100, NULL);
    st.stopping = 1; st.game_mode = 0;
    assert(run_until(st.now_ms + 20000, NULL) == PW_SESSION_NONE);    /* the game is still closing */
    st.game_alive = 0;
    assert(run_until(st.now_ms + 1000, NULL) == PW_SESSION_END && s.phase == PW_SESSION_ENDED);

    /* With the shell running, a stop never unloads it: the shell's exit ends the session as before. */
    pw_desktop_session_begin(&s, 0);
    st = (PwSessionState){.now_ms = 0, .shell_present = 1, .game_alive = 1, .game_mode = 1, .stopping = 1};
    assert(run_until(10000, NULL) == PW_SESSION_NONE && s.phase == PW_SESSION_SHELL);

    /* A shell that does not go within 15 s stays, and is asked again only after game mode starts anew. */
    pw_desktop_session_begin(&s, 0);
    st = (PwSessionState){.now_ms = 0, .shell_present = 1, .game_alive = 1, .game_mode = 1};
    assert(run_until(10000, &when) == PW_SESSION_UNLOAD);
    assert(run_until(when + PW_SESSION_UNLOAD_LIMIT_MS + 200, NULL) == PW_SESSION_NONE);
    assert(s.phase == PW_SESSION_SHELL && s.kept == 1);
    assert(run_until(st.now_ms + 60000, NULL) == PW_SESSION_NONE);    /* not asked again while game mode holds */
    st.game_mode = 0; run_until(st.now_ms + 100, NULL);               /* a break */
    st.game_mode = 1;
    assert(run_until(st.now_ms + 1900, NULL) == PW_SESSION_NONE);
    assert(run_until(st.now_ms + 200, NULL) == PW_SESSION_UNLOAD && s.unloads == 2);
    assert(!strcmp(pw_desktop_session_phase_name(PW_SESSION_UNLOADED), "unloaded"));
    assert(!strcmp(pw_desktop_session_phase_name(PW_SESSION_STARTING), "starting"));
    assert(!strcmp(pw_desktop_session_phase_name(PW_SESSION_ENDED), "ended"));
    puts("desktop session: entry, shell argv, unload after 2 s of game mode, restore off the input loop, "
         "retried starts, stop, a kept shell held until a break");
    return 0;
}
