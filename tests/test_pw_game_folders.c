/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/pw_game_folders.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* A PE header just whole enough for src/pw_pe_arch.c: DOS header, "PE", machine, an optional header's magic. */
static void program(const char *path, unsigned machine)
{
    unsigned char b[0x200] = {'M', 'Z'};
    b[0x3c] = 0x80;
    memcpy(b + 0x80, "PE\0\0", 4);
    b[0x84] = (unsigned char)machine;
    b[0x85] = (unsigned char)(machine >> 8);
    b[0x94] = 240;                                  /* optional header size */
    b[0x98] = 0x0b;
    b[0x99] = machine == 0x14c ? 0x01 : 0x02;       /* PE32 or PE32+ */
    FILE *file = fopen(path, "wb");
    assert(file && fwrite(b, 1, sizeof(b), file) == sizeof(b) && !fclose(file));
}

static void text(const char *path)
{
    FILE *file = fopen(path, "w");
    assert(file && fputs("not a program\n", file) >= 0 && !fclose(file));
}

static char root[256];

static const char *at(const char *relative)
{
    static char path[512];
    snprintf(path, sizeof(path), "%s/%s", root, relative);
    return path;
}

static struct PwGameFolder seen[16];
static unsigned count;

static void found(void *context, const struct PwGameFolder *folder)
{
    assert(context == &count && count < 16);
    seen[count++] = *folder;
}

static const struct PwGameFolder *folder_named(const char *name)
{
    for (unsigned i = 0; i < count; i++)
        if (!strcmp(seen[i].name, name)) return &seen[i];
    return NULL;
}

int main(void)
{
    snprintf(root, sizeof(root), "%s/pw-game-folders-XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    assert(mkdtemp(root));
    assert(pw_game_folders_scan(at("Games"), found, &count) == ENOENT && !count);   /* no C:\Games yet */

    assert(!mkdir(at("Games"), 0777));
    /* One x86-64 program beside a GOG uninstaller, Unity's crash handler and a text file: one game. */
    assert(!mkdir(at("Games/Example Game"), 0777));
    program(at("Games/Example Game/example_game.exe"), 0x8664);
    program(at("Games/Example Game/unins000.exe"), 0x14c);
    program(at("Games/Example Game/UnityCrashHandler64.exe"), 0x8664);
    text(at("Games/Example Game/readme.txt"));
    /* One 32-bit program, its name in capitals. */
    assert(!mkdir(at("Games/Old Game"), 0777));
    program(at("Games/Old Game/GAME.EXE"), 0x14c);
    /* Two programs: the title cannot tell which is the game. */
    assert(!mkdir(at("Games/Two"), 0777));
    program(at("Games/Two/game.exe"), 0x8664);
    program(at("Games/Two/editor.exe"), 0x8664);
    /* Only an ARM64 program, and a ".exe" that is not a program: nothing runs here. */
    assert(!mkdir(at("Games/Arm"), 0777));
    program(at("Games/Arm/game.exe"), 0xaa64);
    text(at("Games/Arm/fake.exe"));
    /* The program only in a subfolder. */
    assert(!mkdir(at("Games/Deep"), 0777));
    assert(!mkdir(at("Games/Deep/bin"), 0777));
    program(at("Games/Deep/bin/game.exe"), 0x8664);
    /* A file at the top of C:\Games and a hidden folder are not games. */
    program(at("Games/loose.exe"), 0x8664);
    assert(!mkdir(at("Games/.cache"), 0777));

    assert(pw_game_folders_scan(at("Games"), found, &count) == 0 && count == 5);
    const struct PwGameFolder *eg = folder_named("Example Game"), *old = folder_named("Old Game");
    assert(eg && eg->programs == 1 && !strcmp(eg->program, "example_game.exe"));
    assert(old && old->programs == 1 && !strcmp(old->program, "GAME.EXE"));
    assert(folder_named("Two") && folder_named("Two")->programs == 2);
    assert(folder_named("Arm") && folder_named("Arm")->programs == 0);
    assert(folder_named("Deep") && folder_named("Deep")->programs == 0);

    /* A library entry's executable covers its folder: any case, either separator, never a folder sharing a prefix. */
    assert(pw_game_folder_covers("Example Game", "C:\\Games\\Example Game\\example_game.exe"));
    assert(pw_game_folder_covers("Example Game", "c:/games/example game/bin/game.exe"));
    assert(!pw_game_folder_covers("Example", "C:\\Games\\Example Game\\example_game.exe"));
    assert(!pw_game_folder_covers("Example Game", "C:\\Games\\Example Game"));
    assert(!pw_game_folder_covers("Example Game", "Z:\\Games\\Example Game\\game.exe"));
    assert(!pw_game_folder_covers("Example Game", "C:\\Gam"));
    assert(!pw_game_folder_covers("Example Game", ""));

    char command[600];
    snprintf(command, sizeof(command), "rm -rf '%s'", root);
    assert(!system(command));
    puts("test_pw_game_folders: ok");
    return 0;
}
