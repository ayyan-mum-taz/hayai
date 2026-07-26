// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
#pragma once

#include <chiaki/log.h>

#include <atomic>
#include <cstdint>

namespace hayai::core {

// Multi-producer, single-consumer ring logger.
//
// libchiaki fires its log callback from the takion receive thread -- the most
// latency-critical thread in the program -- and stdout here is nxlink, i.e.
// blocking TCP. So producers only format into a fixed slot and never touch
// I/O; a low-priority drain thread does the printing. When the ring is full
// the message is dropped and counted, because a dropped log line is better
// than a stalled receive thread.
class RingLog
{
public:
	static constexpr unsigned kSlots = 256;
	static constexpr unsigned kSlotSize = 224;

	// Starts the drain thread. Call once, early.
	bool start();
	// Flushes and stops the drain thread.
	void stop();

	// The devkitPro console is not thread-safe, and while it owns stdout the
	// drain thread must not printf into it concurrently with the UI. The UI
	// flips this around consoleInit/consoleExit; the file sink always runs.
	void set_console_active(bool active) { console_active_.store(active, std::memory_order_relaxed); }

	void write(ChiakiLogLevel level, const char *msg);

	// ChiakiLog glue: chiaki_log_init(&log, mask, RingLog::chiaki_cb, &ring)
	static void chiaki_cb(ChiakiLogLevel level, const char *msg, void *user);

	ChiakiLog *chiaki_log() { return &chiaki_log_; }

	uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }

private:
	struct Slot
	{
		std::atomic<uint32_t> ready{ 0 };
		ChiakiLogLevel level;
		char text[kSlotSize];
	};

	static void drain_entry(void *arg);
	void drain_loop();

	Slot slots_[kSlots];
	std::atomic<uint64_t> head_{ 0 };	// next slot to write (producers)
	uint64_t tail_ = 0;			// next slot to read (drain thread only)
	std::atomic<uint64_t> dropped_{ 0 };
	std::atomic<bool> stop_{ false };
	std::atomic<bool> console_active_{ false };
	void *thread_ = nullptr;	// Thread* (kept opaque to avoid switch.h here)
	void *file_ = nullptr;		// FILE* (drain thread only)
	ChiakiLog chiaki_log_{};
};

// The process-wide logger.
RingLog &log();

} // namespace hayai::core

#define HAYAI_LOGV(...) chiaki_log(::hayai::core::log().chiaki_log(), CHIAKI_LOG_VERBOSE, __VA_ARGS__)
#define HAYAI_LOGD(...) chiaki_log(::hayai::core::log().chiaki_log(), CHIAKI_LOG_DEBUG, __VA_ARGS__)
#define HAYAI_LOGI(...) chiaki_log(::hayai::core::log().chiaki_log(), CHIAKI_LOG_INFO, __VA_ARGS__)
#define HAYAI_LOGW(...) chiaki_log(::hayai::core::log().chiaki_log(), CHIAKI_LOG_WARNING, __VA_ARGS__)
#define HAYAI_LOGE(...) chiaki_log(::hayai::core::log().chiaki_log(), CHIAKI_LOG_ERROR, __VA_ARGS__)
