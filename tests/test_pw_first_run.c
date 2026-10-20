/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/pw_first_run.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char work[256], conf[300], part[320];

static void put(const char *text)
{
    FILE *file = fopen(conf, "wb");
    assert(file && fputs(text, file) >= 0 && fclose(file) == 0);
}

static int holds(const char *text)
{
    char got[256] = { 0 };
    FILE *file = fopen(conf, "rb");
    if (!file) return 0;
    const size_t size = fread(got, 1, sizeof(got) - 1, file);
    fclose(file);
    return size == strlen(text) && !memcmp(got, text, size);
}

int main(void)
{
    snprintf(work, sizeof(work), "%s/pw-first-run-XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    assert(mkdtemp(work));
    snprintf(conf, sizeof(conf), "%s/dev.conf", work);
    snprintf(part, sizeof(part), "%s.mihawkos-part", conf);
    assert(strlen(PW_FIRST_RUN_DEV_CONF) == 35);   /* S6's bytes, tools/console-setup.py DEV_CONF */

    /* Missing: written, and no part file left. */
    assert(pw_first_run_dev_conf(conf) == PW_FIRST_RUN_WRITTEN && holds(PW_FIRST_RUN_DEV_CONF) && access(part, F_OK));
    /* Again: present, untouched. */
    assert(pw_first_run_dev_conf(conf) == PW_FIRST_RUN_PRESENT && holds(PW_FIRST_RUN_DEV_CONF));
    /* The owner's own setting, and one that only starts like S6's: kept as they are. */
    put("DEV_SERVER=sink.example\nDEV_PORT=9000\n");
    assert(pw_first_run_dev_conf(conf) == PW_FIRST_RUN_OTHER && holds("DEV_SERVER=sink.example\nDEV_PORT=9000\n"));
    put(PW_FIRST_RUN_DEV_CONF "EXTRA=1\n");
    assert(pw_first_run_dev_conf(conf) == PW_FIRST_RUN_OTHER && holds(PW_FIRST_RUN_DEV_CONF "EXTRA=1\n"));
    put("");
    assert(pw_first_run_dev_conf(conf) == PW_FIRST_RUN_OTHER && holds(""));
    /* No folder to write in (/data not mounted into the sandbox): an errno, nothing made. */
    char nowhere[320];
    snprintf(nowhere, sizeof(nowhere), "%s/homebrew/dev.conf", work);
    assert(pw_first_run_dev_conf(nowhere) == -ENOENT);

    assert(!unlink(conf) && !rmdir(work));
    puts("test_pw_first_run: ok");
    return 0;
}
