/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The move of a prefix's profile from steamuser to Mihawk (src/pw_wine_profile.h), on a prefix laid out as an alpha-2
 * console prefix is: its three registry files with the lines the console's wineboot wrote, a save in Documents.
 * PW_PROFILE_REAL_REGISTRY=<folder holding a console prefix's system.reg/user.reg/userdef.reg> also moves a copy of a
 * real one (never committed: it is a user's state). */
#include "../src/pw_wine_profile.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char system_reg[] =
    "WINE REGISTRY Version 2\n;; All keys relative to \\\\Machine\n\n#arch=win64\n\n"
    "[Software\\\\Microsoft\\\\Windows NT\\\\CurrentVersion\\\\ProfileList\\\\S-1-5-21-0-0-0-1000] 1791520605\n"
    "#time=1dd57a7ca1a3ef4\n\"Flags\"=dword:00000000\n\"ProfileImagePath\"=\"C:\\\\users\\\\steamuser\"\n\n"
    "[Software\\\\Microsoft\\\\Windows\\\\CurrentVersion\\\\Explorer\\\\Shell Folders] 1791520605\n"
    "\"Common Favorites\"=\"C:\\\\users\\\\steamuser\\\\Favorites\"\n"
    "\"Common Documents\"=\"C:\\\\users\\\\Public\\\\Documents\"\n";
static const char user_reg[] =
    "WINE REGISTRY Version 2\n;; All keys relative to \\\\User\\\\S-1-5-21-0-0-0-1000\n\n#arch=win64\n\n"
    "[Environment] 1791520605\n\"TEMP\"=\"C:\\\\users\\\\steamuser\\\\AppData\\\\Local\\\\Temp\"\n"
    "\"TMP\"=\"C:\\\\users\\\\steamuser\\\\AppData\\\\Local\\\\Temp\"\n\n"
    "[Software\\\\Microsoft\\\\Windows\\\\CurrentVersion\\\\Explorer\\\\User Shell Folders] 1791520605\n"
    "\"Personal\"=\"C:\\\\users\\\\steamuser\\\\Documents\"\n"
    "\"{4C5C32FF-BB9D-43B0-B5B4-2D72E54EAAA4}\"=\"C:\\\\users\\\\steamuser\\\\Saved Games\"\n\n"
    "[Software\\\\Wine\\\\MUICache] 1791520605\n"
    "\"C:\\\\Users\\\\SteamUser\\\\Desktop\\\\Game.exe\"=\"Game\"\n\n"
    "[Software\\\\Game] 1791520605\n"
    "\"Launch\"=\"\\\"C:\\\\users\\\\steamuser\\\\AppData\\\\Game\\\\run.exe\\\" -x\"\n"
    "\"Unix\"=\"Z:\\\\data\\\\ps5proton\\\\prefixes\\\\mihawk\\\\drive_c\\\\users\\\\steamuser\\\\AppData\"\n"
    "\"Slashes\"=\"C:/users/steamuser/Documents/My Games\"\n"
    "\"List\"=\"C:\\\\users\\\\steamuser;C:\\\\users\\\\steamuser,D:\\\\\"\n"
    "\"Other\"=\"C:\\\\users\\\\steamuserdata\\\\x C:\\\\steamuser\\\\x myusers\\\\steamuser\\\\x steamuser2\"\n"
    "\"Hex\"=hex(2):43,00,3a,00,5c,00,75,00,73,00,65,00,72,00,73,00,5c,00\n\n"
    "[Volatile Environment] 1791520605\n\"HOMEPATH\"=\"\\\\users\\\\steamuser\"\n"
    "\"USERNAME\"=\"steamuser\"\n\"USERPROFILE\"=\"C:\\\\users\\\\steamuser\"\n";
static const char userdef_reg[] =
    "WINE REGISTRY Version 2\n;; All keys relative to \\\\User\\\\.Default\n\n#arch=win64\n\n"
    "[Software\\\\Microsoft\\\\Windows\\\\CurrentVersion\\\\Explorer\\\\Shell Folders] 1791520605\n"
    "\"AppData\"=\"C:\\\\users\\\\steamuser\\\\AppData\\\\Roaming\"\n";
/* user_reg as Mihawk's: every reference above replaced, nothing else. */
static const char user_reg_moved[] =
    "WINE REGISTRY Version 2\n;; All keys relative to \\\\User\\\\S-1-5-21-0-0-0-1000\n\n#arch=win64\n\n"
    "[Environment] 1791520605\n\"TEMP\"=\"C:\\\\users\\\\Mihawk\\\\AppData\\\\Local\\\\Temp\"\n"
    "\"TMP\"=\"C:\\\\users\\\\Mihawk\\\\AppData\\\\Local\\\\Temp\"\n\n"
    "[Software\\\\Microsoft\\\\Windows\\\\CurrentVersion\\\\Explorer\\\\User Shell Folders] 1791520605\n"
    "\"Personal\"=\"C:\\\\users\\\\Mihawk\\\\Documents\"\n"
    "\"{4C5C32FF-BB9D-43B0-B5B4-2D72E54EAAA4}\"=\"C:\\\\users\\\\Mihawk\\\\Saved Games\"\n\n"
    "[Software\\\\Wine\\\\MUICache] 1791520605\n"
    "\"C:\\\\Users\\\\Mihawk\\\\Desktop\\\\Game.exe\"=\"Game\"\n\n"
    "[Software\\\\Game] 1791520605\n"
    "\"Launch\"=\"\\\"C:\\\\users\\\\Mihawk\\\\AppData\\\\Game\\\\run.exe\\\" -x\"\n"
    "\"Unix\"=\"Z:\\\\data\\\\ps5proton\\\\prefixes\\\\mihawk\\\\drive_c\\\\users\\\\Mihawk\\\\AppData\"\n"
    "\"Slashes\"=\"C:/users/Mihawk/Documents/My Games\"\n"
    "\"List\"=\"C:\\\\users\\\\Mihawk;C:\\\\users\\\\Mihawk,D:\\\\\"\n"
    "\"Other\"=\"C:\\\\users\\\\steamuserdata\\\\x C:\\\\steamuser\\\\x myusers\\\\steamuser\\\\x steamuser2\"\n"
    "\"Hex\"=hex(2):43,00,3a,00,5c,00,75,00,73,00,65,00,72,00,73,00,5c,00\n\n"
    "[Volatile Environment] 1791520605\n\"HOMEPATH\"=\"\\\\users\\\\Mihawk\"\n"
    "\"USERNAME\"=\"Mihawk\"\n\"USERPROFILE\"=\"C:\\\\users\\\\Mihawk\"\n";

static char base[] = "/tmp/pw-wine-profile-XXXXXX";

static const char *at(const char *relative)
{
    static char paths[4][1024];
    static int next;
    char *path = paths[next++ % 4];
    snprintf(path, sizeof(paths[0]), "%s/%s", base, relative);
    return path;
}

static void put(const char *relative, const char *text)
{
    FILE *file = fopen(at(relative), "wb");
    assert(file && fwrite(text, 1, strlen(text), file) == strlen(text) && fclose(file) == 0);
}

/* The whole file, or "" when it is missing. */
static const char *get(const char *relative)
{
    static char text[8192];
    FILE *file = fopen(at(relative), "rb");
    size_t got = file ? fread(text, 1, sizeof(text) - 1, file) : 0;
    if (file) fclose(file);
    text[got] = 0;
    return text;
}

static int exists(const char *relative)
{
    struct stat state;
    return stat(at(relative), &state) == 0;
}

static void folders(const char *const *names)
{
    for (; *names; names++) assert(mkdir(at(*names), 0777) == 0 || errno == EEXIST);
}

/* An alpha-2 console prefix: its registry, the profile folders wineboot and the title made, a save, Public. */
static void old_prefix(void)
{
    static const char *const made[] = {"drive_c", "drive_c/users", "drive_c/users/Public",
        "drive_c/users/Public/Documents", "drive_c/users/steamuser", "drive_c/users/steamuser/AppData",
        "drive_c/users/steamuser/Documents", "drive_c/users/steamuser/Documents/My Games",
        "drive_c/users/steamuser/Saved Games", NULL};
    char cleanup[128];
    snprintf(cleanup, sizeof(cleanup), "rm -rf '%s'/* '%s'/.pw-*", base, base);
    assert(system(cleanup) == 0);
    folders(made);
    put("system.reg", system_reg);
    put("user.reg", user_reg);
    put("userdef.reg", userdef_reg);
    put("drive_c/users/steamuser/Documents/My Games/save.dat", "day 12, 3 hearts");
    put("drive_c/users/steamuser/Saved Games/slot1", "slot one");
}

static void assert_moved(void)
{
    assert(!exists("drive_c/users/steamuser"));
    assert(!strcmp(get("drive_c/users/Mihawk/Documents/My Games/save.dat"), "day 12, 3 hearts"));
    assert(!strcmp(get("drive_c/users/Mihawk/Saved Games/slot1"), "slot one"));
    assert(exists("drive_c/users/Public/Documents"));
    assert(!strcmp(get("drive_c/users/.pw-symlinks"), "steamuser\tMihawk\n"));
    assert(!strcmp(get("user.reg"), user_reg_moved));
    assert(!strstr(get("system.reg"), "steamuser") && strstr(get("system.reg"), "\"ProfileImagePath\"=\"C:\\\\users\\\\Mihawk\""));
    assert(!strstr(get("userdef.reg"), "steamuser"));
    assert(!exists("user.reg.pw-profile-new") && !exists("drive_c/users/.pw-symlinks.pw-profile-new"));
    assert(!strcmp(get(PW_WINE_PROFILE_MARKER), "Mihawk\n"));
}

static void test_rewrite(void)
{
    char text[sizeof(user_reg)];
    size_t replaced = 99;
    memcpy(text, user_reg, sizeof(user_reg));
    size_t length = pw_wine_profile_rewrite(text, sizeof(user_reg) - 1, &replaced);
    text[length] = 0;
    assert(!strcmp(text, user_reg_moved) && replaced == 13);
    assert(length == sizeof(user_reg) - 1 - 13 * 3);
    /* Once moved, nothing more is. */
    assert(pw_wine_profile_rewrite(text, length, &replaced) == length && replaced == 0);
    /* The name at the very end (no quote after it) and at the start are not a profile's. */
    char edge[] = "\\\\users\\\\steamuser";
    assert(pw_wine_profile_rewrite(edge, sizeof(edge) - 1, &replaced) == sizeof(edge) - 1 && replaced == 0);
    char start[] = "steamuser\"";
    assert(pw_wine_profile_rewrite(start, sizeof(start) - 1, NULL) == sizeof(start) - 1);
}

static void test_migrate(void)
{
    PwWineProfileMove move;
    old_prefix();
    assert(pw_wine_profile_migrate(base, &move) == 0);
    assert(!move.already && move.moved == 1 && !move.kept && move.linked && move.files == 3 && move.replaced == 16);
    assert_moved();
    /* The marker ends it: the next start looks at nothing. */
    assert(pw_wine_profile_migrate(base, &move) == 0 && move.already && !move.moved && !move.files);
    assert_moved();
}

/* Each state a start can find after a move stopped (a crash, power lost, an I/O error), and the end it reaches. */
static void test_interrupted(void)
{
    PwWineProfileMove move;

    /* Stopped after the folder's rename: the link, the registry and the marker are still to do. */
    old_prefix();
    assert(rename(at("drive_c/users/steamuser"), at("drive_c/users/Mihawk")) == 0);
    assert(pw_wine_profile_migrate(base, &move) == 0 && move.moved == 0 && move.linked && move.files == 3);
    assert_moved();

    /* Stopped in the middle of a merge (a deploy had made Mihawk's empty folders): part moved, part not. */
    old_prefix();
    static const char *const skeleton[] = {"drive_c/users/Mihawk", "drive_c/users/Mihawk/Documents",
        "drive_c/users/Mihawk/Downloads", "drive_c/users/Mihawk/Saved Games", NULL};
    folders(skeleton);
    assert(rename(at("drive_c/users/steamuser/Saved Games/slot1"), at("drive_c/users/Mihawk/Saved Games/slot1")) == 0);
    assert(pw_wine_profile_migrate(base, &move) == 0 && move.moved == 2 && !move.kept && move.linked);
    assert_moved();
    assert(exists("drive_c/users/Mihawk/Downloads"));

    /* Stopped while rewriting the registry: system.reg done, user.reg's new copy half written, the link there. */
    old_prefix();
    assert(rename(at("drive_c/users/steamuser"), at("drive_c/users/Mihawk")) == 0);
    put("drive_c/users/.pw-symlinks", "steamuser\tMihawk\n");
    char text[sizeof(system_reg)];
    memcpy(text, system_reg, sizeof(system_reg));
    text[pw_wine_profile_rewrite(text, sizeof(system_reg) - 1, NULL)] = 0;
    put("system.reg", text);
    put("user.reg.pw-profile-new", "WINE REGISTRY Version 2\n[Envir");
    assert(pw_wine_profile_migrate(base, &move) == 0 && move.files == 2 && move.linked);
    assert_moved();

    /* Stopped before the marker: every step finds itself done. */
    assert(unlink(at(PW_WINE_PROFILE_MARKER)) == 0);
    assert(pw_wine_profile_migrate(base, &move) == 0 && !move.already && !move.moved && move.linked && !move.files);
    assert_moved();
    /* A torn marker is no marker. */
    put(PW_WINE_PROFILE_MARKER, "Mih");
    assert(pw_wine_profile_migrate(base, &move) == 0 && !move.already);
    assert_moved();
}

static void test_conflicts_and_tables(void)
{
    PwWineProfileMove move;

    /* A file both profiles hold: Mihawk's stays, steamuser's is kept where it was, so the old name is no link. */
    old_prefix();
    static const char *const newer[] = {"drive_c/users/Mihawk", "drive_c/users/Mihawk/Documents",
        "drive_c/users/Mihawk/Documents/My Games", NULL};
    folders(newer);
    put("drive_c/users/Mihawk/Documents/My Games/save.dat", "day 1");
    assert(pw_wine_profile_migrate(base, &move) == 0 && move.kept == 1 && !move.linked && move.files == 3);
    assert(!strcmp(get("drive_c/users/Mihawk/Documents/My Games/save.dat"), "day 1"));
    assert(!strcmp(get("drive_c/users/steamuser/Documents/My Games/save.dat"), "day 12, 3 hearts"));
    assert(!strcmp(get("drive_c/users/Mihawk/Saved Games/slot1"), "slot one"));
    assert(!exists("drive_c/users/steamuser/Saved Games") && !exists("drive_c/users/.pw-symlinks"));
    assert(!strcmp(get("user.reg"), user_reg_moved) && !strcmp(get(PW_WINE_PROFILE_MARKER), "Mihawk\n"));

    /* A table with other links keeps them; one already naming steamuser is left as it is. */
    old_prefix();
    put("drive_c/users/.pw-symlinks", "games\t/data/games");
    assert(pw_wine_profile_migrate(base, &move) == 0 && move.linked);
    assert(!strcmp(get("drive_c/users/.pw-symlinks"), "games\t/data/games\nsteamuser\tMihawk\n"));
    old_prefix();
    put("drive_c/users/.pw-symlinks", "steamuser\tMihawk\ngames\t/data/games\n");
    assert(pw_wine_profile_migrate(base, &move) == 0 && move.linked);
    assert(!strcmp(get("drive_c/users/.pw-symlinks"), "steamuser\tMihawk\ngames\t/data/games\n"));

    /* A prefix the title is about to make: Mihawk's from the start, so only the marker, and no link. */
    old_prefix();
    char cleanup[128];
    snprintf(cleanup, sizeof(cleanup), "rm -rf '%s'/*", base);
    assert(system(cleanup) == 0);
    assert(pw_wine_profile_migrate(base, &move) == 0 && !move.moved && !move.linked && !move.files);
    assert(!exists("drive_c") && !strcmp(get(PW_WINE_PROFILE_MARKER), "Mihawk\n"));

    /* Refusals name their step. */
    assert(pw_wine_profile_migrate(NULL, &move) == -1 && errno == EINVAL);
    assert(pw_wine_profile_migrate(base, NULL) == -1 && errno == EINVAL);
    old_prefix();
    assert(chmod(at("drive_c/users"), 0500) == 0);
    if (geteuid() != 0) {
        assert(pw_wine_profile_migrate(base, &move) == -1 && !strcmp(move.step, "folder"));
        assert(exists("drive_c/users/steamuser/Documents/My Games/save.dat") && !exists(PW_WINE_PROFILE_MARKER));
    }
    assert(chmod(at("drive_c/users"), 0755) == 0);
    assert(pw_wine_profile_migrate(base, &move) == 0);
    assert_moved();
}

/* A copy of a real console prefix's registry: every reference moves, the files keep every other byte. */
static size_t test_real_registry(void)
{
    const char *folder = getenv("PW_PROFILE_REAL_REGISTRY");
    static const char *const names[] = {"system.reg", "user.reg", "userdef.reg"};
    char command[2048];
    PwWineProfileMove move;
    size_t before = 0, after = 0;
    struct stat state;

    if (!folder || !*folder) return 0;
    old_prefix();
    for (size_t i = 0; i < 3; i++) {
        snprintf(command, sizeof(command), "%s/%s", folder, names[i]);
        if (stat(command, &state) != 0) { assert(unlink(at(names[i])) == 0); continue; }
        before += (size_t)state.st_size;
        snprintf(command, sizeof(command), "cp '%s/%s' '%s'", folder, names[i], at(names[i]));
        assert(system(command) == 0);
    }
    assert(pw_wine_profile_migrate(base, &move) == 0 && move.moved == 1 && move.linked && move.replaced > 0);
    for (size_t i = 0; i < 3; i++) {
        if (stat(at(names[i]), &state) != 0) continue;
        after += (size_t)state.st_size;
        snprintf(command, sizeof(command), "grep -qi 'users\\\\\\\\steamuser' '%s'", at(names[i]));
        assert(system(command) != 0);
    }
    assert(before - after == move.replaced * 3);
    printf("real registry: %zu bytes in %u files, %zu references moved\n", before, move.files, move.replaced);
    return move.replaced;
}

int main(void)
{
    assert(mkdtemp(base));
    test_rewrite();
    test_migrate();
    test_interrupted();
    test_conflicts_and_tables();
    test_real_registry();
    char cleanup[128];
    snprintf(cleanup, sizeof(cleanup), "rm -rf '%s'", base);
    assert(system(cleanup) == 0);
    printf("wine profile passed: steamuser -> Mihawk in registry text (paths, quoted name, case, slashes; hex and "
           "look-alikes kept), a whole alpha-2 prefix moved once, every interrupted state finished, a file both "
           "profiles hold kept in both, other links kept, a new prefix marked, refusals named\n");
    return 0;
}
