// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "core/log.hpp"

#include <switch.h>

#include <sys/stat.h>
#include <unistd.h>

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
	// Keep one generation. Opening "w" alone meant that relaunching after a bad
	// session destroyed the very log that explained it.
	rename("/config/hayai/hayai.log", "/config/hayai/hayai.prev.log");
	FILE *f = fopen("/config/hayai/hayai.log", "w");
	file_ = f;

	char line[kSlotSize + 64];
	// Collapse runs of identical messages. A dying socket can emit the same
	// error thousands of times per second; without this it drowns the log,
	// overflows the ring and costs real time on the producing thread.
	char prev_text[kSlotSize] = { 0 };
	uint64_t repeat = 0;

	while(true)
	{
		bool drained_any = false;
		Slot &slot = slots_[tail_ % kSlots];
		uint32_t state = slot.ready.load(std::memory_order_acquire);
		if(state == 1)
		{
			if(strcmp(slot.text, prev_text) == 0)
			{
				repeat++;
				slot.ready.store(0, std::memory_order_release);
				tail_++;
				// Emit a marker occasionally so the log shows it is still going.
				if((repeat % 500) != 0)
					continue;
			}
			int len;
			if(repeat && (repeat % 500) == 0)
				len = snprintf(line, sizeof(line), "[%c] %s  (repeated %llu times)\n",
					chiaki_log_level_char(slot.level), slot.text,
					static_cast<unsigned long long>(repeat));
			else
			{
				if(repeat)
				{
					const int rl = snprintf(line, sizeof(line),
						"[.] last message repeated %llu times\n",
						static_cast<unsigned long long>(repeat));
					if(f && rl > 0)
					{
						fwrite(line, 1, static_cast<size_t>(rl), f);
						fflush(f);
					}
					const int rfd = sink_fd_.load(std::memory_order_relaxed);
					if(rfd >= 0 && rl > 0)
						::write(rfd, line, static_cast<size_t>(rl));
					repeat = 0;
				}
				strncpy(prev_text, slot.text, sizeof(prev_text) - 1);
				prev_text[sizeof(prev_text) - 1] = '\0';
				len = snprintf(line, sizeof(line), "[%c] %s\n",
					chiaki_log_level_char(slot.level), slot.text);
			}
			slot.ready.store(0, std::memory_order_release);
			tail_++;
			drained_any = true;

			if(len > 0)
			{
				// Never stdio: see set_sink_fd(). Flushed per line because this
				// file is the post-mortem channel and buffered tail data is
				// exactly what a crash eats.
				if(f)
				{
					fwrite(line, 1, static_cast<size_t>(len), f);
					fflush(f);
				}
				const int fd = sink_fd_.load(std::memory_order_relaxed);
				if(fd >= 0)
					::write(fd, line, static_cast<size_t>(len));
			}
		}
		else if(state == 2)
		{
			// Writer claimed it but hasn't finished; check again next round.
		}

		if(!drained_any)
		{
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
}

bool RingLog::start()
{
	chiaki_log_init(&chiaki_log_, CHIAKI_LOG_ALL & ~CHIAKI_LOG_VERBOSE, &RingLog::chiaki_cb, this);

	static Thread thread;
	// Priority 0x3B: below everything that matters. Core 2, away from the
	// receive path on core 1.
	Result rc = threadCreate(&thread, &RingLog::drain_entry, this, nullptr, 0x8000, 0x3B, 2);
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
