// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// The frontend, rendered with deko3d through ui::Draw -- no text console and no
// UI toolkit. It exists to start a session in as few presses as possible and
// then get out of the way, so it is one live list of consoles plus a settings
// screen.

#include "app/ui.hpp"
#include "app/stream.hpp"
#include "core/log.hpp"
#include "core/telemetry.hpp"
#include "ui/draw.hpp"
#include "util/time.hpp"

#include <chiaki/base64.h>
#include <chiaki/regist.h>

#include <switch.h>

#include <atomic>
#include <cstdio>
#include <cstring>

namespace hayai::app {

using ui::Color;
using ui::Font;
namespace theme = ui::theme;

namespace {

PadState g_pad;

float lerp(float a, float b, float t)
{
	return a + (b - a) * t;
}

// Blocking software-keyboard input. Returns false if canceled or empty.
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

struct Row
{
	enum class Kind
	{
		Registered,
		Discovered,
		RegisterIp,
		Settings,
	};
	Kind kind = Kind::Registered;
	std::string title;
	std::string subtitle;
	std::string status;
	Color status_color = theme::text_dim;
	std::string addr;
	bool standby = false;
};

constexpr float kMargin = 64.0f;
constexpr float kRowH = 74.0f;
constexpr float kRowGap = 12.0f;

} // namespace

// ---------------------------------------------------------------- chrome ----

void Ui::frame_begin()
{
	draw_->begin();
	draw_->gradient_v(0, 0, static_cast<float>(draw_->width()), static_cast<float>(draw_->height()),
		theme::bg_top, theme::bg_bottom);
}

void Ui::draw_header(const char *title, const char *subtitle)
{
	draw_->text(kMargin, 36.0f, Font::Title, theme::text, title);
	if(subtitle)
		draw_->text(kMargin, 80.0f, Font::Small, theme::text_dim, subtitle);
	draw_->rect(kMargin, 112.0f, static_cast<float>(draw_->width()) - kMargin * 2, 1.0f,
		theme::text_dim.with_alpha(0.18f));
}

void Ui::draw_hints(const char *hints)
{
	const float y = static_cast<float>(draw_->height()) - 44.0f;
	draw_->rect(kMargin, y - 14.0f, static_cast<float>(draw_->width()) - kMargin * 2, 1.0f,
		theme::text_dim.with_alpha(0.18f));
	draw_->text(kMargin, y, Font::Small, theme::text_dim, hints);
}

void Ui::frame_end()
{
	const int slot = presenter_->acquire();
	draw_->end(slot);
}

void Ui::message(const char *title, const char *body)
{
	// Swallow the press that opened this, so it does not dismiss instantly.
	padUpdate(&g_pad);

	while(appletMainLoop())
	{
		padUpdate(&g_pad);
		if(padGetButtonsDown(&g_pad))
			break;

		frame_begin();
		draw_header("hayai", nullptr);

		const float w = static_cast<float>(draw_->width()) - kMargin * 2;
		const float y = 200.0f;
		draw_->rounded_rect(kMargin, y, w, 176.0f, 16.0f, theme::card);
		draw_->rect(kMargin, y + 14.0f, 3.0f, 148.0f, theme::accent);
		draw_->text(kMargin + 28.0f, y + 26.0f, Font::Body, theme::text, title);

		char buf[512];
		snprintf(buf, sizeof(buf), "%s", body);
		float ty = y + 74.0f;
		char *line = buf;
		while(line && *line)
		{
			char *nl = strchr(line, '\n');
			if(nl)
				*nl = '\0';
			draw_->text(kMargin + 28.0f, ty, Font::Small, theme::text_dim, line);
			ty += 26.0f;
			line = nl ? nl + 1 : nullptr;
		}

		draw_hints("Any button   Continue");
		frame_end();
	}
}

// Shown when a stream ends. Every number here is one the client already
// measured; the point is that tuning bitrate stops being guesswork.
void Ui::session_report(const core::Telemetry::Summary &s, bool errored)
{
	padUpdate(&g_pad);

	// One honest sentence about what the link did, derived from the same data.
	const double mins = s.duration_ms / 60000.0;
	const double fec_per_min = mins > 0.05 ? s.fec_failures / mins : 0.0;
	const char *verdict;
	if(s.frames < 60)
		verdict = "Too short to judge.";
	else if(fec_per_min >= 6.0)
		verdict = "Link struggled. Try one step down the bitrate ladder.";
	else if(fec_per_min >= 1.5)
		verdict = "Occasional recovery stalls. A small bitrate drop would steady it.";
	else if(s.avg_fps < 50.0 && s.frames_lost < 20 && s.fec_failures < 3)
		verdict = "Console sent fewer frames than asked. Wi-Fi interference is the usual cause.";
	else if(s.avg_fps >= 58.0 && s.fec_failures == 0)
		verdict = "Clean throughout - there is headroom for more bitrate.";
	else
		verdict = "Healthy.";

	while(appletMainLoop())
	{
		padUpdate(&g_pad);
		if(padGetButtonsDown(&g_pad))
			break;

		frame_begin();
		draw_header(errored ? "Session ended with an error" : "Session summary", verdict);

		const float x = kMargin;
		const float w = static_cast<float>(draw_->width()) - kMargin * 2;
		float y = 150.0f;

		char l[128], r[64];
		auto row = [&](const char *label, const char *value, Color vc) {
			draw_->rounded_rect(x, y, w, 52.0f, 12.0f, theme::card);
			draw_->text(x + 24.0f, y + 14.0f, Font::Body, theme::text_dim, label);
			const float vw = draw_->text_width(Font::Body, value);
			draw_->text(x + w - vw - 24.0f, y + 14.0f, Font::Body, vc, value);
			y += 60.0f;
		};

		snprintf(l, sizeof(l), "%llu min %llu s",
			static_cast<unsigned long long>(s.duration_ms / 60000),
			static_cast<unsigned long long>((s.duration_ms / 1000) % 60));
		row("Played for", l, theme::text);

		snprintf(l, sizeof(l), "%.1f fps", s.avg_fps);
		row("Average frame rate", l, s.avg_fps >= 57.0 ? theme::good : theme::warn);

		snprintf(l, sizeof(l), "%.1f ms avg, %.0f ms worst", s.avg_present_ms, s.worst_present_ms);
		row("Frame to screen", l, s.avg_present_ms < 4.0 ? theme::good : theme::warn);

		snprintf(l, sizeof(l), "%llu stalls", static_cast<unsigned long long>(s.fec_failures));
		snprintf(r, sizeof(r), "%s", l);
		row("Network recovery", r,
			s.fec_failures == 0 ? theme::good : fec_per_min >= 3.0 ? theme::bad : theme::warn);

		snprintf(l, sizeof(l), "%llu frames lost", static_cast<unsigned long long>(s.frames_lost));
		row("Link", l, s.frames_lost == 0 ? theme::good : s.frames_lost > 60 ? theme::bad : theme::warn);

		snprintf(l, sizeof(l), "%llu gaps", static_cast<unsigned long long>(s.audio_underruns));
		row("Audio", l, s.audio_underruns < 10 ? theme::good : theme::warn);

		draw_hints("Any button   Back to consoles");
		frame_end();
	}
}

// ------------------------------------------------------------- main menu ----

Ui::MenuResult Ui::main_menu()
{
	int cursor = 0;
	float cursor_anim = -1.0f;

	while(appletMainLoop())
	{
		auto discovered = discovery_.hosts();
		std::vector<Row> rows;

		for(const auto &h : config_.hosts)
		{
			Row r;
			r.kind = Row::Kind::Registered;
			r.addr = h.addr;
			r.title = h.nickname.empty() ? h.addr : h.nickname;
			r.subtitle = h.addr;

			const DiscoveredHost *live = nullptr;
			for(const auto &d : discovered)
			{
				if(d.addr == h.addr)
				{
					live = &d;
					break;
				}
			}
			if(!live)
			{
				r.status = "offline";
				r.status_color = theme::text_dim;
			}
			else if(live->ready)
			{
				r.status = "ready";
				r.status_color = theme::good;
			}
			else
			{
				r.status = "standby";
				r.status_color = theme::warn;
				r.standby = true;
			}
			rows.push_back(std::move(r));
		}

		for(const auto &d : discovered)
		{
			if(config_.find_host(d.addr))
				continue;
			Row r;
			r.kind = Row::Kind::Discovered;
			r.addr = d.addr;
			r.title = d.name.empty() ? d.addr : d.name;
			r.subtitle = d.addr;
			r.status = d.ps5 ? "not paired" : "not a PS5";
			r.status_color = d.ps5 ? theme::accent : theme::bad;
			r.standby = !d.ready;
			rows.push_back(std::move(r));
		}

		{
			Row r;
			r.kind = Row::Kind::RegisterIp;
			r.title = "Pair by IP address";
			r.subtitle = "for a console discovery cannot see";
			rows.push_back(r);

			Row s;
			s.kind = Row::Kind::Settings;
			s.title = "Settings";
			s.subtitle = config_.settings.profile_name();
			rows.push_back(s);
		}

		const int count = static_cast<int>(rows.size());
		if(cursor >= count)
			cursor = count - 1;
		if(cursor < 0)
			cursor = 0;

		padUpdate(&g_pad);
		const uint64_t down = padGetButtonsDown(&g_pad);
		if(down & HidNpadButton_Plus)
			return MenuResult::Exit;
		if(down & HidNpadButton_AnyDown)
			cursor = (cursor + 1) % count;
		if(down & HidNpadButton_AnyUp)
			cursor = (cursor + count - 1) % count;
		if(down & HidNpadButton_A)
		{
			Row &r = rows[cursor];
			switch(r.kind)
			{
				case Row::Kind::Registered:
				{
					HostEntry *host = config_.find_host(r.addr);
					if(!host)
						break;
					if(r.standby && !wake_and_wait(*host))
						break;
					selected_ = *host;
					return MenuResult::Stream;
				}
				case Row::Kind::Discovered:
					if(r.standby)
					{
						message("Console is in standby",
							"Turn it on fully to pair for the first time.\nOnce paired, hayai can wake it for you.");
						break;
					}
					selected_ = {};
					selected_.addr = r.addr;
					selected_.nickname = r.title;
					register_host(selected_);
					break;
				case Row::Kind::RegisterIp:
					register_menu();
					break;
				case Row::Kind::Settings:
					settings_menu();
					break;
			}
			continue;
		}

		frame_begin();

		char sub[192];
		const bool applet = appletGetAppletType() != AppletType_Application &&
			appletGetAppletType() != AppletType_SystemApplication;
		snprintf(sub, sizeof(sub), "%s%s", config_.settings.profile_name(),
			applet ? "     applet mode - hold R over a game for full performance" : "");
		draw_header("hayai", sub);

		const float x = kMargin;
		const float w = static_cast<float>(draw_->width()) - kMargin * 2;
		const float y0 = 144.0f;

		const float target = y0 + cursor * (kRowH + kRowGap);
		if(cursor_anim < 0.0f)
			cursor_anim = target;
		cursor_anim = lerp(cursor_anim, target, 0.35f);
		draw_->rounded_rect(x - 6.0f, cursor_anim - 4.0f, w + 12.0f, kRowH + 8.0f, 18.0f, theme::accent_dim);

		for(int i = 0; i < count; i++)
		{
			const Row &r = rows[i];
			const float y = y0 + i * (kRowH + kRowGap);
			if(y + kRowH > static_cast<float>(draw_->height()) - 76.0f)
				break;

			const bool sel = i == cursor;
			draw_->rounded_rect(x, y, w, kRowH, 14.0f, sel ? theme::card.with_alpha(0.13f) : theme::card);
			if(sel)
				draw_->rect(x, y + 14.0f, 3.0f, kRowH - 28.0f, theme::accent);

			draw_->text(x + 26.0f, y + 12.0f, Font::Body, theme::text, r.title.c_str());
			draw_->text(x + 26.0f, y + 43.0f, Font::Small, theme::text_dim, r.subtitle.c_str());

			if(!r.status.empty())
			{
				const float tw = draw_->text_width(Font::Small, r.status.c_str());
				const float pw = tw + 28.0f;
				const float px = x + w - pw - 20.0f;
				draw_->rounded_rect(px, y + 22.0f, pw, 30.0f, 15.0f, r.status_color.with_alpha(0.16f));
				draw_->text(px + 14.0f, y + 27.0f, Font::Small, r.status_color, r.status.c_str());
			}
		}

		draw_hints("A  Select      +  Exit      during a stream: hold L+R+MINUS to quit");
		frame_end();
	}
	return MenuResult::Exit;
}

// ------------------------------------------------------------- wake/pair ----

bool Ui::wake_and_wait(const HostEntry &entry)
{
	if(!Discovery::wakeup(entry.addr, entry.rp_regist_key))
	{
		message("Wake failed", "Could not send the wake packet.");
		return false;
	}

	const uint64_t start = now_ns();
	while(appletMainLoop())
	{
		for(const auto &d : discovery_.hosts())
		{
			if(d.addr == entry.addr && d.ready)
				return true;
		}
		const uint64_t elapsed = (now_ns() - start) / 1000000000ULL;
		if(elapsed > 45)
		{
			message("Console did not wake",
				"No response after 45 seconds. Check that rest-mode\nnetworking is enabled on the console.");
			return false;
		}

		padUpdate(&g_pad);
		if(padGetButtonsDown(&g_pad) & HidNpadButton_B)
			return false;

		frame_begin();
		draw_header("hayai", nullptr);

		const float w = static_cast<float>(draw_->width()) - kMargin * 2;
		draw_->rounded_rect(kMargin, 220.0f, w, 140.0f, 16.0f, theme::card);

		char line[128];
		snprintf(line, sizeof(line), "Waking %s", entry.nickname.c_str());
		draw_->text(kMargin + 28.0f, 246.0f, Font::Body, theme::text, line);
		snprintf(line, sizeof(line), "%llu s", static_cast<unsigned long long>(elapsed));
		draw_->text(kMargin + 28.0f, 284.0f, Font::Small, theme::text_dim, line);

		const float t = static_cast<float>((now_ns() / 1000000ULL) % 1600) / 1600.0f;
		const float bar_w = w - 56.0f;
		draw_->rounded_rect(kMargin + 28.0f, 324.0f, bar_w, 4.0f, 2.0f, theme::text_dim.with_alpha(0.15f));
		draw_->rounded_rect(kMargin + 28.0f + (bar_w - 140.0f) * t, 324.0f, 140.0f, 4.0f, 2.0f, theme::accent);

		draw_hints("B  Cancel");
		frame_end();
	}
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
	char account_b64[32];
	if(config_.settings.psn_account_id_b64.empty())
	{
		if(!keyboard_input("PSN Account ID (base64, 12 characters)", "",
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
		message("Invalid account ID",
			"That is not a valid base64 PSN account ID.\nIt must decode to exactly 8 bytes.");
		return;
	}

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
		message("Pairing failed", "Could not start registration.");
		return;
	}

	while(wait.done.load(std::memory_order_acquire) == 0 && appletMainLoop())
	{
		frame_begin();
		draw_header("hayai", nullptr);
		const float w = static_cast<float>(draw_->width()) - kMargin * 2;
		draw_->rounded_rect(kMargin, 220.0f, w, 130.0f, 16.0f, theme::card);
		char line[128];
		snprintf(line, sizeof(line), "Pairing with %s", entry.addr.c_str());
		draw_->text(kMargin + 28.0f, 248.0f, Font::Body, theme::text, line);
		draw_->text(kMargin + 28.0f, 288.0f, Font::Small, theme::text_dim, "Talking to the console...");
		frame_end();
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
		message("Paired", "The console is saved and ready to stream.");
	}
	else
		message("Pairing failed",
			"Check the PIN, the account ID, and that the console is\nfully on. Details in /config/hayai/hayai.log");
}

// -------------------------------------------------------------- settings ----

void Ui::settings_menu()
{
	Settings &s = config_.settings;
	int cursor = 0;
	constexpr int kItems = 6;
	// A short ladder rather than a slider: every value here is one someone
	// would actually pick, and "auto" follows the profile and resolution.
	static constexpr uint32_t kBitrates[] = {
		0, 4000, 4600, 4800, 5000, 5200, 5500, 5800, 6300, 6700, 8000, 10000, 12000, 15000
	};
	constexpr int kBitrateCount = static_cast<int>(sizeof(kBitrates) / sizeof(kBitrates[0]));
	float cursor_anim = -1.0f;

	while(appletMainLoop())
	{
		padUpdate(&g_pad);
		const uint64_t down = padGetButtonsDown(&g_pad);
		if(down & HidNpadButton_B)
			break;
		if(down & HidNpadButton_AnyDown)
			cursor = (cursor + 1) % kItems;
		if(down & HidNpadButton_AnyUp)
			cursor = (cursor + kItems - 1) % kItems;
		const bool step_back = (down & HidNpadButton_AnyLeft) != 0;
		if(down & (HidNpadButton_A | HidNpadButton_AnyRight | HidNpadButton_AnyLeft))
		{
			const int dir = step_back ? -1 : 1;
			switch(cursor)
			{
				case 0:
				{
					const int p = (static_cast<int>(s.profile) + dir + 3) % 3;
					s.profile = static_cast<Profile>(p);
					s.apply_profile_defaults();
					break;
				}
				case 1:
				{
					const int r = (static_cast<int>(s.resolution) + dir + 4) % 4;
					s.resolution = static_cast<Res>(r);
					s.bitrate = 0;	// follow the new resolution's default
					break;
				}
				case 2:
					s.fps = s.fps == CHIAKI_VIDEO_FPS_PRESET_60 ? CHIAKI_VIDEO_FPS_PRESET_30
						: CHIAKI_VIDEO_FPS_PRESET_60;
					break;
				case 3:
				{
					int bi = 0;
					for(int i = 0; i < kBitrateCount; i++)
					{
						if(kBitrates[i] == s.bitrate)
						{
							bi = i;
							break;
						}
					}
					s.bitrate = kBitrates[(bi + dir + kBitrateCount) % kBitrateCount];
					break;
				}
				case 4:
					s.controller_only = !s.controller_only;
					break;
				case 5:
					s.pin_clocks = !s.pin_clocks;
					break;
			}
		}

		frame_begin();

		char sub[192];
		snprintf(sub, sizeof(sub), "session will request  %ux%u at %u fps,  %u kbps",
			s.width(), s.height(), static_cast<unsigned>(s.fps), s.default_bitrate_kbps());
		draw_header("Settings", sub);

		char fps_buf[16];
		snprintf(fps_buf, sizeof(fps_buf), "%u", static_cast<unsigned>(s.fps));
		char rate_buf[32];
		if(s.bitrate)
			snprintf(rate_buf, sizeof(rate_buf), "%u kbps", s.bitrate);
		else
			snprintf(rate_buf, sizeof(rate_buf), "auto (%u)", s.default_bitrate_kbps());

		struct Item
		{
			const char *label;
			const char *value;
			const char *note;
		};
		const Item items[kItems] = {
			{ "Profile", s.profile_name(),
				s.profile == Profile::Latency ? "immediate present, tightest reorder window"
					: s.profile == Profile::Smooth
						? "vsync, wider reorder window, honest congestion reports"
						: "highest fidelity, assumes a strong network" },
			{ "Resolution", s.res_name(), "lower resolutions leave more radio headroom" },
			{ "Frame rate", fps_buf, s.fps == CHIAKI_VIDEO_FPS_PRESET_30
				? "consistency mode: auto-halves bitrate, so frames stay small"
				: "60 is smoother and lower latency; 30 trades both for stability" },
			{ "Bitrate", rate_buf,
				"lower is often smoother - clean delivery beats sharp but hitching" },
			{ "Controller only", s.controller_only ? "on" : "off",
				"no video at all - the Switch becomes a gamepad" },
			{ "Pin clocks", s.pin_clocks ? "on" : "off",
				"holds CPU and GPU steady so timing does not wander" },
		};

		const float x = kMargin;
		const float w = static_cast<float>(draw_->width()) - kMargin * 2;
		const float y0 = 144.0f;
		const float rh = 66.0f;
		const float gap = 8.0f;

		const float target = y0 + cursor * (rh + gap);
		if(cursor_anim < 0.0f)
			cursor_anim = target;
		cursor_anim = lerp(cursor_anim, target, 0.35f);
		draw_->rounded_rect(x - 6.0f, cursor_anim - 4.0f, w + 12.0f, rh + 8.0f, 16.0f, theme::accent_dim);

		for(int i = 0; i < kItems; i++)
		{
			const float y = y0 + i * (rh + gap);
			const bool sel = i == cursor;
			draw_->rounded_rect(x, y, w, rh, 12.0f, sel ? theme::card.with_alpha(0.13f) : theme::card);
			draw_->text(x + 26.0f, y + 10.0f, Font::Body, theme::text, items[i].label);
			draw_->text(x + 26.0f, y + 38.0f, Font::Small, theme::text_dim, items[i].note);

			const float vw = draw_->text_width(Font::Body, items[i].value);
			draw_->text(x + w - vw - 28.0f, y + 18.0f, Font::Body,
				sel ? theme::accent : theme::text, items[i].value);
		}

		draw_hints("Left / Right  Adjust      B  Back (saves)");
		frame_end();
	}
	config_.save();
}

// ------------------------------------------------------------------- run ----

bool Ui::gfx_up()
{
	presenter_ = new gfx::Presenter();
	// Menus are vsync-paced: nothing to gain from tearing here, and a steady
	// 60 Hz makes the cursor animation read as intentional.
	if(!presenter_->create(1280, 720, gfx::Presenter::Mode::Vsync, 2))
	{
		HAYAI_LOGE("ui: presenter init failed");
		return false;
	}
	draw_ = new ui::Draw();
	if(!draw_->create(*presenter_))
	{
		HAYAI_LOGE("ui: draw init failed");
		return false;
	}
	return true;
}

void Ui::gfx_down()
{
	delete draw_;
	draw_ = nullptr;
	delete presenter_;
	presenter_ = nullptr;
}

void Ui::run()
{
	padConfigureInput(1, HidNpadStyleSet_NpadStandard);
	padInitializeDefault(&g_pad);

	discovery_.start();
	if(!gfx_up())
	{
		gfx_down();
		return;
	}

	while(appletMainLoop())
	{
		const MenuResult result = main_menu();
		if(result == MenuResult::Exit)
			break;

		// The stream wants its own swapchain shape, so hand the display over.
		gfx_down();
		discovery_.stop();

		Stream::EndReason reason;
		{
			// Scoped so the stream -- and any graphics it still owns -- is fully
			// destroyed before the UI creates its own swapchain on the same window.
			Stream stream;
			reason = stream.run(selected_, config_.settings);
		}

		discovery_.start();
		if(!gfx_up())
			break;

		session_report(core::telemetry().summary(), reason == Stream::EndReason::Error);
	}

	gfx_down();
	discovery_.stop();
}

} // namespace hayai::app
