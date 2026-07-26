// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "video/renderer.hpp"

extern "C" {
#include <libavutil/hwcontext_nvtegra.h>
#include <libavutil/pixfmt.h>
}

#include <cstdio>
#include <cstring>

namespace hayai::video {

namespace {

struct ColorConv
{
	alignas(16) float col0[4];
	alignas(16) float col1[4];
	alignas(16) float col2[4];
	alignas(16) float offset[4];
};
static_assert(sizeof(ColorConv) == 64, "must match the std140 block in video_fsh.glsl");

struct Vertex
{
	float pos[2];
	float uv[2];
};

constexpr std::array<DkVtxAttribState, 2> kVertexAttribs = {
	DkVtxAttribState{ 0, 0, offsetof(Vertex, pos), DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0 },
	DkVtxAttribState{ 0, 0, offsetof(Vertex, uv), DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0 },
};

constexpr std::array<DkVtxBufferState, 1> kVertexBuffers = {
	DkVtxBufferState{ sizeof(Vertex), 0 },
};

// Columns of the YUV->RGB matrix, i.e. the coefficients applied to Y, U and V
// respectively. Limited-range variants fold in the 255/219 luma expansion.
void fill_color_conv(ColorConv &out, AVColorSpace space, bool full_range)
{
	struct Mat
	{
		float y[3], u[3], v[3];
	};

	static constexpr Mat bt601_lim = { { 1.1644f, 1.1644f, 1.1644f }, { 0.0f, -0.3917f, 2.0172f }, { 1.5960f, -0.8129f, 0.0f } };
	static constexpr Mat bt601_full = { { 1.0f, 1.0f, 1.0f }, { 0.0f, -0.3441f, 1.7720f }, { 1.4020f, -0.7141f, 0.0f } };
	static constexpr Mat bt709_lim = { { 1.1644f, 1.1644f, 1.1644f }, { 0.0f, -0.2132f, 2.1124f }, { 1.7927f, -0.5329f, 0.0f } };
	static constexpr Mat bt709_full = { { 1.0f, 1.0f, 1.0f }, { 0.0f, -0.1873f, 1.8556f }, { 1.5746f, -0.4681f, 0.0f } };
	static constexpr Mat bt2020_lim = { { 1.1644f, 1.1644f, 1.1644f }, { 0.0f, -0.1874f, 2.1418f }, { 1.6781f, -0.6505f, 0.0f } };
	static constexpr Mat bt2020_full = { { 1.0f, 1.0f, 1.0f }, { 0.0f, -0.1646f, 1.8814f }, { 1.4746f, -0.5714f, 0.0f } };

	const Mat *m = nullptr;
	switch(space)
	{
		case AVCOL_SPC_BT709:
			m = full_range ? &bt709_full : &bt709_lim;
			break;
		case AVCOL_SPC_BT2020_NCL:
		case AVCOL_SPC_BT2020_CL:
			m = full_range ? &bt2020_full : &bt2020_lim;
			break;
		default:
			m = full_range ? &bt601_full : &bt601_lim;
			break;
	}

	for(int i = 0; i < 3; i++)
	{
		out.col0[i] = m->y[i];
		out.col1[i] = m->u[i];
		out.col2[i] = m->v[i];
	}
	out.col0[3] = out.col1[3] = out.col2[3] = 0.0f;

	out.offset[0] = full_range ? 0.0f : 16.0f / 255.0f;
	out.offset[1] = 128.0f / 255.0f;
	out.offset[2] = 128.0f / 255.0f;
	out.offset[3] = 0.0f;
}

} // namespace

bool Renderer::load_shader(const char *path, dk::Shader &out)
{
	FILE *f = fopen(path, "rb");
	if(!f)
	{
		CHIAKI_LOGE(log_, "renderer: cannot open %s", path);
		return false;
	}

	fseek(f, 0, SEEK_END);
	const long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	if(size <= 0)
	{
		fclose(f);
		return false;
	}

	gfx::Pool::Alloc a = code_pool_.allocate(static_cast<uint32_t>(size), DK_SHADER_CODE_ALIGNMENT);
	if(!a)
	{
		fclose(f);
		CHIAKI_LOGE(log_, "renderer: code pool exhausted loading %s", path);
		return false;
	}

	const size_t read = fread(a.cpu, 1, static_cast<size_t>(size), f);
	fclose(f);
	if(read != static_cast<size_t>(size))
		return false;

	dk::ShaderMaker{ code_pool_.block(), a.offset }.initialize(out);
	return true;
}

bool Renderer::create(gfx::Presenter &presenter, ChiakiLog *log)
{
	destroy();
	presenter_ = &presenter;
	log_ = log;

	dk::Device dev = presenter_->device();

	if(!code_pool_.create(dev, 128 * 1024,
			DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code))
		return false;
	if(!data_pool_.create(dev, 256 * 1024, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached))
		return false;
	if(!desc_pool_.create(dev, 64 * 1024, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached))
		return false;

	if(!load_shader("romfs:/shaders/video_vsh.dksh", vsh_))
		return false;
	if(!load_shader("romfs:/shaders/video_fsh.dksh", fsh_))
		return false;

	cmdbuf_ = dk::CmdBufMaker{ dev }.create();
	if(!cmdbuf_)
		return false;

	cmd_mem_ = data_pool_.allocate(kCmdSliceSize * gfx::Presenter::kMaxImages, DK_CMDMEM_ALIGNMENT);
	vertex_ = data_pool_.allocate(sizeof(Vertex) * 4, alignof(Vertex));
	uniform_ = data_pool_.allocate(sizeof(ColorConv), DK_UNIFORM_BUF_ALIGNMENT);
	image_descs_ = desc_pool_.allocate(sizeof(DkImageDescriptor) * kMaxMappings * 2, DK_IMAGE_DESCRIPTOR_ALIGNMENT);
	sampler_descs_ = desc_pool_.allocate(sizeof(DkSamplerDescriptor), DK_SAMPLER_DESCRIPTOR_ALIGNMENT);
	if(!cmd_mem_ || !vertex_ || !uniform_ || !image_descs_ || !sampler_descs_)
		return false;

	// One sampler for both planes: linear so the chroma plane gets upsampled by
	// the texture unit, clamped so edge taps do not wrap.
	dk::Sampler sampler;
	sampler.setFilter(DkFilter_Linear, DkFilter_Linear);
	sampler.setWrapMode(DkWrapMode_ClampToEdge, DkWrapMode_ClampToEdge, DkWrapMode_ClampToEdge);
	static_cast<dk::SamplerDescriptor *>(sampler_descs_.cpu)->initialize(sampler);

	return true;
}

void Renderer::destroy()
{
	if(presenter_ && presenter_->queue())
		presenter_->queue().waitIdle();

	for(unsigned i = 0; i < mapping_count_; i++)
	{
		if(mappings_[i].block)
		{
			mappings_[i].block.destroy();
			mappings_[i].block = nullptr;
		}
	}
	mapping_count_ = 0;
	current_mapping_ = -1;

	if(cmdbuf_)
	{
		cmdbuf_.destroy();
		cmdbuf_ = nullptr;
	}
	desc_pool_.destroy();
	data_pool_.destroy();
	code_pool_.destroy();
	configured_ = false;
	presenter_ = nullptr;
}

bool Renderer::configure(AVFrame *frame)
{
	if(configured_ && frame->width == frame_width_ && frame->height == frame_height_)
		return true;

	// A resolution change invalidates every surface mapping we hold.
	if(configured_)
	{
		presenter_->queue().waitIdle();
		for(unsigned i = 0; i < mapping_count_; i++)
		{
			if(mappings_[i].block)
				mappings_[i].block.destroy();
			mappings_[i] = {};
		}
		mapping_count_ = 0;
		current_mapping_ = -1;
	}

	frame_width_ = frame->width;
	frame_height_ = frame->height;

	dk::Device dev = presenter_->device();
	const uint32_t usage = DkImageFlags_UsageLoadStore | DkImageFlags_Usage2DEngine | DkImageFlags_UsageVideo;

	dk::ImageLayoutMaker{ dev }
		.setType(DkImageType_2D)
		.setFormat(DkImageFormat_R8_Unorm)
		.setDimensions(frame_width_, frame_height_, 1)
		.setFlags(usage)
		.initialize(luma_layout_);

	dk::ImageLayoutMaker{ dev }
		.setType(DkImageType_2D)
		.setFormat(DkImageFormat_RG8_Unorm)
		.setDimensions(frame_width_ / 2, frame_height_ / 2, 1)
		.setFlags(usage)
		.initialize(chroma_layout_);

	// Aspect-fit the stream into the panel by shrinking the quad, so the
	// shader stays a straight 1:1 sample and no UV maths happens per pixel.
	const float frame_ar = static_cast<float>(frame_width_) / static_cast<float>(frame_height_);
	const float screen_ar = static_cast<float>(presenter_->width()) / static_cast<float>(presenter_->height());
	float sx = 1.0f, sy = 1.0f;
	if(frame_ar > screen_ar)
		sy = screen_ar / frame_ar;
	else if(frame_ar < screen_ar)
		sx = frame_ar / screen_ar;

	const Vertex quad[4] = {
		{ { -sx, +sy }, { 0.0f, 0.0f } },
		{ { -sx, -sy }, { 0.0f, 1.0f } },
		{ { +sx, -sy }, { 1.0f, 1.0f } },
		{ { +sx, +sy }, { 1.0f, 0.0f } },
	};
	memcpy(vertex_.cpu, quad, sizeof(quad));

	const bool full_range = frame->color_range == AVCOL_RANGE_JPEG;
	AVColorSpace space = frame->colorspace;
	if(space == AVCOL_SPC_UNSPECIFIED)
		space = AVCOL_SPC_BT709;	// what Remote Play actually sends

	ColorConv conv{};
	fill_color_conv(conv, space, full_range);
	memcpy(uniform_.cpu, &conv, sizeof(conv));

	CHIAKI_LOGI(log_, "renderer: %dx%d -> %ux%u, colorspace %d, %s range",
		frame_width_, frame_height_, presenter_->width(), presenter_->height(),
		static_cast<int>(space), full_range ? "full" : "limited");

	configured_ = true;
	return true;
}

int Renderer::map_frame(AVFrame *frame)
{
	AVNVTegraMap *map = av_nvtegra_frame_get_fbuf_map(frame);
	if(!map)
		return -1;

	const uint32_t handle = av_nvtegra_map_get_handle(map);
	void *cpu = av_nvtegra_map_get_addr(map);
	const uint32_t size = av_nvtegra_map_get_size(map);
	const uint32_t chroma_offset = static_cast<uint32_t>(frame->data[1] - frame->data[0]);

	for(unsigned i = 0; i < mapping_count_; i++)
	{
		const Mapping &m = mappings_[i];
		if(m.handle == handle && m.cpu == cpu && m.size == size && m.chroma_offset == chroma_offset)
			return static_cast<int>(i);
	}

	if(mapping_count_ >= kMaxMappings)
	{
		CHIAKI_LOGE(log_, "renderer: out of surface mappings");
		return -1;
	}

	Mapping &m = mappings_[mapping_count_];
	m.handle = handle;
	m.cpu = cpu;
	m.size = size;
	m.chroma_offset = chroma_offset;

	// setStorage() is what makes this zero-copy: deko3d wraps the decoder's own
	// nvmap pages instead of allocating new ones.
	m.block = dk::MemBlockMaker{ presenter_->device(), size }
			.setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image)
			.setStorage(cpu)
			.create();
	if(!m.block)
	{
		CHIAKI_LOGE(log_, "renderer: failed to wrap decoder surface %u", handle);
		return -1;
	}

	m.luma.initialize(luma_layout_, m.block, 0);
	m.chroma.initialize(chroma_layout_, m.block, m.chroma_offset);

	m.luma_desc = mapping_count_ * 2;
	m.chroma_desc = mapping_count_ * 2 + 1;

	// Writing descriptor slots the GPU has never been told about is safe to do
	// straight from the CPU; only slots already in a submitted command list
	// would need to go through a command buffer.
	auto *descs = static_cast<dk::ImageDescriptor *>(image_descs_.cpu);
	dk::ImageView luma_view{ m.luma };
	dk::ImageView chroma_view{ m.chroma };
	descs[m.luma_desc].initialize(luma_view);
	descs[m.chroma_desc].initialize(chroma_view);

	mapping_count_++;
	return static_cast<int>(mapping_count_ - 1);
}

bool Renderer::draw(int slot, AVFrame *frame)
{
	if(!presenter_ || !frame || frame->format != AV_PIX_FMT_NVTEGRA)
		return false;

	if(!configure(frame))
		return false;

	const int mapping = map_frame(frame);
	if(mapping < 0)
		return false;

	const Mapping &m = mappings_[mapping];

	cmdbuf_.clear();
	cmdbuf_.addMemory(data_pool_.block(), cmd_mem_.offset + cmd_slice_ * kCmdSliceSize, kCmdSliceSize);
	cmd_slice_ = (cmd_slice_ + 1) % gfx::Presenter::kMaxImages;

	dk::ImageView target{ presenter_->image(slot) };
	cmdbuf_.bindRenderTargets(&target);
	cmdbuf_.setViewports(0, { { 0.0f, 0.0f, static_cast<float>(presenter_->width()),
								  static_cast<float>(presenter_->height()), 0.0f, 1.0f } });
	cmdbuf_.setScissors(0, { { 0, 0, presenter_->width(), presenter_->height() } });
	cmdbuf_.clearColor(0, DkColorMask_RGBA, 0.0f, 0.0f, 0.0f, 1.0f);

	cmdbuf_.bindShaders(DkStageFlag_GraphicsMask, { &vsh_, &fsh_ });
	cmdbuf_.bindImageDescriptorSet(image_descs_.gpu, kMaxMappings * 2);
	cmdbuf_.bindSamplerDescriptorSet(sampler_descs_.gpu, 1);
	cmdbuf_.bindTextures(DkStage_Fragment, 0, dkMakeTextureHandle(m.luma_desc, 0));
	cmdbuf_.bindTextures(DkStage_Fragment, 1, dkMakeTextureHandle(m.chroma_desc, 0));
	cmdbuf_.bindUniformBuffer(DkStage_Fragment, 0, uniform_.gpu, uniform_.size);

	dk::RasterizerState rasterizer;
	dk::ColorState color;
	dk::ColorWriteState color_write;
	cmdbuf_.bindRasterizerState(rasterizer);
	cmdbuf_.bindColorState(color);
	cmdbuf_.bindColorWriteState(color_write);

	cmdbuf_.bindVtxBuffer(0, vertex_.gpu, vertex_.size);
	cmdbuf_.bindVtxAttribState(kVertexAttribs);
	cmdbuf_.bindVtxBufferState(kVertexBuffers);
	cmdbuf_.draw(DkPrimitive_Quads, 4, 1, 0, 0);

	presenter_->queue().submitCommands(cmdbuf_.finishList());
	current_mapping_ = mapping;
	return true;
}

} // namespace hayai::video
