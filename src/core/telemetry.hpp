// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
#pragma once

#include "util/time.hpp"

#include <atomic>
#include <cstdint>

namespace hayai::core {

// Per-frame pipeline timeline.
//
// Every stage of the video path stamps into the current record; a summary
// with percentiles goes to the log periodically. Percentiles, not means:
// "mush" lives in the tail.
//
// Also captures vsync phase. The PS5's encoder and the Switch's panel are two
// free-running 60 Hz clocks, so the phase between frame arrival and the
// compositor's vblank latch drifts continuously (beat period = minutes).
// Latency oscillates by up to a frame with no code changing; without phase in
// the record, A/B measurements are noise. See docs/latency.md.
class Telemetry
{
public:
	struct Frame
	{
		uint64_t au_complete_ns;	// video_sample_cb entry (last unit arrived)
		uint64_t decode_done_ns;
		uint64_t present_ns;
		uint32_t au_size;
		uint32_t frames_lost;
	};

	// Whole-session aggregate, for the report shown when a stream ends.
	struct Summary
	{
		uint64_t frames = 0;
		uint64_t duration_ms = 0;
		double avg_fps = 0.0;
		double avg_present_ms = 0.0;
		double worst_present_ms = 0.0;
		uint64_t slow_frames = 0;	// present took longer than a refresh
		uint64_t fec_failures = 0;
		uint64_t frames_dropped = 0;
		uint64_t audio_underruns = 0;
		uint64_t frames_lost = 0;
	};
	Summary summary() const;

	static constexpr unsigned kRing = 1024;

	void start_session();

	// Called on the receive thread. Cheap: writes into the ring only.
	Frame &begin_frame(uint32_t au_size, uint32_t frames_lost);
	void end_frame(Frame &f);

	// Called from the vsync watcher thread.
	void vsync_tick(uint64_t ts_ns);

	// Called from anywhere (drain thread does the logging).
	void log_summary();

	void set_input_sends(uint64_t n) { input_sends_.store(n, std::memory_order_relaxed); }
	void fec_failure() { fec_failures_.fetch_add(1, std::memory_order_relaxed); }
	void frame_dropped() { frames_dropped_.fetch_add(1, std::memory_order_relaxed); }
	void audio_underrun() { audio_underruns_.fetch_add(1, std::memory_order_relaxed); }
	void audio_stats(int fill_ms, int comp_ppm)
	{
		audio_fill_ms_.store(fill_ms, std::memory_order_relaxed);
		audio_comp_ppm_.store(comp_ppm, std::memory_order_relaxed);
	}

private:
	Frame ring_[kRing]{};
	std::atomic<uint64_t> frame_count_{ 0 };
	uint64_t summary_start_idx_ = 0;
	uint64_t summary_last_ns_ = 0;

	std::atomic<uint64_t> last_vsync_ns_{ 0 };
	std::atomic<uint64_t> vsync_period_ns_{ 16'666'667 };

	std::atomic<uint64_t> audio_underruns_{ 0 };
	std::atomic<uint64_t> frames_dropped_{ 0 };
	std::atomic<uint64_t> fec_failures_{ 0 };
	std::atomic<uint64_t> input_sends_{ 0 };
	// Session accumulators (present thread writes, UI reads after the stream).
	std::atomic<uint64_t> sess_frames_{ 0 };
	std::atomic<uint64_t> sess_present_us_{ 0 };
	std::atomic<uint64_t> sess_worst_us_{ 0 };
	std::atomic<uint64_t> sess_slow_{ 0 };
	std::atomic<uint64_t> sess_lost_{ 0 };
	uint64_t sess_start_ns_ = 0;
	uint64_t input_sends_prev_ = 0;
	std::atomic<int> audio_fill_ms_{ 0 };
	std::atomic<int> audio_comp_ppm_{ 0 };
};

Telemetry &telemetry();

} // namespace hayai::core
