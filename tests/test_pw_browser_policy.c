/* SPDX-License-Identifier: GPL-3.0-or-later
 * src/pw_browser_policy.h: MihawkBrowser's address box, HTTPS-only navigation, download names and log privacy. */
#include "../src/pw_browser_policy.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void address(const char *input, int expected, const char *url)
{
    char out[256];
    int result = pw_browser_address(input, out, sizeof out);
    if (result != expected || strcmp(out, url)) {
        fprintf(stderr, "address \"%s\": %d \"%s\", expected %d \"%s\"\n", input, result, out, expected, url);
        assert(0);
    }
}

static void navigation(const char *url, PwBrowserNavigation expected, const char *upgraded)
{
    char out[256];
    assert(pw_browser_navigation(url, out, sizeof out) == expected);
    assert(!strcmp(out, upgraded));
}

static void name(const char *suggested, const char *expected)
{
    char out[256];
    assert(pw_browser_download_name(suggested, out, sizeof out) == PW_OK);
    if (strcmp(out, expected)) { fprintf(stderr, "name \"%s\": \"%s\", expected \"%s\"\n", suggested, out, expected); assert(0); }
}

static void log_url(const char *url, const char *expected_prefix)
{
    char out[256];
    assert(pw_browser_log_url(url, out, sizeof out) == PW_OK);
    if (strncmp(out, expected_prefix, strlen(expected_prefix))) {
        fprintf(stderr, "log \"%s\": \"%s\", expected \"%s...\"\n", url, out, expected_prefix);
        assert(0);
    }
}

int main(void)
{
    /* The address box. */
    address("https://store.example.com/about/", PW_OK, "https://store.example.com/about/");
    address("  http://example.com/a?b=c  ", PW_OK, "https://example.com/a?b=c");
    address("HTTP://Example.com", PW_OK, "https://Example.com");
    address("example.com", PW_OK, "https://example.com");
    address("www.gog.com/galaxy", PW_OK, "https://www.gog.com/galaxy");
    address("localhost:8443/x", PW_OK, "https://localhost:8443/x");
    address("192.0.2.20:1749", PW_OK, "https://192.0.2.20:1749");
    address("[::1]:443/", PW_OK, "https://[::1]:443/");
    address("bücher.de", PW_OK, "https://bücher.de");
    address("about:blank", PW_OK, "about:blank");
    address("launcher download", PW_OK, PW_BROWSER_SEARCH "launcher+download");
    address("1.5", PW_OK, PW_BROWSER_SEARCH "1.5");
    address("v2.0", PW_OK, PW_BROWSER_SEARCH "v2.0");
    address("c++ & co?", PW_OK, PW_BROWSER_SEARCH "c%2B%2B+%26+co%3F");
    address("re:zero", PW_OK, PW_BROWSER_SEARCH "re%3Azero");
    address("file:///c:/windows/win.ini", PW_ERR_UNSUPPORTED, "");
    address("chrome://gpu", PW_ERR_UNSUPPORTED, "");
    address("javascript:alert(1)", PW_ERR_UNSUPPORTED, "");
    address("data:text/html,x", PW_ERR_UNSUPPORTED, "");
    address("ftp://example.com/", PW_ERR_UNSUPPORTED, "");
    address("   ", PW_ERR_PRECONDITION, "");
    char tiny[12];
    assert(pw_browser_address("example.com", tiny, sizeof tiny) == PW_ERR_LIMIT && !tiny[0]);

    /* Navigations: HTTPS only, http upgraded, the rest refused. */
    navigation("https://example.com/", PW_BROWSER_NAV_ALLOW, "");
    navigation("http://example.com/x", PW_BROWSER_NAV_UPGRADE, "https://example.com/x");
    navigation("about:blank", PW_BROWSER_NAV_ALLOW, "");
    navigation("about:srcdoc", PW_BROWSER_NAV_ALLOW, "");
    navigation("blob:https://example.com/0f0e", PW_BROWSER_NAV_ALLOW, "");
    navigation("data:text/plain,hi", PW_BROWSER_NAV_ALLOW, "");
    navigation("file:///c:/users/", PW_BROWSER_NAV_REFUSE, "");
    navigation("chrome://settings/", PW_BROWSER_NAV_REFUSE, "");
    navigation("about:version", PW_BROWSER_NAV_REFUSE, "");
    navigation("launcher://run/730", PW_BROWSER_NAV_REFUSE, "");
    navigation("", PW_BROWSER_NAV_REFUSE, "");

    /* Download names. */
    name("ExampleSetup.exe", "ExampleSetup.exe");
    name("../../windows/system32/evil.dll", "evil.dll");
    name("a\\b\\c.msi", "c.msi");
    name("what?<is>:this|\"*.txt", "what__is__this___.txt");
    name("  spaced.exe. . ", "spaced.exe");
    name("CON", "_CON");
    name("nul.txt", "_nul.txt");
    name("com7.log", "_com7.log");
    name("console.exe", "console.exe");
    name("", "download");
    name("...", "download");
    name("tab\there.exe", "tab_here.exe");
    char longer[400], out[256];
    memset(longer, 'a', 300);
    strcpy(longer + 300, ".exe");
    assert(pw_browser_download_name(longer, out, sizeof out) == PW_OK);
    assert(strlen(out) == PW_BROWSER_NAME_MAX && !strcmp(out + PW_BROWSER_NAME_MAX - 4, ".exe"));
    memset(longer, 0, sizeof longer);
    longer[0] = 'x';  /* 'x' then 'é' x150: the 196-byte cut falls inside an 'é', so it backs off one byte */
    for (int i = 0; i < 150; ++i) memcpy(longer + 1 + 2 * i, "\xc3\xa9", 2);
    strcpy(longer + 301, ".exe");
    assert(pw_browser_download_name(longer, out, sizeof out) == PW_OK);
    assert(strlen(out) == 199 && !strcmp(out + 195, ".exe") && (unsigned char)out[194] == 0xa9);

    assert(pw_browser_download_candidate("setup.exe", 0, out, sizeof out) == PW_OK && !strcmp(out, "setup.exe"));
    assert(pw_browser_download_candidate("setup.exe", 2, out, sizeof out) == PW_OK && !strcmp(out, "setup (2).exe"));
    assert(pw_browser_download_candidate("archive.tar.gz", 1, out, sizeof out) == PW_OK && !strcmp(out, "archive.tar (1).gz"));
    assert(pw_browser_download_candidate("README", 10, out, sizeof out) == PW_OK && !strcmp(out, "README (10)"));
    assert(pw_browser_download_candidate(".hidden", 1, out, sizeof out) == PW_OK && !strcmp(out, ".hidden (1)"));

    assert(pw_browser_is_installer("ExampleSetup.exe") && pw_browser_is_installer("EpicInstaller.MSI"));
    assert(!pw_browser_is_installer("notes.txt") && !pw_browser_is_installer(".exe") && !pw_browser_is_installer(NULL));

    /* Log lines carry no userinfo, query, fragment, path or address (rule 5). */
    log_url("https://user:secret@Store.Example.COM:8443/account/12345?token=abc#frag",
            "scheme=https host=store.example.com path=");
    assert(pw_browser_log_url("https://user:secret@Store.Example.COM:8443/account/12345?token=abc#frag", out, sizeof out) == PW_OK);
    assert(!strstr(out, "user") && !strstr(out, "secret") && !strstr(out, "token") && !strstr(out, "frag") &&
           !strstr(out, "12345") && !strstr(out, "8443"));
    log_url("https://192.0.2.20:1749/fixture.bin", "scheme=https host=ip path=");
    log_url("https://[fe80::1]/", "scheme=https host=ip path=");
    log_url("data:text/html,<b>secret</b>", "scheme=data");
    assert(pw_browser_log_url("data:text/html,<b>secret</b>", out, sizeof out) == PW_OK && !strcmp(out, "scheme=data"));
    char a[128], b[128];
    pw_browser_log_url("https://example.com/a?x=1", a, sizeof a);
    pw_browser_log_url("https://example.com/a?x=2", b, sizeof b);
    assert(!strcmp(a, b));  /* the query never shows */
    pw_browser_log_url("https://example.com/b", b, sizeof b);
    assert(strcmp(a, b));   /* two paths are told apart */

    /* The switch list: --disable-features once, the GPU in the browser process, one renderer. */
    int disable_features = 0, in_process_gpu = 0, renderer_limit = 0;
    for (size_t i = 0; pw_browser_switches[i][0]; ++i) {
        assert(strncmp(pw_browser_switches[i][0], "--", 2));
        disable_features += !strcmp(pw_browser_switches[i][0], "disable-features");
        in_process_gpu += !strcmp(pw_browser_switches[i][0], "in-process-gpu");
        renderer_limit += !strcmp(pw_browser_switches[i][0], "renderer-process-limit") && !strcmp(pw_browser_switches[i][1], "1");
    }
    assert(disable_features == 1 && in_process_gpu == 1 && renderer_limit == 1);
    puts("test_pw_browser_policy: ok");
    return 0;
}
