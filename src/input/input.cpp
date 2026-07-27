// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "input/input.hpp"
#include "core/log.hpp"
#include "util/time.hpp"

#include <cmath>
#include <cstring>

namespace hayai::input {

namespace {

// PS touchpad coordinate space (DualShock 4 / DualSense are the same).
constexpr float kTouchpadMaxX = 1920.0f;
constexpr float kTouchpadMaxY = 942.0f;
constexpr float kScreenMaxX = 1280.0f;
constexpr float kScreenMaxY = 720.0f;

struct ButtonMap
{
	uint64_t sw;
	uint32_t ps;
};

// B/A and Y/X are crossed on purpose: positionally, Switch B is where PS
// Cross is. Positional mapping is what streaming players expect.
constexpr ButtonMap kButtonMap[] = {
	{ HidNpadButton_B, CHIAKI_CONTROLLER_BUTTON_CROSS },
	{ HidNpadButton_A, CHIAKI_CONTROLLER_BUTTON_MOON },
	{ HidNpadButton_Y, CHIAKI_CONTROLLER_BUTTON_BOX },
	{ HidNpadButton_X, CHIAKI_CONTROLLER_BUTTON_PYRAMID },
	{ HidNpadButton_Left, CHIAKI_CONTROLLER_BUTTON_DPAD_LEFT },
	{ HidNpadButton_Right, CHIAKI_CONTROLLER_BUTTON_DPAD_RIGHT },
	{ HidNpadButton_Up, CHIAKI_CONTROLLER_BUTTON_DPAD_UP },
	{ HidNpadButton_Down, CHIAKI_CONTROLLER_BUTTON_DPAD_DOWN },
	{ HidNpadButton_L, CHIAKI_CONTROLLER_BUTTON_L1 },
	{ HidNpadButton_R, CHIAKI_CONTROLLER_BUTTON_R1 },
	{ HidNpadButton_StickL, CHIAKI_CONTROLLER_BUTTON_L3 },
	{ HidNpadButton_StickR, CHIAKI_CONTROLLER_BUTTON_R3 },
	{ HidNpadButton_Plus, CHIAKI_CONTROLLER_BUTTON_OPTIONS },
	{ HidNpadButton_Minus, CHIAKI_CONTROLLER_BUTTON_SHARE },
};

inline int16_t stick_to_ps(int32_t v)
{
	// libnx sticks are already s16 range in AnalogStickState (-32768..32767).
	if(v > INT16_MAX)
		v = INT16_MAX;
	if(v < INT16_MIN)
		v = INT16_MIN;
	return static_cast<int16_t>(v);
}

} // namespace

bool Sampler::start(ChiakiSession *session)
{
	session_ = session;
	stop_.store(false, std::memory_order_relaxed);
	quit_.store(false, std::memory_order_relaxed);

	padInitializeDefault(&pad_);

	hidGetSixAxisSensorHandles(&sixaxis_handles_[0], 1, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld);
	hidGetSixAxisSensorHandles(&sixaxis_handles_[1], 1, HidNpadIdType_No1, HidNpadStyleTag_NpadFullKey);
	hidGetSixAxisSensorHandles(&sixaxis_handles_[2], 2, HidNpadIdType_No1, HidNpadStyleTag_NpadJoyDual);
	for(auto &h : sixaxis_handles_)
		hidStartSixAxisSensor(h);

	hidInitializeTouchScreen();
	chiaki_orientation_tracker_init(&orient_tracker_);
	chiaki_accel_new_zero_set_inactive(&accel_zero_, false);

	memset(touch_map_, -1, sizeof(touch_map_));
	memset(touch_finger_, 0xFF, sizeof(touch_finger_));

	// Core 0, not core 2. This thread wakes 500 times a second and does real
	// work each time (motion filter, touch mapping), and at equal priority it
	// was round-robining with the audio pipeline on the same core -- audio only
	// has a few tens of milliseconds of hardware runway before it starves.
	// Core 0 carries only the idle stream-wait and vsync watcher during play.
	return worker_.start(&Sampler::thread_entry, this, core::kPrioHot, core::kCoreMain, 0x10000);
}

void Sampler::stop()
{
	stop_.store(true, std::memory_order_relaxed);
	worker_.join();
	for(auto &h : sixaxis_handles_)
		hidStopSixAxisSensor(h);
}

void Sampler::set_rumble(uint8_t left, uint8_t right)
{
	rumble_.store(static_cast<uint16_t>((left << 8) | right), std::memory_order_relaxed);
}

void Sampler::thread_entry(void *arg)
{
	static_cast<Sampler *>(arg)->thread_loop();
}

void Sampler::thread_loop()
{
	HAYAI_LOGI("input: sampler up (%llu us cadence)",
		static_cast<unsigned long long>(kCadenceNs / 1000));

	uint64_t next = now_ns();
	ChiakiControllerState state;

	while(!stop_.load(std::memory_order_relaxed))
	{
		chiaki_controller_state_set_idle(&state);
		sample(&state);

		// Decide whether this sample is worth a packet. Sampling stays at 2 ms
		// so whatever we do send is fresh; only the transmit rate is bounded.
		const uint64_t t = now_ns();
		const uint64_t since_send = t - last_send_ns_;
		bool send = false;
		bool discrete = false;
		if(!have_last_sent_)
			send = true;
		else if(is_discrete_change(state))
		{
			// Buttons, triggers and touch: send at the full sampling rate. A
			// human cannot generate these faster than a few dozen per second,
			// so they cost nothing, and they are the events a player feels.
			send = true;
			discrete = true;
		}
		else
		{
			const int dlx = static_cast<int>(state.left_x) - last_sent_.left_x;
			const int dly = static_cast<int>(state.left_y) - last_sent_.left_y;
			const int drx = static_cast<int>(state.right_x) - last_sent_.right_x;
			const int dry = static_cast<int>(state.right_y) - last_sent_.right_y;
			const bool stick_moved =
				dlx > kStickSendThreshold || dlx < -kStickSendThreshold ||
				dly > kStickSendThreshold || dly < -kStickSendThreshold ||
				drx > kStickSendThreshold || drx < -kStickSendThreshold ||
				dry > kStickSendThreshold || dry < -kStickSendThreshold;
			if(stick_moved || since_send >= kMotionIntervalNs)
				send = true;
		}
		// Ceiling for continuous signals only -- analog noise and motion can
		// flood, discrete presses cannot.
		if(send && !discrete && have_last_sent_ && since_send < kMinSendGapNs)
			send = false;

		if(send)
		{
			chiaki_session_set_controller_state(session_, &state);
			last_sent_ = state;
			last_send_ns_ = t;
			have_last_sent_ = true;
			sends_.fetch_add(1, std::memory_order_relaxed);
		}
		update_rumble();

		// Absolute-time cadence: no drift, no ms-granular cond quantization.
		next += kCadenceNs;
		const uint64_t now = now_ns();
		if(next > now)
			svcSleepThread(next - now);
		else
			next = now;	// fell behind; don't burst
	}

	HAYAI_LOGI("input: sampler down (%llu packets sent)",
		static_cast<unsigned long long>(sends_.load(std::memory_order_relaxed)));
}

// Everything a player perceives as "did my press register": digital buttons,
// analog triggers, touch. Deliberately excludes motion, which changes on every
// sample from sensor noise alone.
bool Sampler::is_discrete_change(const ChiakiControllerState &s) const
{
	if(s.buttons != last_sent_.buttons)
		return true;
	if(s.l2_state != last_sent_.l2_state || s.r2_state != last_sent_.r2_state)
		return true;
	for(int i = 0; i < CHIAKI_CONTROLLER_TOUCHES_MAX; i++)
	{
		if(s.touches[i].id != last_sent_.touches[i].id ||
			s.touches[i].x != last_sent_.touches[i].x ||
			s.touches[i].y != last_sent_.touches[i].y)
			return true;
	}
	return false;
}

void Sampler::sample(ChiakiControllerState *state)
{
	padUpdate(&pad_);

	const uint64_t buttons = padGetButtons(&pad_);
	for(const auto &m : kButtonMap)
	{
		if(buttons & m.sw)
			state->buttons |= m.ps;
	}

	// Digital triggers -> full-scale analog L2/R2.
	if(buttons & HidNpadButton_ZL)
	{
		state->buttons |= CHIAKI_CONTROLLER_ANALOG_BUTTON_L2;
		state->l2_state = 0xFF;
	}
	if(buttons & HidNpadButton_ZR)
	{
		state->buttons |= CHIAKI_CONTROLLER_ANALOG_BUTTON_R2;
		state->r2_state = 0xFF;
	}

	const HidAnalogStickState ls = padGetStickPos(&pad_, 0);
	const HidAnalogStickState rs = padGetStickPos(&pad_, 1);
	state->left_x = stick_to_ps(ls.x);
	state->left_y = stick_to_ps(-ls.y);	// PS Y grows downward
	state->right_x = stick_to_ps(rs.x);
	state->right_y = stick_to_ps(-rs.y);

	// Quit chord: L+R+Minus held for a second.
	constexpr uint64_t chord = HidNpadButton_L | HidNpadButton_R | HidNpadButton_Minus;
	if((buttons & chord) == chord)
	{
		const uint64_t now = now_ns();
		if(!quit_hold_start_)
			quit_hold_start_ = now;
		else if(now - quit_hold_start_ >= kQuitHoldNs)
			quit_.store(true, std::memory_order_relaxed);
		// While the chord is held, don't leak SHARE/L1/R1 to the console.
		state->buttons &= ~(CHIAKI_CONTROLLER_BUTTON_SHARE | CHIAKI_CONTROLLER_BUTTON_L1 | CHIAKI_CONTROLLER_BUTTON_R1);
	}
	else
		quit_hold_start_ = 0;

	// --- Motion (axis mapping follows the working Switch port) ---
	HidSixAxisSensorState sixaxis{};
	const uint64_t style = padGetStyleSet(&pad_);
	if(style & HidNpadStyleTag_NpadHandheld)
		hidGetSixAxisSensorStates(sixaxis_handles_[0], &sixaxis, 1);
	else if(style & HidNpadStyleTag_NpadFullKey)
		hidGetSixAxisSensorStates(sixaxis_handles_[1], &sixaxis, 1);
	else if(style & HidNpadStyleTag_NpadJoyDual)
	{
		const u64 attrib = padGetAttributes(&pad_);
		if(attrib & HidNpadAttribute_IsLeftConnected)
			hidGetSixAxisSensorStates(sixaxis_handles_[2], &sixaxis, 1);
		else if(attrib & HidNpadAttribute_IsRightConnected)
			hidGetSixAxisSensorStates(sixaxis_handles_[3], &sixaxis, 1);
	}

	const float gx = sixaxis.angular_velocity.x * 2.0f * static_cast<float>(M_PI);
	const float gy = sixaxis.angular_velocity.z * 2.0f * static_cast<float>(M_PI);
	const float gz = -sixaxis.angular_velocity.y * 2.0f * static_cast<float>(M_PI);
	const float ax = -sixaxis.acceleration.x;
	const float ay = -sixaxis.acceleration.z;
	const float az = sixaxis.acceleration.y;

	chiaki_orientation_tracker_update(&orient_tracker_, gx, gy, gz, ax, ay, az,
		&accel_zero_, false, static_cast<uint32_t>(now_ns() / 1000));
	chiaki_orientation_tracker_apply_to_controller_state(&orient_tracker_, state);

	// --- Touchscreen -> touchpad ---
	HidTouchScreenState touch{};
	hidGetTouchScreenStates(&touch, 1);

	// Release fingers that lifted.
	for(unsigned i = 0; i < 16; i++)
	{
		if(touch_map_[i] < 0)
			continue;
		bool still_down = false;
		for(int t = 0; t < touch.count; t++)
		{
			if(touch.touches[t].finger_id == touch_finger_[i])
			{
				still_down = true;
				break;
			}
		}
		if(!still_down)
		{
			chiaki_controller_state_stop_touch(state, static_cast<uint8_t>(touch_map_[i]));
			touch_map_[i] = -1;
			touch_finger_[i] = 0xFFFFFFFF;
		}
	}

	for(int t = 0; t < touch.count && t < 2; t++)
	{
		const uint16_t x = static_cast<uint16_t>(touch.touches[t].x * (kTouchpadMaxX / kScreenMaxX));
		const uint16_t y = static_cast<uint16_t>(touch.touches[t].y * (kTouchpadMaxY / kScreenMaxY));

		int slot = -1;
		for(unsigned i = 0; i < 16; i++)
		{
			if(touch_map_[i] >= 0 && touch_finger_[i] == touch.touches[t].finger_id)
			{
				slot = static_cast<int>(i);
				break;
			}
		}
		if(slot >= 0)
			chiaki_controller_state_set_touch_pos(state, static_cast<uint8_t>(touch_map_[slot]), x, y);
		else
		{
			for(unsigned i = 0; i < 16; i++)
			{
				if(touch_map_[i] < 0)
				{
					touch_map_[i] = chiaki_controller_state_start_touch(state, x, y);
					touch_finger_[i] = touch.touches[t].finger_id;
					break;
				}
			}
		}

		// Screen edges act as the physical touchpad click.
		if(x <= kTouchpadMaxX * 0.05f || x >= kTouchpadMaxX * 0.95f ||
			y <= kTouchpadMaxY * 0.05f || y >= kTouchpadMaxY * 0.95f)
			state->buttons |= CHIAKI_CONTROLLER_BUTTON_TOUCHPAD;
	}
}

void Sampler::update_rumble()
{
	const uint16_t want = rumble_.load(std::memory_order_relaxed);
	if(want == rumble_applied_)
		return;
	rumble_applied_ = want;

	const float left = static_cast<float>(want >> 8) / 255.0f;
	const float right = static_cast<float>(want & 0xFF) / 255.0f;

	HidVibrationValue value{};
	value.freq_low = 160.0f;
	value.freq_high = 320.0f;
	value.amp_low = left;
	value.amp_high = right;

	HidVibrationValue values[2] = { value, value };
	static HidVibrationDeviceHandle handles[2];
	static bool handles_init = false;
	if(!handles_init)
	{
		if(R_FAILED(hidInitializeVibrationDevices(handles, 2, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld)))
			hidInitializeVibrationDevices(handles, 2, HidNpadIdType_No1, HidNpadStyleTag_NpadFullKey);
		handles_init = true;
	}
	hidSendVibrationValues(handles, values, 2);
}

} // namespace hayai::input
