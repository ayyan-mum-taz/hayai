// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
#pragma once

#include <chiaki/common.h>
#include <chiaki/log.h>

extern "C" {
#include <libavcodec/avcodec.h>
}

#include <cstddef>
#include <cstdint>

namespace hayai::video {

// Hardware video decode on the Tegra X1's NVDEC block.
//
// libavcodec is used strictly as a front-end to NVDEC: no demuxer, no format
// layer, no swscale. Access units arrive already framed by the Remote Play
// protocol, so they go straight into avcodec_send_packet.
//
// Decoded frames stay in AV_PIX_FMT_NVTEGRA, which means they stay in GPU
// memory. Nothing copies them to the CPU -- the renderer aliases the same
// memory as a deko3d image. This is the single biggest difference from
// chiaki-ng's Switch build, which round-trips every frame through
// av_hwframe_transfer_data and glTexImage2D.
class Decoder
{
public:
	Decoder() = default;
	~Decoder() { destroy(); }
	Decoder(const Decoder &) = delete;
	Decoder &operator=(const Decoder &) = delete;

	bool create(ChiakiCodec codec, ChiakiLog *log);
	void destroy();

	// Feeds one complete access unit and pulls whatever comes out.
	// Returns true when a new frame is available from frame().
	bool submit(const uint8_t *data, size_t size);

	// The most recently decoded frame, owned by the decoder and valid until
	// the next submit().
	AVFrame *frame() const { return frame_; }

	bool valid() const { return codec_ctx_ != nullptr; }

private:
	ChiakiLog *log_ = nullptr;
	AVBufferRef *hw_device_ctx_ = nullptr;
	AVCodecContext *codec_ctx_ = nullptr;
	AVPacket *packet_ = nullptr;
	AVFrame *frame_ = nullptr;
};

} // namespace hayai::video
