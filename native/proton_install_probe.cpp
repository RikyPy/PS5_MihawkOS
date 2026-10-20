// SPDX-License-Identifier: GPL-3.0-or-later
//
// docs/SELF_INSTALL.md phase 0, the title's side, in an isolated build (PS5_INSTALL_PROBE) only. The plan
// (/app0/install-probe.txt) is the run's identity, then one step a line. Before them, always, what this title may
// create in /data before it is root (probe 3, the "preroot" line).
//
//   space <dir>                      answered without asking: a title cannot learn free space (below)
//   https <url> <ok|fail|any> <cap> [size]  one sceHttp2 GET (http:// too), certificate and hostname checks on, read
//                                    up to <cap> bytes, and exactly <size> when given (probe 2; "fail" expects the
//                                    request refused: a wrong host, an expired or untrusted chain). The response's Date
//                                    header is recorded beside the console's clock: the clock check needs no date change
//   tls <url> <ok|fail|any> <cap> [size]  the same GET through the title's own TLS (src/pw_https.h: Mbed TLS against
//                                    the console's /system/common/cert/CA_LIST.cer), the candidate for phase 6
//   post <port> <path>               an HTTP POST of "{}" to 127.0.0.1:<port> (probe 7: ShadowMountPlus's rescan)
//   usb                              which of /mnt/usb0-7 and /mnt/ext0-1 exist, how many entries (never their names),
//                                    and the read speed of the largest file in a mihawkos/ folder there (probe 6)
//   writes <mib> <seconds> <file>    buffered write() of <mib> MiB in files of <file> MiB under /data/ps5proton,
//                                    1 MiB a call, fsync() and close() each, stopped after <seconds> (probe 1)
//   small <count> <kib>              <count> files of <kib> KiB: write, fsync, rename into place, as an install does
//   loadexec                         replace this process with the title again; the steps after it run in the new one,
//                                    so a "writes" there says whether the write budget carried across LoadExec
//   notify                           a system notification, sent 4 s later from a thread (probe 5). No toast:
//                                    sceNotificationSend is a null import in the title (evidence/probe-install-2026-10-18-02)
//   launcher <frames>                go on into main's test run: the launcher drawn for <frames> under the notifications
//
// Every step writes one line or more to install-probe-results.txt in the title folder, with as_expected=0/1 where
// the plan names an expectation; the last line is result=PASS when every expectation held. Nothing outside
// /data/ps5proton/install-probe is written, and that folder is removed at the start and at the end.
#include "proton_install_probe.h"
#include "../ps5/src/platform.h"
#include "../ps5/src/proton_launch.h"
#include "../src/pw_https.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

extern "C" {
int sceNetInit();
int sceNetPoolCreate(const char *, int, int);
int sceSslInit(size_t);
int sceHttp2Init(int, int, size_t, int);
int sceHttp2CreateTemplate(int, const char *, int, int);
int sceHttp2DeleteTemplate(int);
int sceHttp2CreateRequestWithURL(int, const char *, const char *, uint64_t);
int sceHttp2DeleteRequest(int);
int sceHttp2SendRequest(int, const void *, size_t);
int sceHttp2GetStatusCode(int, int *);
int sceHttp2ReadData(int, void *, size_t);
int sceHttp2SetAutoRedirect(int, int);
int sceHttp2SetConnectTimeOut(int, unsigned);
int sceHttp2SetRecvTimeOut(int, unsigned);
int sceHttp2GetAllResponseHeaders(int, char **, size_t *);
int sceSystemServiceLoadExec(const char *path, char *const argv[]);
int sceNetPoolDestroy(int);
int sceNetResolverCreate(const char *, int, int);
int sceNetResolverDestroy(int);
int sceNetResolverStartNtoa(int, const char *, uint32_t *, int, int, int);
// The payload SDK's notify_debug sample's layout.
struct ProbeNotification { char unused[45]; char message[3075]; };
int sceKernelSendNotificationRequest(int, ProbeNotification *, size_t, int);
}

namespace {

#define PROBE_DIR PROTON_DATA_ROOT "/install-probe"
std::string gPreroot;
FILE *gOut = nullptr;
bool gAllExpected = true;

double seconds()
{
	return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

__attribute__((format(printf, 1, 2))) void note(const char *format, ...)
{
	char line[1024];
	va_list args;
	va_start(args, format);
	vsnprintf(line, sizeof(line), format, args);
	va_end(args);
	say("install-probe: %s", line);
	if (gOut) {
		fprintf(gOut, "%s\n", line);
		fflush(gOut);
	}
}

void expect(bool held)
{
	gAllExpected = gAllExpected && held;
}

int error_of(int result) { return result == 0 ? 0 : errno; }

std::string printable(const char *text, size_t length)
{
	std::string out;
	for (size_t i = 0; i < length && out.size() < 300; i++) {
		const char c = text[i];
		out += (c == '\r' || c == '\n' || c == '\t') ? ' ' : (c >= 0x20 && c < 0x7f ? c : '?');
	}
	return out;
}

void remove_probe_dir()
{
	for (const char *dir : { PROBE_DIR "/small", PROBE_DIR }) {
		if (DIR *d = opendir(dir)) {
			while (struct dirent *e = readdir(d)) {
				if (e->d_name[0] == '.') continue;
				unlink((std::string(dir) + "/" + e->d_name).c_str());
			}
			closedir(d);
		}
		rmdir(dir);
	}
}

bool write_all(int fd, const uint8_t *data, size_t size)
{
	while (size) {
		const ssize_t done = write(fd, data, size);
		if (done < 0 && errno == EINTR) continue;
		if (done <= 0) return false;
		data += done;
		size -= (size_t)done;
	}
	return true;
}

/* Neither statvfs nor statfs resolves in a title: statvfs ended the first round (evidence/probe-install-2026-10-18-01),
 * statfs is libkernel_sys's and ended the first-install rehearsal (2026-10-19). tools/check-title-imports.py refuses a
 * title that imports either, so the step says so instead of asking. */
void space(const std::string &dir)
{
	note("space dir=%s unavailable: a title has no statfs or statvfs", dir.c_str());
}

/* The Date header of a response, spaces as '_' so the line stays key=value; "-" when there is none. */
std::string date_header(int request)
{
	char *headers = nullptr;
	size_t length = 0;
	const int got = sceHttp2GetAllResponseHeaders(request, &headers, &length);
	if (got < 0 || !headers) return "none(" + std::to_string(got) + ")";
	const std::string all(headers, length);
	size_t at = std::string::npos;
	for (const char *name : { "\nDate:", "\ndate:", "\nDATE:" })
		if ((at = ("\n" + all).find(name)) != std::string::npos) break;
	if (at == std::string::npos) return "-";
	std::string value = all.substr(at + 5, all.find('\r', at + 5) == std::string::npos ? 64 : all.find('\r', at + 5) - at - 5);
	while (!value.empty() && value[0] == ' ') value.erase(0, 1);
	for (char &c : value) c = c == ' ' ? '_' : c;
	return value.substr(0, 64);
}

void https(const std::string &url, int want_ok, uint64_t cap, uint64_t size)
{
	static int http = -1, init = 0;
	if (http < 0 && !init) {
		(void)sceNetInit();
		const int pool = sceNetPoolCreate("MihawkOS probe", 1024 * 1024, 0);
		const int ssl = pool < 0 ? -1 : sceSslInit(1024 * 1024);
		http = ssl < 0 ? -1 : sceHttp2Init(pool, ssl, 1024 * 1024, 1);
		init = http < 0 ? (pool < 0 ? pool : ssl < 0 ? ssl : http) : 1;
		if (http < 0) note("https init failed pool=0x%08x ssl=0x%08x http=0x%08x", (unsigned)pool, (unsigned)ssl, (unsigned)http);
	}
	if (http < 0) {
		note("https url=%s init=0x%08x as_expected=%d", url.c_str(), (unsigned)init, want_ok != 1);
		expect(want_ok != 1);
		return;
	}
	const long long clock = (long long)time(nullptr);
	const double start = seconds();
	const int templ = sceHttp2CreateTemplate(http, "MihawkOS-probe/1", 3, 1);
	int request = -1, sent = 0, status = 0;
	uint64_t bytes = 0;
	int last = 0;
	if (templ >= 0) {
		sceHttp2SetAutoRedirect(templ, 1);
		sceHttp2SetConnectTimeOut(templ, 15000000);
		sceHttp2SetRecvTimeOut(templ, 30000000);
		request = sceHttp2CreateRequestWithURL(templ, "GET", url.c_str(), 0);
	}
	if (request >= 0) sent = sceHttp2SendRequest(request, nullptr, 0);
	const std::string date = request >= 0 && sent >= 0 ? date_header(request) : "-";
	if (request >= 0 && sent >= 0 && sceHttp2GetStatusCode(request, &status) >= 0 && status == 200) {
		static char buffer[65536];
		while (bytes < cap && (last = sceHttp2ReadData(request, buffer, sizeof(buffer))) > 0) bytes += (uint64_t)last;
	}
	const double elapsed = seconds() - start;
	if (request >= 0) sceHttp2DeleteRequest(request);
	if (templ >= 0) sceHttp2DeleteTemplate(templ);
	const bool ok = templ >= 0 && request >= 0 && sent >= 0 && status == 200 && last >= 0 && (bytes > 0 || cap == 0) &&
		(!size || bytes == size);
	const bool held = want_ok < 0 || ok == (want_ok == 1);
	note("https url=%s expect=%s template=0x%08x request=0x%08x send=0x%08x status=%d read_last=0x%08x bytes=%llu "
		"seconds=%.2f mib_s=%.2f console_clock=%lld date_header=%s as_expected=%d", url.c_str(),
		want_ok < 0 ? "any" : want_ok ? "ok" : "fail", (unsigned)templ, (unsigned)request, (unsigned)sent, status,
		(unsigned)last, (unsigned long long)bytes, elapsed, elapsed > 0 ? bytes / 1048576.0 / elapsed : 0.0, clock,
		date.c_str(), held);
	expect(held);
}

/* Names through the console's resolver, as the title's Wine side asks it (native/proton_wine.cpp title_resolve). */
int probe_resolve(const char *name, uint8_t address[4])
{
	uint32_t found = 0;
	const int pool = sceNetPoolCreate("probe-dns", 0x4000, 0);
	if (pool < 0) return -1;
	const int resolver = sceNetResolverCreate("probe-dns", pool, 0);
	const int result = resolver < 0 ? resolver : sceNetResolverStartNtoa(resolver, name, &found, 5000000, 2, 0);
	if (resolver >= 0) sceNetResolverDestroy(resolver);
	sceNetPoolDestroy(pool);
	if (result < 0 || !found) return -1;
	memcpy(address, &found, 4);
	return 0;
}

int discard(void *, const void *, size_t) { return 0; }

void tls(const std::string &url, int want_ok, uint64_t cap, uint64_t size)
{
	static PwHttps *https = nullptr;
	static int created = 1;
	static const char *const roots = "/system/common/cert/CA_LIST.cer";
	if (created == 1) {
		struct stat st {};
		const int seen = error_of(stat(roots, &st));
		created = pw_https_create(&https, roots, 0);
		note("tls roots=%s stat_errno=%d bytes=%lld create=%d", roots, seen, (long long)st.st_size, created);
	}
	if (created) {
		note("tls url=%s create=%d as_expected=%d", url.c_str(), created, want_ok != 1);
		expect(want_ok != 1);
		return;
	}
	const long long clock = (long long)time(nullptr);
	const double start = seconds();
	PwHttpsResult got;
	pw_https_get(https, url.c_str(), cap ? cap : 1, probe_resolve, discard, nullptr, &got);
	const double elapsed = seconds() - start;
	const bool ok = got.status == PW_HTTPS_OK && (!size || got.bytes == size);
	const bool held = want_ok < 0 || ok == (want_ok == 1);
	note("tls url=%s expect=%s status=%d http=%d verify=0x%x tls_error=-0x%x errno=%d redirects=%d host=%s bytes=%llu "
		"seconds=%.2f mib_s=%.2f entropy=%s console_clock=%lld as_expected=%d text=%s", url.c_str(),
		want_ok < 0 ? "any" : want_ok ? "ok" : "fail", got.status, got.http, got.verify_flags, -got.tls_error,
		got.sys_errno, got.redirects, got.host, (unsigned long long)got.bytes, elapsed,
		elapsed > 0 ? got.bytes / 1048576.0 / elapsed : 0.0, pw_https_entropy_source(), clock, (int)held,
		pw_https_status_text(got.status));
	expect(held);
}

void post(int port, const std::string &path)
{
	const int s = socket(AF_INET, SOCK_STREAM, 0);
	struct timeval timeout = { 10, 0 };
	setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
	struct sockaddr_in address = {};
	address.sin_family = AF_INET;
	address.sin_port = htons((uint16_t)port);
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	const double start = seconds();
	const int connected = s < 0 ? errno : error_of(connect(s, (struct sockaddr *)&address, sizeof(address)));
	char reply[2048] = {};
	size_t used = 0;
	if (!connected) {
		// ShadowMountPlus's API takes JSON only, with its length (sm_api_service.c); "{}" asks for a plain rescan.
		const std::string head = "POST " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\n"
			"Content-Length: 2\r\nConnection: close\r\n\r\n{}";
		if (write_all(s, (const uint8_t *)head.data(), head.size())) {
			ssize_t got;
			while (used < sizeof(reply) - 1 && (got = recv(s, reply + used, sizeof(reply) - 1 - used, 0)) > 0) used += (size_t)got;
		}
	}
	if (s >= 0) close(s);
	note("post port=%d path=%s connect_errno=%d reply_bytes=%zu seconds=%.2f reply=%s", port, path.c_str(), connected, used,
		seconds() - start, printable(reply, used).c_str());
}

void usb()
{
	static const char *const roots[] = { "/mnt/usb0", "/mnt/usb1", "/mnt/usb2", "/mnt/usb3", "/mnt/usb4", "/mnt/usb5",
		"/mnt/usb6", "/mnt/usb7", "/mnt/ext0", "/mnt/ext1" };
	for (const char *root : roots) {
		struct stat st {};
		const int found = error_of(stat(root, &st));
		DIR *d = found ? nullptr : opendir(root);
		unsigned count = 0;
		const int listed = d ? 0 : errno;
		if (d) {   // counted, never named: a stick holds the owner's files
			while (struct dirent *e = readdir(d)) count += strcmp(e->d_name, ".") && strcmp(e->d_name, "..");
			closedir(d);
		}
		if (found == ENOENT) continue;
		note("usb root=%s stat_errno=%d mode=0%o opendir_errno=%d entries=%u", root, found, (unsigned)st.st_mode,
			found ? 0 : listed, count);
		const std::string folder = std::string(root) + "/mihawkos";
		DIR *m = opendir(folder.c_str());
		if (!m) continue;
		std::string largest;
		off_t largest_size = -1;
		while (struct dirent *e = readdir(m)) {
			struct stat file {};
			const std::string path = folder + "/" + e->d_name;
			if (e->d_name[0] != '.' && stat(path.c_str(), &file) == 0 && S_ISREG(file.st_mode) && file.st_size > largest_size) {
				largest = path;
				largest_size = file.st_size;
			}
		}
		closedir(m);
		if (largest.empty()) continue;
		const int fd = open(largest.c_str(), O_RDONLY);
		static uint8_t buffer[1 << 20];
		uint64_t bytes = 0;
		ssize_t got = 0;
		const double start = seconds();
		while (fd >= 0 && bytes < (512ull << 20) && (got = read(fd, buffer, sizeof(buffer))) > 0) bytes += (uint64_t)got;
		const double elapsed = seconds() - start;
		note("usb read folder=%s/mihawkos size=%lld open_errno=%d bytes=%llu seconds=%.2f mib_s=%.1f", root,
			(long long)largest_size, fd >= 0 ? 0 : errno, (unsigned long long)bytes, elapsed,
			elapsed > 0 ? bytes / 1048576.0 / elapsed : 0.0);
		if (fd >= 0) close(fd);
	}
	note("usb done");
}

void writes(unsigned mib, unsigned limit, unsigned file_mib, const char *tag)
{
	mkdir(PROBE_DIR, 0777);
	static uint8_t chunk[1 << 20];
	uint64_t state = 0x9e3779b97f4a7c15ull;
	for (size_t i = 0; i < sizeof(chunk); i += 8) {   // xorshift: storage that compresses or dedups sees no shortcut
		state ^= state << 13, state ^= state >> 7, state ^= state << 17;
		memcpy(chunk + i, &state, 8);
	}
	const double start = seconds();
	unsigned written = 0, file = 0;
	int failed = 0;
	while (written < mib && seconds() - start < limit && !failed) {
		const std::string path = std::string(PROBE_DIR "/") + tag + "-" + std::to_string(file) + ".bin";
		const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (fd < 0) {
			failed = errno;
			break;
		}
		const double opened = seconds();
		unsigned in_file = 0;
		while (in_file < file_mib && written < mib && seconds() - start < limit) {
			memcpy(chunk, &written, sizeof(written));
			if (!write_all(fd, chunk, sizeof(chunk))) {
				failed = errno ? errno : EIO;
				break;
			}
			in_file++, written++;
		}
		const double wrote = seconds();
		const int synced = error_of(fsync(fd));
		const double done = seconds();
		close(fd);
		note("%s file=%u mib=%u write_s=%.2f fsync_s=%.2f mib_s=%.1f fsync_errno=%d at_s=%.1f", tag, file, in_file,
			wrote - opened, done - wrote, done > opened ? in_file / (done - opened) : 0.0, synced, done - start);
		file++;
	}
	const double total = seconds() - start;
	note("%s total_mib=%u seconds=%.1f mib_s=%.1f stopped=%s errno=%d", tag, written, total, total > 0 ? written / total : 0.0,
		failed ? "error" : written >= mib ? "size" : "time", failed);
	expect(!failed);
}

void small(unsigned count, unsigned kib)
{
	mkdir(PROBE_DIR, 0777);
	mkdir(PROBE_DIR "/small", 0777);
	std::vector<uint8_t> data((size_t)kib << 10, 0x5a);
	const double start = seconds();
	double slowest = 0;
	unsigned made = 0;
	int failed = 0;
	for (; made < count && seconds() - start < 60 && !failed; made++) {
		const double t = seconds();
		const std::string name = PROBE_DIR "/small/" + std::to_string(made);
		const std::string part = name + ".part";
		const int fd = open(part.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
		if (fd < 0 || !write_all(fd, data.data(), data.size()) || fsync(fd) != 0) failed = errno ? errno : EIO;
		if (fd >= 0) close(fd);
		if (!failed && rename(part.c_str(), name.c_str()) != 0) failed = errno;
		slowest = std::max(slowest, seconds() - t);
	}
	const double total = seconds() - start;
	note("small files=%u kib=%u seconds=%.2f ms_each=%.2f ms_slowest=%.1f errno=%d", made, kib, total,
		made ? total * 1000 / made : 0.0, slowest * 1000, failed);
	expect(!failed);
}

void finish(const std::string &plan, const std::string &marker)
{
	remove_probe_dir();
	remove(plan.c_str());
	remove(marker.c_str());
	note("result=%s", gAllExpected ? "PASS" : "FAIL");
	fclose(gOut);
	gOut = nullptr;
}

} // namespace

void proton_install_probe_preroot()
{
	if (access(proton_title_path("/app0/install-probe.txt").c_str(), F_OK) != 0 ||
		access(proton_title_path("/app0/install-probe.phase2").c_str(), F_OK) == 0) return;
	struct stat st {};
	auto exists = [&](const char *path) { return error_of(stat(path, &st)); };
	const int data = exists("/data"), homebrew = exists("/data/homebrew"), root = exists(PROTON_DATA_ROOT);
	const int devconf = exists("/data/homebrew/dev.conf");
	// Probe names only: the real /data/ps5proton decides how the title asks for root (proton_launch.cpp request_root).
	const char *folder = "/data/mihawkos-preroot-probe", *file = "/data/homebrew/mihawkos-preroot-probe.conf";
	const int made = error_of(mkdir(folder, 0755));
	if (!made) rmdir(folder);
	const int fd = open(file, O_WRONLY | O_CREAT | O_EXCL, 0644);
	int created = fd >= 0 ? 0 : errno;
	if (fd >= 0) {
		if (write(fd, "probe\n", 6) != 6) created = errno ? errno : EIO;
		close(fd);   // nothing stays open across the root request (request_root's credential references)
		unlink(file);
	}
	char line[256];
	snprintf(line, sizeof(line), "preroot uid=%d euid=%d stat_data=%d stat_homebrew=%d stat_ps5proton=%d stat_devconf=%d "
		"mkdir_data=%d create_homebrew=%d", (int)getuid(), (int)geteuid(), data, homebrew, root, devconf, made, created);
	gPreroot = line;
}

bool proton_install_probe(bool elevated, int &status)
{
	const std::string planPath = proton_title_path("/app0/install-probe.txt");
	const std::string marker = proton_title_path("/app0/install-probe.phase2");
	std::ifstream plan(planPath);
	if (!plan) return false;
	std::string identity, text;
	std::getline(plan, identity);
	identity = identity.substr(0, identity.find(' '));
	std::vector<std::vector<std::string>> steps;
	while (std::getline(plan, text)) {
		std::istringstream words(text);
		std::vector<std::string> step;
		for (std::string word; words >> word;) step.push_back(word);
		if (!step.empty()) steps.push_back(step);
	}
	plan.close();
	const bool second = access(marker.c_str(), F_OK) == 0;
	const std::string results = proton_title_path("/app0/install-probe-results.txt");
	gOut = fopen(results.c_str(), second ? "a" : "w");
	if (!gOut) {
		say("install-probe: cannot write %s", results.c_str());
		status = 1;
		return true;
	}
	chmod(results.c_str(), 0666);
	size_t i = 0;
	if (second) {
		while (i < steps.size() && steps[i][0] != "loadexec") i++;
		i++;
		note("phase=2 pid=%d uid=%d elevated=%d", (int)getpid(), (int)getuid(), (int)elevated);
	} else {
		note("install-probe identity=%s phase=1 pid=%d uid=%d elevated=%d", identity.c_str(), (int)getpid(), (int)getuid(),
			(int)elevated);
		if (!gPreroot.empty()) note("%s", gPreroot.c_str());
		remove_probe_dir();
	}
	for (; i < steps.size(); i++) {
		const auto &s = steps[i];
		auto arg = [&](size_t n) { return n < s.size() ? s[n] : std::string(); };
		auto number = [&](size_t n) { return (unsigned long long)strtoull(arg(n).c_str(), nullptr, 0); };
		if (s[0] == "space") {
			space(arg(1));
		} else if (s[0] == "https") {
			https(arg(1), arg(2) == "ok" ? 1 : arg(2) == "any" ? -1 : 0, number(3), number(4));
		} else if (s[0] == "tls") {
			tls(arg(1), arg(2) == "ok" ? 1 : arg(2) == "any" ? -1 : 0, number(3), number(4));
		} else if (s[0] == "post") {
			post((int)number(1), arg(2));
		} else if (s[0] == "usb") {
			usb();
		} else if (s[0] == "writes") {
			writes((unsigned)number(1), (unsigned)number(2), (unsigned)number(3), second ? "after-loadexec" : "first");
		} else if (s[0] == "small") {
			small((unsigned)number(1), (unsigned)number(2));
		} else if (s[0] == "loadexec") {
			if (FILE *m = fopen(marker.c_str(), "wb")) fclose(m);
			note("loadexec at_clock=%lld", (long long)time(nullptr));
			fclose(gOut);
			gOut = nullptr;
			static char cycle[] = "cycle=1";
			char *const argv[] = { cycle, nullptr };
			for (const char *eboot : { "/app0/eboot.bin", PROTON_SANDBOX_APP0 "/eboot.bin" }) sceSystemServiceLoadExec(eboot, argv);
			gOut = fopen(results.c_str(), "a");
			note("loadexec refused");
			expect(false);
			break;
		} else if (s[0] == "notify") {
			// The thread owns the results from here: main draws the launcher (the next step) while it sends.
			std::thread([planPath, marker] {
				usleep(4000000);
				ProbeNotification request {};
				snprintf(request.message, sizeof(request.message), "MihawkOS probe: system notification (1 of 2)");
				const int kernel = sceKernelSendNotificationRequest(0, &request, sizeof(request), 0);
				note("notify kind=kernel result=0x%08x at_clock=%lld", (unsigned)kernel, (long long)time(nullptr));
				expect(kernel >= 0);
				finish(planPath, marker);
			}).detach();
			if (i + 1 >= steps.size() || steps[i + 1][0] != "launcher") {
				usleep(5000000);
				return status = 0, true;
			}
		} else if (s[0] == "launcher") {
			const std::string run = proton_title_path("/app0/test-run.txt");
			if (FILE *t = fopen(run.c_str(), "w")) {
				fprintf(t, "frames %llu\nsamples launcher\n", number(1) ? number(1) : 900ull);
				fclose(t);
			}
			note("launcher frames=%llu", number(1) ? number(1) : 900ull);
			return false;
		} else {
			note("unknown step %s", s[0].c_str());
			expect(false);
		}
	}
	finish(planPath, marker);
	status = gAllExpected ? 0 : 1;
	return true;
}
