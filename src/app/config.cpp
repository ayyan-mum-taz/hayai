// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// hayai.conf: a small hand-rolled INI. The trimmed vendor build has no json-c,
// and a config file this size does not justify a dependency.

#include "app/config.hpp"
#include "core/log.hpp"

#include <sys/stat.h>

#include <cstdio>
#include <cstring>

namespace hayai::app {

namespace {

void hex_encode(const uint8_t *in, size_t len, char *out)
{
	static const char *digits = "0123456789abcdef";
	for(size_t i = 0; i < len; i++)
	{
		out[i * 2] = digits[in[i] >> 4];
		out[i * 2 + 1] = digits[in[i] & 0xF];
	}
	out[len * 2] = '\0';
}

bool hex_decode(const char *in, uint8_t *out, size_t len)
{
	auto nib = [](char c) -> int {
		if(c >= '0' && c <= '9')
			return c - '0';
		if(c >= 'a' && c <= 'f')
			return c - 'a' + 10;
		if(c >= 'A' && c <= 'F')
			return c - 'A' + 10;
		return -1;
	};
	if(strlen(in) < len * 2)
		return false;
	for(size_t i = 0; i < len; i++)
	{
		const int hi = nib(in[i * 2]);
		const int lo = nib(in[i * 2 + 1]);
		if(hi < 0 || lo < 0)
			return false;
		out[i] = static_cast<uint8_t>((hi << 4) | lo);
	}
	return true;
}

char *trim(char *s)
{
	while(*s == ' ' || *s == '\t')
		s++;
	char *end = s + strlen(s);
	while(end > s && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t'))
		*--end = '\0';
	return s;
}

} // namespace

HostEntry *Config::find_host(const std::string &addr)
{
	for(auto &h : hosts)
	{
		if(h.addr == addr)
			return &h;
	}
	return nullptr;
}

void Config::upsert_host(const HostEntry &entry)
{
	if(HostEntry *existing = find_host(entry.addr))
		*existing = entry;
	else
		hosts.push_back(entry);
}

bool Config::load()
{
	FILE *f = fopen(kPath, "r");
	if(!f)
		return false;

	hosts.clear();
	HostEntry *host = nullptr;
	char line[512];

	while(fgets(line, sizeof(line), f))
	{
		char *s = trim(line);
		if(*s == '\0' || *s == '#')
			continue;

		if(*s == '[')
		{
			char *end = strchr(s, ']');
			if(!end)
				continue;
			*end = '\0';
			if(strncmp(s + 1, "host", 4) == 0)
			{
				hosts.emplace_back();
				host = &hosts.back();
			}
			else
				host = nullptr;	// [settings]
			continue;
		}

		char *eq = strchr(s, '=');
		if(!eq)
			continue;
		*eq = '\0';
		char *key = trim(s);
		char *val = trim(eq + 1);

		if(host)
		{
			if(strcmp(key, "nickname") == 0)
				host->nickname = val;
			else if(strcmp(key, "addr") == 0)
				host->addr = val;
			else if(strcmp(key, "server_mac") == 0)
				hex_decode(val, host->server_mac, 6);
			else if(strcmp(key, "rp_regist_key") == 0)
				host->registered |= hex_decode(val, reinterpret_cast<uint8_t *>(host->rp_regist_key), CHIAKI_SESSION_AUTH_SIZE);
			else if(strcmp(key, "rp_key") == 0)
				host->registered |= hex_decode(val, host->rp_key, 0x10);
		}
		else
		{
			if(strcmp(key, "resolution") == 0)
			{
				if(strcmp(val, "1080p") == 0)
					settings.resolution = Res::R1080;
				else if(strcmp(val, "540p") == 0)
					settings.resolution = Res::R540;
				else if(strcmp(val, "480p") == 0)
					settings.resolution = Res::R480;
				else if(strcmp(val, "360p") == 0)
					settings.resolution = Res::R360;
				else
					settings.resolution = Res::R720;
			}
			else if(strcmp(key, "fps") == 0)
				settings.fps = (strcmp(val, "30") == 0) ? CHIAKI_VIDEO_FPS_PRESET_30 : CHIAKI_VIDEO_FPS_PRESET_60;
			else if(strcmp(key, "bitrate") == 0)
				settings.bitrate = static_cast<uint32_t>(atoi(val));
			else if(strcmp(key, "profile") == 0)
			{
				if(strcmp(val, "smooth") == 0)
					settings.profile = Profile::Smooth;
				else if(strcmp(val, "quality") == 0)
					settings.profile = Profile::Quality;
				else
					settings.profile = Profile::Latency;
			}
			else if(strcmp(key, "controller_only") == 0)
				settings.controller_only = atoi(val) != 0;
			else if(strcmp(key, "backlight_off") == 0)
				settings.backlight_off = atoi(val) != 0;
			else if(strcmp(key, "pin_clocks") == 0)
				settings.pin_clocks = atoi(val) != 0;
			else if(strcmp(key, "psn_account_id") == 0)
				settings.psn_account_id_b64 = val;
		}
	}

	fclose(f);
	return true;
}

bool Config::save() const
{
	mkdir("/config", 0755);
	mkdir(kDir, 0755);

	FILE *f = fopen(kPath, "w");
	if(!f)
	{
		HAYAI_LOGE("config: cannot write %s", kPath);
		return false;
	}

	fprintf(f, "# hayai configuration. This file contains secrets - do not share it.\n\n");
	fprintf(f, "[settings]\n");
	fprintf(f, "resolution = %s\n", settings.res_name());
	fprintf(f, "fps = %u\n", static_cast<unsigned>(settings.fps));
	fprintf(f, "bitrate = %u\n", settings.bitrate);
	fprintf(f, "profile = %s\n",
		settings.profile == Profile::Smooth ? "smooth"
			: settings.profile == Profile::Quality ? "quality" : "latency");
	fprintf(f, "controller_only = %d\n", settings.controller_only ? 1 : 0);
	fprintf(f, "backlight_off = %d\n", settings.backlight_off ? 1 : 0);
	fprintf(f, "pin_clocks = %d\n", settings.pin_clocks ? 1 : 0);
	if(!settings.psn_account_id_b64.empty())
		fprintf(f, "psn_account_id = %s\n", settings.psn_account_id_b64.c_str());

	char hex[0x21];
	for(const auto &h : hosts)
	{
		fprintf(f, "\n[host]\n");
		fprintf(f, "nickname = %s\n", h.nickname.c_str());
		fprintf(f, "addr = %s\n", h.addr.c_str());
		hex_encode(h.server_mac, 6, hex);
		fprintf(f, "server_mac = %s\n", hex);
		if(h.registered)
		{
			hex_encode(reinterpret_cast<const uint8_t *>(h.rp_regist_key), CHIAKI_SESSION_AUTH_SIZE, hex);
			fprintf(f, "rp_regist_key = %s\n", hex);
			hex_encode(h.rp_key, 0x10, hex);
			fprintf(f, "rp_key = %s\n", hex);
		}
	}

	fclose(f);
	return true;
}

} // namespace hayai::app
