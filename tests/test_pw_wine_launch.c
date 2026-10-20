/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/pw_wine_launch.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static const PwWineApp apps[] = {
    { "pinball", "Space Cadet Pinball", "Windows XP", "C:\\Games\\Pinball\\PINBALL.EXE" },
    { "notepad", "Notepad", "Wine", "C:\\windows\\notepad.exe" },
};
enum { COUNT = sizeof(apps) / sizeof(apps[0]) };

static PwWineLaunch parse(int argc, char *const *argv)
{
    PwWineLaunch launch;
    assert(!pw_wine_launch_parse(argc, argv, apps, COUNT, &launch));
    return launch;
}

static void test_parse(void)
{
    char *system[] = { "" };
    char *profile[] = { "profile=pinball", "cycle=2" };
    char *path[] = { "path=D:\\Other\\GAME.EXE" };
    char *both[] = { "path=C:\\Games\\Pinball\\DEBUG.EXE", "profile=pinball" };
    char *unknown[] = { "profile=missing" };
    char *relative[] = { "path=Games\\PINBALL.EXE" };
    char *short_path[] = { "path=C:" };
    char *lower[] = { "path=c:\\x.exe", "noise", "cycle=12x" };
    char *launcher[] = { "launcher=1", "cycle=4294967295" };
    char *overflow[] = { "cycle=4294967296" };
    char *nulls[] = { NULL, "profile=notepad" };
    char long_path[PW_WINE_LAUNCH_PATH_MAX + 8] = "path=C:\\";
    char *too_long[] = { long_path };
    PwWineLaunch l;

    /* The system starts the title with one empty argument: the launcher. */
    l = parse(1, system);
    assert(l.mode == PW_WINE_LAUNCH_LAUNCHER && !l.app && !l.refused && !l.cycle);
    l = parse(0, NULL);
    assert(l.mode == PW_WINE_LAUNCH_LAUNCHER);

    l = parse(2, profile);
    assert(l.mode == PW_WINE_LAUNCH_GAME && l.app == &apps[0] && l.cycle == 2);
    assert(!strcmp(l.executable, "C:\\Games\\Pinball\\PINBALL.EXE"));
    l = parse(1, path);
    assert(l.mode == PW_WINE_LAUNCH_GAME && !l.app && !strcmp(l.executable, "D:\\Other\\GAME.EXE"));
    l = parse(2, both);
    assert(l.app == &apps[0] && !strcmp(l.executable, "C:\\Games\\Pinball\\DEBUG.EXE"));
    l = parse(2, nulls);
    assert(l.app == &apps[1]);
    l = parse(3, lower);
    assert(l.mode == PW_WINE_LAUNCH_GAME && !l.cycle);
    l = parse(2, launcher);
    assert(l.mode == PW_WINE_LAUNCH_LAUNCHER && l.cycle == 4294967295u);
    l = parse(1, overflow);
    assert(!l.cycle);

    /* Refusals fall back to the launcher and say so. */
    l = parse(1, unknown);
    assert(l.mode == PW_WINE_LAUNCH_LAUNCHER && l.refused && !l.app);
    l = parse(1, relative);
    assert(l.mode == PW_WINE_LAUNCH_LAUNCHER && l.refused);
    l = parse(1, short_path);
    assert(l.mode == PW_WINE_LAUNCH_LAUNCHER && l.refused);
    memset(long_path + 8, 'a', PW_WINE_LAUNCH_PATH_MAX);
    long_path[sizeof(long_path) - 1] = 0;
    l = parse(1, too_long);
    assert(l.mode == PW_WINE_LAUNCH_LAUNCHER && l.refused);

    assert(pw_wine_launch_parse(1, system, apps, COUNT, NULL) == -1);
    assert(pw_wine_launch_parse(1, system, NULL, 1, &l) == -1);
    assert(!pw_wine_launch_parse(1, profile, NULL, 0, &l) && l.refused);
}

static void test_argv(void)
{
    char storage[256], tiny[12], *argv[PW_WINE_LAUNCH_ARGS];
    PwWineApp broken = { NULL, "x", "x", "C:\\x.exe" };
    PwWineLaunch back;

    assert(pw_wine_launch_argv(&apps[0], 0, storage, sizeof(storage), argv, 4) == 3);
    assert(!strcmp(argv[0], "profile=pinball") && !strcmp(argv[1], "path=C:\\Games\\Pinball\\PINBALL.EXE"));
    assert(!strcmp(argv[2], "cycle=0") && !argv[3]);
    /* What the title builds, it parses back. */
    assert(!pw_wine_launch_parse(3, argv, apps, COUNT, &back));
    assert(back.mode == PW_WINE_LAUNCH_GAME && back.app == &apps[0] && !back.cycle);

    assert(pw_wine_launch_argv(NULL, 4294967295u, storage, sizeof(storage), argv, 4) == 2);
    assert(!strcmp(argv[0], "launcher=1") && !strcmp(argv[1], "cycle=4294967295") && !argv[2]);
    assert(!pw_wine_launch_parse(2, argv, apps, COUNT, &back));
    assert(back.mode == PW_WINE_LAUNCH_LAUNCHER && back.cycle == 4294967295u && !back.refused);
    assert(pw_wine_launch_argv(NULL, 17, storage, sizeof(storage), argv, 4) == 2 &&
           !strcmp(argv[1], "cycle=17"));

    assert(!pw_wine_launch_argv(&apps[0], 0, tiny, sizeof(tiny), argv, 4));
    assert(!pw_wine_launch_argv(&apps[0], 0, storage, 30, argv, 4));
    assert(!pw_wine_launch_argv(NULL, 0, tiny, 11, argv, 4));
    assert(!pw_wine_launch_argv(&apps[0], 0, storage, sizeof(storage), argv, 3));
    assert(!pw_wine_launch_argv(&broken, 0, storage, sizeof(storage), argv, 4));
    assert(!pw_wine_launch_argv(&apps[0], 0, NULL, sizeof(storage), argv, 4));
    assert(!pw_wine_launch_argv(&apps[0], 0, storage, sizeof(storage), NULL, 4));
}


static void test_split(void)
{
    char storage[64], small[6];
    const char *words[PW_WINE_LAUNCH_WORDS];

    /* 7-Zip's benchmark line: spaces and tabs separate, runs collapse. */
    assert(pw_wine_launch_split("  b -mmt1\t-md22 ", storage, sizeof(storage), words, 8) == 3);
    assert(!strcmp(words[0], "b") && !strcmp(words[1], "-mmt1") && !strcmp(words[2], "-md22"));
    /* Quotes group a word and are dropped, also inside a word. */
    assert(pw_wine_launch_split("x \"C:\\My Games\\a.dat\" -o\"out dir\" \"\"", storage,
                                sizeof(storage), words, 8) == 4);
    assert(!strcmp(words[1], "C:\\My Games\\a.dat") && !strcmp(words[2], "-oout dir") &&
           !strcmp(words[3], ""));
    assert(pw_wine_launch_split("", storage, sizeof(storage), words, 8) == 0);
    assert(pw_wine_launch_split(" \t ", storage, sizeof(storage), words, 0) == 0);

    /* Refusals: an unclosed quote, too many words, storage exhausted. */
    assert(pw_wine_launch_split("a \"b", storage, sizeof(storage), words, 8) == -1);
    assert(pw_wine_launch_split("a b c", storage, sizeof(storage), words, 2) == -1);
    assert(pw_wine_launch_split("abcdef", small, sizeof(small), words, 8) == -1);
    assert(pw_wine_launch_split("ab cde", small, sizeof(small), words, 8) == -1);
    assert(pw_wine_launch_split("ab c", small, sizeof(small), words, 8) == 2 &&
           !strcmp(words[1], "c"));
    assert(pw_wine_launch_split(NULL, storage, sizeof(storage), words, 8) == -1);
    assert(pw_wine_launch_split("a", NULL, sizeof(storage), words, 8) == -1);
    assert(pw_wine_launch_split("a", storage, sizeof(storage), NULL, 8) == -1);
}

/* A profile's working directory, mapped into its prefix. */
static int make_dir(const char *path, void *context)
{
    (*(int *)context)++;
    return mkdir(path, 0777) == 0 || errno == EEXIST ? 0 : -1;
}

static void test_profile_dirs(void)
{
    char base[] = "/tmp/pw-profile-XXXXXX", path[512];
    struct stat status;
    int calls = 0;
    assert(mkdtemp(base));
    snprintf(path, sizeof(path), "%s/drive_c", base);
    assert(mkdir(path, 0777) == 0);
    /* Parents come before children, so one pass makes the whole tree; a second pass changes nothing. */
    assert(pw_wine_launch_profile_dirs(base, make_dir, &calls) == 0 && calls == 24);
    snprintf(path, sizeof(path), "%s/drive_c/users/steamuser/AppData/Roaming", base);
    assert(stat(path, &status) == 0 && S_ISDIR(status.st_mode));
    snprintf(path, sizeof(path), "%s/drive_c/users/steamuser/Saved Games", base);
    assert(stat(path, &status) == 0 && S_ISDIR(status.st_mode));
    snprintf(path, sizeof(path), "%s/drive_c/users/Public/Videos", base);
    assert(stat(path, &status) == 0 && S_ISDIR(status.st_mode));
    assert(pw_wine_launch_profile_dirs(base, make_dir, &calls) == 0 && calls == 48);
    /* Without drive_c nothing can be made, and every failure is counted. */
    calls = 0;
    snprintf(path, sizeof(path), "%s/no-such-prefix", base);
    assert(pw_wine_launch_profile_dirs(path, make_dir, &calls) == 24 && calls == 24);
    assert(pw_wine_launch_profile_dirs(NULL, make_dir, &calls) == -1);
    char cleanup[600];
    snprintf(cleanup, sizeof(cleanup), "rm -rf '%s'", base);
    assert(system(cleanup) == 0);
}

static void test_host_dir(void)
{
    char out[64], small[16];

    assert(pw_wine_launch_host_dir("/data/pw/prefix", "C:\\Tools", out, sizeof(out)) == 0 &&
           !strcmp(out, "/data/pw/prefix/drive_c/Tools"));
    assert(pw_wine_launch_host_dir("/data/pw/prefix", "c:\\Games\\Old One\\", out, sizeof(out)) == 0 &&
           !strcmp(out, "/data/pw/prefix/drive_c/Games/Old One"));
    assert(pw_wine_launch_host_dir("/p", "C:\\", out, sizeof(out)) == 0 && !strcmp(out, "/p/drive_c"));
    assert(pw_wine_launch_host_dir("/p", "Z:\\", out, sizeof(out)) == 0 && !strcmp(out, "/"));
    assert(pw_wine_launch_host_dir("/p", "z:\\tmp\\x", out, sizeof(out)) == 0 && !strcmp(out, "/tmp/x"));
    assert(pw_wine_launch_host_dir("/p", "D:\\Games", out, sizeof(out)) == -1);        /* no such drive */
    assert(pw_wine_launch_host_dir("/p", "C:", out, sizeof(out)) == -1);
    assert(pw_wine_launch_host_dir("/data/pw/prefix", "C:\\x", small, sizeof(small)) == -1);
    assert(pw_wine_launch_host_dir("/p", "C:\\a_long_directory", small, sizeof(small)) == -1);
    assert(pw_wine_launch_host_dir(NULL, "C:\\x", out, sizeof(out)) == -1);
    assert(pw_wine_launch_host_dir("/p", NULL, out, sizeof(out)) == -1);
    assert(pw_wine_launch_host_dir("/p", "C:\\x", NULL, sizeof(out)) == -1);
}

static void put(const char *path, const char *text)
{
    FILE *file = fopen(path, "wb");
    assert(file && fputs(text, file) >= 0 && fclose(file) == 0);
}

static int same(const char *path, const char *text)
{
    char buffer[256] = {0};
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    const size_t got = fread(buffer, 1, sizeof(buffer) - 1, file);
    fclose(file);
    return got == strlen(text) && !memcmp(buffer, text, got);
}

static void test_sync(void)
{
    char root[] = "/tmp/pw-sync-XXXXXX", from[64], to[64], path[128];
    assert(mkdtemp(root));
    snprintf(from, sizeof(from), "%s/runtime", root);
    snprintf(to, sizeof(to), "%s/system32", root);
    assert(!mkdir(from, 0755));
    const char *const names[] = {"opengl32.dll", "libgallium_wgl.dll"};
    snprintf(path, sizeof(path), "%s/opengl32.dll", from); put(path, "mesa opengl32");
    snprintf(path, sizeof(path), "%s/libgallium_wgl.dll", from); put(path, "mesa gallium");
    snprintf(path, sizeof(path), "%s/version", from); put(path, "one\n");

    /* No system32 yet (a new prefix): nothing is made. */
    errno = 0;
    assert(pw_wine_launch_sync(from, to, names, 2, "version") == -1);
    assert(mkdir(to, 0755) == 0);
    /* Wine's placeholder opengl32.dll is replaced; the stamp is written. */
    snprintf(path, sizeof(path), "%s/opengl32.dll", to); put(path, "wine placeholder, another size");
    assert(pw_wine_launch_sync(from, to, names, 2, "version") == 3);
    snprintf(path, sizeof(path), "%s/opengl32.dll", to); assert(same(path, "mesa opengl32"));
    snprintf(path, sizeof(path), "%s/libgallium_wgl.dll", to); assert(same(path, "mesa gallium"));
    snprintf(path, sizeof(path), "%s/version", to); assert(same(path, "one\n"));
    snprintf(path, sizeof(path), "%s/opengl32.dll.proton-new", to); assert(access(path, F_OK) != 0);
    /* Current: nothing is copied. */
    assert(pw_wine_launch_sync(from, to, names, 2, "version") == 0);
    /* A file put back at another size (wineboot's placeholder) is copied again. */
    snprintf(path, sizeof(path), "%s/libgallium_wgl.dll", to); put(path, "x");
    assert(pw_wine_launch_sync(from, to, names, 2, "version") == 3);
    assert(same(path, "mesa gallium"));
    /* A new runtime version is copied even at the same sizes. */
    snprintf(path, sizeof(path), "%s/opengl32.dll", from); put(path, "MESA OPENGL32");
    snprintf(path, sizeof(path), "%s/version", from); put(path, "two\n");
    assert(pw_wine_launch_sync(from, to, names, 2, "version") == 3);
    snprintf(path, sizeof(path), "%s/opengl32.dll", to); assert(same(path, "MESA OPENGL32"));
    /* A runtime without the files or the stamp refuses. */
    snprintf(path, sizeof(path), "%s/version", from); unlink(path);
    assert(pw_wine_launch_sync(from, to, names, 2, "version") == -1);
    assert(pw_wine_launch_sync(NULL, to, names, 2, "version") == -1);

    const char *const files[] = {"runtime/opengl32.dll", "runtime/libgallium_wgl.dll", "system32/opengl32.dll",
                                 "system32/libgallium_wgl.dll", "system32/version"};
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        snprintf(path, sizeof(path), "%s/%s", root, files[i]); unlink(path);
    }
    rmdir(to); rmdir(from); rmdir(root);
}

static void test_prefix_id(void)
{
    char id[65], long_id[66];
    /* Every catalog game is in Mihawk OS, whatever folders exist; an explicit prefix still wins. */
    assert(!pw_wine_launch_prefix_id(NULL, "new-game", id, sizeof(id)) && !strcmp(id, "mihawk"));
    assert(!pw_wine_launch_prefix_id("", "7", id, sizeof(id)) && !strcmp(id, "mihawk"));
    assert(!pw_wine_launch_prefix_id("work_2", "old-game", id, sizeof(id)) && !strcmp(id, "work_2"));
    assert(!pw_wine_launch_prefix_id(NULL, NULL, id, sizeof(id)) && !strcmp(id, "probe"));
    assert(!pw_wine_launch_prefix_id(NULL, "", id, sizeof(id)) && !strcmp(id, "probe"));
    const char *bad[] = {"../game", "/tmp", "a/b", "a\\b", "a:b", "a b", ".", ".."};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        assert(pw_wine_launch_prefix_id(bad[i], "game", id, sizeof(id)) == -1 && !*id);
        /* a game's catalog folder name never becomes a prefix path */
        assert(!pw_wine_launch_prefix_id(NULL, bad[i], id, sizeof(id)) && !strcmp(id, "mihawk"));
    }
    memset(long_id, 'a', 64); long_id[64] = 0;
    assert(!pw_wine_launch_prefix_id(long_id, "game", id, sizeof(id)));
    long_id[64] = 'a'; long_id[65] = 0;
    assert(pw_wine_launch_prefix_id(long_id, "game", id, sizeof(id)) == -1);
    assert(pw_wine_launch_prefix_id(NULL, "game", id, 6) == -1 && !*id);
    assert(pw_wine_launch_prefix_id(NULL, "game", NULL, 65) == -1);
}

int main(void)
{
    test_prefix_id();
    test_parse();
    test_argv();
    test_split();
    test_profile_dirs();
    test_host_dir();
    test_sync();
    printf("wine launch passed: launcher on no game, profile and path, overrides, refusals, "
           "cycle counts, LoadExec argv round trip, bounded storage, "
           "argument words, working directory in the prefix, runtime files synced into a prefix\n");
    return 0;
}
