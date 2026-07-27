// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "audio/audio.hpp"
#include "core/log.hpp"
#include "core/telemetry.hpp"

#include <opus/opus.h>

extern "C" {
#include <libswresample/swresample.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
}

#include <switch.h>

#include <cstdlib>
#include <cstring>

namespace hayai::audio {

namespace {
constexpr unsigned kMaxChannels = 2;
// PCM staging between the resampler and audout. A few frames deep; the PLL
// keeps it near the setpoint.
constexpr unsigned kFifoCapacity = 48000 / 4;	// 250 ms hard cap, stereo samples
} // namespace

void Pipeline::sink(ChiakiAudioSink *out)
{
	out->header_cb = &Pipeline::header_cb;
	out->frame_cb = &Pipeline::frame_cb;
	out->user = this;
}

void Pipeline::header_cb(ChiakiAudioHeader *header, void *user)
{
	auto *self = static_cast<Pipeline *>(user);
	self->channels_.store(header->channels, std::memory_order_relaxed);
	self->rate_.store(header->rate, std::memory_order_relaxed);
	HAYAI_LOGI("audio: header ch=%u rate=%u frame=%u", header->channels, header->rate, header->frame_size);
}

// Receive thread: copy the encoded packet and get out. Nothing else.
void Pipeline::frame_cb(uint8_t *buf, size_t buf_size, void *user)
{
	auto *self = static_cast<Pipeline *>(user);
	if(buf_size > kPacketMax)
		return;

	const uint64_t idx = self->pkt_head_.load(std::memory_order_relaxed);
	Packet &slot = self->packets_[idx % kPacketSlots];
	if(slot.full.load(std::memory_order_acquire))
	{
		// Consumer is behind; dropping one 10 ms packet beats blocking the
		// receive thread. Opus PLC covers the gap.
		self->pkt_dropped_.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	slot.size = static_cast<uint32_t>(buf_size);
	memcpy(slot.data, buf, buf_size);
	slot.full.store(1, std::memory_order_release);
	self->pkt_head_.store(idx + 1, std::memory_order_release);
}

bool Pipeline::start()
{
	stop_.store(false, std::memory_order_relaxed);
	// 128 KB: opus_decode plus swresample need considerably more headroom than
	// libnx's default, and this thread is the one that historically blew it.
	if(!worker_.start(&Pipeline::thread_entry, this, core::kPrioHot, core::kCoreAux, 0x20000))
		return false;
	return true;
}

void Pipeline::stop()
{
	stop_.store(true, std::memory_order_relaxed);
	worker_.join();
}

void Pipeline::thread_entry(void *arg)
{
	static_cast<Pipeline *>(arg)->thread_loop();
}

void Pipeline::apply_compensation(int backlog_samples)
{
	// Proportional controller: backlog error (in output samples) -> ppm.
	// 10 ms of error corrects in roughly ten seconds; gentle on purpose,
	// because pitch artifacts start around a few thousand ppm.
	const int error = backlog_samples - kFillSetpointSamples;
	int ppm = -error * 2;
	if(ppm > 2000)
		ppm = 2000;
	if(ppm < -2000)
		ppm = -2000;
	if(ppm == comp_ppm_ || !swr_)
		return;
	comp_ppm_ = ppm;

	// Delta in output samples over one second of output.
	const int distance = 48000;
	const int delta = static_cast<int>(static_cast<int64_t>(ppm) * distance / 1000000);
	if(swr_set_compensation(swr_, delta, distance) < 0)
		HAYAI_LOGW("audio: swr_set_compensation(%d, %d) failed", delta, distance);
}

void Pipeline::thread_loop()
{
	if(R_FAILED(audoutInitialize()))
	{
		HAYAI_LOGE("audio: audoutInitialize failed");
		return;
	}
	if(R_FAILED(audoutStartAudioOut()))
	{
		HAYAI_LOGE("audio: audoutStartAudioOut failed");
		audoutExit();
		return;
	}

	const unsigned out_bytes = kOutSamples * kMaxChannels * sizeof(int16_t);
	const unsigned out_buf_size = (out_bytes + 0xFFF) & ~0xFFFu;

	AudioOutBuffer bufs[kOutBuffers]{};
	void *bufmem[kOutBuffers]{};
	for(unsigned i = 0; i < kOutBuffers; i++)
	{
		bufmem[i] = aligned_alloc(0x1000, out_buf_size);
		memset(bufmem[i], 0, out_buf_size);
		bufs[i].buffer = bufmem[i];
		bufs[i].buffer_size = out_buf_size;
		bufs[i].data_size = out_bytes;
		audoutAppendAudioOutBuffer(&bufs[i]);	// prime with silence
	}

	// Decode + resample staging. Both static: a 5760-sample decode buffer is
	// 23 KB, and opus_decode itself is stack-hungry -- together they overflowed
	// this thread's stack (crash inside opus_decode, faulting just below the
	// stack base). Only this thread touches them.
	static int16_t decode_buf[5760 * kMaxChannels];	// max opus frame (120 ms @ 48k)
	static int16_t fifo[kFifoCapacity * kMaxChannels];
	unsigned fifo_count = 0;	// in stereo samples

	uint32_t cfg_channels = 0;
	uint32_t cfg_rate = 0;
	uint64_t last_pll_ns = armTicksToNs(armGetSystemTick());
	uint64_t dropped_seen = 0;

	running_.store(true, std::memory_order_relaxed);
	HAYAI_LOGI("audio: pipeline up (%u x %u-sample buffers)", kOutBuffers, kOutSamples);

	while(!stop_.load(std::memory_order_relaxed))
	{
		// (Re)configure on header change.
		const uint32_t want_ch = channels_.load(std::memory_order_relaxed);
		const uint32_t want_rate = rate_.load(std::memory_order_relaxed);
		if(want_ch != cfg_channels || want_rate != cfg_rate)
		{
			if(opus_)
			{
				opus_decoder_destroy(opus_);
				opus_ = nullptr;
			}
			if(swr_)
				swr_free(&swr_);

			int err = 0;
			opus_ = opus_decoder_create(static_cast<int32_t>(want_rate), static_cast<int>(want_ch), &err);
			if(!opus_)
			{
				HAYAI_LOGE("audio: opus_decoder_create failed (%d)", err);
				break;
			}

			AVChannelLayout in_layout;
			av_channel_layout_default(&in_layout, static_cast<int>(want_ch));
			AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_STEREO;
			if(swr_alloc_set_opts2(&swr_,
					&out_layout, AV_SAMPLE_FMT_S16, 48000,
					&in_layout, AV_SAMPLE_FMT_S16, static_cast<int>(want_rate),
					0, nullptr) < 0 ||
				swr_init(swr_) < 0)
			{
				HAYAI_LOGE("audio: swresample init failed");
				break;
			}
			comp_ppm_ = 0;
			cfg_channels = want_ch;
			cfg_rate = want_rate;
		}

		// Drain every encoded packet currently in the ring.
		while(true)
		{
			Packet &slot = packets_[pkt_tail_ % kPacketSlots];
			if(!slot.full.load(std::memory_order_acquire))
				break;

			const int samples = opus_decode(opus_, slot.data, static_cast<int32_t>(slot.size),
					decode_buf, 5760, 0);
			slot.full.store(0, std::memory_order_release);
			pkt_tail_++;

			if(samples > 0 && fifo_count + static_cast<unsigned>(samples) + 128 < kFifoCapacity)
			{
				uint8_t *out_planes[1] = { reinterpret_cast<uint8_t *>(fifo + fifo_count * kMaxChannels) };
				const uint8_t *in_planes[1] = { reinterpret_cast<const uint8_t *>(decode_buf) };
				const int out_room = static_cast<int>(kFifoCapacity - fifo_count);
				const int got = swr_convert(swr_, out_planes, out_room, in_planes, samples);
				if(got > 0)
					fifo_count += static_cast<unsigned>(got);
			}
		}

		// Hard bound on backlog. The ppm servo corrects genuine clock drift,
		// but it cannot claw back a burst (the console front-loads audio
		// during handshake), and unbounded backlog is just latency that never
		// goes away. Drop the oldest samples back to the setpoint instead --
		// a sub-frame skip once, rather than permanent delay.
		if(fifo_count > kMaxBacklogSamples)
		{
			const unsigned drop = fifo_count - kFillSetpointSamples;
			memmove(fifo, fifo + drop * kMaxChannels,
				(fifo_count - drop) * kMaxChannels * sizeof(int16_t));
			fifo_count -= drop;
			HAYAI_LOGW("audio: backlog %u ms exceeded bound, dropped %u ms",
				(drop + fifo_count) / 48, drop / 48);
		}

		// If a network stall left the FIFO short, ask Opus for packet-loss
		// concealment frames instead of playing silence. PLC extrapolates the
		// last audio's spectrum, which the ear reads as a brief blur rather
		// than the hard click of a gap. Only once real audio has flowed, and
		// bounded so a long outage still decays to quiet.
		if(opus_ && pkt_tail_ > 0)
		{
			unsigned plc_frames = 0;
			while(fifo_count < kOutSamples && plc_frames < 3)
			{
				const int got_plc = opus_decode(opus_, nullptr, 0, decode_buf,
					static_cast<int>(kOutSamples), 0);
				if(got_plc <= 0)
					break;
				uint8_t *outp[1] = { reinterpret_cast<uint8_t *>(fifo + fifo_count * kMaxChannels) };
				const uint8_t *inp[1] = { reinterpret_cast<const uint8_t *>(decode_buf) };
				const int room = static_cast<int>(kFifoCapacity - fifo_count);
				const int got = swr_convert(swr_, outp, room, inp, got_plc);
				if(got <= 0)
					break;
				fifo_count += static_cast<unsigned>(got);
				plc_frames++;
			}
		}

		// Feed any released device buffers.
		AudioOutBuffer *released = nullptr;
		u32 released_count = 0;
		if(R_SUCCEEDED(audoutWaitPlayFinish(&released, &released_count, 5'000'000ULL)) && released)
		{
			// audout returns a chain; walk it, capped at our own buffer count
			// in case the service leaves a dangling next pointer.
			unsigned walked = 0;
			for(AudioOutBuffer *b = released; b && walked < kOutBuffers; walked++)
			{
				AudioOutBuffer *next = b->next;
				const unsigned need = kOutSamples;
				if(fifo_count >= need)
				{
					memcpy(b->buffer, fifo, need * kMaxChannels * sizeof(int16_t));
					fifo_count -= need;
					memmove(fifo, fifo + need * kMaxChannels, fifo_count * kMaxChannels * sizeof(int16_t));
				}
				else
				{
					memset(b->buffer, 0, need * kMaxChannels * sizeof(int16_t));
					if(running_.load(std::memory_order_relaxed) && pkt_head_.load(std::memory_order_relaxed) > 0)
						core::telemetry().audio_underrun();
				}
				b->data_size = out_bytes;
				audoutAppendAudioOutBuffer(b);
				b = next;
			}
		}

		// PLL update, once a second.
		const uint64_t now = armTicksToNs(armGetSystemTick());
		if(now - last_pll_ns >= 1'000'000'000ULL)
		{
			last_pll_ns = now;
			const uint64_t queued_pkts = pkt_head_.load(std::memory_order_relaxed) - pkt_tail_;
			const int backlog = static_cast<int>(fifo_count + queued_pkts * kOutSamples);
			apply_compensation(backlog);
			core::telemetry().audio_stats(backlog / 48, comp_ppm_);

			const uint64_t dropped = pkt_dropped_.load(std::memory_order_relaxed);
			if(dropped != dropped_seen)
			{
				HAYAI_LOGW("audio: dropped %llu encoded packets",
					static_cast<unsigned long long>(dropped - dropped_seen));
				dropped_seen = dropped;
			}
		}
	}

	running_.store(false, std::memory_order_relaxed);
	audoutStopAudioOut();
	audoutExit();
	for(unsigned i = 0; i < kOutBuffers; i++)
		free(bufmem[i]);
	if(opus_)
	{
		opus_decoder_destroy(opus_);
		opus_ = nullptr;
	}
	if(swr_)
		swr_free(&swr_);
	HAYAI_LOGI("audio: pipeline down");
}

} // namespace hayai::audio
