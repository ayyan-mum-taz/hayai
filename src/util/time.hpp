// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
#pragma once

#include <switch.h>
#include <cstdint>

namespace hayai {

// The ARM system counter, in nanoseconds.
//
// This is the only clock hayai measures latency with. Reading it is a register
// read rather than a syscall, it is monotonic, and it is shared across cores,
// so a timestamp taken on the network thread is directly comparable to one
// taken on the render thread.
inline uint64_t now_ns()
{
	return armTicksToNs(armGetSystemTick());
}

constexpr uint64_t operator""_ms(unsigned long long v) { return v * 1000000ULL; }
constexpr uint64_t operator""_us(unsigned long long v) { return v * 1000ULL; }

} // namespace hayai
