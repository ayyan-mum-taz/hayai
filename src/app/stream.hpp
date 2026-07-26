// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
#pragma once

#include "app/config.hpp"
#include "audio/audio.hpp"
#include "core/thread.hpp"
#include "gfx/presenter.hpp"
#include "input/input.hpp"
#include "video/decoder.hpp"
#include "video/renderer.hpp"

#include <chiaki/session.h>

#include <atomic>

namespace hayai::app {

// One remote play session, start to finish.
//
// The defining decision: the video callback runs decode, render and present
// synchronously on the takion receive thread. Zero thread handoffs between
// datagram and pixel -- every handoff on Horizon is a scheduler wakeup with a
// tail. The socket's receive buffer absorbs the packets that arrive during the
// few ms we spend inside NVDEC.
//
// In controller-only mode no graphics exist at all: video is dropped at the
// takion layer (pre-parse), the minimum video profile is requested to cut
// airtime, and the backlight can be switched off.
class Stream
{
public:
	enum class EndReason
	{
		Quit,		// user chord or console-side quit
		Error,
	};

	// Blocks until the session ends. UI must have released the default
	// framebuffer before calling (deko3d takes the window).
	EndReason run(const HostEntry &host, const Settings &settings);

private:
	static void event_cb(ChiakiEvent *event, void *user);
	static bool video_cb(uint8_t *buf, size_t buf_size, int32_t frames_lost, bool frame_recovered, void *user);

	bool setup_video();

	Settings settings_{};
	ChiakiSession session_{};
	gfx::Presenter presenter_;
	video::Decoder decoder_;
	video::Renderer renderer_;
	audio::Pipeline audio_;
	input::Sampler input_;

	core::Worker vsync_watcher_;
	std::atomic<bool> vsync_stop_{ false };

	std::atomic<bool> session_quit_{ false };
	std::atomic<bool> failed_{ false };
	bool video_up_ = false;
};

} // namespace hayai::app
