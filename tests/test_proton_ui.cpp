/* PS5_Proton: theme persistence and deferred launcher handoff, on the host.
 * Copyright (C) 2026 Mihawk
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "kit.hpp"
#include "proton_launch.h"
#include <fstream>
#include <stdexcept>

bool ps5TestRun = false;
std::vector<Ps5Press> ps5Presses;

static void check(bool ok, const char *what)
{
	if (!ok) throw std::runtime_error(what);
}

int main(int argc, char **argv)
{
	const std::string path = std::string(PS5_APP_ROOT) + "/hui/theme.txt";
	if (argc == 2) {
		// A fresh process must read the saved id, or fall back for a corrupt file.
		check(std::string(ps5ui::active_theme().id) == argv[1], "theme reload");
		return 0;
	}
	check(hui::ui::themes().size() == 30, "thirty themes");
	check(std::string(ps5ui::active_theme().id) == "gloss", "Gloss default");
	for (int i = 0; i < 30; ++i) {
		ps5ui::set_active_theme(i);
		std::ifstream saved(path);
		std::string id;
		saved >> id;
		check(id == hui::ui::themes()[i].id, "theme saved by stable id");
		check(ps5ui::active_theme_index() == i, "theme applied");
	}
	ps5ui::set_active_theme(-1);
	ps5ui::set_active_theme(30);
	check(ps5ui::active_theme_index() == 29, "invalid index ignored");
	ps5TestRun = true;
	ps5ui::set_active_theme(0);
	check(std::string(ps5ui::active_theme().id) == "gloss", "deterministic test default");
	std::ifstream saved(path);
	std::string id;
	saved >> id;
	check(id == hui::ui::themes()[29].id, "scripted tests preserve saved preference");

	// Queuing must own the game data; dispatch after its originating UI has gone.
	{
		ProtonGame game;
		game.id = "ui-test";
		proton_queue_restart(&game, 7, "ui-test");
		game.id = "changed";
	}
	proton_finish_restart();
	proton_finish_restart(); // consumed once
	puts("UI state: 30 themes, saved selection, invalid indices and test isolation PASS");
}
