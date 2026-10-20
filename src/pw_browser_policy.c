/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pw_browser_policy.h"
#include <string.h>

const char *const pw_browser_switches[][2] = {
    {"in-process-gpu", NULL}, {"disable-gpu-watchdog", NULL},
    {"renderer-process-limit", "1"}, {"disable-site-isolation-trials", NULL},
    {"enable-features", "NetworkServiceInProcess2"},
    {"disable-features", "AudioServiceOutOfProcess,SpareRendererForSitePerProcess,BackForwardCache,AsyncDns,DnsOverHttps,"
                         "AimEligibilityService"},
    {"enable-low-end-device-mode", NULL},
    {"disk-cache-size", "67108864"},
    {"disable-direct-composition", NULL},
    {"disable-quic", NULL}, {"no-proxy-server", NULL},
    {"disable-extensions", NULL}, {"disable-component-update", NULL}, {"disable-background-networking", NULL},
    {"autoplay-policy", "user-gesture-required"},
    /* Chromium's call-stack sampler copies the main thread's stack up to the TEB's stack base, whose top 1 MiB is
     * unreadable in a PS5 helper (2026-10-16; record removed from the tree); with metrics off nothing reads its samples. */
    {"disable-stack-profiler", NULL},
    /* The profile's own Google account calls (ListAccounts at every start) go nowhere; pages still reach Google. */
    {"gaia-url", "https://gaia.invalid/"},
    {NULL, NULL},
};

const PwBrowserPolicy pw_browser_policies[] = {
    {"BrowserSignin", 0, NULL},
    {"SyncDisabled", 1, NULL},
    {"SafeBrowsingProtectionLevel", 0, NULL},
    {"MetricsReportingEnabled", 0, NULL},
    {"NetworkPredictionOptions", 2, NULL},
    {"SearchSuggestEnabled", 0, NULL},
    {"AlternateErrorPagesEnabled", 0, NULL},
    {"TranslateEnabled", 0, NULL},
    {"PasswordManagerEnabled", 0, NULL},
    {"AutofillAddressEnabled", 0, NULL},
    {"AutofillCreditCardEnabled", 0, NULL},
    {"SpellcheckEnabled", 0, NULL},
    {"AlwaysOpenPdfExternally", 1, NULL},
    {"PromptForDownloadLocation", 0, NULL},
    {"DefaultSearchProviderEnabled", 1, NULL},
    {"DefaultSearchProviderName", 0, "DuckDuckGo"},
    {"DefaultSearchProviderKeyword", 0, "duckduckgo.com"},
    {"DefaultSearchProviderSearchURL", 0, PW_BROWSER_SEARCH "{searchTerms}"},
    {NULL, 0, NULL},
};

typedef struct Out { char *text; size_t capacity, length; int over; } Out;

static Out out_start(char *text, size_t capacity)
{
    Out out = {text, capacity, 0, capacity == 0};
    if (capacity) text[0] = 0;
    return out;
}

static void put(Out *out, const char *s, size_t n)
{
    if (out->over || out->length + n >= out->capacity) { out->over = 1; return; }
    memcpy(out->text + out->length, s, n);
    out->length += n;
    out->text[out->length] = 0;
}

static void put_char(Out *out, char c) { put(out, &c, 1); }

static void put_unsigned(Out *out, unsigned value)
{
    char digits[12];
    size_t n = 0;
    do digits[sizeof digits - ++n] = (char)('0' + value % 10); while ((value /= 10) && n < sizeof digits);
    put(out, digits + sizeof digits - n, n);
}

static int out_end(Out *out)
{
    if (!out->over) return PW_OK;
    if (out->capacity) out->text[0] = 0;
    return PW_ERR_LIMIT;
}

static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }
static int is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static int is_digit(char c) { return c >= '0' && c <= '9'; }
static int is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

static int equal_ci(const char *s, size_t n, const char *literal)
{
    size_t i = 0;
    for (; i < n && literal[i]; ++i) if (lower(s[i]) != literal[i]) return 0;
    return i == n && !literal[i];
}

/* RFC 3986's scheme: ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ), then ':'. Its length, or 0. */
static size_t scheme_length(const char *s, size_t n)
{
    if (!n || !is_alpha(s[0])) return 0;
    for (size_t i = 1; i < n; ++i) {
        if (s[i] == ':') return i;
        if (!is_alpha(s[i]) && !is_digit(s[i]) && s[i] != '+' && s[i] != '-' && s[i] != '.') return 0;
    }
    return 0;
}

static size_t span_until(const char *s, size_t n, const char *stops)
{
    size_t i = 0;
    while (i < n && !strchr(stops, s[i])) ++i;
    return i;
}

/* "example.com/x", "localhost:8080", "192.0.2.2", "[::1]:443": a host the user means to visit. A dotted name needs a
 * top-level part of two or more characters with a letter in it, so "1.5" and "v2.0" are searches. */
static int looks_like_host(const char *s, size_t n)
{
    size_t h = span_until(s, n, "/?#");
    if (h && s[0] == '[') return memchr(s, ']', h) != NULL;
    size_t colon = h;
    for (size_t i = 0; i < h; ++i) if (s[i] == ':') colon = i;
    if (colon < h) {
        if (colon + 1 == h || h - colon - 1 > 5) return 0;
        for (size_t i = colon + 1; i < h; ++i) if (!is_digit(s[i])) return 0;
    }
    h = colon;
    if (!h || s[0] == '.' || s[0] == '-' || s[h - 1] == '.' || s[h - 1] == '-') return 0;
    size_t dots = 0, last_dot = 0, numeric = 1;
    for (size_t i = 0; i < h; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c == '.') { ++dots; last_dot = i; continue; }
        if (!is_digit((char)c)) numeric = 0;
        if (!is_alpha((char)c) && !is_digit((char)c) && c != '-' && c < 0x80) return 0;
    }
    if (equal_ci(s, h, "localhost")) return 1;
    if (numeric) return dots == 3;
    if (!dots || h - last_dot - 1 < 2) return 0;
    for (size_t i = last_dot + 1; i < h; ++i) if (is_alpha(s[i]) || (unsigned char)s[i] >= 0x80) return 1;
    return 0;
}

int pw_browser_address(const char *input, char *url, size_t capacity)
{
    if (!input || !url || !capacity) return PW_ERR_PRECONDITION;
    url[0] = 0;
    while (is_space(*input)) ++input;
    size_t n = strlen(input);
    while (n && is_space(input[n - 1])) --n;
    if (!n) return PW_ERR_PRECONDITION;
    int spaces = 0;
    for (size_t i = 0; i < n; ++i) spaces |= is_space(input[i]);
    Out out = out_start(url, capacity);
    size_t scheme = spaces ? 0 : scheme_length(input, n);
    if (scheme && scheme + 2 < n && input[scheme + 1] == '/' && input[scheme + 2] == '/') {
        if (equal_ci(input, scheme, "https")) put(&out, input, n);
        else if (equal_ci(input, scheme, "http")) { put(&out, "https", 5); put(&out, input + 4, n - 4); }
        else return PW_ERR_UNSUPPORTED;
        return out_end(&out);
    }
    if (!spaces && equal_ci(input, n, "about:blank")) { put(&out, "about:blank", 11); return out_end(&out); }
    if (!spaces && looks_like_host(input, n)) { put(&out, "https://", 8); put(&out, input, n); return out_end(&out); }
    static const char *const refused[] = {"about", "blob", "chrome", "data", "file", "ftp", "javascript", "mailto",
                                          "view-source", "vbscript", NULL};
    for (size_t i = 0; scheme && refused[i]; ++i) if (equal_ci(input, scheme, refused[i])) return PW_ERR_UNSUPPORTED;
    put(&out, PW_BROWSER_SEARCH, strlen(PW_BROWSER_SEARCH));
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)input[i];
        if (is_alpha((char)c) || is_digit((char)c) || strchr("-._~", c)) put_char(&out, (char)c);
        else if (c == ' ') put_char(&out, '+');
        else { put_char(&out, '%'); put_char(&out, "0123456789ABCDEF"[c >> 4]); put_char(&out, "0123456789ABCDEF"[c & 15]); }
    }
    return out_end(&out);
}

PwBrowserNavigation pw_browser_navigation(const char *url, char *upgraded, size_t capacity)
{
    if (upgraded && capacity) upgraded[0] = 0;
    if (!url) return PW_BROWSER_NAV_REFUSE;
    size_t n = strlen(url), scheme = scheme_length(url, n);
    if (!scheme) return PW_BROWSER_NAV_REFUSE;
    if (equal_ci(url, scheme, "https") || equal_ci(url, scheme, "data") || equal_ci(url, scheme, "blob"))
        return PW_BROWSER_NAV_ALLOW;
    if (equal_ci(url, n, "about:blank") || equal_ci(url, n, "about:srcdoc")) return PW_BROWSER_NAV_ALLOW;
    if (!equal_ci(url, scheme, "http") || !upgraded) return PW_BROWSER_NAV_REFUSE;
    Out out = out_start(upgraded, capacity);
    put(&out, "https", 5);
    put(&out, url + 4, n - 4);
    return out_end(&out) == PW_OK ? PW_BROWSER_NAV_UPGRADE : PW_BROWSER_NAV_REFUSE;
}

static int is_device_name(const char *s, size_t n)
{
    static const char *const names[] = {"con", "prn", "aux", "nul", NULL};
    size_t stem = span_until(s, n, ".");
    for (size_t i = 0; names[i]; ++i) if (equal_ci(s, stem, names[i])) return 1;
    return stem == 4 && (equal_ci(s, 3, "com") || equal_ci(s, 3, "lpt")) && s[3] >= '1' && s[3] <= '9';
}

int pw_browser_download_name(const char *suggested, char *name, size_t capacity)
{
    if (!suggested || !name || !capacity) return PW_ERR_PRECONDITION;
    const char *base = suggested;
    for (const char *p = suggested; *p; ++p) if (*p == '/' || *p == '\\') base = p + 1;
    char clean[512];
    size_t n = 0;
    for (const char *p = base; *p && n < sizeof clean; ++p)
        clean[n++] = (unsigned char)*p < 0x20 || strchr("<>:\"|?*", *p) ? '_' : *p;
    size_t start = 0;
    while (start < n && clean[start] == ' ') ++start;
    while (n > start && (clean[n - 1] == '.' || clean[n - 1] == ' ')) --n;
    const char *s = clean + start;
    n -= start;
    if (!n) { s = "download"; n = 8; }
    size_t extension = 0;  /* bytes of ".ext" kept when the name is cut */
    for (size_t i = n; i-- > 1 && n - i <= 16;) if (s[i] == '.') { extension = n - i; break; }
    int device = is_device_name(s, n);
    size_t stem = n - extension, room = PW_BROWSER_NAME_MAX - (size_t)device - extension;
    if (stem > room) {
        stem = room;
        while (stem && ((unsigned char)s[stem] & 0xC0) == 0x80) --stem;  /* not inside a UTF-8 sequence */
    }
    Out out = out_start(name, capacity);
    if (device) put_char(&out, '_');
    put(&out, s, stem);
    put(&out, s + n - extension, extension);
    return out_end(&out);
}

int pw_browser_download_candidate(const char *name, unsigned n, char *out_text, size_t capacity)
{
    if (!name || !out_text || !capacity) return PW_ERR_PRECONDITION;
    size_t length = strlen(name), dot = length;
    for (size_t i = length; i-- > 1;) if (name[i] == '.') { dot = i; break; }
    Out out = out_start(out_text, capacity);
    put(&out, name, n ? dot : length);
    if (n) { put(&out, " (", 2); put_unsigned(&out, n); put_char(&out, ')'); put(&out, name + dot, length - dot); }
    return out_end(&out);
}

int pw_browser_is_installer(const char *name)
{
    size_t n = name ? strlen(name) : 0;
    return n > 4 && (equal_ci(name + n - 4, 4, ".exe") || equal_ci(name + n - 4, 4, ".msi"));
}

int pw_browser_log_url(const char *url, char *out_text, size_t capacity)
{
    if (!url || !out_text || !capacity) return PW_ERR_PRECONDITION;
    size_t n = strlen(url), scheme = scheme_length(url, n);
    Out out = out_start(out_text, capacity);
    put(&out, "scheme=", 7);
    if (!scheme) put(&out, "none", 4);
    for (size_t i = 0; i < scheme; ++i) put_char(&out, is_alpha(url[i]) || is_digit(url[i]) ? lower(url[i]) : '_');
    if (scheme && scheme + 2 < n && url[scheme + 1] == '/' && url[scheme + 2] == '/') {
        const char *authority = url + scheme + 3;
        size_t length = span_until(authority, n - scheme - 3, "/?#");
        for (size_t i = length; i-- > 0;) if (authority[i] == '@') { authority += i + 1; length -= i + 1; break; }
        size_t host = authority[0] == '[' ? 0 : span_until(authority, length, ":");
        int ip = authority[0] == '[';
        if (!ip && host) {
            ip = 1;
            for (size_t i = 0; i < host; ++i) if (!is_digit(authority[i]) && authority[i] != '.') ip = 0;
        }
        put(&out, " host=", 6);
        if (ip) put(&out, "ip", 2);
        for (size_t i = 0; !ip && i < host; ++i) {
            char c = lower(authority[i]);
            put_char(&out, is_alpha(c) || is_digit(c) || c == '.' || c == '-' ? c : '_');
        }
        const char *path = authority + length;
        size_t path_length = span_until(path, strlen(path), "?#");
        uint64_t hash = 0xcbf29ce484222325ull;  /* FNV-1a: tells two paths apart without writing either */
        for (size_t i = 0; i < path_length; ++i) hash = (hash ^ (unsigned char)path[i]) * 0x100000001b3ull;
        put(&out, " path=", 6);
        for (int shift = 60; shift >= 0; shift -= 4) put_char(&out, "0123456789abcdef"[(hash >> shift) & 15]);
    }
    return out_end(&out);
}
