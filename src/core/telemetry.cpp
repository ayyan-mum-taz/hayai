// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "core/telemetry.hpp"
#include "core/log.hpp"

#include <algorithm>
#include <cstring>

namespace hayai::core {

Telemetry &telemetry()
{
	static Telemetry instance;
	return instance;
}

void Telemetry::start_session()
{
	frame_count_.store(0, std::memory_order_relaxed);
	summary_start_idx_ = 0;
	summary_last_ns_ = now_ns();
	audio_underruns_.store(0, std::memory_order_relaxed);
}

Telemetry::Frame &Telemetry::begin_frame(uint32_t au_size, uint32_t frames_lost)
{
	const uint64_t idx = frame_count_.fetch_add(1, std::memory_order_relaxed);
	Frame &f = ring_[idx % kRing];
	f = {};
	f.au_complete_ns = now_ns();
	f.au_size = au_size;
	f.frames_lost = frames_lost;
	return f;
}

void Telemetry::end_frame(Frame &f)
{
	f.present_ns = now_ns();
}

void Telemetry::vsync_tick(uint64_t ts_ns)
{
	const uint64_t prev = last_vsync_ns_.exchange(ts_ns, std::memory_order_relaxed);
	if(prev && ts_ns > prev && ts_ns - prev < 40'000'000ULL)
		vsync_period_ns_.store(ts_ns - prev, std::memory_order_relaxed);
}

namespace {
// Percentile over a small copied array; n <= a few hundred.
uint32_t percentile_us(uint32_t *vals, unsigned n, unsigned pct)
{
	if(n == 0)
		return 0;
	std::sort(vals, vals + n);
	unsigned idx = (n * pct) / 100;
	if(idx >= n)
		idx = n - 1;
	return vals[idx];
}
} // namespace

void Telemetry::log_summary()
{
	const uint64_t now = now_ns();
	if(now - summary_last_ns_ < 2'000'000'000ULL)
		return;
	summary_last_ns_ = now;

	const uint64_t end = frame_count_.load(std::memory_order_relaxed);
	uint64_t begin = summary_start_idx_;
	summary_start_idx_ = end;
	if(end == begin)
		return;
	if(end - begin > kRing)
		begin = end - kRing;

	static uint32_t decode_us[kRing];
	static uint32_t total_us[kRing];
	unsigned n = 0;
	uint32_t lost = 0;

	for(uint64_t i = begin; i < end; i++)
	{
		const Frame &f = ring_[i % kRing];
		if(!f.present_ns || f.present_ns < f.au_complete_ns)
			continue;
		decode_us[n] = static_cast<uint32_t>((f.decode_done_ns - f.au_complete_ns) / 1000);
		total_us[n] = static_cast<uint32_t>((f.present_ns - f.au_complete_ns) / 1000);
		lost += f.frames_lost;
		n++;
	}
	if(n == 0)
		return;

	// Phase of the newest present relative to vsync, as a fraction of the
	// refresh period -- the beat-cycle position.
	const Frame &newest = ring_[(end - 1) % kRing];
	const uint64_t vs = last_vsync_ns_.load(std::memory_order_relaxed);
	const uint64_t period = vsync_period_ns_.load(std::memory_order_relaxed);
	int phase_pct = -1;
	if(vs && newest.present_ns > vs && period)
		phase_pct = static_cast<int>(((newest.present_ns - vs) % period) * 100 / period);

	const uint32_t d50 = percentile_us(decode_us, n, 50);
	const uint32_t d99 = percentile_us(decode_us, n, 99);
	const uint32_t t50 = percentile_us(total_us, n, 50);
	const uint32_t t99 = percentile_us(total_us, n, 99);

	HAYAI_LOGI("tl: %u frames | au->decoded %u.%03u/%u.%03u ms | au->presented %u.%03u/%u.%03u ms (p50/p99) | lost %u | phase %d%% | a_fill %d ms a_comp %d ppm a_under %llu | logdrop %llu",
		n,
		d50 / 1000, d50 % 1000, d99 / 1000, d99 % 1000,
		t50 / 1000, t50 % 1000, t99 / 1000, t99 % 1000,
		lost, phase_pct,
		audio_fill_ms_.load(std::memory_order_relaxed),
		audio_comp_ppm_.load(std::memory_order_relaxed),
		static_cast<unsigned long long>(audio_underruns_.load(std::memory_order_relaxed)),
		static_cast<unsigned long long>(log().dropped()));
}

} // namespace hayai::core
