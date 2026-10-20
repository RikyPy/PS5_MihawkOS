/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_WINE_PROFILE_H
#define PW_WINE_PROFILE_H
/*
 * The Windows user of every prefix, and the one-time move of an older prefix's profile to that user.
 *
 * Wine names the profile after GetUserNameW, which patch 1240 makes "Mihawk" (Proton answered a fixed "steamuser"):
 * shell32 expands %USERPROFILE% to C:\users\<name>, so Documents, AppData and Saved Games live there, and wineboot
 * records the folder in the registry. A prefix made by v2.0.0-alpha-1 or -2 keeps the user's saves and settings in
 * C:\users\steamuser, and its registry names that folder: User Shell Folders and Shell Folders (user.reg, userdef.reg),
 * TEMP and TMP, the saved Volatile Environment (USERNAME, USERPROFILE, HOMEPATH), ProfileList's ProfileImagePath
 * (system.reg) and any path a program recorded. Started as it is, the new Wine would make an empty C:\users\Mihawk
 * beside it, and every game would lose its saves.
 *
 * pw_wine_profile_migrate moves such a prefix, in the title, before Wine starts on it (no wineserver holds the registry
 * then), in four steps that are each safe to repeat, so an interrupted move finishes on the next start:
 *   1. users/steamuser moves into users/Mihawk: one rename, or, when Mihawk exists too (a deploy or an earlier run made
 *      its folders), name by name, renaming whatever Mihawk lacks and descending into folders both hold. A file both
 *      hold stays where it is in steamuser: no copy is lost, steamuser stays a real folder, and step 2 is skipped.
 *   2. users/steamuser becomes a link to Mihawk, for programs that stored the old absolute path (a config file, a
 *      registry hex value). A title cannot make symbolic links: it is a line of users/.pw-symlinks, the table Wine's
 *      Unix side follows (wine/ps5/pw_wine_cwd.h). Explorer does not list it (a dot file), so C:\users shows Mihawk
 *      and Public. The link matches the exact name only: a stored C:\users\SteamUser does not follow it.
 *   3. Each registry file's references become Mihawk (pw_wine_profile_rewrite), written beside the file, synced and
 *      renamed over it, so a crash leaves the old file or the new one.
 *   4. PW_WINE_PROFILE_MARKER is written, last: a prefix holding it is not looked at again.
 * A prefix with no registry yet (the title makes it, Wine fills it) is Mihawk's from the start: only the marker is
 * written. A refusal (an I/O error) stops at its step without the marker; the title logs it and starts Wine anyway,
 * and the next start goes on from there.
 */
#include <stddef.h>

#define PW_WINE_PROFILE_USER "Mihawk"
#define PW_WINE_PROFILE_FORMER "steamuser"
#define PW_WINE_PROFILE_MARKER ".pw-profile-user"

typedef struct PwWineProfileMove {
    int already;         /* the marker was there: nothing was looked at */
    unsigned moved;      /* renames into users/Mihawk (1: the whole folder at once) */
    unsigned kept;       /* names both profiles held as files, left in users/steamuser */
    int linked;          /* users/steamuser leads to users/Mihawk (a link added or already there) */
    unsigned files;      /* registry files rewritten */
    size_t replaced;     /* references to the former profile they held */
    const char *step;    /* on -1: "folder", "link", "registry" or "marker" */
} PwWineProfileMove;

/* Rewrite registry text (a .reg file, in place) so the former profile is Mihawk's: "steamuser", in either case, after
 * a users folder and a separator (\\ as the file escapes a backslash, or /) and followed by a separator, a quote, ';',
 * ',' or ']' (C:\users\steamuser\Documents, \users\steamuser, C:/users/steamuser), or as a whole quoted string
 * ("USERNAME"="steamuser"). Other text, hex values among it, is kept. Returns the new length (never longer); counts
 * the replacements into *replaced when it is not NULL. */
size_t pw_wine_profile_rewrite(char *text, size_t length, size_t *replaced);

/* Move the prefix folder's profile to Mihawk as above. Returns 0 (move says what was done), or -1 with errno set and
 * move->step naming the step that failed. */
int pw_wine_profile_migrate(const char *prefix, PwWineProfileMove *move);
#endif
