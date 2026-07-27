// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
#pragma once

#include "core/thread.hpp"

#include <chiaki/audio.h>
#include <chiaki/audioreceiver.h>	// ChiakiAudioSink

#include <atomic>
#include <cstdint>

struct OpusDecoder;
struct SwrContext;

namespace hayai::audio {

// The audio pipeline, built around two rules:
//
// 1. The receive thread never decodes. libchiaki's own opus sink runs the
//    decoder inside the takion event path -- a ~0.5-1 ms stall injected into
//    the video-critical thread every 10 ms. Our sink only copies the encoded
//    opus packet (a few hundred bytes) into an SPSC ring and returns.
//
// 2. Drift is corrected continuously, not catastrophically. The PS5's audio
//    clock and the Switch's DAC will always drift apart; the fork lets SDL's
//    queue grow to ~83 ms and then flushes it (a latency sawtooth ending in a
//    glitch). We keep the buffer at a small setpoint with ppm-scale resampler
//    compensation -- a software PLL. Steady ~15 ms, no glitches.
class Pipeline
{
public:
	bool start();
	void stop();

	// The ChiakiAudioSink glue; safe to call before start().
	void sink(ChiakiAudioSink *out);

	bool running() const { return running_.load(std::memory_order_relaxed); }

private:
	static constexpr unsigned kPacketSlots = 64;	// SPSC ring of encoded packets
	static constexpr unsigned kPacketMax = 1024;	// opus frames are a few hundred bytes

	// 20 ms per device buffer, three in flight -- 60 ms of hardware runway.
	//
	// This was 10 ms x 3 = 30 ms, and underruns accumulated at ~5/s even in
	// windows where the network delivered a clean 60 fps with zero loss, which
	// makes them ours rather than the link's. With only 30 ms of runway, any
	// scheduling gap longer than that starves the device outright. Doubling the
	// buffer both doubles the margin and halves this thread's wakeup rate,
	// which matters because it shares a core with other work.
	static constexpr unsigned kOutSamples = 960;
	static constexpr unsigned kOutBuffers = 3;
	// Audio needs a real jitter buffer, unlike video. A late video frame is an
	// invisible 16 ms repeat; a late audio buffer is an audible click. Wi-Fi
	// delivers opus packets in bursts, so 10 ms of slack produced thousands of
	// underruns -- 30 ms costs nothing perceptible and eliminates them.
	static constexpr int kFillSetpointSamples = 1440;	// 30 ms
	// Only cut back when the backlog is genuinely standing latency (100 ms),
	// rather than fighting normal burstiness.
	static constexpr unsigned kMaxBacklogSamples = 4800;

	struct Packet
	{
		std::atomic<uint32_t> full{ 0 };
		uint32_t size = 0;
		uint8_t data[kPacketMax];
	};

	static void header_cb(ChiakiAudioHeader *header, void *user);
	static void frame_cb(uint8_t *buf, size_t buf_size, void *user);
	static void thread_entry(void *arg);
	void thread_loop();
	void apply_compensation(int backlog_samples);

	Packet packets_[kPacketSlots];
	std::atomic<uint64_t> pkt_head_{ 0 };	// producer (recv thread)
	uint64_t pkt_tail_ = 0;			// consumer (audio thread)
	std::atomic<uint64_t> pkt_dropped_{ 0 };

	std::atomic<uint32_t> channels_{ 2 };
	std::atomic<uint32_t> rate_{ 48000 };

	OpusDecoder *opus_ = nullptr;
	SwrContext *swr_ = nullptr;
	int comp_ppm_ = 0;

	uint64_t backlog_trims_ = 0;
	uint64_t last_backlog_log_ns_ = 0;

	core::Worker worker_;
	std::atomic<bool> stop_{ false };
	std::atomic<bool> running_{ false };
};

} // namespace hayai::audio
