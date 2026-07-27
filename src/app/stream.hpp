// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
#pragma once

#include "app/config.hpp"
#include "audio/audio.hpp"
#include "core/telemetry.hpp"
#include "core/thread.hpp"
#include "gfx/presenter.hpp"
#include "input/input.hpp"
#include "video/decoder.hpp"
#include "video/renderer.hpp"

#include <chiaki/session.h>

extern "C" {
#include <libavutil/frame.h>
}

#include <atomic>

namespace hayai::app {

// One remote play session, start to finish.
//
// Decode runs synchronously on the takion receive thread -- that part is free,
// NVDEC is fixed-function and returns in ~1.7 ms. Presentation does not:
// dkQueueAcquireImage blocks until the compositor releases a buffer, which can
// be most of a refresh interval, and blocking there would stall the thread that
// drains the socket. So the decoded frame is handed to a one-slot mailbox and a
// dedicated present thread does acquire/draw/present.
//
// The mailbox is newest-wins by design: if a frame is still waiting when the
// next one decodes, the waiting one is dropped. Showing a stale frame to
// preserve a queue is exactly the trade this project exists to refuse.
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
	static void present_entry(void *arg);

	void present_loop();
	bool setup_video();
	// Idempotent; every exit path from run() goes through it.
	void teardown_video();

	Settings settings_{};
	ChiakiSession session_{};
	gfx::Presenter presenter_;
	video::Decoder decoder_;
	video::Renderer renderer_;
	audio::Pipeline audio_;
	input::Sampler input_;

	// --- present mailbox ---
	core::Worker present_thread_;
	Mutex mailbox_mutex_{};
	CondVar mailbox_cond_{};
	AVFrame *mailbox_frame_ = nullptr;	// holds a ref while pending
	core::Telemetry::Frame *mailbox_tl_ = nullptr;
	bool mailbox_full_ = false;
	std::atomic<bool> present_stop_{ false };
	std::atomic<uint64_t> frames_dropped_{ 0 };

	core::Worker vsync_watcher_;
	std::atomic<bool> vsync_stop_{ false };

	std::atomic<bool> session_quit_{ false };
	std::atomic<bool> failed_{ false };
	std::atomic<bool> login_pin_needed_{ false };
	bool video_up_ = false;
};

} // namespace hayai::app
