/*
 * MihawkOS - one HTTP(S) GET from the title (proton_fetch.h).
 *
 * Copyright (C) 2026 Mihawk
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "proton_fetch.h"
#include "proton_launch.h"

#include "../../src/pw_https.h"

#include <string.h>

#include <mutex>

#if !defined(PS5_HOST_REFERENCE)
extern "C" {
int sceNetPoolCreate(const char *, int, int);
int sceNetPoolDestroy(int);
int sceNetResolverCreate(const char *, int, int);
int sceNetResolverDestroy(int);
int sceNetResolverStartNtoa(int, const char *, uint32_t *, int, int, int);
}
#endif

namespace {

/* The console's roots, readable once the title is elevated (ps5-native-app-boilerplate/docs/CURL.md). */
constexpr const char *kRoots = "/system/common/cert/CA_LIST.cer";

/* Names through the console's resolver, as the title's Wine side asks it (native/proton_wine.cpp title_resolve):
 * getaddrinfo is a null import in the title. Five seconds, two retries. */
int resolve(const char *name, uint8_t address[4])
{
#if defined(PS5_HOST_REFERENCE)
	(void)name, (void)address;
	return -1;
#else
	uint32_t found = 0;
	const int pool = sceNetPoolCreate("mihawkos-fetch", 0x4000, 0);
	if (pool < 0) return -1;
	const int resolver = sceNetResolverCreate("mihawkos-fetch", pool, 0);
	const int result = resolver < 0 ? resolver : sceNetResolverStartNtoa(resolver, name, &found, 5000000, 2, 0);
	if (resolver >= 0) sceNetResolverDestroy(resolver);
	sceNetPoolDestroy(pool);
	if (result < 0 || !found) return -1;
	memcpy(address, &found, 4);
	return 0;
#endif
}

int to_sink(void *context, const void *data, size_t size)
{
	return (*static_cast<const std::function<bool(const void *, size_t)> *>(context))(data, size) ? 0 : 1;
}

} // namespace

ProtonFetch proton_fetch(const std::string &url, uint64_t limit, const std::function<bool(const void *, size_t)> &sink)
{
	/* One TLS context for the title's life: the roots are parsed once. One fetch at a time (the install's thread). */
	static std::mutex lock;
	static PwHttps *https = nullptr;
	static int created = 1;
	std::lock_guard<std::mutex> hold(lock);
	ProtonFetch result;
	if (created) created = pw_https_create(&https, kRoots, 0);
	if (created) {
		result.status = created;
		result.error = std::string("The console's trusted certificates could not be read (") + kRoots + ").";
		return result;
	}
	PwHttpsResult got;
	pw_https_get(https, url.c_str(), limit, resolve, to_sink, const_cast<std::function<bool(const void *, size_t)> *>(&sink),
		&got);
	result.status = got.status;
	result.http = got.http;
	result.bytes = got.bytes;
	result.ok = got.status == PW_HTTPS_OK;
	if (!result.ok && got.status != PW_HTTPS_STOPPED) {
		const std::string host = got.host[0] ? got.host : url;
		result.error = std::string("Downloading from ") + host + " failed: " + pw_https_status_text(got.status) +
			(got.status == PW_HTTPS_STATUS ? " (" + std::to_string(got.http) + ")" : std::string()) + ".";
		proton_say("fetch: %s status=%d http=%d verify=0x%x tls=-0x%x errno=%d bytes=%llu", host.c_str(), got.status,
			got.http, got.verify_flags, -got.tls_error, got.sys_errno, (unsigned long long)got.bytes);
	}
	return result;
}
