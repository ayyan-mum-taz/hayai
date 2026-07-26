// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "app/stream.hpp"
#include "core/log.hpp"
#include "core/telemetry.hpp"
#include "util/time.hpp"

#include <switch.h>

#include <cstring>

namespace hayai::app {

namespace {

// Pin CPU/GPU clocks for the duration of the stream. Not an overclock: these
// are the stock rates, held fixed so the DVFS governor cannot downclock a
// mostly-idle GPU and stretch our tiny render pass (see docs/latency.md,
// "DVFS trap"). Fails gracefully where clkrst is unavailable.
class ClockPin
{
public:
	void pin()
	{
		if(R_FAILED(clkrstInitialize()))
		{
			HAYAI_LOGW("clocks: clkrst unavailable, not pinning");
			return;
		}
		up_ = true;

		const bool docked = appletGetOperationMode() == AppletOperationMode_Console;
		pin_one(PcvModuleId_CpuBus, 1020000000, "cpu");
		pin_one(PcvModuleId_GPU, docked ? 768000000 : 460800000, "gpu");
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
	void pin_one(PcvModuleId module, u32 hz, const char *name)
	{
		if(n_ >= 2)
			return;
		ClkrstSession *s = &sessions_[n_];
		if(R_FAILED(clkrstOpenSession(s, module, 3)))
		{
			HAYAI_LOGW("clocks: open %s failed", name);
			return;
		}
		if(R_FAILED(clkrstSetClockRate(s, hz)))
		{
			HAYAI_LOGW("clocks: pin %s @ %u MHz failed", name, hz / 1000000);
			clkrstCloseSession(s);
			return;
		}
		HAYAI_LOGI("clocks: %s pinned @ %u MHz", name, hz / 1000000);
		n_++;
	}

	ClkrstSession sessions_[2]{};
	int n_ = 0;
	bool up_ = false;
};

struct VsyncCtx
{
	std::atomic<bool> *stop;
};

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

// The hot path. Runs on the takion receive thread; between entry and return
// there is no queue, no lock and no other thread.
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

	if(got_frame)
	{
		const int slot = self->presenter_.acquire();
		if(self->renderer_.draw(slot, self->decoder_.frame()))
			self->presenter_.present(slot);
		core::telemetry().end_frame(tl);
	}

	return true;
}

bool Stream::setup_video()
{
	if(!presenter_.create(1280, 720,
			settings_.vsync ? gfx::Presenter::Mode::Vsync : gfx::Presenter::Mode::Immediate))
	{
		HAYAI_LOGE("stream: presenter init failed");
		return false;
	}
	if(!decoder_.create(CHIAKI_CODEC_H265, core::log().chiaki_log()))
	{
		HAYAI_LOGE("stream: decoder init failed");
		return false;
	}
	if(!renderer_.create(presenter_, core::log().chiaki_log()))
	{
		HAYAI_LOGE("stream: renderer init failed");
		return false;
	}
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
	info.packet_loss_max = 0.05;
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
		chiaki_connect_video_profile_preset(&info.video_profile,
			settings_.resolution, settings_.fps);
		if(settings_.bitrate)
			info.video_profile.bitrate = settings_.bitrate;
		info.video_profile.codec = CHIAKI_CODEC_H265;
	}

	HAYAI_LOGI("stream: %s '%s' %ux%u@%u %u kbps%s",
		host.addr.c_str(), host.nickname.c_str(),
		info.video_profile.width, info.video_profile.height, info.video_profile.max_fps,
		info.video_profile.bitrate,
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
			return EndReason::Error;
		}
		chiaki_session_set_video_sample_cb(&session_, &Stream::video_cb, this);
	}

	ChiakiAudioSink sink{};
	audio_.sink(&sink);
	chiaki_session_set_audio_sink(&session_, &sink);
	audio_.start();

	input_.start(&session_);

	core::telemetry().start_session();
	vsync_stop_.store(false, std::memory_order_relaxed);
	vsync_watcher_.start(&vsync_watch_entry, &vsync_stop_, core::kPrioIdle, core::kCoreMain);

	ClockPin clocks;
	if(settings_.pin_clocks)
		clocks.pin();

	const bool backlight_off = settings_.controller_only && settings_.backlight_off;
	if(backlight_off)
	{
		if(R_SUCCEEDED(lblInitialize()))
			lblSwitchBacklightOff(500'000'000ULL);
	}

	err = chiaki_session_start(&session_);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		HAYAI_LOGE("stream: session start failed: %s", chiaki_error_string(err));
		session_quit_.store(true, std::memory_order_relaxed);
		failed_.store(true, std::memory_order_relaxed);
	}

	// Everything runs on its own threads; this one just waits for an exit.
	while(!session_quit_.load(std::memory_order_relaxed))
	{
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

	chiaki_session_join(&session_);
	chiaki_session_fini(&session_);

	// Session threads are dead: no more callbacks. Tear down in reverse.
	input_.stop();
	audio_.stop();

	vsync_stop_.store(true, std::memory_order_relaxed);
	vsync_watcher_.join();

	if(backlight_off)
	{
		lblSwitchBacklightOn(500'000'000ULL);
		lblExit();
	}
	clocks.unpin();

	renderer_.destroy();
	decoder_.destroy();
	presenter_.destroy();
	video_up_ = false;

	return failed_.load(std::memory_order_relaxed) ? EndReason::Error : EndReason::Quit;
}

} // namespace hayai::app
