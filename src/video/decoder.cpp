// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "video/decoder.hpp"

extern "C" {
#include <libavutil/hwcontext.h>
}

namespace hayai::video {

namespace {

// Refuse to fall back to software decode. If NVDEC is unavailable something is
// wrong with the environment, and silently dropping to the A57 would turn a
// loud failure into a mysteriously slow session.
AVPixelFormat pick_format(AVCodecContext *ctx, const AVPixelFormat *formats)
{
	(void)ctx;
	for(const AVPixelFormat *p = formats; *p != AV_PIX_FMT_NONE; p++)
	{
		if(*p == AV_PIX_FMT_NVTEGRA)
			return *p;
	}
	return AV_PIX_FMT_NONE;
}

} // namespace

bool Decoder::create(ChiakiCodec codec, ChiakiLog *log)
{
	destroy();
	log_ = log;

	const char *name = chiaki_codec_is_h265(codec) ? "hevc" : "h264";
	const AVCodec *av_codec = avcodec_find_decoder_by_name(name);
	if(!av_codec)
	{
		CHIAKI_LOGE(log_, "decoder: %s not built into libavcodec", name);
		return false;
	}

	int r = av_hwdevice_ctx_create(&hw_device_ctx_, AV_HWDEVICE_TYPE_NVTEGRA, nullptr, nullptr, 0);
	if(r < 0)
	{
		CHIAKI_LOGE(log_, "decoder: no nvtegra hwdevice (%d)", r);
		return false;
	}

	codec_ctx_ = avcodec_alloc_context3(av_codec);
	if(!codec_ctx_)
		return false;

	codec_ctx_->hw_device_ctx = av_buffer_ref(hw_device_ctx_);
	codec_ctx_->get_format = pick_format;
	codec_ctx_->pix_fmt = AV_PIX_FMT_NVTEGRA;

	// Emit each frame as soon as it is decodable instead of holding a reorder
	// window. Remote Play never sends B-frames, so this is free.
	codec_ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
	codec_ctx_->flags2 |= AV_CODEC_FLAG2_FAST;
	codec_ctx_->has_b_frames = 0;
	// Frame- and slice-threading both add a frame of pipelining for no benefit
	// when the decode itself happens on fixed-function hardware.
	codec_ctx_->thread_count = 1;
	codec_ctx_->thread_type = 0;

	r = avcodec_open2(codec_ctx_, av_codec, nullptr);
	if(r < 0)
	{
		CHIAKI_LOGE(log_, "decoder: avcodec_open2(%s) failed (%d)", name, r);
		destroy();
		return false;
	}

	packet_ = av_packet_alloc();
	frame_ = av_frame_alloc();
	if(!packet_ || !frame_)
	{
		destroy();
		return false;
	}

	CHIAKI_LOGI(log_, "decoder: %s on nvtegra, low-delay", name);
	return true;
}

void Decoder::destroy()
{
	if(frame_)
		av_frame_free(&frame_);
	if(packet_)
		av_packet_free(&packet_);
	if(codec_ctx_)
		avcodec_free_context(&codec_ctx_);
	if(hw_device_ctx_)
		av_buffer_unref(&hw_device_ctx_);
}

bool Decoder::submit(const uint8_t *data, size_t size)
{
	if(!codec_ctx_ || !data || !size)
		return false;

	packet_->data = const_cast<uint8_t *>(data);
	packet_->size = static_cast<int>(size);

	int r = avcodec_send_packet(codec_ctx_, packet_);
	if(r == AVERROR(EAGAIN))
	{
		// The decoder still holds an undrained frame. Drain it and retry once
		// rather than dropping the access unit.
		av_frame_unref(frame_);
		if(avcodec_receive_frame(codec_ctx_, frame_) == 0)
			r = avcodec_send_packet(codec_ctx_, packet_);
	}
	if(r < 0)
	{
		CHIAKI_LOGW(log_, "decoder: send_packet failed (%d)", r);
		return false;
	}

	av_frame_unref(frame_);
	r = avcodec_receive_frame(codec_ctx_, frame_);
	if(r < 0)
		return false;	// EAGAIN on the first frames is normal

	return true;
}

} // namespace hayai::video
