// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
#pragma once

#include "gfx/pool.hpp"
#include "gfx/presenter.hpp"

#include <chiaki/log.h>

extern "C" {
#include <libavutil/frame.h>
}

#include <deko3d.hpp>

#include <array>
#include <cstdint>

namespace hayai::video {

// Draws an NVDEC output surface to the screen without copying it.
//
// The trick is that an AV_PIX_FMT_NVTEGRA frame is backed by an nvmap
// allocation we can hand straight to deko3d: dk::MemBlockMaker::setStorage()
// takes the same CPU address the decoder was given, so the resulting MemBlock
// refers to the identical physical pages. Two images are then aliased over it,
// an R8 luma plane at offset 0 and an RG8 chroma plane at the frame's plane
// offset, and the fragment shader converts to RGB while sampling.
//
// Nothing is copied, and nothing touches the CPU between NVDEC and the display.
class Renderer
{
public:
	Renderer() = default;
	~Renderer() { destroy(); }
	Renderer(const Renderer &) = delete;
	Renderer &operator=(const Renderer &) = delete;

	bool create(gfx::Presenter &presenter, ChiakiLog *log);
	void destroy();

	// Records and submits the draw for one frame into the given swapchain slot.
	// Presentation is the caller's business.
	bool draw(int slot, AVFrame *frame);

private:
	// NVDEC recycles a small pool of surfaces, so mappings are created during
	// the first few frames and reused for the rest of the session.
	static constexpr unsigned kMaxMappings = 24;
	static constexpr unsigned kCmdSliceSize = 0x1000;

	struct Mapping
	{
		uint32_t handle = 0;
		void *cpu = nullptr;
		uint32_t size = 0;
		uint32_t chroma_offset = 0;
		dk::MemBlock block = nullptr;
		dk::Image luma;
		dk::Image chroma;
		uint32_t luma_desc = 0;
		uint32_t chroma_desc = 0;
	};

	bool load_shader(const char *path, dk::Shader &out);
	bool configure(AVFrame *frame);
	int map_frame(AVFrame *frame);

	ChiakiLog *log_ = nullptr;
	gfx::Presenter *presenter_ = nullptr;

	gfx::Pool code_pool_;
	gfx::Pool data_pool_;
	gfx::Pool desc_pool_;

	dk::Shader vsh_;
	dk::Shader fsh_;
	dk::CmdBuf cmdbuf_ = nullptr;

	gfx::Pool::Alloc vertex_{};
	gfx::Pool::Alloc uniform_{};
	gfx::Pool::Alloc image_descs_{};
	gfx::Pool::Alloc sampler_descs_{};
	gfx::Pool::Alloc cmd_mem_{};
	unsigned cmd_slice_ = 0;

	dk::ImageLayout luma_layout_;
	dk::ImageLayout chroma_layout_;

	std::array<Mapping, kMaxMappings> mappings_{};
	unsigned mapping_count_ = 0;
	int current_mapping_ = -1;

	int frame_width_ = 0;
	int frame_height_ = 0;
	bool configured_ = false;
};

} // namespace hayai::video
