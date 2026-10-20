/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../wine/ps5/pw_wine_prefix.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void put(const char *path, const char *text)
{
    FILE *file = fopen(path, "w"); assert(file);
    assert(fputs(text, file) >= 0); assert(!fclose(file));
}

int main(void)
{
    char temp[] = "/tmp/pw-prefix-XXXXXX"; assert(mkdtemp(temp));
    assert(!chdir(temp));
    assert(!mkdir("bundle", 0700)); assert(!mkdir("prefix", 0700));
    assert(!mkdir("bundle/system32", 0700)); assert(!mkdir("bundle/syswow64", 0700));
    put("bundle/system32/.pw-proton-version", "v1\n"); put("bundle/syswow64/.pw-proton-version", "v1\n");
    put("bundle/system32/test.dll", "64"); put("bundle/syswow64/test.dll", "32");
    put("prefix/user-settings.txt", "preserve me");
    assert(pw_wine_prefix_prepare("bundle", "missing") == -1);
    assert(pw_wine_prefix_prepare("bundle", "prefix") == 4);
    assert(pw_wine_prefix_prepare("bundle", "prefix") == 0);
    assert(!unlink("prefix/syswow64/test.dll"));
    assert(pw_wine_prefix_prepare("bundle", "prefix") == 2);
    put("bundle/system32/.pw-proton-version", "v2\n"); put("bundle/system32/test.dll", "new64");
    assert(pw_wine_prefix_prepare("bundle", "prefix") == 2);
    char data[32] = {0}; FILE *f = fopen("prefix/user-settings.txt", "r"); assert(f);
    assert(fread(data, 1, sizeof(data), f) == strlen("preserve me")); fclose(f);
    assert(!strcmp(data, "preserve me"));
    assert(!symlink("test.dll", "bundle/system32/unsafe.dll"));
    assert(pw_wine_prefix_prepare("bundle", "prefix") == -1 && errno == EINVAL);
    assert(pw_wine_prefix_prepare(NULL, "prefix") == -1 && errno == EINVAL);
    assert(!unlink("bundle/system32/unsafe.dll"));
    const char *files[] = {"bundle/system32/test.dll", "bundle/syswow64/test.dll",
        "bundle/system32/.pw-proton-version", "bundle/syswow64/.pw-proton-version",
        "prefix/system32/test.dll", "prefix/syswow64/test.dll", "prefix/system32/.pw-proton-version",
        "prefix/syswow64/.pw-proton-version", "prefix/user-settings.txt"};
    for (unsigned i = 0; i < sizeof(files)/sizeof(files[0]); ++i) assert(!unlink(files[i]));
    assert(!rmdir("bundle/system32")); assert(!rmdir("bundle/syswow64")); assert(!rmdir("bundle"));
    assert(!rmdir("prefix/system32")); assert(!rmdir("prefix/syswow64")); assert(!rmdir("prefix"));
    assert(!chdir("/")); assert(!rmdir(temp));
    return 0;
}
