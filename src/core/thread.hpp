// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
#pragma once

#include <switch.h>

#include <cstdint>

namespace hayai::core {

// Core assignments. Horizon gives applications cores 0-2 preemptively;
// core 3 belongs to the OS.
//
//   core 0: main/UI, discovery, log drain, input sampler
//   core 1: takion receive thread = decode = present (the hot path, alone)
//   core 2: audio pipeline, alone -- it has only tens of ms of hardware
//           runway, so it must never round-robin with anything
//
// The point is that the hot path never shares a core with anything that could
// preempt it -- and that measurements become repeatable.
constexpr int kCoreMain = 0;
constexpr int kCoreStream = 1;
constexpr int kCoreAux = 2;

// Priorities: lower number = higher priority on Horizon. 0x2C is the default
// main-thread priority for applications.
constexpr int kPrioHot = 0x24;
constexpr int kPrioAux = 0x2C;
constexpr int kPrioIdle = 0x3B;

// Minimal owned-thread wrapper over libnx threads (we avoid std::thread so
// core and priority are explicit at creation).
class Worker
{
public:
	using Fn = void (*)(void *);

	Worker() = default;
	~Worker() { join(); }
	Worker(const Worker &) = delete;
	Worker &operator=(const Worker &) = delete;

	bool start(Fn fn, void *arg, int prio, int core, size_t stack = 0x8000)
	{
		if(started_)
			return false;
		if(R_FAILED(threadCreate(&thread_, fn, arg, nullptr, stack, static_cast<u32>(prio), core)))
			return false;
		if(R_FAILED(threadStart(&thread_)))
		{
			threadClose(&thread_);
			return false;
		}
		started_ = true;
		return true;
	}

	void join()
	{
		if(!started_)
			return;
		threadWaitForExit(&thread_);
		threadClose(&thread_);
		started_ = false;
	}

	bool running() const { return started_; }

private:
	Thread thread_{};
	bool started_ = false;
};

// Pin libchiaki's own threads as they announce themselves. Registered once via
// chiaki_thread_set_affinity_cb; runs in the context of the announcing thread,
// so svcSetThreadCoreMask/priority on the current thread applies to it.
void install_chiaki_affinity_hook();

} // namespace hayai::core
