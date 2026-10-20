/* SPDX-License-Identifier: GPL-3.0-or-later
 * HTTPS as a Windows program does it, through WinHTTP: its TLS is schannel (secur32, over GnuTLS here) and the
 * server's certificate is checked against crypt32's root store. It asks for https://<host>/ (example.com when no
 * name is given). A console whose clock is behind sees every current certificate as not yet valid (public
 * certificates live about 90 days), so when the strict request fails it asks again with only the certificate's
 * dates waived, and says what the clock reads. Either answer proves the handshake. Then it builds each server's
 * certificate chain itself through crypt32 and reads the trust bits, which do not depend on the order WinHTTP checks
 * them in (with the date waived, Wine's WinHTTP never looks at the root): example.com's chain must reach a trusted
 * root, and untrusted-root.badssl.com's must not, or the root store would be accepting everything. */
#include <windows.h>
#include <winhttp.h>
#include <wincrypt.h>

static void say(const char *text)
{
    OutputDebugStringA(text);
}

/* The status code of GET https://host/, or 0 with *error set. flags: WinHTTP security flags to waive. */
static DWORD get(HINTERNET session, const WCHAR *host, WORD port, DWORD flags, DWORD *error)
{
    HINTERNET connection = WinHttpConnect(session, host, port, 0), request = NULL;
    DWORD status = 0, size = sizeof(status);
    *error = 0;
    if (connection) request = WinHttpOpenRequest(connection, L"GET", L"/", NULL, WINHTTP_NO_REFERER,
                                                 WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (request && flags) WinHttpSetOption(request, WINHTTP_OPTION_SECURITY_FLAGS, &flags, sizeof(flags));
    if (!request || !WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, NULL) ||
        !WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                             &status, &size, WINHTTP_NO_HEADER_INDEX)) {
        *error = GetLastError();
        status = 0;
    }
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    return status;
}

/* The trust errors of host's certificate chain (CERT_TRUST_*), or ~0u when no certificate came. The request waives
 * the dates and an unknown root, so the certificate arrives whatever they are. */
static DWORD chain_errors(HINTERNET session, const WCHAR *host, WORD port)
{
    HINTERNET connection = WinHttpConnect(session, host, port, 0), request = NULL;
    DWORD flags = SECURITY_FLAG_IGNORE_CERT_DATE_INVALID | SECURITY_FLAG_IGNORE_UNKNOWN_CA, errors = ~0u, size;
    const CERT_CONTEXT *certificate = NULL;
    if (connection) request = WinHttpOpenRequest(connection, L"GET", L"/", NULL, WINHTTP_NO_REFERER,
                                                 WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (request) WinHttpSetOption(request, WINHTTP_OPTION_SECURITY_FLAGS, &flags, sizeof(flags));
    size = sizeof(certificate);
    if (request && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, NULL) &&
        WinHttpQueryOption(request, WINHTTP_OPTION_SERVER_CERT_CONTEXT, &certificate, &size) && certificate) {
        CERT_CHAIN_PARA parameters = {sizeof(parameters)};
        const CERT_CHAIN_CONTEXT *chain = NULL;
        if (CertGetCertificateChain(NULL, certificate, NULL, certificate->hCertStore, &parameters, 0, NULL, &chain)) {
            errors = chain->TrustStatus.dwErrorStatus;
            CertFreeCertificateChain(chain);
        }
        CertFreeCertificateContext(certificate);
    }
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    return errors;
}

/* The LAN fixture uses an unpredictable root only inside the runner-owned
 * throwaway prefix. Add-new never replaces an existing certificate; remove the
 * exact returned context before exit. Production prefixes are never targeted. */
static HCERTSTORE fixture_store;
static const CERT_CONTEXT *fixture_root;
static BOOL fixture_error(const char *stage, DWORD error)
{
    char line[180];
    wsprintfA(line, "tls: fixture %s failed error=%#lx\n", stage, error);
    say(line);
    return FALSE;
}
static BOOL install_fixture_root(const char *path)
{
    WCHAR wide[MAX_PATH];
    static BYTE bytes[8192];
    DWORD count = 0;
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, MAX_PATH);
    HANDLE file = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE) return fixture_error("read-root-open", GetLastError());
    BOOL read = ReadFile(file, bytes, sizeof(bytes), &count, NULL);
    DWORD read_error = GetLastError();
    CloseHandle(file);
    if (!read || !count || count == sizeof(bytes)) return fixture_error("read-root-bytes", read_error);
    fixture_store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0,
        CERT_SYSTEM_STORE_CURRENT_USER, L"ROOT");
    if (!fixture_store) return fixture_error("open-root-store", GetLastError());
    if (!CertAddEncodedCertificateToStore(fixture_store, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
        bytes, count, CERT_STORE_ADD_NEW, &fixture_root)) return fixture_error("add-root", GetLastError());
    /* The chain engine opens its own registry-backed store. Publish this
     * transaction before it takes that snapshot. */
    if (!CertControlStore(fixture_store, 0, CERT_STORE_CTRL_COMMIT, NULL))
        return fixture_error("commit-root", GetLastError());
    return TRUE;
}

static unsigned port_argument(char **command)
{
    unsigned value = 0;
    while (**command == ' ') ++*command;
    while (**command >= '0' && **command <= '9') {
        value = value * 10 + *(*command)++ - '0';
        if (value > 65535) return 0;
    }
    return value;
}

void mainCRTStartup(void)
{
    char *command = GetCommandLineA(), host[256] = "example.com", line[300];
    WCHAR wide[256], untrusted[256] = L"untrusted-root.badssl.com";
    WORD trusted_port = INTERNET_DEFAULT_HTTPS_PORT, untrusted_port = INTERNET_DEFAULT_HTTPS_PORT;
    BOOL fixture = FALSE;
    DWORD error = 0, status, waived = 0, waived_error = 0, trusted_chain, untrusted_chain;
    unsigned length = 0;
    SYSTEMTIME now;

    if (*command == '"') { ++command; while (*command && *command != '"') ++command; if (*command) ++command; }
    else while (*command && *command != ' ') ++command;
    while (*command == ' ') ++command;
    if (*command) {
        while (*command && *command != ' ' && length < sizeof(host) - 1) host[length++] = *command++;
        host[length] = 0;
    }
    while (*command == ' ') ++command;
    if (*command) {
        unsigned first = port_argument(&command), second = port_argument(&command);
        while (*command == ' ') ++command;
        if (!first || !second || !*command || !install_fixture_root(command)) {
            say("PS5_PROTON_TLS_PROBE FAIL: LAN fixture arguments/root\n");
            if (fixture_store) CertCloseStore(fixture_store, 0);
            ExitProcess(1);
        }
        trusted_port = first; untrusted_port = second; fixture = TRUE;
    }
    MultiByteToWideChar(CP_UTF8, 0, host, -1, wide, 256);
    if (fixture) lstrcpyW(untrusted, wide);
    GetSystemTime(&now);
    wsprintfA(line, "tls: the clock reads %04u-%02u-%02u %02u:%02u UTC\n", now.wYear, now.wMonth, now.wDay, now.wHour,
              now.wMinute);
    say(line);
    HINTERNET session = WinHttpOpen(L"ps5-proton-tls-probe/1", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        say("PS5_PROTON_TLS_PROBE FAIL: WinHttpOpen\n");
        ExitProcess(1);
    }
    WinHttpSetTimeouts(session, 15000, 15000, 15000, 15000);
    status = get(session, wide, trusted_port, 0, &error);
    wsprintfA(line, "tls: https://%s/ status=%lu error=%lu\n", fixture ? "owned-fixture" : host, status, error);
    say(line);
    if (!status) {
        waived = get(session, wide, trusted_port, SECURITY_FLAG_IGNORE_CERT_DATE_INVALID, &waived_error);
        wsprintfA(line, "tls: https://%s/ with the certificate's dates waived: status=%lu error=%lu\n", fixture ? "owned-fixture" : host, waived,
                  waived_error);
        say(line);
    }
    trusted_chain = chain_errors(session, wide, trusted_port);
    untrusted_chain = chain_errors(session, untrusted, untrusted_port);
    const int refused = trusted_chain != ~0u && !(trusted_chain & CERT_TRUST_IS_UNTRUSTED_ROOT) &&
                        untrusted_chain != ~0u && (untrusted_chain & CERT_TRUST_IS_UNTRUSTED_ROOT);
    wsprintfA(line, "tls: chain errors: %s %#lx, %s %#lx (roots checked: %d)\n", fixture ? "owned-fixture" : host,
              trusted_chain, fixture ? "untrusted fixture" : "untrusted-root.badssl.com", untrusted_chain, refused);
    say(line);
    WinHttpCloseHandle(session);
    const int answered = (status >= 200 && status < 600) || (waived >= 200 && waived < 600);
    BOOL removed = TRUE;
    if (fixture) {
        removed = CertDeleteCertificateFromStore(fixture_root);
        fixture_root = NULL;
        if (removed) removed = CertControlStore(fixture_store, 0, CERT_STORE_CTRL_COMMIT, NULL);
        if (removed) say("tls: fixture root removed\n");
        CertCloseStore(fixture_store, 0);
    }
    const int passed = answered && refused && removed;
    say(passed ? (status ? "PS5_PROTON_TLS_PROBE PASS: an HTTPS answer; a trusted root reached, an unknown one not\n"
                         : "PS5_PROTON_TLS_PROBE PASS: an HTTPS answer once the certificate's dates were waived (the "
                           "clock is off); a trusted root reached, an unknown one not\n")
               : "PS5_PROTON_TLS_PROBE FAIL\n");
    ExitProcess(passed ? 0 : 1);
}
