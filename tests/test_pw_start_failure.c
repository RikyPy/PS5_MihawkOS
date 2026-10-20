/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The notice a failed helper leaves behind (src/pw_start_failure.h): the helper log's firmware line and its last
 * unfinished "loading" line name the console and the module; the title's notice says both instead of a bare errno. */
#include "../src/pw_start_failure.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int says(const char *text, const char *needle) { return strstr(text, needle) != NULL; }

int main(void)
{
    char text[512], firmware[8];
    PwStartFailure failure;

    /* The tester's case (fw 12.40, klog "pager error ... 0x800488000" at ntdll.prx's text, the title's errno 92): the
     * helper's log ends at the line written before ntdll's load. */
    const char tester[] =
        "wine-child: entry\n"
        "wine-child: firmware status=0 version=0x12400000\n"
        "wine-child: GPU broker host ranges reserved\n"
        "wine-child: loading ntdll path=/data/ps5proton/runtime/ge11-7/lib/wine/x86_64-unix/ntdll.prx\n";
    assert(!pw_start_failure_parse(tester, sizeof(tester) - 1, &failure));
    assert(!strcmp(failure.module, "ntdll.prx") && failure.firmware == 0x12400000u);
    pw_start_failure_describe(&failure, 92, text, sizeof(text));
    printf("  %s\n", text);
    assert(!strcmp(text, "A program could not start (errno 92): its helper stopped while loading ntdll.prx on "
                         "firmware 12.40."));

    /* ntdll's own line ends its load; a later module's unfinished load is then the one. */
    const char later[] = "wine-child: firmware status=0 version=0x13400000\n"
                         "wine-child: loading ntdll path=/x/ntdll.prx\n"
                         "wine-child: ntdll loaded\n"
                         "pw_wine_dl: loading /x/lib/wine/x86_64-unix/win32u-helper.prx\n";
    assert(!pw_start_failure_parse(later, sizeof(later) - 1, &failure));
    assert(!strcmp(failure.module, "win32u-helper.prx"));
    pw_start_failure_describe(&failure, 92, text, sizeof(text));
    printf("  %s\n", text);
    assert(says(text, "stopped while loading win32u-helper.prx on firmware 13.40"));

    /* A line written whole behind another writer's piece still counts (gate-2026-10-19-splice-01's shape). */
    const char spliced[] = "wine-child: loading ntdll path=/x/ntdll.prx\n"
                           "wine-child: ntdll loaded\n"
                           "wine-ps5: kill thread 18beepw_wine_dl: loading /x/winevulkan.prx\n";
    assert(!pw_start_failure_parse(spliced, sizeof(spliced) - 1, &failure) &&
           !strcmp(failure.module, "winevulkan.prx"));

    /* Every finished load names nothing: loaded, refused, a failed open's report, ntdll's failure. */
    const char finished[] = "wine-child: firmware status=0 version=0x10010000\n"
                            "wine-child: loading ntdll path=/x/ntdll.prx\n"
                            "wine-child: ntdll loaded\n"
                            "pw_wine_dl: loading /x/a.prx\n"
                            "pw_wine_dl: loaded /x/a.prx handle=0x2c\n"
                            "pw_wine_dl: loading /x/b.prx\n"
                            "pw_wine_dl: refused /x/b.prx handle=0x80020002\n"
                            "pw_wine_dl: loading /y/b.prx\n"
                            "pw_wine_dl: cannot open b.so, tried /x/b.prx /y/b.prx: not found\n"
                            "wine-child: payload ending monotonic_ms=1\n";
    assert(!pw_start_failure_parse(finished, sizeof(finished) - 1, &failure));
    assert(!failure.module[0] && failure.firmware == 0x10010000u);
    pw_start_failure_describe(&failure, 5, text, sizeof(text));
    assert(!strcmp(text, "A program could not start (errno 5) on firmware 10.01."));
    const char failed[] = "wine-child: loading ntdll path=/x/ntdll.prx\nwine-child: ntdll load failed\n";
    assert(pw_start_failure_parse(failed, sizeof(failed) - 1, &failure) == -1 && !failure.module[0]);

    /* Nothing to go on keeps the bare errno. */
    assert(pw_start_failure_parse("wine-child: entry\n", 18, &failure) == -1);
    pw_start_failure_describe(&failure, 92, text, sizeof(text));
    assert(!strcmp(text, "A program could not start (errno 92)."));

    assert(pw_start_failure_firmware(0x13400000u, firmware, sizeof(firmware)) && !strcmp(firmware, "13.40"));
    assert(!pw_start_failure_firmware(0, firmware, sizeof(firmware)));

    /* The title's reader sees the same lines. */
    char path[] = "/tmp/pw-start-failure-XXXXXX";
    const int fd = mkstemp(path);
    assert(fd >= 0);
    FILE *file = fdopen(fd, "w");
    assert(file != NULL);
    fputs(later, file);
    fclose(file);
    assert(!pw_start_failure_read(path, &failure) && !strcmp(failure.module, "win32u-helper.prx"));
    unlink(path);
    assert(pw_start_failure_read("/nonexistent/pw-start-failure", &failure) == -1);

    printf("test_pw_start_failure: ok\n");
    return 0;
}
