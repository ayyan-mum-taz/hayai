// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "app/ui.hpp"
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
	// Note: consoleExit does not restore stdout, so nothing may printf after
	// this point. The logger writes to a file/socket, never stdio.
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

// One row of the live console list.
struct MenuEntry
{
	enum class Kind
	{
		Registered,	// config host, streamable
		Discovered,	// on the network but not registered yet
		RegisterIp,
		Settings,
	};
	Kind kind;
	std::string label;
	std::string addr;	// for Registered/Discovered
	bool standby = false;
};

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
		// Rebuild the merged list every frame from config + live discovery,
		// the way the classic clients do it.
		auto discovered = discovery_.hosts();
		std::vector<MenuEntry> entries;

		for(const auto &h : config_.hosts)
		{
			MenuEntry e;
			e.kind = MenuEntry::Kind::Registered;
			e.addr = h.addr;
			const DiscoveredHost *live = nullptr;
			for(const auto &d : discovered)
			{
				if(d.addr == h.addr)
				{
					live = &d;
					break;
				}
			}
			const char *state = live ? (live->ready ? "ready" : "standby") : "not found";
			e.standby = live && !live->ready;
			e.label = h.nickname + "  " + h.addr + "  [" + state + "]";
			entries.push_back(std::move(e));
		}
		for(const auto &d : discovered)
		{
			if(config_.find_host(d.addr))
				continue;
			MenuEntry e;
			e.kind = MenuEntry::Kind::Discovered;
			e.addr = d.addr;
			e.standby = !d.ready;
			e.label = d.name + "  " + d.addr + "  [" + (d.ready ? "ready" : "standby") +
				(d.ps5 ? ", unregistered]" : ", not a PS5]");
			entries.push_back(std::move(e));
		}
		{
			MenuEntry e;
			e.kind = MenuEntry::Kind::RegisterIp;
			e.label = "Register by IP...";
			entries.push_back(std::move(e));
			e.kind = MenuEntry::Kind::Settings;
			e.label = "Settings";
			entries.push_back(std::move(e));
		}

		const int count = static_cast<int>(entries.size());
		if(cursor >= count)
			cursor = count - 1;

		consoleClear();
		printf("hayai 0.3.0 - latency-first PS5 remote play\n");
		if(appletGetAppletType() != AppletType_Application &&
			appletGetAppletType() != AppletType_SystemApplication)
			printf("!! applet mode: less memory, worse scheduling. Launch by holding R over a game.\n");
		printf("\nConsoles (searching continuously...):\n\n");

		for(int i = 0; i < count; i++)
		{
			const MenuEntry &e = entries[i];
			if(i > 0 && e.kind == MenuEntry::Kind::RegisterIp &&
				entries[i - 1].kind != MenuEntry::Kind::RegisterIp)
				printf("\n");
			printf(" %c %s\n", cursor == i ? '>' : ' ', e.label.c_str());
		}

		printf("\n A select (wakes standby consoles) | + exit\n");
		printf(" In-stream: hold L+R+MINUS ~1s to quit.\n");
		printf(" Log: /config/hayai/hayai.log\n");
		consoleUpdate(nullptr);

		const uint64_t down = poll_buttons();
		if(down & HidNpadButton_Plus)
			return MenuResult::Exit;
		if(down & HidNpadButton_AnyDown)
			cursor = (cursor + 1) % count;
		if(down & HidNpadButton_AnyUp)
			cursor = (cursor + count - 1) % count;
		if(down & HidNpadButton_A)
		{
			MenuEntry &e = entries[cursor];
			switch(e.kind)
			{
				case MenuEntry::Kind::Registered:
				{
					HostEntry *host = config_.find_host(e.addr);
					if(!host)
						break;
					if(e.standby)
					{
						if(!wake_and_wait(*host))
							break;
					}
					selected_ = *host;
					return MenuResult::Stream;
				}
				case MenuEntry::Kind::Discovered:
				{
					if(e.standby)
					{
						wait_any_button("Console must be fully on (not standby) to register.\nPress any button.");
						break;
					}
					selected_ = {};
					selected_.addr = e.addr;
					selected_.nickname = e.addr;
					register_host(selected_);
					break;
				}
				case MenuEntry::Kind::RegisterIp:
					register_menu();
					break;
				case MenuEntry::Kind::Settings:
					settings_menu();
					break;
			}
		}
		svcSleepThread(50'000'000ULL);	// 20 Hz menu; discovery updates at 2 Hz anyway
	}
	return MenuResult::Exit;
}

bool Ui::wake_and_wait(const HostEntry &entry)
{
	if(!Discovery::wakeup(entry.addr, entry.rp_regist_key))
	{
		wait_any_button("Failed to send wakeup. Press any button.");
		return false;
	}

	const int max_wait_s = 40;
	for(int elapsed_ds = 0; elapsed_ds < max_wait_s * 10 && appletMainLoop(); elapsed_ds++)
	{
		auto discovered = discovery_.hosts();
		for(const auto &d : discovered)
		{
			if(d.addr == entry.addr && d.ready)
				return true;
		}

		if(elapsed_ds % 5 == 0)
		{
			consoleClear();
			printf("Waking %s (%s)... %ds\n\n B cancel\n",
				entry.nickname.c_str(), entry.addr.c_str(), elapsed_ds / 10);
			consoleUpdate(nullptr);
		}
		if(poll_buttons() & HidNpadButton_B)
			return false;
		svcSleepThread(100'000'000ULL);
	}

	wait_any_button("Console did not wake in time. Press any button.");
	return false;
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
		if(!keyboard_input("PSN Account ID (base64, 12 chars)", "",
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
	if(!keyboard_input("PIN from PS5: Settings > System > Remote Play > Link Device", "",
			pin_str, sizeof(pin_str), true))
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
		wait_any_button("Registration failed (check PIN / account ID / console fully on).\nDetails: /config/hayai/hayai.log. Press any button.");
}

void Ui::settings_menu()
{
	Settings &s = config_.settings;
	int cursor = 0;
	constexpr int kItems = 6;

	while(appletMainLoop())
	{
		consoleClear();
		const char *res = s.res_name();
		printf("Settings\n\n");
		printf(" Session will request: %ux%u @ %u fps, %u kbps\n\n",
			s.width(), s.height(), static_cast<unsigned>(s.fps), s.default_bitrate_kbps());
		printf(" %c Resolution: %s%s\n", cursor == 0 ? '>' : ' ', res,
			s.resolution == Res::R480 ? "  (848x480, non-standard)" : "");
		printf(" %c FPS: %u\n", cursor == 1 ? '>' : ' ', static_cast<unsigned>(s.fps));
		printf(" %c Profile: %s\n", cursor == 2 ? '>' : ' ', s.profile_name());
		printf("     %s\n", s.profile == Profile::Latency
			? "immediate present, 2 buffers, console holds quality"
			: s.profile == Profile::Smooth
				? "vsync + 3 buffers, honest congestion reports so the console eases off"
				: "vsync + 3 buffers, console picks its highest bitrate");
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
					switch(s.resolution)
					{
						case Res::R360: s.resolution = Res::R480; break;
						case Res::R480: s.resolution = Res::R540; break;
						case Res::R540: s.resolution = Res::R720; break;
						case Res::R720: s.resolution = Res::R1080; break;
						default: s.resolution = Res::R360; break;
					}
					s.bitrate = 0;	// follow the new resolution's default
					break;
				case 1:
					s.fps = s.fps == CHIAKI_VIDEO_FPS_PRESET_60 ? CHIAKI_VIDEO_FPS_PRESET_30 : CHIAKI_VIDEO_FPS_PRESET_60;
					break;
				case 2:
					s.profile = s.profile == Profile::Latency ? Profile::Smooth
						: s.profile == Profile::Smooth ? Profile::Quality : Profile::Latency;
					s.apply_profile_defaults();
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
	discovery_.start();

	while(appletMainLoop())
	{
		console_begin();
		const MenuResult result = main_menu();
		console_end();

		if(result == MenuResult::Exit)
			break;

		// Quiet the radio and free the discovery port while streaming; the
		// console released the default framebuffer, the stream may take it.
		discovery_.stop();

		Stream stream;
		const Stream::EndReason reason = stream.run(selected_, config_.settings);

		discovery_.start();

		if(reason == Stream::EndReason::Error)
		{
			console_begin();
			wait_any_button("Session ended with an error (see /config/hayai/hayai.log).\nPress any button.");
			console_end();
		}
	}

	discovery_.stop();
}

} // namespace hayai::app
