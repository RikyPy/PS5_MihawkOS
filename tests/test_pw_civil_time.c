/* SPDX-License-Identifier: GPL-3.0-or-later */
/* pw_civil_time (the PRXs' gmtime_r) against the host's gmtime_r: every hour from 1900 to 2200 and a sweep of
 * seconds around the epoch and leap days. */
#include "../wine/ps5/pw_civil_time.h"
#include <stdio.h>
#include <string.h>

static int same(time_t t)
{
    struct tm ours, host;
    pw_civil_time(&t, &ours);
    gmtime_r(&t, &host);
    if (ours.tm_year == host.tm_year && ours.tm_mon == host.tm_mon && ours.tm_mday == host.tm_mday &&
        ours.tm_hour == host.tm_hour && ours.tm_min == host.tm_min && ours.tm_sec == host.tm_sec &&
        ours.tm_wday == host.tm_wday && ours.tm_yday == host.tm_yday && !ours.tm_isdst) return 1;
    fprintf(stderr, "test_pw_civil_time: %lld: ours %d-%d-%d %d:%d:%d w%d y%d, host %d-%d-%d %d:%d:%d w%d y%d\n",
            (long long)t, ours.tm_year, ours.tm_mon, ours.tm_mday, ours.tm_hour, ours.tm_min, ours.tm_sec,
            ours.tm_wday, ours.tm_yday, host.tm_year, host.tm_mon, host.tm_mday, host.tm_hour, host.tm_min,
            host.tm_sec, host.tm_wday, host.tm_yday);
    return 0;
}

int main(void)
{
    long checked = 0;
    for (long long t = -2208988800LL; t < 7258118400LL; t += 3600, ++checked)
        if (!same((time_t)t)) return 1;
    for (long long t = -200000; t < 200000; t += 7, ++checked)
        if (!same((time_t)t)) return 1;
    for (long long t = 951782400LL - 100000; t < 951782400LL + 100000; t += 13, ++checked)   /* 2000-02-29 */
        if (!same((time_t)t)) return 1;
    printf("test_pw_civil_time: %ld times match the host's gmtime_r\n", checked);
    return 0;
}
