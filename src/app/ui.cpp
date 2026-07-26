// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "app/ui.hpp"
#include "app/discovery.hpp"
#include "app/stream.hpp"
#include "core/log.hpp"

#include <chiaki/base64.h>
#include <chiaki/regist.h>

#include <switch.h>

#include <atomic>
#include <cstdio>
#include <cstring>

namespace hayai::app {

namespace {

PadState g_pad;

void console_begin()
{
	consoleInit(nullptr);
	padConfigureInput(1, HidNpadStyleSet_NpadStandard);
	padInitializeDefault(&g_pad);
}

void console_end()
{
	consoleExit(nullptr);
}

uint64_t poll_buttons()
{
	padUpdate(&g_pad);
	return padGetButtonsDown(&g_pad);
}

// Blocking software-keyboard text input. Returns false if canceled/empty.
bool keyboard_input(const char *guide, const char *initial, char *out, size_t out_size, bool numeric)
{
	SwkbdConfig kbd;
	if(R_FAILED(swkbdCreate(&kbd, 0)))
		return false;
	swkbdConfigMakePresetDefault(&kbd);
	swkbdConfigSetGuideText(&kbd, guide);
	if(initial && *initial)
		swkbdConfigSetInitialText(&kbd, initial);
	if(numeric)
		swkbdConfigSetType(&kbd, SwkbdType_NumPad);
	swkbdConfigSetStringLenMax(&kbd, static_cast<u32>(out_size - 1));

	out[0] = '\0';
	const Result rc = swkbdShow(&kbd, out, out_size);
	swkbdClose(&kbd);
	return R_SUCCEEDED(rc) && out[0] != '\0';
}

struct RegistWait
{
	std::atomic<int> done{ 0 };	// 0 pending, 1 success, -1 failed
	ChiakiRegisteredHost host{};
};

void regist_cb(ChiakiRegistEvent *event, void *user)
{
	auto *wait = static_cast<RegistWait *>(user);
	if(event->type == CHIAKI_REGIST_EVENT_TYPE_FINISHED_SUCCESS && event->registered_host)
	{
		wait->host = *event->registered_host;
		wait->done.store(1, std::memory_order_release);
	}
	else
		wait->done.store(-1, std::memory_order_release);
}

} // namespace

void Ui::wait_any_button(const char *prompt)
{
	printf("\n%s\n", prompt);
	consoleUpdate(nullptr);
	while(appletMainLoop())
	{
		if(poll_buttons())
			break;
		consoleUpdate(nullptr);
		svcSleepThread(16'000'000ULL);
	}
}

Ui::MenuResult Ui::main_menu()
{
	int cursor = 0;
	while(appletMainLoop())
	{
		const int host_count = static_cast<int>(config_.hosts.size());
		const int item_count = host_count + 3;	// hosts + discover/register + settings

		consoleClear();
		printf("hayai 0.1.0-dev - latency-first PS5 remote play\n");
		if(appletGetAppletType() != AppletType_Application &&
			appletGetAppletType() != AppletType_SystemApplication)
			printf("!! applet mode: less memory, worse scheduling. Launch via title takeover.\n");
		printf("\n");

		for(int i = 0; i < host_count; i++)
		{
			const HostEntry &h = config_.hosts[i];
			printf(" %c Stream %s (%s)%s\n", cursor == i ? '>' : ' ',
				h.nickname.c_str(), h.addr.c_str(), h.registered ? "" : " [unregistered]");
		}
		printf(" %c Discover & register consoles\n", cursor == host_count ? '>' : ' ');
		printf(" %c Register by IP\n", cursor == host_count + 1 ? '>' : ' ');
		printf(" %c Settings\n", cursor == host_count + 2 ? '>' : ' ');
		printf("\n A select | + exit\n");
		printf("\n In-stream: hold L+R+MINUS ~1s to quit the stream.\n");
		consoleUpdate(nullptr);

		const uint64_t down = poll_buttons();
		if(down & HidNpadButton_Plus)
			return MenuResult::Exit;
		if(down & HidNpadButton_AnyDown)
			cursor = (cursor + 1) % item_count;
		if(down & HidNpadButton_AnyUp)
			cursor = (cursor + item_count - 1) % item_count;
		if(down & HidNpadButton_A)
		{
			if(cursor < host_count)
			{
				if(!config_.hosts[cursor].registered)
				{
					wait_any_button("Host is not registered yet - use one of the register options.");
					continue;
				}
				selected_ = config_.hosts[cursor];
				return MenuResult::Stream;
			}
			if(cursor == host_count)
				discover_menu();
			else if(cursor == host_count + 1)
				register_menu();
			else
				settings_menu();
		}
		svcSleepThread(16'000'000ULL);
	}
	return MenuResult::Exit;
}

void Ui::discover_menu()
{
	Discovery discovery;
	if(!discovery.start())
	{
		wait_any_button("Discovery failed to start (network up?). Press any button.");
		return;
	}

	int cursor = 0;
	while(appletMainLoop())
	{
		auto hosts = discovery.hosts();
		const int count = static_cast<int>(hosts.size());
		if(cursor >= count)
			cursor = count ? count - 1 : 0;

		consoleClear();
		printf("Discovering consoles on the local network...\n\n");
		for(int i = 0; i < count; i++)
		{
			printf(" %c %s  %s  [%s]%s\n", cursor == i ? '>' : ' ',
				hosts[i].name.c_str(), hosts[i].addr.c_str(),
				hosts[i].ready ? "ready" : "standby",
				hosts[i].ps5 ? "" : " (not a PS5)");
		}
		if(!count)
			printf(" (nothing yet)\n");
		printf("\n A register selected | Y wake selected | B back\n");
		consoleUpdate(nullptr);

		const uint64_t down = poll_buttons();
		if(down & HidNpadButton_B)
			break;
		if(down & HidNpadButton_AnyDown && count)
			cursor = (cursor + 1) % count;
		if(down & HidNpadButton_AnyUp && count)
			cursor = (cursor + count - 1) % count;
		if((down & HidNpadButton_Y) && count)
		{
			if(HostEntry *known = config_.find_host(hosts[cursor].addr); known && known->registered)
				Discovery::wakeup(known->addr, known->rp_regist_key);
			else
				wait_any_button("Can only wake registered hosts. Press any button.");
		}
		if((down & HidNpadButton_A) && count)
		{
			const std::string addr = hosts[cursor].addr;
			const std::string name = hosts[cursor].name;
			discovery.stop();
			selected_ = {};
			selected_.addr = addr;
			selected_.nickname = name;
			register_host(selected_);
			return;
		}
		svcSleepThread(100'000'000ULL);
	}
	discovery.stop();
}

void Ui::register_menu()
{
	char addr[64];
	if(!keyboard_input("Console IP address (e.g. 192.168.1.50)", "", addr, sizeof(addr), false))
		return;
	selected_ = {};
	selected_.addr = addr;
	selected_.nickname = addr;
	register_host(selected_);
}

void Ui::register_host(HostEntry &entry)
{
	// PSN account ID (base64, 8 bytes). Cached in settings after first use.
	char account_b64[32];
	if(config_.settings.psn_account_id_b64.empty())
	{
		if(!keyboard_input("PSN Account ID (base64; see psn.flipscreen.games)", "",
				account_b64, sizeof(account_b64), false))
			return;
		config_.settings.psn_account_id_b64 = account_b64;
	}
	else
		snprintf(account_b64, sizeof(account_b64), "%s", config_.settings.psn_account_id_b64.c_str());

	uint8_t account_id[CHIAKI_PSN_ACCOUNT_ID_SIZE];
	size_t account_id_size = sizeof(account_id);
	if(chiaki_base64_decode(account_b64, strlen(account_b64), account_id, &account_id_size) != CHIAKI_ERR_SUCCESS ||
		account_id_size != CHIAKI_PSN_ACCOUNT_ID_SIZE)
	{
		config_.settings.psn_account_id_b64.clear();
		wait_any_button("Invalid account ID (must be 8 bytes of base64). Press any button.");
		return;
	}

	// Console PIN from Settings > System > Remote Play > Link Device.
	char pin_str[16];
	if(!keyboard_input("Remote Play link PIN (8 digits)", "", pin_str, sizeof(pin_str), true))
		return;

	ChiakiRegistInfo info{};
	info.target = CHIAKI_TARGET_PS5_1;
	info.host = entry.addr.c_str();
	info.broadcast = false;
	info.psn_online_id = nullptr;
	memcpy(info.psn_account_id, account_id, sizeof(account_id));
	info.pin = static_cast<uint32_t>(strtoul(pin_str, nullptr, 10));

	RegistWait wait;
	ChiakiRegist regist;
	if(chiaki_regist_start(&regist, core::log().chiaki_log(), &info, &regist_cb, &wait) != CHIAKI_ERR_SUCCESS)
	{
		wait_any_button("Failed to start registration. Press any button.");
		return;
	}

	consoleClear();
	printf("Registering with %s ...\n", entry.addr.c_str());
	consoleUpdate(nullptr);

	while(wait.done.load(std::memory_order_acquire) == 0 && appletMainLoop())
	{
		consoleUpdate(nullptr);
		svcSleepThread(50'000'000ULL);
	}
	chiaki_regist_stop(&regist);
	chiaki_regist_fini(&regist);

	if(wait.done.load(std::memory_order_acquire) == 1)
	{
		entry.nickname = wait.host.server_nickname;
		memcpy(entry.server_mac, wait.host.server_mac, 6);
		memcpy(entry.rp_regist_key, wait.host.rp_regist_key, CHIAKI_SESSION_AUTH_SIZE);
		memcpy(entry.rp_key, wait.host.rp_key, 0x10);
		entry.registered = true;
		config_.upsert_host(entry);
		config_.save();
		wait_any_button("Registered! Press any button.");
	}
	else
		wait_any_button("Registration failed (check PIN / account ID / console state). Press any button.");
}

void Ui::settings_menu()
{
	Settings &s = config_.settings;
	int cursor = 0;
	constexpr int kItems = 6;

	while(appletMainLoop())
	{
		consoleClear();
		const char *res = s.resolution == CHIAKI_VIDEO_RESOLUTION_PRESET_1080p ? "1080p"
			: s.resolution == CHIAKI_VIDEO_RESOLUTION_PRESET_540p ? "540p"
			: s.resolution == CHIAKI_VIDEO_RESOLUTION_PRESET_360p ? "360p" : "720p";
		printf("Settings (latency-first defaults)\n\n");
		printf(" %c Resolution: %s\n", cursor == 0 ? '>' : ' ', res);
		printf(" %c FPS: %u\n", cursor == 1 ? '>' : ' ', static_cast<unsigned>(s.fps));
		printf(" %c Present: %s\n", cursor == 2 ? '>' : ' ', s.vsync ? "vsync (tear-free, +latency)" : "immediate (lowest latency)");
		printf(" %c Controller-only mode: %s\n", cursor == 3 ? '>' : ' ', s.controller_only ? "on" : "off");
		printf(" %c   Backlight off while streaming: %s\n", cursor == 4 ? '>' : ' ', s.backlight_off ? "on" : "off");
		printf(" %c Pin clocks during stream: %s\n", cursor == 5 ? '>' : ' ', s.pin_clocks ? "on" : "off");
		printf("\n A change | B back (saves)\n");
		consoleUpdate(nullptr);

		const uint64_t down = poll_buttons();
		if(down & HidNpadButton_B)
			break;
		if(down & HidNpadButton_AnyDown)
			cursor = (cursor + 1) % kItems;
		if(down & HidNpadButton_AnyUp)
			cursor = (cursor + kItems - 1) % kItems;
		if(down & HidNpadButton_A)
		{
			switch(cursor)
			{
				case 0:
					s.resolution = s.resolution == CHIAKI_VIDEO_RESOLUTION_PRESET_720p
						? CHIAKI_VIDEO_RESOLUTION_PRESET_1080p
						: s.resolution == CHIAKI_VIDEO_RESOLUTION_PRESET_1080p
							? CHIAKI_VIDEO_RESOLUTION_PRESET_540p
							: CHIAKI_VIDEO_RESOLUTION_PRESET_720p;
					break;
				case 1:
					s.fps = s.fps == CHIAKI_VIDEO_FPS_PRESET_60 ? CHIAKI_VIDEO_FPS_PRESET_30 : CHIAKI_VIDEO_FPS_PRESET_60;
					break;
				case 2:
					s.vsync = !s.vsync;
					break;
				case 3:
					s.controller_only = !s.controller_only;
					break;
				case 4:
					s.backlight_off = !s.backlight_off;
					break;
				case 5:
					s.pin_clocks = !s.pin_clocks;
					break;
			}
		}
		svcSleepThread(16'000'000ULL);
	}
	config_.save();
}

void Ui::run()
{
	while(appletMainLoop())
	{
		console_begin();
		const MenuResult result = main_menu();
		console_end();

		if(result == MenuResult::Exit)
			break;

		// The console released the default framebuffer; the stream may take it.
		Stream stream;
		const Stream::EndReason reason = stream.run(selected_, config_.settings);

		if(reason == Stream::EndReason::Error)
		{
			console_begin();
			wait_any_button("Session ended with an error (see nxlink log). Press any button.");
			console_end();
		}
	}
}

} // namespace hayai::app
