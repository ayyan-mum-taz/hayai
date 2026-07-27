// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "app/stream.hpp"
#include "core/log.hpp"
#include "core/telemetry.hpp"
#include "util/time.hpp"

#include <switch.h>

#include <cstring>

namespace hayai::app {

namespace {

// Hold CPU/GPU clocks up for the duration of the stream.
//
// This is not an overclock and it must never be a *downclock*. The DVFS
// governor drops a mostly-idle GPU, which stretches our tiny render pass and
// adds variance -- but plenty of people run sys-clk with the console pinned far
// above stock, and naively setting stock rates here would slow those systems
// down mid-session. So we read what is already in effect and only ever raise:
// if the current rate meets or beats our floor, we leave the rail alone
// entirely and never open a session on it.
class ClockPin
{
public:
	void pin()
	{
		if(R_FAILED(clkrstInitialize()))
		{
			HAYAI_LOGW("clocks: clkrst unavailable, leaving governor alone");
			return;
		}
		up_ = true;

		const bool docked = appletGetOperationMode() == AppletOperationMode_Console;
		raise_to(PcvModuleId_CpuBus, 1020000000, "cpu");
		raise_to(PcvModuleId_GPU, docked ? 768000000 : 460800000, "gpu");
	}

	void unpin()
	{
		for(int i = 0; i < n_; i++)
			clkrstCloseSession(&sessions_[i]);
		n_ = 0;
		if(up_)
		{
			clkrstExit();
			up_ = false;
		}
	}

private:
	void raise_to(PcvModuleId module, u32 floor_hz, const char *name)
	{
		if(n_ >= 2)
			return;
		ClkrstSession *s = &sessions_[n_];
		if(R_FAILED(clkrstOpenSession(s, module, 3)))
		{
			HAYAI_LOGW("clocks: cannot query %s, leaving it alone", name);
			return;
		}

		u32 current = 0;
		if(R_SUCCEEDED(clkrstGetClockRate(s, &current)) && current >= floor_hz)
		{
			// Already at or above our floor -- almost certainly sys-clk or a
			// similar manager. Close the session so we do not fight it.
			HAYAI_LOGI("clocks: %s already at %u MHz, not touching it", name, current / 1000000);
			clkrstCloseSession(s);
			return;
		}

		if(R_FAILED(clkrstSetClockRate(s, floor_hz)))
		{
			HAYAI_LOGW("clocks: raising %s to %u MHz failed", name, floor_hz / 1000000);
			clkrstCloseSession(s);
			return;
		}
		HAYAI_LOGI("clocks: %s raised %u -> %u MHz", name, current / 1000000, floor_hz / 1000000);
		n_++;
	}

	ClkrstSession sessions_[2]{};
	int n_ = 0;
	bool up_ = false;
};

// Numeric prompt via the software keyboard; usable mid-stream (the library
// applet overlays whatever owns the display).
bool prompt_login_pin(char *out, size_t out_size)
{
	SwkbdConfig kbd;
	if(R_FAILED(swkbdCreate(&kbd, 0)))
		return false;
	swkbdConfigMakePresetDefault(&kbd);
	swkbdConfigSetGuideText(&kbd, "PS5 Remote Play login PIN");
	swkbdConfigSetType(&kbd, SwkbdType_NumPad);
	swkbdConfigSetStringLenMax(&kbd, static_cast<u32>(out_size - 1));
	out[0] = '\0';
	const Result rc = swkbdShow(&kbd, out, out_size);
	swkbdClose(&kbd);
	return R_SUCCEEDED(rc) && out[0] != '\0';
}

// Watches the display vsync event to timestamp vblanks -- the reference for
// beat-phase telemetry -- and hosts the periodic summary logging, keeping both
// off the hot path. Falls back to a plain timer if vi says no.
void vsync_watch_entry(void *arg)
{
	auto *stop = static_cast<std::atomic<bool> *>(arg);

	ViDisplay display{};
	Event vsync_event{};
	bool have_event = false;
	if(R_SUCCEEDED(viOpenDefaultDisplay(&display)))
	{
		if(R_SUCCEEDED(viGetDisplayVsyncEvent(&display, &vsync_event)))
			have_event = true;
		else
			viCloseDisplay(&display);
	}
	if(!have_event)
		HAYAI_LOGW("vsync: no event from vi, phase telemetry disabled");

	while(!stop->load(std::memory_order_relaxed))
	{
		if(have_event)
		{
			if(R_SUCCEEDED(eventWait(&vsync_event, 100'000'000ULL)))
				core::telemetry().vsync_tick(now_ns());
		}
		else
			svcSleepThread(100'000'000ULL);

		core::telemetry().log_summary();
	}

	if(have_event)
	{
		eventClose(&vsync_event);
		viCloseDisplay(&display);
	}
}

} // namespace

void Stream::event_cb(ChiakiEvent *event, void *user)
{
	auto *self = static_cast<Stream *>(user);
	switch(event->type)
	{
		case CHIAKI_EVENT_CONNECTED:
			HAYAI_LOGI("session: connected");
			break;
		case CHIAKI_EVENT_RUMBLE:
			self->input_.set_rumble(event->rumble.left, event->rumble.right);
			break;
		case CHIAKI_EVENT_LOGIN_PIN_REQUEST:
			HAYAI_LOGI("session: console requests login PIN");
			self->login_pin_needed_.store(true, std::memory_order_relaxed);
			break;
		case CHIAKI_EVENT_VIDEO_FEC_FAILURE:
			core::telemetry().fec_failure();
			break;
		case CHIAKI_EVENT_QUIT:
			HAYAI_LOGI("session: quit (%s)", chiaki_quit_reason_string(event->quit.reason));
			if(chiaki_quit_reason_is_error(event->quit.reason))
				self->failed_.store(true, std::memory_order_relaxed);
			self->session_quit_.store(true, std::memory_order_relaxed);
			break;
		default:
			break;
	}
}

// The hot path. Runs on the takion receive thread: decrypt/reassemble happen
// upstream of us, we decode, and then we hand off. Nothing here blocks on the
// display -- see the note on Stream for why present lives on its own thread.
bool Stream::video_cb(uint8_t *buf, size_t buf_size, int32_t frames_lost, bool frame_recovered, void *user)
{
	(void)frame_recovered;
	auto *self = static_cast<Stream *>(user);
	if(!self->video_up_)
		return true;

	core::Telemetry::Frame &tl = core::telemetry().begin_frame(
		static_cast<uint32_t>(buf_size), static_cast<uint32_t>(frames_lost));

	const bool got_frame = self->decoder_.submit(buf, buf_size);
	tl.decode_done_ns = now_ns();
	if(!got_frame)
		return true;

	// Publish to the mailbox. av_frame_ref is a refcount bump on the NVDEC
	// surface, not a pixel copy, so this stays O(1) and keeps the decoder free
	// to reuse its own frame on the next packet.
	mutexLock(&self->mailbox_mutex_);
	if(self->mailbox_full_)
	{
		// Present thread is still busy with the previous frame: drop it, keep
		// the newer one. Latency over completeness.
		av_frame_unref(self->mailbox_frame_);
		self->frames_dropped_.fetch_add(1, std::memory_order_relaxed);
		core::telemetry().frame_dropped();
	}
	if(av_frame_ref(self->mailbox_frame_, self->decoder_.frame()) == 0)
	{
		self->mailbox_tl_ = &tl;
		self->mailbox_full_ = true;
		condvarWakeOne(&self->mailbox_cond_);
	}
	mutexUnlock(&self->mailbox_mutex_);

	return true;
}

void Stream::present_entry(void *arg)
{
	static_cast<Stream *>(arg)->present_loop();
}

void Stream::present_loop()
{
	AVFrame *frame = av_frame_alloc();
	if(!frame)
	{
		HAYAI_LOGE("present: frame alloc failed");
		return;
	}

	HAYAI_LOGI("present: thread up (%u images, %s)",
		presenter_.image_count(),
		presenter_.mode() == gfx::Presenter::Mode::Vsync ? "vsync" : "immediate");

	while(!present_stop_.load(std::memory_order_relaxed))
	{
		core::Telemetry::Frame *tl = nullptr;

		mutexLock(&mailbox_mutex_);
		while(!mailbox_full_ && !present_stop_.load(std::memory_order_relaxed))
			condvarWaitTimeout(&mailbox_cond_, &mailbox_mutex_, 50'000'000ULL);
		if(mailbox_full_)
		{
			av_frame_move_ref(frame, mailbox_frame_);
			tl = mailbox_tl_;
			mailbox_full_ = false;
		}
		mutexUnlock(&mailbox_mutex_);

		if(!tl)
			continue;

		// Blocking here is fine: this thread owns nothing the network needs.
		const int slot = presenter_.acquire();
		if(renderer_.draw(slot, frame))
			presenter_.present(slot);
		core::telemetry().end_frame(*tl);
		av_frame_unref(frame);
	}

	av_frame_free(&frame);
	HAYAI_LOGI("present: thread down (%llu frames dropped as stale)",
		static_cast<unsigned long long>(frames_dropped_.load(std::memory_order_relaxed)));
}

// Idempotent, and called on every exit from run(). The swapchain lives on the
// console's one and only NWindow, so leaving it alive after an early return
// meant the UI would later create a *second* swapchain on the same window --
// deko3d then raised on acquireImage and the process broke. That is precisely
// how the error dialog itself crashed.
void Stream::teardown_video()
{
	present_stop_.store(true, std::memory_order_relaxed);
	if(mailbox_frame_)
	{
		mutexLock(&mailbox_mutex_);
		condvarWakeAll(&mailbox_cond_);
		mutexUnlock(&mailbox_mutex_);
	}
	present_thread_.join();

	if(mailbox_frame_)
	{
		av_frame_free(&mailbox_frame_);
		mailbox_full_ = false;
	}

	renderer_.destroy();
	decoder_.destroy();
	presenter_.destroy();
	video_up_ = false;
}

bool Stream::setup_video()
{
	// Breadcrumbs: each stage logs before it runs, and the log is flushed per
	// line, so if any of this faults the last line in hayai.log names the stage.
	HAYAI_LOGI("video: creating deko3d device/swapchain (1280x720, %s, %u images)",
		settings_.vsync() ? "vsync" : "immediate", settings_.swapchain_images());
	if(!presenter_.create(1280, 720,
			settings_.vsync() ? gfx::Presenter::Mode::Vsync : gfx::Presenter::Mode::Immediate,
			settings_.swapchain_images()))
	{
		HAYAI_LOGE("stream: presenter init failed");
		return false;
	}

	HAYAI_LOGI("video: opening NVDEC decoder");
	if(!decoder_.create(CHIAKI_CODEC_H265, core::log().chiaki_log()))
	{
		HAYAI_LOGE("stream: decoder init failed");
		return false;
	}

	HAYAI_LOGI("video: loading shaders and building render state");
	if(!renderer_.create(presenter_, core::log().chiaki_log()))
	{
		HAYAI_LOGE("stream: renderer init failed");
		return false;
	}

	// Present runs on its own thread so that dkQueueAcquireImage never blocks
	// the socket-draining thread. Core 0 is otherwise idle during a stream.
	mailbox_frame_ = av_frame_alloc();
	if(!mailbox_frame_)
		return false;
	mutexInit(&mailbox_mutex_);
	condvarInit(&mailbox_cond_);
	present_stop_.store(false, std::memory_order_relaxed);
	if(!present_thread_.start(&Stream::present_entry, this, core::kPrioHot, core::kCoreMain, 0x10000))
	{
		HAYAI_LOGE("stream: present thread failed to start");
		return false;
	}

	HAYAI_LOGI("video: pipeline ready");
	video_up_ = true;
	return true;
}

Stream::EndReason Stream::run(const HostEntry &host, const Settings &settings)
{
	settings_ = settings;
	session_quit_.store(false, std::memory_order_relaxed);
	failed_.store(false, std::memory_order_relaxed);
	video_up_ = false;

	ChiakiConnectInfo info{};
	info.ps5 = true;
	info.host = host.addr.c_str();
	memcpy(info.regist_key, host.rp_regist_key, sizeof(info.regist_key));
	memcpy(info.morning, host.rp_key, sizeof(info.morning));
	info.video_profile_auto_downgrade = true;
	info.enable_keyboard = false;
	info.enable_dualsense = false;
	info.packet_loss_max = settings_.packet_loss_max();
	info.enable_idr_on_fec_failure = true;

	if(settings_.controller_only)
	{
		// The console decides what it sends; we minimize and discard. The
		// lowest profile cuts airtime, the takion flag drops video pre-parse.
		chiaki_connect_video_profile_preset(&info.video_profile,
			CHIAKI_VIDEO_RESOLUTION_PRESET_360p, CHIAKI_VIDEO_FPS_PRESET_30);
		info.audio_video_disabled = CHIAKI_VIDEO_DISABLED;
	}
	else
	{
		// Built explicitly rather than via the preset helper: the launch spec
		// carries width/height directly; all shapes here are natively advertised.
		info.video_profile.width = settings_.width();
		info.video_profile.height = settings_.height();
		info.video_profile.max_fps = static_cast<unsigned>(settings_.fps);
		info.video_profile.bitrate = settings_.default_bitrate_kbps();
		info.video_profile.codec = CHIAKI_CODEC_H265;
	}

	// Per-profile head-of-line wait for out-of-order AV packets.
	chiaki_takion_set_av_reorder_timeout_us(settings_.reorder_timeout_us());

	HAYAI_LOGI("stream: %s '%s' %ux%u@%u %u kbps | profile %s, loss_max %.0f%%, reorder %llu us%s",
		host.addr.c_str(), host.nickname.c_str(),
		info.video_profile.width, info.video_profile.height, info.video_profile.max_fps,
		info.video_profile.bitrate, settings_.profile_name(),
		settings_.packet_loss_max() * 100.0,
		static_cast<unsigned long long>(settings_.reorder_timeout_us()),
		settings_.controller_only ? " (controller-only)" : "");

	ChiakiErrorCode err = chiaki_session_init(&session_, &info, core::log().chiaki_log());
	if(err != CHIAKI_ERR_SUCCESS)
	{
		HAYAI_LOGE("stream: session init failed: %s", chiaki_error_string(err));
		return EndReason::Error;
	}

	chiaki_session_set_event_cb(&session_, &Stream::event_cb, this);

	if(!settings_.controller_only)
	{
		if(!setup_video())
		{
			chiaki_session_fini(&session_);
			teardown_video();
			core::log().set_deferred(false);
			return EndReason::Error;
		}
		chiaki_session_set_video_sample_cb(&session_, &Stream::video_cb, this);
	}

	HAYAI_LOGI("stream: starting audio pipeline");
	ChiakiAudioSink sink{};
	audio_.sink(&sink);
	chiaki_session_set_audio_sink(&session_, &sink);
	audio_.start();

	HAYAI_LOGI("stream: starting input sampler");
	input_.start(&session_);

	HAYAI_LOGI("stream: starting vsync watcher");
	core::telemetry().start_session();
	vsync_stop_.store(false, std::memory_order_relaxed);
	vsync_watcher_.start(&vsync_watch_entry, &vsync_stop_, core::kPrioIdle, core::kCoreMain);

	ClockPin clocks;
	if(settings_.pin_clocks)
	{
		HAYAI_LOGI("stream: pinning clocks");
		clocks.pin();
	}

	const bool backlight_off = settings_.controller_only && settings_.backlight_off;
	if(backlight_off)
	{
		if(R_SUCCEEDED(lblInitialize()))
			lblSwitchBacklightOff(500'000'000ULL);
	}

	// No storage I/O for the duration of the stream.
	core::log().set_deferred(true);

	HAYAI_LOGI("stream: starting session (handshake)");
	err = chiaki_session_start(&session_);
	const bool started = err == CHIAKI_ERR_SUCCESS;
	if(!started)
	{
		HAYAI_LOGE("stream: session start failed: %s", chiaki_error_string(err));
		session_quit_.store(true, std::memory_order_relaxed);
		failed_.store(true, std::memory_order_relaxed);
	}

	// Everything runs on its own threads; this one just waits for an exit.
	while(!session_quit_.load(std::memory_order_relaxed))
	{
		if(login_pin_needed_.exchange(false, std::memory_order_relaxed))
		{
			char pin[12];
			if(prompt_login_pin(pin, sizeof(pin)))
				chiaki_session_set_login_pin(&session_,
					reinterpret_cast<const uint8_t *>(pin), strlen(pin));
			else
			{
				HAYAI_LOGW("stream: login PIN canceled, stopping");
				chiaki_session_stop(&session_);
				break;
			}
		}
		core::telemetry().set_input_sends(input_.sends());
		if(input_.quit_requested())
		{
			HAYAI_LOGI("stream: quit chord");
			chiaki_session_stop(&session_);
			break;
		}
		if(!appletMainLoop())
		{
			chiaki_session_stop(&session_);
			break;
		}
		svcSleepThread(10'000'000ULL);	// 10 ms
	}

	// Order matters here. Join first so no session thread can fire another
	// callback; then stop our threads that poke the session (input calls
	// chiaki_session_set_controller_state); only then fini, which destroys
	// the mutexes those calls take.
	if(started)
		chiaki_session_join(&session_);
	input_.stop();
	audio_.stop();
	chiaki_session_fini(&session_);

	vsync_stop_.store(true, std::memory_order_relaxed);
	vsync_watcher_.join();

	// No more callbacks can publish frames now.

	if(backlight_off)
	{
		lblSwitchBacklightOn(500'000'000ULL);
		lblExit();
	}
	clocks.unpin();

	teardown_video();
	core::log().set_deferred(false);

	return failed_.load(std::memory_order_relaxed) ? EndReason::Error : EndReason::Quit;
}

} // namespace hayai::app
