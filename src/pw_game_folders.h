/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_GAME_FOLDERS_H
#define PW_GAME_FOLDERS_H

/* Games added by copying a folder (docs/SELF_INSTALL.md, phase 2): each folder in the Mihawk OS prefix's C:\Games is
 * one game, and the one program at its top is what its desktop shortcut starts. No game.json is needed; one is still
 * read for arguments and environment, and a library entry whose executable is in the folder covers it.
 *
 * A program is a top-level "*.exe" (any case) that runs on this runtime: x86-64, .NET Any CPU or 32-bit x86
 * (src/pw_pe_arch.h). Inno Setup's uninstallers (unins*.exe) and Unity's crash handler (UnityCrashHandler*.exe) sit
 * beside nearly every GOG and Unity game and are never the game, so they are not counted. A folder with one program
 * is a game; one with several needs its game named (game.json), and the title says so; one with none (its program in
 * a subfolder, or not for x86) is left alone.
 *
 * Portable: opendir, readdir, stat and the PE reader; nothing is allocated; errors are errno codes. */

#ifdef __cplusplus
extern "C" {
#endif

enum { PW_GAME_FOLDER_NAME = 256 };

struct PwGameFolder {
    char name[PW_GAME_FOLDER_NAME];      /* the folder's name, under C:\Games */
    char program[PW_GAME_FOLDER_NAME];   /* the program's file name, when programs is 1 (else the first one found) */
    unsigned programs;                   /* top-level programs that run on this runtime */
};

typedef void (*PwGameFolderFound)(void *context, const struct PwGameFolder *folder);

/* Calls found for every folder under games (a host path), whatever its programs count. 0, or the errno of opening
 * games (ENOENT: no C:\Games yet). Entries whose names do not fit are skipped. */
int pw_game_folders_scan(const char *games, PwGameFolderFound found, void *context);

/* 1 when the Windows path lies in C:\Games\<name>\ (drive and names in any case, either separator). */
int pw_game_folder_covers(const char *name, const char *windows_path);

#ifdef __cplusplus
}
#endif

#endif
