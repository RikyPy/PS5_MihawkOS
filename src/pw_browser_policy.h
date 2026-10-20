/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_BROWSER_POLICY_H
#define PW_BROWSER_POLICY_H
#include "../include/prospero_win.h"

/* MihawkBrowser's decisions that need neither Windows nor CEF (docs/MIHAWK_BROWSER.md): what the address box's text
 * becomes, which navigations happen (HTTPS only: http is upgraded, every other scheme refused, §1), what a download
 * is named in the Downloads folder (§3.5), and the only things a log line may say about a URL (§3.4, rule 5: no
 * userinfo, query, fragment, path or IP address). src/pw_browser.c calls these at the CEF boundary, after converting
 * CEF's UTF-16 to UTF-8.
 *
 * Strings are UTF-8 and NUL-terminated. Every output is bounded by its capacity: PW_ERR_LIMIT when it would not fit,
 * and the output is then the empty string. No allocation, no libc beyond the compiler's own headers, so the same file
 * builds for the host tests and for x86_64-windows. */

/* DuckDuckGo's script-free results page: light, and it works with jitless V8 (decision 7). */
#define PW_BROWSER_SEARCH "https://html.duckduckgo.com/html/?q="

typedef enum PwBrowserNavigation {
    PW_BROWSER_NAV_ALLOW,    /* https:, and about:blank, about:srcdoc, data:, blob: inside pages */
    PW_BROWSER_NAV_UPGRADE,  /* http: — load the https: URL written to upgraded instead */
    PW_BROWSER_NAV_REFUSE,   /* file:, chrome:, ftp:, javascript:, custom schemes, anything unparsable */
} PwBrowserNavigation;

/* The address box's text as a URL to load: https:// kept, http:// upgraded, a bare host ("example.com/x",
 * "localhost", "192.0.2.2:8443") given https://, about:blank kept, anything else a search. PW_OK;
 * PW_ERR_PRECONDITION for blank input; PW_ERR_UNSUPPORTED for another scheme ("file:///c:/", "chrome://gpu"). */
int pw_browser_address(const char *input, char *url, size_t capacity);
/* The verdict on a navigation CEF is about to make (on_before_browse), from its canonical URL. */
PwBrowserNavigation pw_browser_navigation(const char *url, char *upgraded, size_t capacity);
/* A download's file name from the server's suggestion: the last path component, Windows' reserved characters and
 * controls as '_', no leading spaces or trailing dots and spaces, a device name (CON, NUL, COM1...) prefixed with
 * '_', at most PW_BROWSER_NAME_MAX bytes with the extension kept, "download" when nothing is left. PW_OK. */
enum { PW_BROWSER_NAME_MAX = 200 };
int pw_browser_download_name(const char *suggested, char *name, size_t capacity);
/* The n-th name to try when the file exists, as Windows numbers copies: n 0 is the name, 2 makes "setup (2).exe". */
int pw_browser_download_candidate(const char *name, unsigned n, char *out, size_t capacity);
/* 1 when the file is an installer Open hands off to (.exe, .msi, any case), else 0. */
int pw_browser_is_installer(const char *name);
/* "scheme=https host=store.example.com path=<16 hex digits of the path's FNV-1a>" for hierarchical URLs, with
 * "host=ip" for an address literal; "scheme=data" for the rest. Nothing else of the URL is written. PW_OK. */
int pw_browser_log_url(const char *url, char *out, size_t capacity);

/* The browser process's switches (§3.1), as {name, value} pairs with value NULL for a flag, ended by {NULL, NULL}.
 * --disable-features appears once: a repeated switch keeps only its last value. */
extern const char *const pw_browser_switches[][2];

/* Chrome policies (chromeenterprise.google/policies), written under HKCU\PW_BROWSER_POLICY_KEY before CEF starts and
 * named to CEF by cef_settings_t.chrome_policy_id. CEF 150 runs every browser on Chrome's profile, which signs in,
 * reports downloads to Safe Browsing and asks google.com for AI eligibility on its own (browser-host-30's NetLog:
 * accounts.google.com, sb-ssl.google.com, www.google.com/async/folae); these turn that off, make DuckDuckGo the
 * search engine and replace the two preferences of §3.1. A policy with text set is a string, else a DWORD. */
#define PW_BROWSER_POLICY_KEY "Software\\Policies\\MihawkBrowser"
typedef struct PwBrowserPolicy { const char *name; uint32_t value; const char *text; } PwBrowserPolicy;
extern const PwBrowserPolicy pw_browser_policies[];  /* ended by {NULL} */

#endif
