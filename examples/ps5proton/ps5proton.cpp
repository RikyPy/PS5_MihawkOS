/*
 * PS5_Proton - game library and the template UI kit's thirty themes.
 * Derived from PS5_VulkanTemplate's Samples menu and Themes picker.
 * Copyright (C) 2026 Mihawk
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "kit.hpp"
#include "launcher_music.hpp"
#include "setup_tour.hpp"
#include <algorithm>
#include <sys/stat.h>

namespace {
class VulkanExample : public ps5ui::KitExample
{
	std::vector<ProtonGame> games;
	std::string problem;
	std::string sessionNote;   // why the last session failed, or that it never came back
	std::string noticesNote;   // what the last session said on screen
	hui::ui::ListView library, themes, settingsList, controlsList, buttonList;
	pw_controls controls{};
	std::string controlsNotice;
	int gameMode{1};
	enum class Page { library, themes, settings, controls, buttons };
	Page page{Page::library};
	unsigned mouseAction{0};
	hui::gfx::DrawList scene;
	hui::ui::Feedback feedback;
	ps5ui::HoldToLeave leaving;
	bool dataAvailable{ false };
	float clock{ 0.0f }, applied{ 0.0f };
	int selectedTheme{ 0 };
	ProtonInstall install;   // Settings > Install runtime (proton_install.h)
	// The first-launch guided setup (setup_tour.hpp), while it shows
	std::unique_ptr<SetupTour> tour;
	PwSetupFacts setupFacts{};
	PwSetupStep setupStep{ PW_SETUP_NONE };
	bool setupSimulated{ false };
	bool setupUnattended{ false };   // proton_setup_unattended: a scripted run with the real engine and its sessions
	// MihawkOS's own music (launcher_music.hpp): R1 or Settings > Music switches it; the name shows for a moment
	Jukebox music;
	float musicShown{ 0.0f };
	ProtonUpdate updates;    // Settings > Check for updates (proton_update.h)
	bool drawnOnce{ false };

	const hui::ui::Theme &theme() const
	{
		return hui::ui::themes()[page == Page::themes ? themes.focus() : selectedTheme];
	}

	void syncThemes()
	{
		for (int i = 0; i < (int)themes.items().size(); ++i)
			themes.item(i).badge = i == selectedTheme ? "IN USE" : "";
	}

	void openThemes()
	{
		page = Page::themes;
		themes.set_focus(selectedTheme);
		themes.enter();
		syncThemes();
	}

	void syncMusic()
	{
		settingsList.item(5).subtitle = music.title() ? music.title() : "Off";
		settingsList.item(5).badge = music.title() ? "ON" : "OFF";
	}

	void nextMusic()
	{
		music.next();
		musicShown = 2.5f;
		feedback.play(hui::audio::Cue::toggle);
		syncMusic();
	}

	void syncGameMode()
	{
		settingsList.item(1).subtitle = gameMode ?
			"On: full-screen games take the screen directly and the desktop unloads while they play" :
			"Off: every game is shown inside the desktop, which stays loaded";
		settingsList.item(1).badge = gameMode ? "ON" : "OFF";
	}

	void toggleGameMode()
	{
		const int changed = !gameMode;
		// Scripted UI runs preview the choice without overwriting the player's file.
		const int error = ps5.frameBudget ? 0 : pw_game_mode_save(changed, proton_game_mode_path().c_str());
		if (error) {
			controlsNotice = "Could not save Game Mode. Your previous choice is unchanged.";
			proton_say("Game Mode: save failed (%d)", error);
		} else {
			gameMode = changed;
			controlsNotice = gameMode ? "Game Mode on. Applies from the next launch." : "Game Mode off. Applies from the next launch.";
			feedback.play(hui::audio::Cue::saved);
			proton_say("Game Mode: %s %s", ps5.frameBudget ? "preview" : "saved", gameMode ? "on" : "off");
		}
		syncGameMode();
	}

	void syncUpdates()
	{
		updates = proton_update_state();
		const char *badges[] = { "", "CHECKING", "NEWEST", "AVAILABLE", "DOWNLOADING", "READY", "FAILED" };
		settingsList.item(6).badge = badges[updates.phase];
		settingsList.item(6).subtitle = updates.version.empty() ? "On request only" : PS5_TITLE_NAME " " + updates.version;
	}

	void syncInstall()
	{
		install = proton_install_state();
		const char *badges[] = { "", "READY", "CHECKING", "INSTALLING", "DONE", "FAILED" };
		settingsList.item(3).badge = badges[install.phase];
		settingsList.item(3).subtitle = install.phase == ProtonInstall::running ? "Installing" :
			install.phase == ProtonInstall::done ? "Installed" : "From /data/ps5proton/install or a USB stick";
	}

	void syncControls()
	{
		for (unsigned i = 0; i < 3; ++i)
			controlsList.item(i).subtitle = pw_controls_button_label(controls.button[i]);
	}

	hui::ui::ListView &activeList()
	{
		switch (page) {
		case Page::themes: return themes;
		case Page::settings: return settingsList;
		case Page::controls: return controlsList;
		case Page::buttons: return buttonList;
		default: return library;
		}
	}

	void saveControls(const pw_controls &changed)
	{
		// Scripted UI runs preview choices without overwriting the player's file.
		const int error = ps5.frameBudget ? 0 : pw_controls_save(&changed, proton_controls_path().c_str());
		if (error) {
			controlsNotice = "Could not save controls. Your previous choices are unchanged.";
			proton_say("Controls: save failed (%d)", error);
		} else {
			controls = changed;
			controlsNotice = "Saved. Applies to every game you launch.";
			feedback.play(hui::audio::Cue::saved);
			proton_say("Controls: %s left=%s right=%s middle=%s", ps5.frameBudget ? "preview" : "saved", pw_controls_button_id(controls.button[0]),
				pw_controls_button_id(controls.button[1]), pw_controls_button_id(controls.button[2]));
		}
		syncControls();
	}

	void update(float dt)
	{
		feedback.clear();
		const hui::InputFrame input = ps5.frameBudget ? kit.scripted("ps5proton", ps5.framesDrawn) : kit.input();
		clock += dt;
		applied = std::max(0.0f, applied - dt);
		if (page == Page::library && input.is_pressed(hui::Action::west)) {
			openThemes();
		} else if (page == Page::library && input.is_pressed(hui::Action::north)) {
			page = Page::settings;
			settingsList.enter();
		} else if (page == Page::library && input.is_pressed(hui::Action::page_next)) {
			nextMusic();
		} else {
			const auto event = activeList().handle(input, feedback);
			if (event == hui::ui::Event::cancelled || input.is_pressed(hui::Action::back)) {
				if (page == Page::buttons) page = Page::controls;
				else if (page == Page::controls) page = Page::settings;
				else page = Page::library;
			} else if (event == hui::ui::Event::activated) {
				switch (page) {
				case Page::themes:
					selectedTheme = themes.focus();
					ps5ui::set_active_theme(selectedTheme);
					syncThemes();
					applied = 2.5f;
					feedback.play(hui::audio::Cue::saved);
					proton_say("launcher theme: %s", theme().id);
					break;
				case Page::settings:
					if (settingsList.focus() == 5) {
						nextMusic();
					} else if (settingsList.focus() == 4) {
						setupFacts = SetupTour::facts(dataAvailable, true, &setupSimulated);
						startSetup(PW_SETUP_WELCOME);
					} else if (settingsList.focus() == 1) {
						toggleGameMode();
					} else if (settingsList.focus() == 3) {
						// Look again each time (a stick may have been plugged in), then start when the pieces are all there.
						if (!ps5.frameBudget && proton_install_find().phase == ProtonInstall::ready) proton_install_start();
						syncInstall();
					} else if (settingsList.focus() == 6) {
						// One press a step: look, then download what was found, then restart into it.
						const ProtonUpdate::Phase now = proton_update_state().phase;
						if (ps5.frameBudget || now == ProtonUpdate::checking || now == ProtonUpdate::downloading) {
							// a check or a download is running; a scripted run asks nothing of the network
						} else if (now == ProtonUpdate::available) {
							proton_update_download();
						} else if (now == ProtonUpdate::ready) {
							proton_update_restart();
						} else {
							proton_update_check();
						}
						syncUpdates();
					} else if (settingsList.focus() == 0) {
						page = Page::controls;
						controlsList.enter();
					}  // About (2) is read where it is
					break;
				case Page::controls:
					if (controlsList.focus() == 3) {
						pw_controls defaults;
						pw_controls_defaults(&defaults);
						saveControls(defaults);
					} else {
						mouseAction = controlsList.focus();
						page = Page::buttons;
						buttonList.set_focus(controls.button[mouseAction]);
						buttonList.enter();
					}
					break;
				case Page::buttons: {
					pw_controls changed = controls;
					pw_controls_assign(&changed, mouseAction, buttonList.focus());
					saveControls(changed);
					page = Page::controls;
					break;
				}
				case Page::library:
					if (!ps5.frameBudget) {
						const int index = library.items()[library.focus()].tag;
						proton_queue_restart(&games[index], proton_launch().cycle, "chosen");
						quit = true;
					}
					break;
				}
			}
		}
		leaving.label = "Close " PS5_TITLE_NAME;
		if (!ps5.frameBudget && leaving.update(dt, (ps5_pad().held & PAD_OPTIONS) != 0)) quit = true;
		for (auto *list : {&library, &themes, &settingsList, &controlsList, &buttonList}) {
			list->style.theme = theme();
			list->update(dt);
		}
		musicShown = std::max(0.0f, musicShown - dt);
		music.pump(dt);
		if (page == Page::settings) {
			syncInstall();
			syncUpdates();
		}
		// The launcher has drawn: this title started once, so a previous title an update replaced can go.
		if (!drawnOnce && ps5.framesDrawn > 0) {
			drawnOnce = true;
			proton_update_started();
		}
		kit.play(feedback, theme().sounds);
		kit.tick(dt);
	}

	// What plays, for a moment after it changes: a note and the piece's name, top right
	void drawMusic(const hui::ui::Theme &look)
	{
		if (musicShown <= 0.0f) return;
		const float shown = std::min(1.0f, std::min(musicShown / 0.3f, (2.5f - musicShown) / 0.2f + 0.0001f));
		const std::string label = music.title() ? std::string("Music: ") + music.title() : std::string("Music off");
		const float w = kit.fonts.semibold.measure(label, 24) + 92;
		const hui::gfx::Rect plate{ 1800 - w, 40 - (1.0f - shown) * 16.0f, w, 60 };
		scene.push_opacity(shown);
		scene.shadow(plate, 30, 22, look.shadow.a > 0 ? look.shadow.with_alpha(0.45f) : hui::gfx::Color::rgb(0x000000, 0.35f));
		scene.rounded_rect(plate, 30, look.surface.with_alpha(0.97f));
		const float x = plate.x + 34, cy = plate.y + 30;   // an eighth note: a head, a stem, a flag
		const auto ink = music.title() ? look.accent : look.text_muted;
		scene.circle(x - 4, cy + 8, 7, ink);
		scene.line(x + 2.5f, cy + 8, x + 2.5f, cy - 13, 3, ink);
		scene.line(x + 2.5f, cy - 13, x + 11, cy - 6, 3, ink);
		hui::ui::text(scene, kit.fonts.semibold, label, plate.x + 62, cy + 8, 24, look.text);
		scene.pop_opacity();
	}

	void paragraph(hui::ui::Painter &paint, const std::string &text, float x, float &y,
		float size, hui::gfx::Color ink)
	{
		for (const auto &line : kit.fonts.regular.font->wrap(text, size, 500.0f)) {
			paint.body(line, x, y, size, ink);
			y += size * 1.45f;
		}
	}

	void compose()
	{
		const auto &look = theme();
		const auto glass = kit.renderer.glass_texture();
		scene.clear();
		hui::ui::Canvas canvas{ scene, kit.fonts, glass, clock };
		hui::ui::Painter paint(scene, kit.fonts, look, glass);
		const bool picking = page == Page::themes;
		const bool configuring = page != Page::library && !picking;
		const char *titles[] = {"Left mouse click", "Right mouse click", "Middle mouse click"};
		const char *heading = picking ? "Themes" : page == Page::settings ? "Settings" :
			page == Page::controls ? "Controls" : page == Page::buttons ? titles[mouseAction] : PS5_TITLE_NAME;
		paint.heading(heading, 120, 145, 64, paint.page_text());
		paint.body(picking ? "Find your look. Preview any theme, then press CROSS to keep it." :
			configuring ? "Choose how your controller operates the mouse." : "Your Windows game library", 122, 195, 24, paint.page_text_muted());
		char count[64];
		if (picking) snprintf(count, sizeof(count), "%02d / %02zu", themes.focus() + 1, themes.items().size());
		else if (configuring) snprintf(count, sizeof(count), "Settings / Controls");
		else if (!dataAvailable) snprintf(count, sizeof(count), "Library unavailable");
		else snprintf(count, sizeof(count), "%zu game%s", games.size(), games.size() == 1 ? "" : "s");
		paint.label(count, 1800, 145, 24, paint.page_text_muted(), hui::gfx::Align::right);
		activeList().draw(canvas);

		const hui::gfx::Rect panel{ 1200, 230, 600, 740 };
		paint.panel(panel);
		float y = 300;
		if (picking) {
			paint.heading(look.name, 1240, y, 42, look.text);
			y += 48;
			paragraph(paint, look.family, 1240, y, 24, look.text_muted);
			y += 35;
			paragraph(paint, look.summary, 1240, y, 24, look.text_muted);
			paint.label("COLOR PALETTE", 1240, 535, 20, look.text_muted);
			const char *labels[] = { "Background", "Panels", "Text", "Accent" };
			const hui::gfx::Color colors[] = { look.page, look.surface, look.text, look.accent };
			for (int i = 0; i < 4; ++i) {
				const float x = 1240.0f + i * 130.0f;
				scene.rounded_rect({ x, 560, 110, 90 }, 8, look.outline);
				scene.rounded_rect({ x + 1, 561, 108, 88 }, 7, colors[i]);
				paint.body(labels[i], x, 684, 19, look.text_muted);
			}
			paint.body(selectedTheme == themes.focus() ? "Current theme" : "Preview", 1240, 805, 26, look.text);
			if (applied > 0) paint.body("Theme applied", 1240, 855, 24, look.success);
			ps5ui::draw_hints(scene, kit.fonts, look,
				{ { hui::ui::Button::dpad, "Browse" }, { hui::ui::Button::cross, "Use theme" },
				  { hui::ui::Button::circle, "Back" } }, 120, 1040);
		} else if (page == Page::settings && settingsList.focus() == 1) {
			paint.heading("Game Mode", 1240, y, 38, look.text);
			y += 65;
			paragraph(paint, "On: a full-screen game's frames go straight to the screen, and the Mihawk desktop unloads while it plays.",
				1240, y, 24, look.text_muted);
			y += 30;
			paragraph(paint, "Off: every game is shown inside the desktop, which stays loaded. Use it if a game's screen stays black.",
				1240, y, 24, look.text_muted);
			y += 30;
			paragraph(paint, controlsNotice, 1240, y, 24, look.text);
			ps5ui::draw_hints(scene, kit.fonts, look,
				{ { hui::ui::Button::dpad, "Choose" }, { hui::ui::Button::cross, gameMode ? "Turn off" : "Turn on" },
				  { hui::ui::Button::circle, "Back" } }, 120, 1040);
		} else if (page == Page::settings && settingsList.focus() == 2) {
			// The Appropriate Legal Notices GPLv3 asks an interactive program to show (LICENSE, section 0); wording approved
			// by the owner on 2026-10-18.
			paint.heading("About " PS5_TITLE_NAME, 1240, y, 38, look.text);
			y += 65;
			paragraph(paint, PS5_TITLE_NAME ". Copyright (C) 2026 Mihawk and the authors credited in NOTICE.md.", 1240, y, 22,
				look.text_muted);
			y += 22;
			paragraph(paint, "This program is free software under the GNU General Public License, version 3 or later. It comes "
				"with ABSOLUTELY NO WARRANTY. You may share and change it under that licence's terms.", 1240, y, 22, look.text_muted);
			y += 22;
			paragraph(paint, "Licence texts: /data/homebrew/PPSA99170/licenses (this title) and " PROTON_DATA_ROOT
				"/runtime/ge11-7/share/licenses (the Windows runtime).", 1240, y, 22, look.text_muted);
			y += 22;
			paragraph(paint, "Source: github.com/mihawk-99/PS5_MihawkOS", 1240, y, 22, look.text_muted);
			y += 22;
			paragraph(paint, "Not affiliated with Sony, Microsoft or Valve.", 1240, y, 22, look.text_muted);
			ps5ui::draw_hints(scene, kit.fonts, look,
				{ { hui::ui::Button::dpad, "Choose" }, { hui::ui::Button::circle, "Back" } }, 120, 1040);
		} else if (page == Page::settings && settingsList.focus() == 3) {
			paint.heading("Runtime", 1240, y, 38, look.text);
			y += 65;
			paragraph(paint, "Windows programs run on a Wine runtime of about 1.2 GB. Install it, or repair it, from its pieces "
				"in " PROTON_DATA_ROOT "/install or in a mihawkos folder on a USB stick.", 1240, y, 24, look.text_muted);
			y += 30;
			const auto &done = install.progress;
			if (install.phase == ProtonInstall::checking || install.phase == ProtonInstall::running) {
				const float share = done.to_write ? std::min(1.0f, (float)done.written / (float)done.to_write) : 0.0f;
				scene.rounded_rect({ 1240, y, 520, 18 }, 9, look.outline);
				scene.rounded_rect({ 1240, y, std::max(18.0f, 520 * share), 18 }, 9, look.accent);
				y += 50;
				char line[160];
				snprintf(line, sizeof(line), "%.0f of %.0f MB, %.1f MB/s, piece %zu of %zu",
					done.written / 1e6, done.to_write / 1e6, install.seconds > 0 ? done.written / 1e6 / install.seconds : 0.0,
					done.chunks_done, done.chunks_needed);
				paragraph(paint, install.phase == ProtonInstall::checking ? "Checking what is installed already" : line,
					1240, y, 24, look.text);
				y += 30;
				paragraph(paint, "You can keep the console on this screen. Closing " PS5_TITLE_NAME " stops the install; "
					"it goes on from where it stopped next time.", 1240, y, 22, look.text_muted);
			} else {
				paragraph(paint, install.message.empty() ? "Press CROSS to look for the runtime's pieces." : install.message,
					1240, y, 24, install.phase == ProtonInstall::failed ? look.danger :
					install.phase == ProtonInstall::done ? look.success : look.text);
			}
			ps5ui::draw_hints(scene, kit.fonts, look,
				{ { hui::ui::Button::dpad, "Choose" }, { hui::ui::Button::cross, "Install" },
				  { hui::ui::Button::circle, "Back" } }, 120, 1040);
		} else if (page == Page::settings && settingsList.focus() == 5) {
			paint.heading("Music", 1240, y, 38, look.text);
			y += 65;
			paragraph(paint, "MihawkOS's own music, made for it: Bubble Garden, Glass Meadow and Sky Lounge, quietly looping under "
				"the menus and the guided setup. Press CROSS, or R1 in the library, for the next one or for silence.", 1240, y, 24,
				look.text_muted);
			y += 30;
			paragraph(paint, music.title() ? std::string("Playing: ") + music.title() : std::string("Off"), 1240, y, 24, look.text);
			ps5ui::draw_hints(scene, kit.fonts, look,
				{ { hui::ui::Button::dpad, "Choose" }, { hui::ui::Button::cross, "Next" },
				  { hui::ui::Button::circle, "Back" } }, 120, 1040);
		} else if (page == Page::settings && settingsList.focus() == 4) {
			paint.heading("Guided setup", 1240, y, 38, look.text);
			y += 65;
			paragraph(paint, "Run the first-launch tour again: your language, installing the Windows runtime, and the first start "
				"of Mihawk OS. What is installed already is kept.", 1240, y, 24, look.text_muted);
			ps5ui::draw_hints(scene, kit.fonts, look,
				{ { hui::ui::Button::dpad, "Choose" }, { hui::ui::Button::cross, "Start" },
				  { hui::ui::Button::circle, "Back" } }, 120, 1040);
		} else if (page == Page::settings && settingsList.focus() == 6) {
			paint.heading("Updates", 1240, y, 38, look.text);
			y += 65;
			paragraph(paint, PS5_TITLE_NAME " looks for a newer release only when you ask. A release is installed only when it is "
				"signed with " PS5_TITLE_NAME "'s key, and it is switched in the next time " PS5_TITLE_NAME " starts.",
				1240, y, 24, look.text_muted);
			y += 30;
			const auto &done = updates.progress;
			if (updates.phase == ProtonUpdate::downloading) {
				const float share = done.to_write ? std::min(1.0f, (float)done.written / (float)done.to_write) : 0.0f;
				scene.rounded_rect({ 1240, y, 520, 18 }, 9, look.outline);
				scene.rounded_rect({ 1240, y, std::max(18.0f, 520 * share), 18 }, 9, look.accent);
				y += 50;
				char line[160];
				snprintf(line, sizeof(line), "%.0f of %.0f MB, piece %zu of %zu", done.written / 1e6, done.to_write / 1e6,
					done.chunks_done, done.chunks_needed);
				paragraph(paint, line, 1240, y, 24, look.text);
			} else {
				paragraph(paint, updates.message.empty() ? "Press CROSS to check for a newer release." : updates.message, 1240,
					y, 24, updates.phase == ProtonUpdate::failed ? look.danger :
					updates.phase == ProtonUpdate::ready ? look.success : look.text);
			}
			ps5ui::draw_hints(scene, kit.fonts, look,
				{ { hui::ui::Button::dpad, "Choose" },
				  { hui::ui::Button::cross, updates.phase == ProtonUpdate::available ? "Download" :
					updates.phase == ProtonUpdate::ready ? "Restart" : "Check" },
				  { hui::ui::Button::circle, "Back" } }, 120, 1040);
		} else if (configuring) {
			paint.heading("Mouse controls", 1240, y, 38, look.text);
			y += 65;
			paragraph(paint, "Move the pointer with the touchpad. Choose a controller button for each mouse click.",
				1240, y, 24, look.text_muted);
			y += 30;
			paragraph(paint, "Assigned buttons send mouse clicks instead of gamepad presses. Each button can have one assignment.",
				1240, y, 24, look.text_muted);
			y += 30;
			paragraph(paint, "Choices are saved for all games. Restore defaults to use the touchpad for left click.",
				1240, y, 24, look.text_muted);
			y += 30;
			paragraph(paint, controlsNotice, 1240, y, 24, look.text);
			ps5ui::draw_hints(scene, kit.fonts, look,
				{ { hui::ui::Button::dpad, "Choose" }, { hui::ui::Button::cross, "Select" },
				  { hui::ui::Button::circle, "Back" } }, 120, 1040);
		} else {
			scene.push_clip({ 1240, 250, 520, 500 });
			// The library always holds Mihawk OS (proton_read_games), so a reason a game cannot start is its complaint.
			const auto &item = library.items()[library.focus()];
			paragraph(paint, item.title, 1240, y, 34, look.text);
			y += 30;
			paragraph(paint, item.subtitle, 1240, y, 24, look.text_muted);
			for (const std::string *note : { &games[item.tag].complaint, &sessionNote, &noticesNote }) {
				if (note->empty()) continue;
				y += 30;
				paragraph(paint, *note, 1240, y, 22, look.danger);
			}
			scene.pop_clip();
			ps5ui::draw_hints(scene, kit.fonts, look,
				{ { hui::ui::Button::dpad, "Choose" }, { hui::ui::Button::cross, "Select" },
				  { hui::ui::Button::square, "Themes" }, { hui::ui::Button::triangle, "Settings" },
				  { hui::ui::Button::r1, "Music" }, { hui::ui::Button::options, "Hold to close" } }, 120, 1040);
		}
		drawMusic(look);
		leaving.draw(scene, kit.fonts, look);
		auto backdrop = look.backdrop;
		backdrop.time = clock;
		kit.renderer.begin();
		kit.renderer.backdrop(backdrop);
		kit.renderer.glass();
		kit.renderer.draw(scene);
	}

	void selftestStep()
	{
		if (ps5.frameBudget || !proton_selftest_active()) return;
		const auto &launch = proton_launch();
		proton_selftest_note(launch.game ? "game" : "launcher", launch.cycle);
		if (launch.game) {
			proton_queue_restart(nullptr, launch.cycle + 1, "selftest");
			quit = true;
		} else if (launch.cycle < 2 && !games.empty()) {
			proton_queue_restart(&games[0], launch.cycle, "selftest");
			quit = true;
		} else {
			proton_say("selftest: done after %u cycle(s)", launch.cycle);
			remove(proton_title_path("/app0/restart-check").c_str());
		}
	}

public:
	VulkanExample()
	{
		title = PS5_TITLE_NAME;
		name = "ps5proton";
		settings.overlay = false;
		ps5.ownOverlay = true;
		dataAvailable = proton_data_grant(); // Cached startup result; never retries after threads start.
		games = proton_read_games(&problem);
		proton_say("launcher: %zu game(s); %s", games.size(), problem.c_str());
		// The library shows Mihawk OS (the user's choice, 2026-10-12): every other game in its prefix stays installed, is
		// opened from the desktop's shortcuts, and is still what proton_read_games gives the owner when it starts a
		// profile. An entry with a prefix of its own has no shortcut (a session serves one prefix), so it stays listed.
		// A file named library-show-all in the data folder brings the whole list back.
		struct stat showAll;
		if (!(dataAvailable && stat(PROTON_DATA_ROOT "/library-show-all", &showAll) == 0)) {
			const size_t installed = games.size();
			std::string desktop;
			for (const auto &game : games)
				if (game.id == "mihawk-os") desktop = proton_game_prefix(game);
			games.erase(std::remove_if(games.begin(), games.end(), [&desktop](const ProtonGame &game) {
				return game.id != "mihawk-os" && proton_game_prefix(game) == desktop; }), games.end());
			proton_say("launcher: library shows %zu of %zu (Mihawk OS and entries with a prefix of their own; %s/library-show-all "
				"shows them all)", games.size(), installed, PROTON_DATA_ROOT);
		}
		// Checked here, before LoadExec: a game that cannot start is shown why instead of being started.
		const std::string blocked = dataAvailable ? proton_runtime_problem() : proton_data_problem();
		for (auto &game : games)
			if (game.complaint.empty()) game.complaint = blocked;
		if (!blocked.empty()) proton_say("launcher: games cannot start: %s", blocked.c_str());
		// A failed session's reason (ended=), or a session that never came back to say one (its marker is still there).
		const std::string marker = proton_title_path(PROTON_SESSION_MARKER);
		if (FILE *file = fopen(marker.c_str(), "rb")) {
			char name[128] = {};
			const size_t got = fread(name, 1, sizeof(name) - 1, file);
			fclose(file);
			std::string game(name, got);
			game.erase(std::remove(game.begin(), game.end(), '\n'), game.end());
			sessionNote = "The last session" + (game.empty() ? std::string() : " (" + game + ")") +
				" did not end normally: it crashed, froze or lost power. Its log: " PROTON_DATA_ROOT "/logs/title-last.log";
			remove(marker.c_str());
		}
		if (!proton_launch().ended.empty()) sessionNote = "Last session ended: " + proton_launch().ended;
		if (!sessionNote.empty()) proton_say("launcher: %s", sessionNote.c_str());
		const std::string notices = proton_title_path(PROTON_SESSION_NOTICES);
		if (FILE *file = fopen(notices.c_str(), "rb")) {
			char line[512];
			while (fgets(line, sizeof(line), file)) {
				std::string text(line);
				text.erase(std::remove(text.begin(), text.end(), '\n'), text.end());
				if (!text.empty()) noticesNote += (noticesNote.empty() ? "During the last session: " : " ") + text;
			}
			fclose(file);
			remove(notices.c_str());
			if (!noticesNote.empty()) proton_say("launcher: %s", noticesNote.c_str());
		}
	}

	void prepare() override
	{
		VulkanExampleBase::prepare();
		// The guided setup: on the first launch, or with the runtime or the mihawk prefix missing (src/pw_setup.h). A
		// test run shows it only when asked: by the first-run marker, or on the PC by
		// PROTON_SETUP_TEST=<step>[:<fake install's scenario>][@<share held>], which shows a step for a picture.
		const char *setupTest = getenv("PROTON_SETUP_TEST");
		setupUnattended = proton_setup_unattended(ps5.frameBudget != 0);
		if (setupUnattended) proton_say("setup: unattended (scripted presses, real engine)");
		setupFacts = SetupTour::facts(dataAvailable || setupTest, false, &setupSimulated);
		setupStep = setupTest ? (PwSetupStep)atoi(setupTest) : pw_setup_start(&setupFacts);
		if (ps5.frameBudget && !setupTest && !setupSimulated && !setupUnattended) setupStep = PW_SETUP_NONE;
		if (setupStep != PW_SETUP_NONE) unlink(SetupTour::preparingMarker);
		prepareKit({ .music = false, .covers = false });
		music.init(kit.mixer, getAssetPath() + "music", ps5.frameBudget != 0);
		music.play(music.remembered(), false, 2.5f);
		selectedTheme = ps5ui::active_theme_index();
		std::vector<hui::ui::ListItem> rows;
		for (size_t i = 0; i < games.size(); ++i) {
			hui::ui::ListItem item;
			item.title = games[i].name;
			item.subtitle = games[i].detail;
			item.badge = games[i].complaint.empty() ? "" : games[i].x86_32 ? "32-BIT" : "UNAVAILABLE";
			item.disabled = !games[i].complaint.empty();
			item.tag = (int)i;
			rows.push_back(item);
		}
		library.set_items(rows);
		rows.clear();
		for (const auto &look : hui::ui::themes()) {
			hui::ui::ListItem item;
			item.title = look.name;
			item.subtitle = look.family;
			rows.push_back(item);
		}
		themes.set_items(rows);
		rows.clear();
		hui::ui::ListItem item;
		item.title = "Controls";
		item.subtitle = "Mouse button assignments";
		rows.push_back(item);
		item = {};
		item.title = "Game Mode";
		rows.push_back(item);
		item = {};
		item.title = "About " PS5_TITLE_NAME;
		item.subtitle = "Licence, warranty and source";
		rows.push_back(item);
		item = {};
		item.title = "Install runtime";
		rows.push_back(item);
		item = {};
		item.title = "Guided setup";
		item.subtitle = "Language, install and the first start, step by step";
		rows.push_back(item);
		item = {};
		item.title = "Music";
		rows.push_back(item);
		item = {};
		item.title = "Check for updates";
		rows.push_back(item);
		settingsList.set_items(rows);
		rows.clear();
		for (const char *label : {"Left mouse click", "Right mouse click", "Middle mouse click", "Restore defaults"}) {
			item = {};
			item.title = label;
			rows.push_back(item);
		}
		controlsList.set_items(rows);
		rows.clear();
		for (unsigned i = 0; i < PW_CONTROLS_BUTTONS; ++i) {
			item = {};
			item.title = pw_controls_button_label(i);
			rows.push_back(item);
		}
		buttonList.set_items(rows);
		if (ps5.frameBudget) pw_controls_defaults(&controls);
		else pw_controls_load(&controls, proton_controls_path().c_str());
		syncControls();
		if (!ps5.frameBudget) pw_game_mode_load(&gameMode, proton_game_mode_path().c_str());
		syncGameMode();
		syncInstall();
		syncMusic();
		syncUpdates();
		for (auto *list : { &library, &themes, &settingsList, &controlsList, &buttonList }) {
			list->style.panel = true;
			list->style.row_height = 84;
			list->style.title_size = 28;
			list->style.subtitle_size = 20;
			list->set_bounds({ 120, 230, 1020, 740 });
			list->enter();
		}
		syncThemes();
		proton_say("launcher UI: %zu themes, active=%s", themes.items().size(), theme().id);
		if (setupStep != PW_SETUP_NONE) startSetup(setupStep);
		prepared = true;
	}

	void startSetup(PwSetupStep step)
	{
		tour = std::make_unique<SetupTour>();
		const VkPipelineShaderStageCreateInfo stages[2] = {
			loadShader(getShadersPath() + "ps5proton/aero.vert.spv", VK_SHADER_STAGE_VERTEX_BIT),
			loadShader(getShadersPath() + "ps5proton/aero.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT) };
		if (!tour->sky.init(device, physicalDevice, vulkanDevice->queueFamilyIndices.graphics, renderPass, pipelineCache, stages,
				maxConcurrentFrames))
			proton_say("setup: the sky's pipeline failed; the theme's backdrop stands in");
		// The installer's engine (proton_install.h); a test (PROTON_SETUP_TEST on the PC) or the first-run marker plays the
		// install with the fake one, so nothing of the console's runtime is written.
		const char *test = getenv("PROTON_SETUP_TEST");
		std::string scenario = test && strchr(test, ':') ? strchr(test, ':') + 1 : "ok";
		scenario = scenario.substr(0, scenario.find('@'));
		std::unique_ptr<SetupEngine> engine;
		if (test || setupSimulated) engine = std::make_unique<FakeSetupEngine>(scenario);
		else engine = std::make_unique<InstallSetupEngine>();
		tour->begin(kit, getAssetPath(), setupFacts, step, setupSimulated, ps5.frameBudget != 0 && !setupUnattended,
			std::move(engine));
		if (test && step == PW_SETUP_INSTALLING) tour->fake()->find(), tour->fake()->start();
		if (test && strstr(test, "@")) tour->fake()->hold(atof(strstr(test, "@") + 1));
		if (const char *language = getenv("PROTON_SETUP_LANGUAGE"); test && language) tour->preview_language(language);
		if (test && step == PW_SETUP_PREPARING && scenario == "cover") tour->cover();
	}

	void renderSetup()
	{
		static const int gloss = [] {
			const auto all = hui::ui::themes();
			for (size_t i = 0; i < all.size(); ++i) if (!strcmp(all[i].id, "gloss")) return (int)i;
			return 0;
		}();
		const auto &look = hui::ui::themes()[gloss];
		const hui::InputFrame input = ps5.frameBudget ? kit.scripted("ps5proton", ps5.framesDrawn) : kit.input();
		tour->update(std::min(frameTimer, 0.05f), input, ps5.frameBudget ? nullptr : &ps5_pad());
		music.pump(std::min(frameTimer, 0.05f));
		scene.clear();
		tour->compose(scene, look);
		kit.renderer.begin();
		if (!tour->sky.ready()) {
			auto backdrop = look.backdrop;
			backdrop.time = tour->clock();
			kit.renderer.backdrop(backdrop);
		}
		kit.renderer.draw(scene);
		buildKitCommandBuffer([&](VkCommandBuffer cmd) { tour->sky.draw(cmd, width, height, tour->clock(), tour->calm()); },
			[&](VkCommandBuffer cmd) { tour->sky.reset(cmd, currentBuffer); });
		// Start's frame shows the first session's cover: kept for the title to show while the prefix is made (src/pw_setup.h).
		if (tour->outcome == SetupTour::Outcome::prepare && (!ps5.frameBudget || setupUnattended))
			ps5.capturePath = PROTON_DATA_ROOT "/" PW_SETUP_COVER_NAME;
		submitFrame();
		const auto outcome = tour->outcome;
		if (outcome == SetupTour::Outcome::none) return;
		const auto mihawk = std::find_if(games.begin(), games.end(), [](const ProtonGame &game) { return game.id == "mihawk-os"; });
		if (outcome != SetupTour::Outcome::library && (!ps5.frameBudget || setupUnattended) && mihawk != games.end()) {
			proton_queue_restart(&*mihawk, proton_launch().cycle, outcome == SetupTour::Outcome::prepare ? "setup-prepare" : "setup");
			quit = true;
			return;
		}
		vkDeviceWaitIdle(device);
		tour->sky.release();
		tour.reset();
		page = Page::library;
	}

	void render() override
	{
		if (!prepared) return;
		prepareFrame();
		if (tour) {
			renderSetup();
			return;
		}
		update(std::min(frameTimer, 0.05f));
		compose();
		buildKitCommandBuffer();
		submitFrame();
		selftestStep();
	}

	~VulkanExample() override
	{
		if (device) vkDeviceWaitIdle(device);
		if (tour) tour->sky.release();
		// The audio thread reads the music's rings: it stops before they go (KitExample's own release comes after
		// this class's members are destroyed)
		kit.release();
	}
};
} // namespace
VULKAN_EXAMPLE_MAIN()
