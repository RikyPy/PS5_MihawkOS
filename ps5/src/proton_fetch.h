/*
 * MihawkOS - one HTTP(S) GET from the title (docs/SELF_INSTALL.md, phase 6).
 *
 * The network code lives in the title only: a Wine helper that loaded libSceNet took the title down
 * (native/proton_wine.cpp, name lookups). The TLS is the title's own (src/pw_https.h): an elevated title's sceSsl
 * rejects every public certificate (evidence/probe-install-2026-10-18-02), Mbed TLS against the console's
 * /system/common/cert/CA_LIST.cer does not (evidence/probe-tls-2026-10-18-01). Certificates and hostnames are always
 * checked; 15 s to connect, 30 s without a byte, redirects followed, the body handed on as it comes.
 *
 * Copyright (C) 2026 Mihawk
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef PROTON_FETCH_H
#define PROTON_FETCH_H

#include <stdint.h>

#include <functional>
#include <string>

struct ProtonFetch {
	bool ok{ false };
	int status{ 0 };         /* PW_HTTPS_* (src/pw_https.h) */
	int http{ 0 };           /* the HTTP status, 0 when none came */
	uint64_t bytes{ 0 };
	std::string error;       /* for the screen: what failed, in plain words */
};

/* GET url and hand its body to `sink` (false from it stops the transfer). More than `limit` bytes is a failure. */
ProtonFetch proton_fetch(const std::string &url, uint64_t limit, const std::function<bool(const void *, size_t)> &sink);

#endif
