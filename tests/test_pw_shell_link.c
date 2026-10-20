/* SPDX-License-Identifier: GPL-3.0-or-later
 * src/pw_shell_link.h: the .lnk bytes, file names and the desktop folder's sync.
 *
 *   test_pw_shell_link                 the checks (tools/host-tests.sh)
 *   test_pw_shell_link --samples DIR   writes DIR/<sample>.lnk and DIR/<sample>.expected, what Wine's reader must
 *                                      print for it (tools/check-shell-link.sh, tests/shell-link-oracle.c) */
#include "../src/pw_shell_link.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <uchar.h>
#include <unistd.h>

static uint32_t u32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t u16(const unsigned char *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

/* A StringData string at *p against the compiler's own UTF-16 (char16_t literals): the oracle for the encoder. */
static void expect_string(const unsigned char **p, const char16_t *expected)
{
    size_t units = 0;
    while (expected[units]) ++units;
    assert(u16(*p) == units);
    for (size_t i = 0; i < units; ++i) assert(u16(*p + 2 + 2 * i) == expected[i]);
    *p += 2 + 2 * units;
}

static const unsigned char clsid[16] = {0x01, 0x14, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
                                        0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46};

static unsigned char big[70000];
static char long_text[40000];

static void test_layout(void)
{
    const char *target = "C:\\Games\\Example Game\\Example Game.exe";
    PwShellLink link = {
        .target = target,
        .arguments = "--name \"Caf\xc3\xa9\" \xf0\x9f\x98\x80",  /* U+00E9 and U+1F600, a surrogate pair */
        .working_directory = "C:\\Games\\Example Game",
        .icon_index = 0,
        .description = "Example Game",
    };
    unsigned char out[4096];
    size_t bytes = 0, target_bytes = strlen(target);
    assert(!pw_shell_link_build(&link, out, sizeof out, &bytes));

    /* ShellLinkHeader */
    assert(u32(out) == 0x4C && PW_SHELL_LINK_HEADER_BYTES == 0x4C);
    assert(!memcmp(out + 4, clsid, sizeof clsid));
    assert(u32(out + 0x14) == 0xF6);  /* HasLinkInfo HasName HasWorkingDir HasArguments HasIconLocation IsUnicode */
    assert(u32(out + 0x14) == (PW_SHELL_LINK_HAS_LINK_INFO | PW_SHELL_LINK_HAS_NAME | PW_SHELL_LINK_HAS_WORKING_DIR |
                               PW_SHELL_LINK_HAS_ARGUMENTS | PW_SHELL_LINK_HAS_ICON_LOCATION |
                               PW_SHELL_LINK_IS_UNICODE));
    assert(u32(out + 0x18) == 0x80);  /* FILE_ATTRIBUTE_NORMAL */
    for (size_t i = 0x1C; i < 0x38; ++i) assert(!out[i]);  /* three FILETIMEs and FileSize */
    assert(u32(out + 0x38) == 0);     /* IconIndex */
    assert(u32(out + 0x3C) == 1);     /* SW_SHOWNORMAL */
    for (size_t i = 0x40; i < 0x4C; ++i) assert(!out[i]);  /* HotKey and the reserved fields */

    /* LinkInfo: no LinkTargetIDList before it */
    const unsigned char *info = out + 0x4C;
    assert(u32(info) == 0x1C + 0x11 + target_bytes + 1 + 1);
    assert(u32(info + 0x04) == 0x1C);
    assert(u32(info + 0x08) == 1);   /* VolumeIDAndLocalBasePath */
    assert(u32(info + 0x0C) == 0x1C);
    assert(u32(info + 0x10) == 0x2D);
    assert(u32(info + 0x14) == 0);   /* no CommonNetworkRelativeLink */
    assert(u32(info + 0x18) == 0x2D + target_bytes + 1);
    assert(u32(info + 0x1C) == 0x11 && u32(info + 0x20) == 3 && u32(info + 0x24) == 0 && u32(info + 0x28) == 0x10);
    assert(info[0x2C] == 0);         /* the empty volume label */
    assert(!memcmp(info + 0x2D, target, target_bytes) && info[0x2D + target_bytes] == 0);
    assert(info[0x2D + target_bytes + 1] == 0);  /* the empty CommonPathSuffix */

    /* StringData in Wine's order, then the terminal block */
    const unsigned char *p = info + u32(info);
    expect_string(&p, u"Example Game");
    expect_string(&p, u"C:\\Games\\Example Game");
    expect_string(&p, u"--name \"Caf\u00e9\" \U0001F600");
    expect_string(&p, u"C:\\Games\\Example Game\\Example Game.exe");  /* the icon location: the target */
    assert(p + 4 == out + bytes && u32(p) == 0);

    /* The size query and a capacity one byte short: ENOSPC, the size needed, out untouched. */
    size_t needed = 0;
    assert(pw_shell_link_build(&link, NULL, 0, &needed) == ENOSPC && needed == bytes);
    unsigned char short_out[4096];
    memset(short_out, 0xAA, sizeof short_out);
    needed = 0;
    assert(pw_shell_link_build(&link, short_out, bytes - 1, &needed) == ENOSPC && needed == bytes);
    for (size_t i = 0; i < sizeof short_out; ++i) assert(short_out[i] == 0xAA);
    assert(!pw_shell_link_build(&link, short_out, bytes, &needed) && needed == bytes && !memcmp(short_out, out, bytes));

    /* Target only: LinkInfo, Unicode and the icon location, nothing else; a negative icon index (a resource id). */
    PwShellLink bare = {.target = "D:/Tools/run.exe", .arguments = "", .icon_index = -3};
    assert(!pw_shell_link_build(&bare, out, sizeof out, &bytes));
    assert(u32(out + 0x14) ==
           (PW_SHELL_LINK_HAS_LINK_INFO | PW_SHELL_LINK_IS_UNICODE | PW_SHELL_LINK_HAS_ICON_LOCATION));
    assert(u32(out + 0x38) == 0xFFFFFFFDu);
    p = out + 0x4C + u32(out + 0x4C);
    expect_string(&p, u"D:/Tools/run.exe");
    assert(p + 4 == out + bytes && u32(p) == 0);

    /* An icon path of its own, with an index. */
    PwShellLink icon = {.target = "C:\\a.exe", .icon_path = "C:\\Icons\\\xce\xa9.ico", .icon_index = 7};
    assert(!pw_shell_link_build(&icon, out, sizeof out, &bytes));
    assert(u32(out + 0x38) == 7);
    p = out + 0x4C + u32(out + 0x4C);
    expect_string(&p, u"C:\\Icons\\\u03a9.ico");
}

static void test_refusals(void)
{
    unsigned char out[512];
    size_t bytes = 12345;
    PwShellLink link = {.target = "C:\\Jeux\\Caf\xc3\xa9.exe"};
    assert(pw_shell_link_build(&link, out, sizeof out, &bytes) == EINVAL && bytes == 12345);  /* LinkInfo is ANSI */
    link.target = NULL;
    assert(pw_shell_link_build(&link, out, sizeof out, &bytes) == EINVAL);
    link.target = "";
    assert(pw_shell_link_build(&link, out, sizeof out, &bytes) == EINVAL);
    link.target = "Games\\relative.exe";
    assert(pw_shell_link_build(&link, out, sizeof out, &bytes) == EINVAL);
    link.target = "\\\\server\\share\\x.exe";
    assert(pw_shell_link_build(&link, out, sizeof out, &bytes) == EINVAL);
    link.target = "C:\\ok.exe";
    assert(pw_shell_link_build(NULL, out, sizeof out, &bytes) == EINVAL);
    assert(pw_shell_link_build(&link, out, sizeof out, NULL) == EINVAL);
    assert(pw_shell_link_build(&link, NULL, sizeof out, &bytes) == EINVAL);
    assert(bytes == 12345);

    static const char *const invalid[] = {
        "cut \xc3", "overlong \xc0\xaf", "overlong \xe0\x80\xaf", "surrogate \xed\xa0\x80", "beyond \xf4\x90\x80\x80",
        "stray \x80", "five \xf8\x88\x80\x80\x80", "cut \xf0\x9f\x98",
    };
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; ++i) {
        link.arguments = invalid[i];
        assert(pw_shell_link_build(&link, out, sizeof out, &bytes) == EILSEQ);
        link.arguments = NULL;
        link.description = invalid[i];
        assert(pw_shell_link_build(&link, out, sizeof out, &bytes) == EILSEQ);
        link.description = NULL;
    }

    /* 32767 units is Wine's limit (shelllink.c:503); a surrogate pair counts two. */
    memset(long_text, 'a', 32765);
    memcpy(long_text + 32765, "\xf0\x9f\x98\x80", 5);
    link.arguments = long_text;
    assert(!pw_shell_link_build(&link, big, sizeof big, &bytes));
    memcpy(long_text + 32765, "a\xf0\x9f\x98\x80", 6);
    assert(pw_shell_link_build(&link, big, sizeof big, &bytes) == ENAMETOOLONG);
    link.arguments = NULL;

    /* The icon location must fit Explorer's MAX_PATH: from icon_path, or from the target when there is none. */
    char path[400] = "C:\\";
    memset(path + 3, 'i', 256);  /* 259 units */
    link.icon_path = path;
    assert(!pw_shell_link_build(&link, big, sizeof big, &bytes));
    path[259] = 'i';             /* 260 */
    assert(pw_shell_link_build(&link, big, sizeof big, &bytes) == ENAMETOOLONG);
    link.icon_path = NULL;
    link.target = path;
    assert(pw_shell_link_build(&link, big, sizeof big, &bytes) == ENAMETOOLONG);
    link.icon_path = "C:\\short.ico";
    assert(!pw_shell_link_build(&link, big, sizeof big, &bytes));
}

static void name_is(const char *display_name, const char *expected)
{
    char out[PW_SHELL_LINK_NAME_MAX + 1];
    int error = pw_shell_link_file_name(display_name, out, sizeof out);
    if (error || strcmp(out, expected)) fprintf(stderr, "file name of \"%s\": %d \"%s\"\n", display_name, error, out);
    assert(!error && !strcmp(out, expected));
}

static void test_file_names(void)
{
    name_is("Example Game", "Example Game.lnk");
    name_is("Example Game 2: Episode One", "Example Game 2 Episode One.lnk");
    name_is("What? <Game> \"x\" a/b\\c|d*", "What Game x abcd.lnk");
    name_is("Tab\there\n\x1f", "Tabhere.lnk");
    name_is("Game...  . ", "Game.lnk");
    name_is("  Leading", "  Leading.lnk");
    name_is("Mr. Bean", "Mr. Bean.lnk");
    name_is("Caf\xc3\xa9 \xce\xa9 \xf0\x9f\x98\x80", "Caf\xc3\xa9 \xce\xa9 \xf0\x9f\x98\x80.lnk");
    /* Devices, as ntdll's RtlIsDosDeviceName_U sees them: the part before the first dot, trailing spaces trimmed. */
    name_is("con", "con (game).lnk");
    name_is("CON", "CON (game).lnk");
    name_is("Nul.txt", "Nul (game).txt.lnk");
    name_is("aux .x", "aux (game) .x.lnk");
    name_is("prn.", "prn (game).lnk");
    name_is("com1", "com1 (game).lnk");
    name_is("LPT9", "LPT9 (game).lnk");
    name_is("CONIN$", "CONIN$ (game).lnk");
    name_is("conout$", "conout$ (game).lnk");
    name_is("COM0", "COM0.lnk");
    name_is("COM10", "COM10.lnk");
    name_is("Console", "Console.lnk");
    name_is("Con: The Game", "Con The Game.lnk");

    char out[PW_SHELL_LINK_NAME_MAX + 1];
    assert(pw_shell_link_file_name("", out, sizeof out) == EINVAL);
    assert(pw_shell_link_file_name("???", out, sizeof out) == EINVAL);
    assert(pw_shell_link_file_name(". . .", out, sizeof out) == EINVAL);
    assert(pw_shell_link_file_name(NULL, out, sizeof out) == EINVAL);
    assert(pw_shell_link_file_name("\xff", out, sizeof out) == EILSEQ);
    assert(pw_shell_link_file_name("Game", out, 8) == ENOSPC);
    assert(!pw_shell_link_file_name("Game", out, 9) && !strcmp(out, "Game.lnk"));
    char name[400];
    memset(name, 'a', 251);
    name[251] = 0;
    assert(!pw_shell_link_file_name(name, out, sizeof out) && strlen(out) == 255);
    memset(name + 251, ' ', 20);  /* trimmed: still 255 */
    name[271] = 0;
    assert(!pw_shell_link_file_name(name, out, sizeof out) && strlen(out) == 255);
    name[251] = 'a';
    name[252] = 0;
    assert(pw_shell_link_file_name(name, out, sizeof out) == ENAMETOOLONG);
    memcpy(name, "con.", 4);      /* 252 bytes, a device: " (game)" would not fit */
    name[248] = 0;
    assert(pw_shell_link_file_name(name, out, sizeof out) == ENAMETOOLONG);
}

/* --- the sync, in a temporary directory --- */

static char root[512], folder[700], manifest[700];

/* "<directory>/<name>" into out (700 bytes). */
static void at(char *out, const char *directory, const char *name)
{
    size_t nd = strlen(directory), nn = strlen(name);
    assert(nd + 1 + nn < 700);
    memcpy(out, directory, nd);
    out[nd] = '/';
    memcpy(out + nd + 1, name, nn + 1);
}

static long file_bytes(const char *path, unsigned char *out, size_t capacity)
{
    FILE *file = fopen(path, "rb");
    if (!file) return -1;
    size_t size = fread(out, 1, capacity, file);
    fclose(file);
    return (long)size;
}

static void put_file(const char *path, const char *text)
{
    FILE *file = fopen(path, "wb");
    assert(file && fwrite(text, 1, strlen(text), file) == strlen(text) && !fclose(file));
}

static int exists(const char *path)
{
    struct stat info;
    return !lstat(path, &info);
}

static ino_t inode(const char *path)
{
    struct stat info;
    assert(!lstat(path, &info));
    return info.st_ino;
}

static void manifest_is(const char *expected)
{
    unsigned char text[4096];
    long size = file_bytes(manifest, text, sizeof text - 1);
    assert(size >= 0);
    text[size] = 0;
    if (strcmp((const char *)text, expected)) fprintf(stderr, "manifest:\n%s---\nexpected:\n%s---\n", text, expected);
    assert(!strcmp((const char *)text, expected));
}

/* The file holds exactly what pw_shell_link_build makes of the entry. */
static void holds(const PwShellLinkEntry *entry)
{
    char name[PW_SHELL_LINK_NAME_MAX + 1], path[700];
    unsigned char expected[PW_SHELL_LINK_SYNC_BYTES], actual[PW_SHELL_LINK_SYNC_BYTES + 1];
    size_t size = 0;
    assert(!pw_shell_link_file_name(entry->display_name, name, sizeof name));
    assert(!pw_shell_link_build(&entry->link, expected, sizeof expected, &size));
    at(path, folder, name);
    assert(file_bytes(path, actual, sizeof actual) == (long)size && !memcmp(actual, expected, size));
}

static void no_temporaries(void)
{
    char path[700];
    at(path, folder, ".pw-shell-link.tmp");
    assert(!exists(path));
    size_t n = strlen(manifest);
    assert(n + 5 <= sizeof path);
    memcpy(path, manifest, n);
    memcpy(path + n, ".tmp", 5);
    assert(!exists(path));
}

static void counts(const PwShellLinkSyncResult *result, unsigned written, unsigned unchanged, unsigned removed,
                   unsigned failed)
{
    if (result->written != written || result->unchanged != unchanged || result->removed != removed ||
        result->failed != failed)
        fprintf(stderr, "sync: written %u unchanged %u removed %u failed %u (first %d %s entry %ld)\n",
                result->written, result->unchanged, result->removed, result->failed, result->first_error,
                result->first_error_step, result->first_error_entry);
    assert(result->written == written && result->unchanged == unchanged && result->removed == removed &&
           result->failed == failed);
    if (!failed)
        assert(!result->first_error && !strcmp(result->first_error_step, "") && result->first_error_entry == -1);
    no_temporaries();
}

/* A fresh root, its Desktop folder (not made) and manifest. */
static void fresh_root(void)
{
    const char *temporary = getenv("TMPDIR");
    snprintf(root, sizeof root, "%s/pw-shell-link-XXXXXX", temporary && *temporary ? temporary : "/tmp");
    assert(mkdtemp(root));
    at(folder, root, "Desktop");
    at(manifest, root, "shell-links.txt");
}

static void test_sync(void)
{
    fresh_root();  /* the folder is missing: the first sync makes it */
    char path[700], other[700];
    PwShellLinkEntry entries[4] = {
        {"Example Game", {.target = "C:\\Games\\Example Game\\Example Game.exe",
                            .working_directory = "C:\\Games\\Example Game"}},
        {"Java Game", {.target = "C:\\Games\\JavaGame\\runtime\\bin\\javaw.exe",
                       .arguments = "@C:/Games/JavaGame/launch/args.txt",
                       .working_directory = "C:\\Games\\JavaGame",
                       .icon_path = "C:\\Games\\JavaGame\\JavaGame.exe"}},
        {"Unicode Game", {.target = "C:\\Games\\UnicodeGame\\UnicodeGame.exe", .description = "\xc3\x9cnicode Game"}},
        {"Native Game", {.target = "C:\\Games\\NativeGame\\x64\\NativeGame.exe"}},
    };
    PwShellLinkSyncResult result;

    /* First sync: the folder is made, three files and the manifest written. */
    assert(!exists(folder));
    assert(!pw_shell_link_sync(folder, manifest, entries, 3, &result));
    counts(&result, 3, 0, 0, 0);
    for (int i = 0; i < 3; ++i) holds(&entries[i]);
    manifest_is("Example Game.lnk\nJava Game.lnk\nUnicode Game.lnk\n");

    /* The user's own shortcut, and a second sync: nothing written, nothing touched. */
    char user[700];
    at(user, folder, "My Game.lnk");
    put_file(user, "the user's bytes");
    ino_t inodes[3];
    const char *names[3] = {"Example Game.lnk", "Java Game.lnk", "Unicode Game.lnk"};
    for (int i = 0; i < 3; ++i) at(path, folder, names[i]), inodes[i] = inode(path);
    assert(!pw_shell_link_sync(folder, manifest, entries, 3, &result));
    counts(&result, 0, 3, 0, 0);
    for (int i = 0; i < 3; ++i) at(path, folder, names[i]), assert(inode(path) == inodes[i]);

    /* One entry changes: only its file is rewritten. */
    entries[1].link.arguments = "@C:/Games/JavaGame/launch/args-1.21.txt";
    assert(!pw_shell_link_sync(folder, manifest, entries, 3, &result));
    counts(&result, 1, 2, 0, 0);
    holds(&entries[1]);
    at(path, folder, names[0]), assert(inode(path) == inodes[0]);
    at(path, folder, names[2]), assert(inode(path) == inodes[2]);

    /* One entry goes: only its file is removed. */
    assert(!pw_shell_link_sync(folder, manifest, entries, 2, &result));
    counts(&result, 0, 2, 1, 0);
    at(path, folder, "Unicode Game.lnk");
    assert(!exists(path));
    manifest_is("Example Game.lnk\nJava Game.lnk\n");
    unsigned char text[64];
    assert(file_bytes(user, text, sizeof text) == 16 && !memcmp(text, "the user's bytes", 16));

    /* A catalog name that is the user's file: a conflict, the file untouched and not owned. */
    PwShellLinkEntry mine[3] = {entries[0], entries[1], {"My Game", {.target = "C:\\Games\\Mine\\mine.exe"}}};
    assert(pw_shell_link_sync(folder, manifest, mine, 3, &result) == EEXIST);
    counts(&result, 0, 2, 0, 1);
    assert(!strcmp(result.first_error_step, "conflict") && result.first_error_entry == 2);
    assert(file_bytes(user, text, sizeof text) == 16 && !memcmp(text, "the user's bytes", 16));
    manifest_is("Example Game.lnk\nJava Game.lnk\n");

    /* The manifest lost (a sync that stopped before writing it): identical files are adopted. */
    assert(!unlink(manifest));
    assert(!pw_shell_link_sync(folder, manifest, entries, 2, &result));
    counts(&result, 0, 2, 0, 0);
    manifest_is("Example Game.lnk\nJava Game.lnk\n");

    /* Manifest lines this module could not have written are never acted on. */
    at(path, root, "outside.lnk");
    put_file(path, "outside");
    at(other, folder, "notes.txt");
    put_file(other, "notes");
    put_file(manifest, "Example Game.lnk\nJava Game.lnk\n../outside.lnk\nnotes.txt\n\n.lnk\n/etc/passwd.lnk");
    assert(!pw_shell_link_sync(folder, manifest, entries, 2, &result));
    counts(&result, 0, 2, 0, 0);
    assert(exists(path) && exists(other) && exists(user));
    manifest_is("Example Game.lnk\nJava Game.lnk\n");

    /* Two entries with one file name (ignoring case): the later one fails, the first is kept. */
    PwShellLinkEntry twice[3] = {entries[0], entries[1], {"EXAMPLE GAME", {.target = "C:\\other.exe"}}};
    assert(pw_shell_link_sync(folder, manifest, twice, 3, &result) == EEXIST);
    counts(&result, 0, 2, 0, 1);
    assert(!strcmp(result.first_error_step, "duplicate") && result.first_error_entry == 2);
    holds(&entries[0]);

    /* Failures do not stop the others; an owned entry that fails keeps its file and its ownership. */
    PwShellLinkEntry failing[4] = {entries[0], entries[1], {"???", {.target = "C:\\x.exe"}}, entries[3]};
    failing[0].link.target = "C:\\Jeux\\Caf\xc3\xa9.exe";
    assert(pw_shell_link_sync(folder, manifest, failing, 4, &result) == EINVAL);
    counts(&result, 1, 1, 0, 2);
    assert(!strcmp(result.first_error_step, "build") && result.first_error_entry == 0);
    holds(&entries[0]);  /* the previous bytes */
    holds(&entries[3]);
    manifest_is("Example Game.lnk\nJava Game.lnk\nNative Game.lnk\n");

    /* A link too large for the sync's buffer is that entry's failure. */
    memset(long_text, 'x', 5000);
    long_text[5000] = 0;
    PwShellLinkEntry large[3] = {entries[0], entries[1], {"Large", {.target = "C:\\l.exe", .arguments = long_text}}};
    assert(pw_shell_link_sync(folder, manifest, large, 3, &result) == ENOSPC);
    counts(&result, 0, 2, 1, 1);  /* and Native Game, not listed, is removed */
    assert(!strcmp(result.first_error_step, "build") && result.first_error_entry == 2);
    manifest_is("Example Game.lnk\nJava Game.lnk\n");

    /* A write that fails after its claim (a directory where the temporary goes, which O_EXCL refuses): nothing is
     * owned, and the next sync writes it. */
    at(path, folder, ".pw-shell-link.tmp");
    assert(!mkdir(path, 0755));
    assert(pw_shell_link_sync(folder, manifest, entries + 3, 1, &result) == EEXIST);
    assert(result.written == 0 && result.removed == 2 && result.failed == 1);
    assert(!strcmp(result.first_error_step, "write") && result.first_error_entry == 0);
    manifest_is("");  /* claimed, never written: nothing to own */
    assert(!rmdir(path));
    assert(!pw_shell_link_sync(folder, manifest, entries, 4, &result));
    counts(&result, 4, 0, 0, 0);
    manifest_is("Example Game.lnk\nJava Game.lnk\nUnicode Game.lnk\nNative Game.lnk\n");

    /* A display name whose case changed: on this case-sensitive file system, the old file goes. */
    PwShellLinkEntry recased[4] = {{"example game", entries[0].link}, entries[1], entries[2], entries[3]};
    assert(!pw_shell_link_sync(folder, manifest, recased, 4, &result));
    counts(&result, 1, 3, 1, 0);
    at(path, folder, "Example Game.lnk");
    assert(!exists(path));
    holds(&recased[0]);
    manifest_is("example game.lnk\nJava Game.lnk\nUnicode Game.lnk\nNative Game.lnk\n");

    /* A removal that fails stays owned, and the next sync removes it. */
    if (geteuid()) {
        assert(!chmod(folder, 0555));
        assert(pw_shell_link_sync(folder, manifest, recased, 3, &result) == EACCES);
        assert(result.removed == 0 && result.failed == 1 && !strcmp(result.first_error_step, "remove"));
        manifest_is("example game.lnk\nJava Game.lnk\nUnicode Game.lnk\nNative Game.lnk\n");
        assert(!chmod(folder, 0755));
        assert(!pw_shell_link_sync(folder, manifest, recased, 3, &result));
        counts(&result, 0, 3, 1, 0);
        manifest_is("example game.lnk\nJava Game.lnk\nUnicode Game.lnk\n");
    }

    /* No entries: every owned file goes, the user's stays. */
    assert(!pw_shell_link_sync(folder, manifest, NULL, 0, &result));
    counts(&result, 0, 0, geteuid() ? 3 : 4, 0);
    manifest_is("");
    assert(exists(user) && exists(other));

    /* Refusals before anything is touched. */
    char missing[700];
    at(missing, root, "missing/Desktop");
    assert(pw_shell_link_sync(missing, manifest, entries, 1, &result) == ENOENT);
    assert(!strcmp(result.first_error_step, "folder") && result.failed == 1 && result.first_error_entry == -1);
    assert(pw_shell_link_sync(user, manifest, entries, 1, &result) == ENOTDIR);
    assert(pw_shell_link_sync(NULL, manifest, entries, 1, &result) == EINVAL);
    assert(pw_shell_link_sync(folder, "", entries, 1, &result) == EINVAL);
    assert(pw_shell_link_sync(folder, manifest, NULL, 1, &result) == EINVAL);
    assert(!strcmp(result.first_error_step, "arguments"));
    assert(pw_shell_link_sync(folder, manifest, NULL, 1, NULL) == EINVAL);

    /* Clean up: only what this test made. */
    unlink(user);
    unlink(other);
    rmdir(folder);
    at(path, root, "outside.lnk");
    unlink(path);
    unlink(manifest);
    assert(!rmdir(root));
}

static void exit_now(int signal_number)
{
    _exit(40 + (signal_number == SIGXFSZ));
}

/* A sync that stops part way (here: killed by the file size limit while writing its second file) still owns the
 * file it wrote, through the claim it appended to the old manifest before writing; the next sync rewrites it. */
static void test_sync_stopped(void)
{
    fresh_root();
    memset(long_text, 'x', 2000);
    long_text[2000] = 0;
    PwShellLinkEntry entries[2] = {
        {"Alpha", {.target = "C:\\Alpha\\alpha.exe"}},
        {"Beta", {.target = "C:\\Beta\\beta.exe", .arguments = long_text}},
    };
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        struct rlimit limit = {1000, 1000};
        signal(SIGXFSZ, exit_now);
        if (setrlimit(RLIMIT_FSIZE, &limit)) _exit(1);
        pw_shell_link_sync(folder, manifest, entries, 2, NULL);
        _exit(0);
    }
    int status = 0;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 41);
    holds(&entries[0]);
    manifest_is("\nAlpha.lnk\n\nBeta.lnk\n");  /* the claims, and no final manifest */
    char leftover[700];
    at(leftover, folder, ".pw-shell-link.tmp");
    assert(exists(leftover));  /* Beta's partial bytes */
    /* Alpha changed: owned through its claim, so rewritten, not a conflict. Beta, claimed and never written, has
     * nothing to remove. */
    entries[0].link.arguments = "--changed";
    PwShellLinkSyncResult result;
    assert(!pw_shell_link_sync(folder, manifest, entries, 1, &result));
    counts(&result, 1, 0, 0, 0);
    holds(&entries[0]);
    manifest_is("Alpha.lnk\n");
    /* A temporary left over is removed by a sync that writes nothing. */
    put_file(leftover, "partial");
    assert(!pw_shell_link_sync(folder, manifest, entries, 1, &result));
    counts(&result, 0, 1, 0, 0);
    assert(!pw_shell_link_sync(folder, manifest, NULL, 0, &result));
    counts(&result, 0, 0, 1, 0);
    assert(!rmdir(folder) && !unlink(manifest) && !rmdir(root));
}

/* On a case-insensitive file system a name kept in another case is the same file, and is not removed: simulated
 * on this one with a hard link, so the old and the new spelling are one inode. */
static void test_sync_same_file(void)
{
    fresh_root();
    PwShellLinkEntry entry = {"Example Game", {.target = "C:\\Games\\Example Game\\Example Game.exe"}};
    PwShellLinkSyncResult result;
    assert(!pw_shell_link_sync(folder, manifest, &entry, 1, &result));
    char old_path[700], new_path[700];
    at(old_path, folder, "Example Game.lnk");
    at(new_path, folder, "example game.lnk");
    assert(!link(old_path, new_path));
    entry.display_name = "example game";
    assert(!pw_shell_link_sync(folder, manifest, &entry, 1, &result));
    counts(&result, 0, 1, 0, 0);
    assert(exists(old_path) && exists(new_path));
    manifest_is("example game.lnk\n");
    assert(!unlink(old_path) && !unlink(new_path) && !rmdir(folder) && !unlink(manifest) && !rmdir(root));
}

/* A new manifest that cannot be written whole never replaces the old one: here the file size limit stops it at
 * 1000 bytes while each shortcut stays under that. */
static void test_sync_manifest_failure(void)
{
    fresh_root();
    static char names[100][16];
    static PwShellLinkEntry entries[100];
    for (int i = 0; i < 100; ++i) {
        snprintf(names[i], sizeof names[i], "Game %03d", i);
        entries[i] = (PwShellLinkEntry){names[i], {.target = "C:\\g.exe"}};
    }
    PwShellLinkSyncResult result;
    assert(!pw_shell_link_sync(folder, manifest, entries, 100, &result));
    counts(&result, 100, 0, 0, 0);
    unsigned char before[4096];
    long before_size = file_bytes(manifest, before, sizeof before);
    assert(before_size == 100 * 13);
    entries[0].link.arguments = "--changed";
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        struct rlimit limit = {1000, 1000};
        signal(SIGXFSZ, SIG_IGN);
        if (setrlimit(RLIMIT_FSIZE, &limit)) _exit(1);
        int error = pw_shell_link_sync(folder, manifest, entries, 100, &result);
        _exit(error == EFBIG && !strcmp(result.first_error_step, "manifest") && result.written == 1 ? 0 : 2);
    }
    int status = 0;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    unsigned char after[4096];
    assert(file_bytes(manifest, after, sizeof after) == before_size && !memcmp(before, after, (size_t)before_size));
    no_temporaries();
    holds(&entries[0]);
    assert(!pw_shell_link_sync(folder, manifest, NULL, 0, &result));
    counts(&result, 0, 0, 100, 0);
    assert(!rmdir(folder) && !unlink(manifest) && !rmdir(root));
}

/* An owned shortcut whose rewrite fails keeps its file and stays owned, under the name its file has. */
static void test_sync_owned_failures(void)
{
    fresh_root();
    PwShellLinkEntry entry = {"Alpha", {.target = "C:\\Alpha\\alpha.exe"}};
    PwShellLinkEntry before = entry;
    PwShellLinkSyncResult result;
    assert(!pw_shell_link_sync(folder, manifest, &entry, 1, &result));
    char blocker[700];
    at(blocker, folder, ".pw-shell-link.tmp");
    assert(!mkdir(blocker, 0755));
    entry.link.arguments = "--changed";
    assert(pw_shell_link_sync(folder, manifest, &entry, 1, &result) == EEXIST);
    assert(result.failed == 1 && !strcmp(result.first_error_step, "write"));
    holds(&before);
    manifest_is("Alpha.lnk\n");
    assert(!rmdir(blocker));
    /* Renamed in case and failing to build: the file under its old spelling stays, owned. */
    PwShellLinkEntry broken = {"ALPHA", {.target = "C:\\\xc3\xa9.exe"}};
    assert(pw_shell_link_sync(folder, manifest, &broken, 1, &result) == EINVAL);
    assert(result.failed == 1 && result.removed == 0 && !strcmp(result.first_error_step, "build"));
    holds(&before);
    manifest_is("Alpha.lnk\n");
    assert(!pw_shell_link_sync(folder, manifest, NULL, 0, &result));
    counts(&result, 0, 0, 1, 0);
    assert(!rmdir(folder) && !unlink(manifest) && !rmdir(root));
}

static void put_text(const char *path, const char *text, size_t size)
{
    FILE *file = fopen(path, "wb");
    assert(file && fwrite(text, 1, size, file) == size && !fclose(file));
}

static void file_is(const char *path, const char *expected)
{
    unsigned char text[256];
    size_t size = strlen(expected);
    assert(file_bytes(path, text, sizeof text) == (long)size && !memcmp(text, expected, size));
}

/* What is not this module's is never written, written through or removed (each found in review, 2026-10-10). */
static void test_sync_not_owned(void)
{
    fresh_root();
    char path[700], other[700], user[700];
    PwShellLinkSyncResult result;
    PwShellLinkEntry entry = {"Foo", {.target = "C:\\Foo\\foo.exe"}};
    PwShellLinkEntry before = entry;
    assert(!pw_shell_link_sync(folder, manifest, &entry, 1, &result));

    /* A display name whose case changed, onto the user's own file of that spelling (this case-sensitive file
     * system): the old spelling owns another file, not this one. A conflict; the user's file untouched; the old
     * shortcut kept, still owned. */
    at(user, folder, "foo.lnk");
    put_file(user, "the user's bytes");
    entry.display_name = "foo";
    assert(pw_shell_link_sync(folder, manifest, &entry, 1, &result) == EEXIST);
    counts(&result, 0, 0, 0, 1);
    assert(!strcmp(result.first_error_step, "conflict") && result.first_error_entry == 0);
    file_is(user, "the user's bytes");
    holds(&before);
    manifest_is("Foo.lnk\n");
    assert(!unlink(user));

    /* Another spelling that is the same file (a case-insensitive file system, simulated with a hard link) is owned
     * through it, and rewritten. */
    at(path, folder, "Foo.lnk");
    assert(!link(path, user));
    entry.link.arguments = "--changed";
    assert(!pw_shell_link_sync(folder, manifest, &entry, 1, &result));
    assert(result.written == 1 && result.failed == 0);
    holds(&entry);
    manifest_is("foo.lnk\n");

    /* Two entries whose different names are one file: the later one is a duplicate, the earlier one's file kept. */
    PwShellLinkEntry pair[2] = {entry, {"Bar", {.target = "C:\\Bar\\bar.exe"}}};
    at(other, folder, "Bar.lnk");
    assert(!link(user, other));
    assert(pw_shell_link_sync(folder, manifest, pair, 2, &result) == EEXIST);
    counts(&result, 0, 1, 0, 1);
    assert(!strcmp(result.first_error_step, "duplicate") && result.first_error_entry == 1);
    holds(&entry);
    assert(inode(other) == inode(user));
    manifest_is("foo.lnk\n");
    assert(!unlink(other));

    /* An owned name that someone made a symbolic link or a directory: released, never removed or followed. */
    char outside[700];
    at(outside, root, "outside.txt");
    put_file(outside, "the user's bytes");
    assert(!unlink(user) && !symlink(outside, user));
    at(other, folder, "Dir.lnk");
    put_text(manifest, "foo.lnk\nDir.lnk\n", 16);
    assert(!mkdir(other, 0755));
    assert(!pw_shell_link_sync(folder, manifest, NULL, 0, &result));
    counts(&result, 0, 0, 0, 0);
    assert(exists(user) && exists(other));
    file_is(outside, "the user's bytes");
    manifest_is("");
    assert(!unlink(user) && !rmdir(other));

    /* A symbolic link where the temporary goes: removed, the file it points at untouched. */
    at(path, folder, ".pw-shell-link.tmp");
    assert(!symlink(outside, path));
    assert(!pw_shell_link_sync(folder, manifest, &entry, 1, &result));
    counts(&result, 1, 0, 0, 0);
    file_is(outside, "the user's bytes");
    holds(&entry);

    /* Manifest lines naming a file pw_shell_link_file_name could not make are never acted on. */
    at(other, folder, "a|b.lnk");
    put_file(other, "the user's bytes");
    put_text(manifest, "foo.lnk\na|b.lnk\n", 16);
    assert(!pw_shell_link_sync(folder, manifest, NULL, 0, &result));
    counts(&result, 0, 0, 1, 0);
    file_is(other, "the user's bytes");
    assert(!unlink(other) && !unlink(outside) && !rmdir(folder) && !unlink(manifest) && !rmdir(root));
}

/* --- the oracle's samples --- */

typedef struct Sample {
    const char *name;
    PwShellLink link;
    /* What Wine's IShellLinkW must give back, written out (not computed by the module under test). */
    const char *path, *arguments, *working_directory, *icon_path, *icon_index, *description;
} Sample;

static int write_samples(const char *directory)
{
    static char boundary[32767 + 8];
    memset(boundary, 'a', 32765);
    memcpy(boundary + 32765, "\xf0\x9f\x98\x80", 5);  /* 32767 units, the last two a surrogate pair */
    const Sample samples[] = {
        {"example",
         {.target = "C:\\Games\\Example Game\\Example Game.exe", .working_directory = "C:\\Games\\Example Game",
          .description = "Example Game"},
         "C:\\Games\\Example Game\\Example Game.exe", "", "C:\\Games\\Example Game",
         "C:\\Games\\Example Game\\Example Game.exe", "0", "Example Game"},
        {"java",
         {.target = "C:\\Games\\JavaGame\\runtime\\bin\\javaw.exe", .arguments = "@C:/Games/JavaGame/launch/args.txt",
          .working_directory = "C:\\Games\\JavaGame", .icon_path = "C:\\Games\\JavaGame\\Java Launcher.exe",
          .icon_index = 1, .description = "Java Game: Special Edition"},
         "C:\\Games\\JavaGame\\runtime\\bin\\javaw.exe", "@C:/Games/JavaGame/launch/args.txt", "C:\\Games\\JavaGame",
         "C:\\Games\\JavaGame\\Java Launcher.exe", "1", "Java Game: Special Edition"},
        {"unicode",
         {.target = "C:\\Games\\UnicodeGame\\UnicodeGame.exe",
          .arguments = "--profile \"\xc3\x9cn\xc3\xaf" "c\xc3\xb6\x64\xc3\xa9 \xe4\xb8\x96\xe7\x95\x8c\" "
                       "--emoji \xf0\x9f\x98\x80",
          .working_directory = "C:\\Games\\\xc3\x9cnicode \xce\xa9", .icon_index = -3,
          .description = "\xc3\x9cnicode \xe2\x80\x94 \xf0\x9f\x8d\x93"},
         "C:\\Games\\UnicodeGame\\UnicodeGame.exe",
         "--profile \"\xc3\x9cn\xc3\xaf" "c\xc3\xb6\x64\xc3\xa9 \xe4\xb8\x96\xe7\x95\x8c\" --emoji \xf0\x9f\x98\x80",
         "C:\\Games\\\xc3\x9cnicode \xce\xa9", "C:\\Games\\UnicodeGame\\UnicodeGame.exe", "-3",
         "\xc3\x9cnicode \xe2\x80\x94 \xf0\x9f\x8d\x93"},
        {"boundary", {.target = "C:\\b.exe", .arguments = boundary}, "C:\\b.exe", boundary, "", "C:\\b.exe", "0", ""},
        /* tools/check-shell-link.sh also opens this one as Explorer does: cmd.exe writes the file the variable
         * names (cmd expands it, not shell32). */
        {"open",
         {.target = "C:\\windows\\system32\\cmd.exe", .arguments = "/c echo opened> \"%PW_SHELL_LINK_MARKER%\"",
          .working_directory = "C:\\windows"},
         "C:\\windows\\system32\\cmd.exe", "/c echo opened> \"%PW_SHELL_LINK_MARKER%\"", "C:\\windows",
         "C:\\windows\\system32\\cmd.exe", "0", ""},
    };
    for (size_t i = 0; i < sizeof samples / sizeof samples[0]; ++i) {
        const Sample *sample = &samples[i];
        char path[4096];
        size_t bytes = 0;
        int error = pw_shell_link_build(&sample->link, big, sizeof big, &bytes);
        if (error) {
            fprintf(stderr, "%s: pw_shell_link_build: %s\n", sample->name, strerror(error));
            return 1;
        }
        snprintf(path, sizeof path, "%s/%s.lnk", directory, sample->name);
        FILE *file = fopen(path, "wb");
        if (!file || fwrite(big, 1, bytes, file) != bytes || fclose(file)) {
            fprintf(stderr, "%s: %s\n", path, strerror(errno));
            return 1;
        }
        snprintf(path, sizeof path, "%s/%s.expected", directory, sample->name);
        file = fopen(path, "wb");
        if (!file) {
            fprintf(stderr, "%s: %s\n", path, strerror(errno));
            return 1;
        }
        fprintf(file, "path=%s\narguments=%s\nworking_directory=%s\nicon_path=%s\nicon_index=%s\ndescription=%s\n",
                sample->path, sample->arguments, sample->working_directory, sample->icon_path, sample->icon_index,
                sample->description);
        if (fclose(file)) return 1;
        printf("%s.lnk %zu bytes\n", sample->name, bytes);
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "--samples")) return write_samples(argv[2]);
    test_layout();
    test_refusals();
    test_file_names();
    test_sync();
    test_sync_stopped();
    test_sync_same_file();
    test_sync_manifest_failure();
    test_sync_owned_failures();
    test_sync_not_owned();
    puts("test_pw_shell_link: ok");
    return 0;
}
