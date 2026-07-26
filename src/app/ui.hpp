// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
#pragma once

#include "app/config.hpp"
#include "app/discovery.hpp"

namespace hayai::app {

// Text-mode frontend, structured like the classic Remote Play clients:
// discovery runs continuously, and the main menu is a live list of consoles --
// registered ones show their ready/standby state, unregistered ones found on
// the network can be registered on the spot, standby consoles are woken
// automatically on connect.
//
// Deliberately libnx console, not a UI toolkit: the console owns the default
// framebuffer only while menus are visible; it is torn down before a stream
// brings up deko3d, so nothing UI-related exists while streaming.
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
	void register_menu();
	void register_host(HostEntry &entry);
	// Sends wakeup and waits for the console to report READY. Returns false
	// if the user canceled or the console never woke.
	bool wake_and_wait(const HostEntry &entry);
	void settings_menu();
	void wait_any_button(const char *prompt);

	Config &config_;
	Discovery discovery_;
	HostEntry selected_{};
};

} // namespace hayai::app
