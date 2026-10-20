/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_DESKTOP_SESSION_H
#define PW_DESKTOP_SESSION_H
#include <stddef.h>
#include <stdint.h>
/* The Mihawk OS desktop session, supervised by the title itself (docs/MIHAWK_OS.md, game mode step 3).
 *
 * The session's entry is still C:\windows\system32\mihawk.exe [program arguments], as the catalog and the runners
 * name it, but no such Windows program runs: the title starts Explorer as the session's root, with the arguments
 * Explorer runs once its desktop is up. A Windows mihawk.exe used to do that and wait (9 MiB for the session).
 *
 * Game mode, step 3: while a full-screen or borderless full-screen game holds the screen (the broker flips its frames
 * directly, src/pw_window_direct.h) for PW_SESSION_UNLOAD_MS without a break, Explorer is unloaded: its process ends
 * and the session goes on. Unloaded, never frozen: a frozen Explorer would hang any game that broadcasts a window
 * message; the server keeps the desktop window itself, so every other window survives. Explorer starts again once the
 * game has been out of game mode for PW_SESSION_RESTORE_MS, or at once when the game's process is gone. The title
 * starts it off its input loop (PW_SESSION_STARTING), so the running game keeps its controller meanwhile: a restore
 * took 225-614 ms (step3-focus-2026-10-10-04), and a payload loader in trouble can hold a start for its timeouts.
 * A start that fails is tried again every PW_SESSION_RETRY_MS while the game runs: the game is never closed for it.
 * Only with the game gone and PW_SESSION_START_ATTEMPTS failures in a row does the session end (end_status 1).
 * An Explorer that does not end within PW_SESSION_UNLOAD_LIMIT_MS stays, and is not asked again until game mode has
 * had a break. The session ends when Explorer leaves by itself (log off), as before, or, with Explorer unloaded, when
 * a stop was asked and the game is gone (end_status 0). Every rule is a pure function of the state passed to the
 * tick, so the host tests run the title's. */
#define PW_DESKTOP_SESSION_ENTRY "C:\\windows\\system32\\mihawk.exe"
#define PW_DESKTOP_SESSION_SHELL "C:\\windows\\system32\\explorer.exe"
#define PW_DESKTOP_SESSION_DESKTOP "/desktop=Default,1920x1080"
enum { PW_SESSION_UNLOAD_MS = 2000, PW_SESSION_RESTORE_MS = 1500, PW_SESSION_UNLOAD_LIMIT_MS = 15000,
       PW_SESSION_RETRY_MS = 5000, PW_SESSION_START_ATTEMPTS = 3 };
typedef enum { PW_SESSION_SHELL = 0, PW_SESSION_UNLOADING, PW_SESSION_UNLOADED, PW_SESSION_STARTING,
               PW_SESSION_ENDED } PwSessionPhase;
typedef enum { PW_SESSION_NONE = 0, PW_SESSION_UNLOAD, PW_SESSION_START, PW_SESSION_END } PwSessionAction;
/* What the title sees at one tick. */
typedef struct PwSessionState {
    uint64_t now_ms;
    int game_mode;        /* a window's frames are flipped directly */
    int32_t game_pid;     /* its native process, while game_mode */
    int game_alive;       /* the last game-mode process still exists */
    int shell_present;    /* Explorer, the session root, is running */
    int stopping;         /* a stop was asked */
} PwSessionState;
typedef struct PwDesktopSession {
    PwSessionPhase phase;
    uint64_t phase_ms;          /* when the phase began */
    uint64_t game_since_ms;     /* start of the current unbroken game mode; 0 outside game mode */
    uint64_t desktop_since_ms;  /* start of the current unbroken time outside game mode; 0 in game mode */
    int32_t game_pid;           /* the last game-mode process */
    uint64_t retry_ms;          /* in PW_SESSION_UNLOADED after a failed start: not before this */
    int hold;                   /* the shell stayed: no unload until game mode has had a break */
    int end_status;             /* PW_SESSION_ENDED: 0 a stop with the shell unloaded, 1 the shell could not start */
    unsigned unloads, starts, kept;  /* kept: unloads that did not finish in time (the shell stayed) */
    unsigned failed_starts;     /* starts that failed in a row */
} PwDesktopSession;
#ifdef __cplusplus
extern "C" {
#endif
/* Whether a launch's program is the session entry (case-insensitive). */
int pw_desktop_session_entry(const char *program);
/* Wine's argv for Explorer as the session root: loader name, Explorer, its desktop, then the entry's arguments.
 * The number of words, or -1 when they do not fit max (one is left for the terminating NULL). */
int pw_desktop_session_argv(const char **argv, size_t max, const char *loader, const char *const *arguments,
                            size_t count);
void pw_desktop_session_begin(PwDesktopSession *session, uint64_t now_ms);
/* The action this tick asks for. PW_SESSION_UNLOAD and PW_SESSION_START are done by the caller; PW_SESSION_START
 * enters PW_SESSION_STARTING, where ticks ask nothing until the caller reports with pw_desktop_session_started. */
PwSessionAction pw_desktop_session_tick(PwDesktopSession *session, const PwSessionState *state);
/* The start the tick asked for has finished: ok, the shell runs again; not ok, it is tried again after
 * PW_SESSION_RETRY_MS (the next tick that finds the game gone after PW_SESSION_START_ATTEMPTS failures ends it). */
void pw_desktop_session_started(PwDesktopSession *session, int ok, uint64_t now_ms);
const char *pw_desktop_session_phase_name(PwSessionPhase phase);
#ifdef __cplusplus
}
#endif
#endif
