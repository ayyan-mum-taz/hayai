// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "core/log.hpp"

#include <switch.h>

#include <sys/stat.h>

#include <cstdio>
#include <cstring>

namespace hayai::core {

RingLog &log()
{
	static RingLog instance;
	return instance;
}

void RingLog::chiaki_cb(ChiakiLogLevel level, const char *msg, void *user)
{
	static_cast<RingLog *>(user)->write(level, msg);
}

void RingLog::write(ChiakiLogLevel level, const char *msg)
{
	const uint64_t idx = head_.fetch_add(1, std::memory_order_relaxed);
	Slot &slot = slots_[idx % kSlots];

	// If the drain thread hasn't consumed this slot yet, drop rather than spin:
	// producers on the receive thread must never wait.
	uint32_t expected = 0;
	if(!slot.ready.compare_exchange_strong(expected, 2, std::memory_order_acquire))
	{
		dropped_.fetch_add(1, std::memory_order_relaxed);
		return;
	}

	slot.level = level;
	strncpy(slot.text, msg, kSlotSize - 1);
	slot.text[kSlotSize - 1] = '\0';
	slot.ready.store(1, std::memory_order_release);
}

void RingLog::drain_entry(void *arg)
{
	static_cast<RingLog *>(arg)->drain_loop();
}

void RingLog::drain_loop()
{
	// File sink first: it works regardless of who owns stdout, and it survives
	// crashes, which makes it the post-mortem channel for SD-card users.
	mkdir("/config", 0755);
	mkdir("/config/hayai", 0755);
	FILE *f = fopen("/config/hayai/hayai.log", "w");
	file_ = f;

	while(true)
	{
		bool drained_any = false;
		Slot &slot = slots_[tail_ % kSlots];
		uint32_t state = slot.ready.load(std::memory_order_acquire);
		if(state == 1)
		{
			if(f)
				fprintf(f, "[%c] %s\n", chiaki_log_level_char(slot.level), slot.text);
			// stdout only while the console does not own it (i.e. during
			// streaming, where stdout is nxlink or a null device). Writing to
			// the console from two threads corrupts its state.
			if(!console_active_.load(std::memory_order_relaxed))
				printf("[%c] %s\n", chiaki_log_level_char(slot.level), slot.text);
			slot.ready.store(0, std::memory_order_release);
			tail_++;
			drained_any = true;
		}
		else if(state == 2)
		{
			// Writer claimed it but hasn't finished; check again next round.
		}

		if(!drained_any)
		{
			if(f)
				fflush(f);
			if(!console_active_.load(std::memory_order_relaxed))
				fflush(stdout);
			if(stop_.load(std::memory_order_relaxed))
				break;
			svcSleepThread(20'000'000ULL);	// 20 ms; logs are not latency-critical
		}
	}
	if(f)
	{
		fflush(f);
		fclose(f);
		file_ = nullptr;
	}
	fflush(stdout);
}

bool RingLog::start()
{
	chiaki_log_init(&chiaki_log_, CHIAKI_LOG_ALL & ~CHIAKI_LOG_VERBOSE, &RingLog::chiaki_cb, this);

	static Thread thread;
	// Priority 0x3B: below everything that matters. Core 2, away from the
	// receive path on core 1.
	Result rc = threadCreate(&thread, &RingLog::drain_entry, this, nullptr, 0x4000, 0x3B, 2);
	if(R_FAILED(rc))
		return false;
	rc = threadStart(&thread);
	if(R_FAILED(rc))
	{
		threadClose(&thread);
		return false;
	}
	thread_ = &thread;
	return true;
}

void RingLog::stop()
{
	if(!thread_)
		return;
	stop_.store(true, std::memory_order_relaxed);
	Thread *t = static_cast<Thread *>(thread_);
	threadWaitForExit(t);
	threadClose(t);
	thread_ = nullptr;
}

} // namespace hayai::core
