// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
#pragma once

#include "core/thread.hpp"

#include <chiaki/orientation.h>
#include <chiaki/session.h>

#include <switch.h>

#include <atomic>
#include <cstdint>

namespace hayai::input {

// Just-in-time input sampling.
//
// libchiaki's feedback sender transmits whatever state was most recently set,
// whenever its gate allows -- so the wire carries state as old as the last
// poll. Polling from a 60 Hz UI loop makes that up to 16.7 ms. This thread
// samples HID at a 2 ms cadence and hands the state to the session at the
// moment of sampling, collapsing staleness to the cadence bound. The gate in
// the feedback sender (FEEDBACK_STATE_TIMEOUT_MIN_MS, a build knob) then
// decides the on-wire rate; since motion data changes every sample, the gate
// value is also the effective gyro rate.
//
// The quit chord (L+R+MINUS held ~1 s) is detected here because this is the
// only place that reads the pad during streaming.
class Sampler
{
public:
	bool start(ChiakiSession *session);
	void stop();

	bool quit_requested() const { return quit_.load(std::memory_order_relaxed); }

	// Rumble from session events (any thread).
	void set_rumble(uint8_t left, uint8_t right);

private:
	static constexpr uint64_t kCadenceNs = 2'000'000;	// 2 ms
	static constexpr uint64_t kQuitHoldNs = 1'000'000'000;

	static void thread_entry(void *arg);
	void thread_loop();
	void sample(ChiakiControllerState *state);
	void update_rumble();

	ChiakiSession *session_ = nullptr;
	PadState pad_{};
	HidSixAxisSensorHandle sixaxis_handles_[4]{};
	ChiakiOrientationTracker orient_tracker_{};
	ChiakiAccelNewZero accel_zero_{};

	// touchscreen finger id -> chiaki touch id (-1 = none); 16 hw slots max
	int8_t touch_map_[16];
	uint32_t touch_finger_[16];

	std::atomic<uint16_t> rumble_{ 0 };	// left<<8 | right
	uint16_t rumble_applied_ = 0xFFFF;

	core::Worker worker_;
	std::atomic<bool> stop_{ false };
	std::atomic<bool> quit_{ false };
	uint64_t quit_hold_start_ = 0;
};

} // namespace hayai::input
