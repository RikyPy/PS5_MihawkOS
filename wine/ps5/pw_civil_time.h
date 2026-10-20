/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Broken-down UTC time for the PRX libc shims (pw_gnutls_libc.c, pw_winedmo_libc.c): no stub a title links against
 * has gmtime_r, and the console gives a title no time zone, so local time is UTC. Host-tested against the host's
 * gmtime_r (tests/test_pw_civil_time.c). */
#ifndef PW_CIVIL_TIME_H
#define PW_CIVIL_TIME_H
#include <time.h>

/* Days since 1970-01-01 to a civil date (Howard Hinnant's days_from_civil, inverted). */
static inline struct tm *pw_civil_time(const time_t *when, struct tm *out)
{
    const long long seconds = (long long)*when;
    *out = (struct tm){0};
    long long days = seconds / 86400, rest = seconds % 86400;
    if (rest < 0) { rest += 86400; --days; }
    out->tm_hour = (int)(rest / 3600);
    out->tm_min = (int)(rest % 3600 / 60);
    out->tm_sec = (int)(rest % 60);
    out->tm_wday = (int)((days % 7 + 11) % 7);   /* 1970-01-01 was a Thursday */
    const long long z = days + 719468, era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned day = doy - (153 * mp + 2) / 5 + 1, month = mp < 10 ? mp + 3 : mp - 9;
    const long long year = (long long)yoe + era * 400 + (month <= 2);
    static const int before[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    const int leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    out->tm_year = (int)(year - 1900);
    out->tm_mon = (int)month - 1;
    out->tm_mday = (int)day;
    out->tm_yday = before[month - 1] + (int)day - 1 + (leap && month > 2);
    out->tm_isdst = 0;
    return out;
}

#endif
