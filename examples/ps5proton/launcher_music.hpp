/*
 * MihawkOS - the launcher's and the first-launch tour's music: MihawkOS's own pieces (tools/make-mihawk-music.py, in
 * the title's assets/music), one at a time, looping, crossfaded when the player switches.
 *
 * Made of the kit's own parts: its MusicTrack (stb_vorbis, looping by the pieces' LOOPSTART/LOOPLENGTH) feeding a
 * StreamRing on one of the mixer's two music decks, which exist for crossfades. Not its MusicPlayer: that plays a
 * folder shuffled, each song once with a pause after it, so it can neither hold the piece the player chose nor fade
 * between two. The choice is kept beside the theme's (hui/music.txt in the title folder). The sound stops with the
 * kit's audio when the launcher ends for a game or a session (KitExample's teardown).
 * Copyright (C) 2026 Mihawk
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#ifndef PS5_APP_ROOT
#define PS5_APP_ROOT "/app0"
#endif

class Jukebox
{
public:
	static constexpr int kPieces = 3;
	static constexpr const char *kIds[kPieces] = { "bubble-garden", "glass-meadow", "sky-lounge" };
	static constexpr const char *kTitles[kPieces] = { "Bubble Garden", "Glass Meadow", "Sky Lounge" };
	static constexpr float kGain = 0.14f;   // the tour's level: low, under the interface's sounds
	static constexpr float kFade = 1.6f;    // seconds a switch crossfades over

	// folder: the title's assets/music. test: a test run, which never saves the choice
	void init(hui::audio::Mixer &mixer, std::string folder, bool test)
	{
		mixer_ = &mixer;
		folder_ = std::move(folder);
		test_ = test;
		buffer_.resize(kChunk * 2);
	}

	// The choice kept beside the theme's: a piece's index, or -1 for off; Bubble Garden when nothing is kept
	int remembered() const
	{
		std::string id;
		hui::save::read_file(choicePath(), &id, 64);
		while (!id.empty() && (id.back() == '\n' || id.back() == '\r' || id.back() == ' ')) id.pop_back();
		if (id == "off") return -1;
		for (int i = 0; i < kPieces; ++i) if (id == kIds[i]) return i;
		return 0;
	}

	// Switches to a piece (-1: off), crossfading over fade seconds; save keeps the choice for the next launch
	void play(int piece, bool save, float fade = kFade)
	{
		if (!mixer_ || piece == piece_) return;
		Deck &old = decks_[live_];
		if (old.track) {
			mixer_->set_stream_gain(live_, 0.0f, fade);
			old.fading = fade + 0.1f;
		}
		piece_ = piece;
		if (piece >= 0) {
			const int slot = 1 - live_;
			Deck &deck = decks_[slot];
			std::string data;
			auto track = std::make_unique<hui::audio::MusicTrack>();
			std::string error = "unreadable";
			if (hui::save::read_file(folder_ + "/" + kIds[piece] + ".ogg", &data, 8u << 20)) error = track->open(std::move(data));
			if (!error.empty()) {
				proton_say("music: %s not played: %s", kIds[piece], error.c_str());
			} else {
				track->set_looping(true);
				// A fresh ring for the deck: the old one may still hold the last piece's sound, and is kept until the
				// audio thread has let go of it
				if (deck.ring) retired_.push_back({ std::move(deck.ring), 2.0f });
				deck.ring = std::make_unique<hui::audio::StreamRing>();
				deck.track = std::move(track);
				deck.fading = 0.0f;
				mixer_->set_stream_gain(slot, 0.0f, 0.0f);
				mixer_->swap_stream(slot, deck.ring.get());
				mixer_->set_stream_gain(slot, kGain, fade);
				live_ = slot;
			}
		}
		proton_say("music: %s", piece >= 0 ? kIds[piece] : "off");
		if (save && !test_) {
			hui::save::ensure_directory(choiceFolder());
			const std::string error = hui::save::write_atomic(choicePath(), std::string(piece >= 0 ? kIds[piece] : "off") + "\n");
			if (!error.empty()) proton_say("music: choice not saved: %s", error.c_str());
		}
	}

	// The next in the cycle: Bubble Garden, Glass Meadow, Sky Lounge, off
	void next() { play(piece_ + 1 < kPieces ? piece_ + 1 : -1, true); }

	int piece() const { return piece_; }
	const char *title() const { return piece_ >= 0 ? kTitles[piece_] : nullptr; }

	// Once a frame: each sounding deck decoded about 0.7 s ahead; a faded deck stops; old rings let go
	void pump(float dt)
	{
		for (int slot = 0; slot < 2; ++slot) {
			Deck &deck = decks_[slot];
			if (!deck.track) continue;
			if (deck.fading > 0.0f && (deck.fading -= dt) <= 0.0f) {
				deck.track.reset();
				continue;
			}
			while (deck.ring->available() < kAhead) {
				const int room = (int)std::min<size_t>(kChunk, deck.ring->space());
				if (room <= 0) break;
				const int got = deck.track->decode(buffer_.data(), room);
				if (got > 0) deck.ring->write(buffer_.data(), (size_t)got);
				if (got < room) break; // a broken stream: what it gave plays, then silence
			}
		}
		for (auto it = retired_.begin(); it != retired_.end();) it = (it->second -= dt) <= 0.0f ? retired_.erase(it) : it + 1;
	}

private:
	struct Deck {
		std::unique_ptr<hui::audio::StreamRing> ring;
		std::unique_ptr<hui::audio::MusicTrack> track;
		float fading{ 0.0f };   // seconds until a fading deck stops decoding
	};
	static constexpr int kChunk = 2048;
	static constexpr size_t kAhead = 32768;

	static std::string choiceFolder() { return proton_title_path(PS5_APP_ROOT "/hui"); }
	static std::string choicePath() { return choiceFolder() + "/music.txt"; }

	hui::audio::Mixer *mixer_{ nullptr };
	std::string folder_;
	bool test_{ false };
	Deck decks_[2];
	int live_{ 0 }, piece_{ -1 };
	std::vector<float> buffer_;
	std::vector<std::pair<std::unique_ptr<hui::audio::StreamRing>, float>> retired_;
};
