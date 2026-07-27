// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
#pragma once

#include "app/config.hpp"
#include "app/discovery.hpp"
#include "core/telemetry.hpp"
#include "gfx/presenter.hpp"

namespace hayai::ui { class Draw; }

namespace hayai::app {

// The frontend. Discovery runs continuously and the main screen is one live
// list of consoles: paired ones show ready/standby/offline, unpaired ones found
// on the network can be paired in place, and a standby console is woken
// automatically when selected.
//
// Rendered with deko3d via ui::Draw rather than a UI toolkit, so the streaming
// path never shares a frame pacer with a widget library. The UI releases the
// display entirely for the duration of a stream.
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

	// Graphics are owned by the UI and handed to the stream for its duration,
	// because the two want different swapchain shapes.
	bool gfx_up();
	void gfx_down();

	void frame_begin();
	void frame_end();
	void draw_header(const char *title, const char *subtitle);
	void draw_hints(const char *hints);
	void message(const char *title, const char *body);
	void session_report(const core::Telemetry::Summary &s, bool errored);

	Config &config_;
	Discovery discovery_;
	gfx::Presenter *presenter_ = nullptr;
	ui::Draw *draw_ = nullptr;
	HostEntry selected_{};
};

} // namespace hayai::app
