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

struct Settings
{
	// Latency-first defaults; see docs/latency.md.
	ChiakiVideoResolutionPreset resolution = CHIAKI_VIDEO_RESOLUTION_PRESET_720p;
	ChiakiVideoFPSPreset fps = CHIAKI_VIDEO_FPS_PRESET_60;
	uint32_t bitrate = 0;		// 0 = preset default; explicit value caps it
	bool vsync = false;		// false = immediate present
	bool controller_only = false;
	bool backlight_off = false;	// controller-only extra
	bool pin_clocks = true;		// remove DVFS variance during stream
	// PSN account id, base64 (needed for registration only)
	std::string psn_account_id_b64;
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
