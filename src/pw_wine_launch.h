/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_WINE_LAUNCH_H
#define PW_WINE_LAUNCH_H
/*
 * What the wine64 title runs, from its arguments.
 *
 * The title runs one game per process: the launcher restarts the title with
 * sceSystemServiceLoadExec and the chosen game's arguments, and closing the
 * game restarts it into the launcher. The desktop session has a persistent
 * shell as its root and launches applications as children. Arguments are "key=value" words:
 *   profile=<id>   a game from the title's catalog;
 *   path=<exe>     an absolute Windows path, run directly (overrides the
 *                  profile's executable);
 *   cycle=<n>      open/close cycles completed, for unattended validation.
 * With neither profile nor path the title shows the launcher, as when the
 * system starts it (argc 1, empty argv[0]).
 */
#include <stddef.h>
#include <stdint.h>

enum { PW_WINE_LAUNCH_PATH_MAX = 260, PW_WINE_LAUNCH_ARGS = 4, PW_WINE_LAUNCH_WORDS = 32 };

typedef struct PwWineApp {
    const char *id;           /* profile=<id> */
    const char *name;         /* shown in the launcher */
    const char *detail;       /* second line in the launcher */
    const char *executable;   /* absolute Windows path */
} PwWineApp;

typedef enum PwWineLaunchMode { PW_WINE_LAUNCH_LAUNCHER = 1, PW_WINE_LAUNCH_GAME } PwWineLaunchMode;

typedef struct PwWineLaunch {
    PwWineLaunchMode mode;
    const PwWineApp *app;                         /* NULL for a bare path= */
    char executable[PW_WINE_LAUNCH_PATH_MAX];     /* GAME: what Wine runs */
    uint32_t cycle;
    unsigned refused;   /* an unknown profile or an invalid path: shown in the launcher */
} PwWineLaunch;

/* Decide from argv. Always fills out; a refused game falls back to the
 * launcher. 0, or -1 when out or the catalog is invalid. */
int pw_wine_launch_parse(int argc, char *const *argv, const PwWineApp *apps, size_t count,
                         PwWineLaunch *out);

/* The argv for LoadExec: app's profile, path and cycle, or, with app NULL,
 * the launcher with the cycle. Words are built in storage; argv is
 * NULL-terminated. Returns the number of words, or 0 when storage or argv
 * is too small. */
size_t pw_wine_launch_argv(const PwWineApp *app, uint32_t cycle, char *storage, size_t size,
                           char **argv, size_t max);
/* A profile's arguments line as words for Wine's argv, which Wine quotes
 * back into the Windows command line: words split at spaces and tabs, and
 * double quotes group a word ("a b" is one word; the quotes are dropped).
 * Words are copied into storage. Returns the number of words (0 for an
 * empty line), or -1 when text is NULL, a quote is unclosed, or the words
 * do not fit storage or max. */
int pw_wine_launch_split(const char *text, char *storage, size_t size, const char **words,
                         size_t max);

/* A profile's working directory (a Windows path pw_app_profile validated:
 * drive, backslashes, no "..") as the host directory it names in prefix:
 * C: is the prefix's drive_c and Z: the host root, as in a default prefix.
 * Returns 0, or -1 for another drive or when out is too small. */
int pw_wine_launch_host_dir(const char *prefix, const char *windows_path, char *out, size_t size);

/* Select a prefix folder ID, never a path. Every catalog game lives in Mihawk OS: it runs in "mihawk" unless its
 * game.json names another prefix ("prefix", IDs of ASCII letters, digits, '_' and '-'); bare diagnostic paths keep
 * "probe". A per-game folder no longer pins a game to itself: tools/mihawk-games.py moved every game's state into
 * mihawk and retired those folders (2026-10-10). Returns 0, or -1 for an invalid ID or insufficient storage. */
int pw_wine_launch_prefix_id(const char *requested, const char *game_id, char *out, size_t size);

/* The user folders wineboot makes in a prefix on Linux (C:\users\Mihawk\AppData\Roaming, Documents,
 * Saved Games, ... and C:\users\Public\...), which no process can make on the console: wineboot is a child
 * process. Without AppData\Roaming, .NET's GetFolderPath(ApplicationData) is empty, so a program builds a
 * relative save path; a .NET (MonoGame) game then saved beside the game and its load menu, combining that relative
 * folder with the relative names it listed, found none of its saves (2026-10-04). Each folder is made with
 * make (a mkdir that treats an existing folder as success), parents first. Returns how many make calls
 * failed, or -1 when prefix is NULL or a path does not fit. */
int pw_wine_launch_profile_dirs(const char *prefix, int (*make)(const char *path, void *context), void *context);

/* Runtime-owned files that belong in a prefix folder (Mesa's WGL/Zink opengl32.dll and libgallium_wgl.dll in
 * system32, for OpenGL in a helper process), copied from the runtime folder `from` into `to` when they differ:
 * when the file `stamp` (a version line, in both folders) differs or is missing, or a file is missing or has
 * another size. Each file is written under a temporary name and renamed, the stamp last, so an interrupted copy
 * is redone. `to` must exist (wineboot makes system32; a folder made before it breaks a new prefix). Returns
 * how many files were copied (0: already current), or -1 with errno set. */
int pw_wine_launch_sync(const char *from, const char *to, const char *const *names, size_t count, const char *stamp);
#endif
