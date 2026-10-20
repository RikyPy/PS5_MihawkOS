/*
 * MihawkOS - "Check for updates" (proton_update.h).
 *
 * Copyright (C) 2026 Mihawk
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "proton_update.h"
#include "proton_fetch.h"
#include "proton_install.h"
#include "proton_launch.h"
#include "../../src/pw_release.h"
#include "../../src/pw_sha256.h"
#include "../../src/pw_update.h"

#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

#include "json.hpp" // external/tinygltf's vendored nlohmann::json, already in the title

#if !defined(PS5_HOST_REFERENCE)
extern "C" int sceSystemServiceLoadExec(const char *path, char *const argv[]);
#endif

/* Where the owner publishes the signed manifest, and the public half of their key (64 hex digits,
 * platform/update/release-key.txt, which tools/sign-release.py keygen writes). Both empty until the owner publishes. */
#ifndef PROTON_UPDATE_URL
#define PROTON_UPDATE_URL ""
#endif
#ifndef PROTON_RELEASE_KEY
#define PROTON_RELEASE_KEY ""
#endif

namespace {

constexpr const char *kUpdate = PROTON_DATA_ROOT "/update";
constexpr uint64_t kMarginBytes = 64ull << 20;

std::mutex gLock;
std::atomic<bool> gCancel{ false };   /* shortcut: no cancel on screen; the title pack is small (about 25 MB) */
ProtonUpdate gState;
PwPackManifest gManifest{};
bool gHaveManifest = false;
std::string gBase;
PwPack *gPack = nullptr;
bool gSwitchedHere = false;   /* this process switched and its restart was refused: it is not the new title */
std::chrono::steady_clock::time_point gStarted;

std::string read_file(const std::string &path)
{
	std::string text;
	if (FILE *file = fopen(path.c_str(), "rb")) {
		char buffer[16384];
		size_t got;
		while ((got = fread(buffer, 1, sizeof(buffer), file)) > 0) text.append(buffer, got);
		fclose(file);
	}
	return text;
}

std::string running_version()
{
	const nlohmann::json param = nlohmann::json::parse(read_file(proton_title_path("/app0/sce_sys/param.json")), nullptr, false);
	if (!param.is_object()) return {};
	const auto version = param.find("contentVersion");
	return version != param.end() && version->is_string() ? version->get<std::string>() : std::string();
}

/* A PC fixture's address in the title folder (a rehearsal), else the build's. */
std::string source()
{
	std::string url = read_file(proton_title_path("/app0/update-source.txt"));
	url = url.substr(0, url.find_first_of("\r\n"));
	if (url.empty()) url = PROTON_UPDATE_URL;
	const bool web = !url.compare(0, 7, "http://") || !url.compare(0, 8, "https://");
	return web && url.find_first_of(" \t\"'") == std::string::npos ? url : std::string();
}

std::string megabytes(uint64_t bytes)
{
	char text[32];
	snprintf(text, sizeof(text), "%.0f MB", bytes / 1e6);
	return text;
}

std::string fetch_text(const std::string &url, uint64_t limit, ProtonFetch &got)
{
	std::string text;
	got = proton_fetch(url, limit, [&](const void *data, size_t size) {
		text.append(static_cast<const char *>(data), size);
		return true;
	});
	return text;
}

void finish(ProtonUpdate found)
{
	proton_say("update: %s %s: %s", found.phase == ProtonUpdate::failed ? "failed" : "state", found.version.c_str(),
		found.message.c_str());
	std::lock_guard<std::mutex> hold(gLock);
	gState = found;
}

void check()
{
	ProtonUpdate found;
	found.phase = ProtonUpdate::failed;
	const std::string running = running_version();
	char staged[64];
	if (pw_update_pending(kUpdate, staged, sizeof(staged))) {
		found.phase = ProtonUpdate::ready;
		found.version = staged;
		found.message = PS5_TITLE_NAME " " + found.version + " is downloaded. It is switched in when " PS5_TITLE_NAME
			" starts: press CROSS to restart now.";
		return finish(found);
	}
	uint8_t key[32];
	const std::string url = source();
	if (!pw_release_hex32(PROTON_RELEASE_KEY, key)) {
		found.message = "This build carries no release key, so it cannot check for updates.";
		return finish(found);
	}
	if (url.empty()) {
		found.message = "This build has no update address: " PS5_TITLE_NAME " is not published yet.";
		return finish(found);
	}
	ProtonFetch got;
	const std::string text = fetch_text(url, 16384, got);
	if (!got.ok) {
		found.message = got.error.empty() ? "The update check stopped." : got.error;
		return finish(found);
	}
	PwRelease release;
	const int parsed = pw_release_parse(text.data(), text.size(), key, &release);
	if (parsed != PW_RELEASE_OK) {
		found.message = parsed == PW_RELEASE_SIGNATURE ?
			"The update offered is not signed with " PS5_TITLE_NAME "'s key, so none of it is installed." :
			"The update's description is damaged, so none of it is installed.";
		return finish(found);
	}
	found.version = release.version;
	if (pw_release_version_compare(release.version, running.c_str()) <= 0) {
		found.phase = ProtonUpdate::current;
		found.message = PS5_TITLE_NAME " " + running + " is the newest release.";
		return finish(found);
	}
	/* The title pack's manifest, by the hash the signed text gives; the runtime manifest it carries, by the other. */
	const std::string manifest = fetch_text(std::string(release.base) + release.title, 4u << 20, got);
	uint8_t digest[32];
	PwSha256 hash;
	pw_sha256_init(&hash);
	pw_sha256_update(&hash, manifest.data(), manifest.size());
	pw_sha256_final(&hash, digest);
	PwPackManifest title{};
	if (!got.ok || memcmp(digest, release.title_sha256, 32) ||
		pw_pack_parse(manifest.data(), manifest.size(), &title) != PW_PACK_OK) {
		found.message = got.ok ? "The update's title manifest is not the one its signed description names." : got.error;
		return finish(found);
	}
	bool runtime = false;
	for (size_t f = 0; f < title.file_count; f++)
		runtime = runtime || (!strcmp(title.files[f].path, "runtime-pack.txt") && !memcmp(title.files[f].sha256, release.runtime_sha256, 32));
	if (!title.title || !runtime) {
		pw_pack_manifest_free(&title);
		found.message = "The update's title does not pin the runtime its signed description names.";
		return finish(found);
	}
	found.phase = ProtonUpdate::available;
	found.download = title.packed;
	found.message = PS5_TITLE_NAME " " + found.version + " is available: " + megabytes(title.packed) +
		" to download. Press CROSS to download it; it is switched in the next time " PS5_TITLE_NAME " starts.";
	{
		std::lock_guard<std::mutex> hold(gLock);
		if (gHaveManifest) pw_pack_manifest_free(&gManifest);
		gManifest = title;
		gHaveManifest = true;
		gBase = release.base;
	}
	finish(found);
}

void download(std::string version)
{
	ProtonUpdate found;
	found.phase = ProtonUpdate::failed;
	found.version = version;
	// No free-space check: a title process has no statfs (proton_install.cpp); a full disk is said by ENOSPC.
	const std::string stage = std::string(kUpdate) + "/stage";
	mkdir(kUpdate, 0777);
	mkdir(stage.c_str(), 0777);
	ProtonPackJob job;
	job.manifest = &gManifest;
	// shortcut: the stage starts empty, so each update downloads the whole title pack (about 25 MB); link the title's
	// unchanged files into the stage first if releases grow.
	job.root = stage;
	job.source = gBase;
	job.record = PW_UPDATE_RECORD;
	job.cancel = &gCancel;
	job.publish = [](PwPack *pack) {
		std::lock_guard<std::mutex> hold(gLock);
		gPack = pack;
	};
	ProtonInstall::Problem problem = ProtonInstall::none;
	const std::string message = proton_pack_install(job, problem, found.progress);
	found.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - gStarted).count();
	const int ready = message.empty() ? pw_update_mark_ready(kUpdate, version.c_str()) : 0;
	if (!message.empty() || ready) {
		found.message = !message.empty() ? message : std::string("The update could not be marked ready: ") + strerror(-ready) + ".";
		return finish(found);
	}
	found.phase = ProtonUpdate::ready;
	found.message = PS5_TITLE_NAME " " + version + " is downloaded and checked. Press CROSS to restart into it now, or it is "
		"switched in the next time " PS5_TITLE_NAME " starts.";
	finish(found);
}

/* This title again, from the start: the switch runs as it begins. */
void restart(char *const argv[])
{
#if defined(PS5_HOST_REFERENCE)
	(void)argv;
#else
	static const char *const eboots[] = { "/app0/eboot.bin", PROTON_SANDBOX_APP0 "/eboot.bin" };
	for (const char *eboot : eboots) {
		proton_say("update: restarting (%s)", eboot);
		sceSystemServiceLoadExec(eboot, argv);
	}
	proton_say("update: the restart was refused");
#endif
}

} // namespace

void proton_update_check()
{
	std::lock_guard<std::mutex> hold(gLock);
	if (gState.phase == ProtonUpdate::checking || gState.phase == ProtonUpdate::downloading) return;
	gState = {};
	gState.phase = ProtonUpdate::checking;
	gState.message = "Asking for the newest release";
	std::thread(check).detach();
}

void proton_update_download()
{
	std::lock_guard<std::mutex> hold(gLock);
	if (gState.phase != ProtonUpdate::available || !gHaveManifest) return;
	gState.phase = ProtonUpdate::downloading;
	gState.message = "Downloading " PS5_TITLE_NAME " " + gState.version;
	gStarted = std::chrono::steady_clock::now();
	std::thread(download, gState.version).detach();
}

void proton_update_restart()
{
	if (proton_update_state().phase != ProtonUpdate::ready) return;
	char *none[] = { nullptr };
	restart(none);
	std::lock_guard<std::mutex> hold(gLock);
	gState.message = "The restart was refused. Close " PS5_TITLE_NAME " and open it again: the update is switched in then.";
}

ProtonUpdate proton_update_state()
{
	std::lock_guard<std::mutex> hold(gLock);
	ProtonUpdate state = gState;
	if (gPack) {
		pw_pack_progress(gPack, &state.progress);
		state.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - gStarted).count();
	}
	return state;
}

void proton_update_on_start(int argc, char **argv)
{
	char version[64];
	if (!pw_update_pending(kUpdate, version, sizeof(version))) return;
	/* A switch that is due, but this title is as new already (a newer folder copied over by hand): dropped. A switch
	 * cut short (`switching`) always goes on. */
	const std::string ready = std::string(kUpdate) + "/ready";
	if (access(ready.c_str(), F_OK) == 0 && pw_release_version_compare(version, running_version().c_str()) <= 0) {
		proton_say("update: %s is staged, but this title is %s already: dropped", version, running_version().c_str());
		unlink(ready.c_str());
		return;
	}
	char failed[512];
	const int status = pw_update_switch(kUpdate, proton_app_root(), failed, sizeof(failed));
	proton_say("update: switch to %s into %s: status %d%s%s", version, proton_app_root(), status, *failed ? " at " : "",
		failed);
	if (status != 1) {
		ProtonUpdate found;
		found.phase = ProtonUpdate::failed;
		found.version = version;
		found.message = std::string("The switch to " PS5_TITLE_NAME " ") + version + " stopped at " + failed + ": " +
			strerror(-status) + ". It goes on the next time " PS5_TITLE_NAME " starts.";
		finish(found);
		return;
	}
	gSwitchedHere = true;
	/* None of this start's arguments: the new title asks for root afresh (an elevation-retry=1 is this process's). */
	(void)argc, (void)argv;
	char *none[] = { nullptr };
	restart(none);
}

void proton_update_started()
{
	if (gSwitchedHere) return;
	const int status = pw_update_confirm(kUpdate);
	if (status) proton_say("update: the new title started; the previous title's files are removed (%d)", status);
}
