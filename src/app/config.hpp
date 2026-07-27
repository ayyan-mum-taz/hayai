// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
#pragma once

#include <chiaki/session.h>

#include <cstdint>
#include <string>
#include <vector>

namespace hayai::app {

struct HostEntry
{
	std::string nickname;
	std::string addr;
	uint8_t server_mac[6]{};
	// Registration material (chiaki regist output)
	char rp_regist_key[CHIAKI_SESSION_AUTH_SIZE]{};	// \0-padded
	uint8_t rp_key[0x10]{};
	bool registered = false;
};

// How the client trades latency against steadiness.
//
// The console, not the client, owns bitrate during a session: it ships several
// encode profiles and switches between them based on the congestion feedback we
// send. So "adaptive" here does not mean we re-encode anything -- it means we
// stop lying to the console about how bad the link is, and we give the display
// pipeline enough slack to ride out a hiccup.
enum class Profile
{
	// Everything this project was built for. Immediate present, two swapchain
	// images, congestion under-reported so the console holds quality.
	Latency,
	// Handheld-friendly. Vsync-paced so motion is evenly spaced, with truthful
	// loss reporting so the console drops its own bitrate when the radio is
	// struggling instead of hammering a link that cannot carry it.
	Smooth,
	// Highest fidelity the link will bear; assumes a good network.
	Quality,
};

// Stream resolutions, all of them shapes the console advertises natively.
// A custom 848x480 was tried and removed: non-standard geometry is a plausible
// cause of the console declining to send video at all.
enum class Res
{
	R360,
	R540,
	R720,
	R1080,
};

struct Settings
{
	Profile profile = Profile::Latency;

	// Latency-first defaults; see docs/latency.md.
	Res resolution = Res::R720;
	ChiakiVideoFPSPreset fps = CHIAKI_VIDEO_FPS_PRESET_60;
	uint32_t bitrate = 0;		// 0 = profile default; explicit value overrides
	bool controller_only = false;
	bool backlight_off = false;	// controller-only extra
	bool pin_clocks = true;		// remove DVFS variance during stream
	// PSN account id, base64 (needed for registration only)
	std::string psn_account_id_b64;

	// --- derived from profile ---
	bool vsync() const { return profile != Profile::Latency; }

	// Latency: two images, immediate present -- acquire never blocks, frames go
	// out the moment they exist. Smooth/Quality: three, and this is measured,
	// not theorised. We shipped vsync with two images once: the present thread
	// then blocks until vblank, so each frame's on-screen fate depends on its
	// arrival phase against the panel clock, and since the console's encoder
	// clock free-runs against the panel, that phase drifts through the beat
	// cycle -- telemetry showed present times oscillating 2 ms -> 11 ms -> 2 ms
	// over the minutes, which the eye reads as periodic repeat/skip judder.
	// The third image is the shock absorber for that beat: it costs up to one
	// frame of queueing and buys even pacing, which is exactly Smooth's trade.
	unsigned swapchain_images() const { return profile == Profile::Latency ? 2 : 3; }
	// Ceiling on the packet loss we report. libchiaki clamps to this, so a low
	// value hides congestion from the console; Smooth reports it honestly.
	double packet_loss_max() const
	{
		switch(profile)
		{
			case Profile::Smooth: return 0.30;
			case Profile::Quality: return 0.05;
			default: return 0.10;
		}
	}
	// Base bitrate for the chosen resolution, before profile adjustment.
	uint32_t res_bitrate_kbps() const
	{
		switch(resolution)
		{
			case Res::R360: return 2000;
			case Res::R540: return 6000;
			case Res::R1080: return 15000;
			default: return 10000;
		}
	}

	// 30 fps only pays off if the bitrate comes down with it.
	//
	// At a fixed bitrate, halving the frame rate doubles the bytes in every
	// frame: each frame then takes twice as long to serialize and carries twice
	// as many packets, so it is twice as likely to lose one -- and a loss now
	// costs a 33 ms hole instead of 16 ms. That is why 30 fps "does nothing".
	// Halving the bitrate alongside it keeps frames exactly the same size as at
	// 60, while the radio carries half as many per second. That is the real
	// consistency win: same per-frame risk, half the airtime, half the chances
	// to lose anything. The cost is honest and bounded -- one extra frame
	// interval of sampling latency, and less fluid motion.
	uint32_t default_bitrate_kbps() const
	{
		if(bitrate)
			return bitrate;
		uint32_t base = res_bitrate_kbps();
		if(fps == CHIAKI_VIDEO_FPS_PRESET_30)
			base /= 2;
		switch(profile)
		{
			// Deliberate headroom below what the radio can carry. Smaller
			// frames serialize faster, so the last packet of each frame -- the
			// one that gates decode -- arrives sooner, and there is slack left
			// for retransmits when the link wobbles.
			// Three quarters, not two thirds: a chunk of the headroom that
			// margin was protecting existed only because our own input flood
			// was stealing Wi-Fi airtime from the video coming back.
			case Profile::Smooth: return base * 2 / 3;
			case Profile::Quality: return base;
			default: return base;
		}
	}

	unsigned width() const
	{
		switch(resolution)
		{
			case Res::R360: return 640;
			case Res::R540: return 960;
			case Res::R1080: return 1920;
			default: return 1280;
		}
	}
	unsigned height() const
	{
		switch(resolution)
		{
			case Res::R360: return 360;
			case Res::R540: return 540;
			case Res::R1080: return 1080;
			default: return 720;
		}
	}
	const char *res_name() const
	{
		switch(resolution)
		{
			case Res::R360: return "360p";
			case Res::R540: return "540p";
			case Res::R1080: return "1080p";
			default: return "720p";
		}
	}

	// Head-of-line wait for out-of-order AV packets. Latency keeps it tight;
	// Smooth sets it near real Wi-Fi jitter so late packets are used rather
	// than mistaken for loss (an FEC failure plus IDR round trip is far more
	// visible than a couple of milliseconds of wait).
	uint64_t reorder_timeout_us() const
	{
		switch(profile)
		{
			case Profile::Smooth: return 10000;
			case Profile::Quality: return 6000;
			default: return 2500;
		}
	}
	const char *profile_name() const
	{
		switch(profile)
		{
			case Profile::Smooth: return "Smooth (adaptive)";
			case Profile::Quality: return "Quality";
			default: return "Latency";
		}
	}

	// Selecting a profile resets the stream shape to something coherent.
	// Leaving stale resolution/fps behind is how a session silently ends up at
	// 540p30 while the user believes they are on 720p60.
	void apply_profile_defaults()
	{
		fps = CHIAKI_VIDEO_FPS_PRESET_60;
		bitrate = 0;
		switch(profile)
		{
			// Smooth targets the handheld panel honestly: 540p upscaled to 720p
			// costs little visible detail at arm's length and roughly halves
			// the pixels the radio has to carry, which is where smoothness
			// actually comes from.
			case Profile::Smooth: resolution = Res::R540; break;
			case Profile::Quality: resolution = Res::R1080; break;
			default: resolution = Res::R720; break;
		}
	}
};

class Config
{
public:
	static constexpr const char *kDir = "/config/hayai";
	static constexpr const char *kPath = "/config/hayai/hayai.conf";

	bool load();
	bool save() const;

	Settings settings;
	std::vector<HostEntry> hosts;

	HostEntry *find_host(const std::string &addr);
	void upsert_host(const HostEntry &entry);
};

} // namespace hayai::app
