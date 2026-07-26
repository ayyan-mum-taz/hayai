// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
#pragma once

#include "app/config.hpp"

namespace hayai::app {

// Text-mode frontend.
//
// Deliberately libnx console, not a UI toolkit: the menus exist to get a
// session started and get out of the way. The console owns the default
// framebuffer only while menus are visible; it is torn down before the stream
// brings up deko3d and re-created afterwards, so nothing UI-related exists
// while streaming.
class Ui
{
public:
	explicit Ui(Config &config) : config_(config) {}

	// Runs the whole app loop (menus <-> streams) until the user exits.
	void run();

private:
	enum class MenuResult
	{
		Stream,		// selected_ points at the host to stream
		Exit,
	};

	MenuResult main_menu();
	void discover_menu();
	void register_menu();
	void register_host(HostEntry &entry);
	void settings_menu();
	void wait_any_button(const char *prompt);

	Config &config_;
	HostEntry selected_{};
};

} // namespace hayai::app
