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
	// Sampling stays fast, but *sending* is rate-limited by event class.
	//
	// libchiaki treats any change as a reason to transmit, and its motion
	// comparison uses a 1e-7 epsilon -- which gyro noise exceeds on every
	// sample. Sampling at 2 ms therefore produced ~500 packets/s, which
	// exhausted the socket buffers, overflowed the send buffer and eventually
	// killed the session. Sends are now classed by what actually changed:
	// buttons go out immediately (rare, and the thing players feel), stick
	// motion is bounded, and motion-only updates ride a fixed cadence.
	static constexpr uint64_t kCadenceNs = 2'000'000;	// sampling
	static constexpr uint64_t kMinSendGapNs = 4'000'000;	// 250 Hz ceiling
	static constexpr uint64_t kMotionIntervalNs = 8'000'000;	// 125 Hz motion
	static constexpr int16_t kStickSendThreshold = 192;	// ignore analog noise
	static constexpr uint64_t kQuitHoldNs = 1'000'000'000;

	static void thread_entry(void *arg);
	void thread_loop();
	void sample(ChiakiControllerState *state);
	// True when this state must go out now (buttons/triggers/touch) rather
	// than waiting for the motion cadence.
	bool is_discrete_change(const ChiakiControllerState &s) const;
	void update_rumble();

public:
	uint64_t sends() const { return sends_.load(std::memory_order_relaxed); }

private:

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

	// Send-decision state
	ChiakiControllerState last_sent_{};
	uint64_t last_send_ns_ = 0;
	bool have_last_sent_ = false;
	std::atomic<uint64_t> sends_{ 0 };

	core::Worker worker_;
	std::atomic<bool> stop_{ false };
	std::atomic<bool> quit_{ false };
	uint64_t quit_hold_start_ = 0;
};

} // namespace hayai::input
