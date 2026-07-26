// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "ui/draw.hpp"
#include "core/log.hpp"

#include <switch.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include <stb_truetype.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace hayai::ui {

Draw::FontData Draw::s_fonts_[Draw::kFontCount];

namespace {

constexpr std::array<DkVtxAttribState, 3> kAttribs = {
	DkVtxAttribState{ 0, 0, offsetof(Draw::Vertex, pos), DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0 },
	DkVtxAttribState{ 0, 0, offsetof(Draw::Vertex, uv), DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0 },
	DkVtxAttribState{ 0, 0, offsetof(Draw::Vertex, color), DkVtxAttribSize_4x32, DkVtxAttribType_Float, 0 },
};

constexpr std::array<DkVtxBufferState, 1> kBuffers = {
	DkVtxBufferState{ sizeof(Draw::Vertex), 0 },
};

constexpr float kFontPx[3] = { 22.0f, 34.0f, 17.0f };

} // namespace

bool Draw::load_shader(const char *path, dk::Shader &out)
{
	FILE *f = fopen(path, "rb");
	if(!f)
	{
		HAYAI_LOGE("ui: cannot open %s", path);
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
		return false;
	}
	const size_t rd = fread(a.cpu, 1, static_cast<size_t>(size), f);
	fclose(f);
	if(rd != static_cast<size_t>(size))
		return false;
	dk::ShaderMaker{ code_pool_.block(), a.offset }.initialize(out);
	return true;
}

namespace {
// Rasterising the system font takes a noticeable moment, and the UI is torn
// down and rebuilt around every stream. The pixels and metrics never change,
// so keep them for the life of the process.
uint8_t *g_atlas_cache = nullptr;
bool g_atlas_cached = false;
} // namespace

bool Draw::build_atlas()
{
	if(g_atlas_cached)
		return upload_atlas(g_atlas_cache);

	// The console's own font, so the UI matches the system's language coverage
	// and looks native rather than bundled.
	if(R_FAILED(plInitialize(PlServiceType_User)))
	{
		HAYAI_LOGE("ui: plInitialize failed");
		return false;
	}
	PlFontData font_data{};
	const Result rc = plGetSharedFontByType(&font_data, PlSharedFontType_Standard);
	if(R_FAILED(rc))
	{
		HAYAI_LOGE("ui: plGetSharedFontByType failed");
		plExit();
		return false;
	}

	stbtt_fontinfo info;
	if(!stbtt_InitFont(&info, static_cast<const unsigned char *>(font_data.address),
			stbtt_GetFontOffsetForIndex(static_cast<const unsigned char *>(font_data.address), 0)))
	{
		HAYAI_LOGE("ui: stbtt_InitFont failed");
		plExit();
		return false;
	}

	auto *pixels = static_cast<uint8_t *>(calloc(kAtlasW * kAtlasH, 1));
	if(!pixels)
	{
		plExit();
		return false;
	}
	g_atlas_cache = pixels;

	// (0,0): a solid texel for untextured fills.
	pixels[0] = 0xFF;
	white_u_ = 0.5f / kAtlasW;
	white_v_ = 0.5f / kAtlasH;

	unsigned pen_x = 4;
	unsigned pen_y = 0;
	unsigned row_h = 0;

	// Quarter disc for rounded corners: sampled as a 9-slice corner.
	{
		const float r = static_cast<float>(kCornerPx);
		for(unsigned y = 0; y < kCornerPx; y++)
		{
			for(unsigned x = 0; x < kCornerPx; x++)
			{
				// Distance from the corner centre, antialiased at the edge.
				const float dx = r - (static_cast<float>(x) + 0.5f);
				const float dy = r - (static_cast<float>(y) + 0.5f);
				const float d = std::sqrt(dx * dx + dy * dy);
				float a = r - d;
				if(a < 0.0f)
					a = 0.0f;
				if(a > 1.0f)
					a = 1.0f;
				pixels[(pen_y + y) * kAtlasW + (pen_x + x)] = static_cast<uint8_t>(a * 255.0f);
			}
		}
		corner_u0_ = static_cast<float>(pen_x) / kAtlasW;
		corner_v0_ = static_cast<float>(pen_y) / kAtlasH;
		corner_u1_ = static_cast<float>(pen_x + kCornerPx) / kAtlasW;
		corner_v1_ = static_cast<float>(pen_y + kCornerPx) / kAtlasH;
		pen_x += kCornerPx + 2;
		row_h = kCornerPx;
	}

	for(unsigned fi = 0; fi < kFontCount; fi++)
	{
		const float px = kFontPx[fi];
		const float scale = stbtt_ScaleForPixelHeight(&info, px);
		int ascent = 0, descent = 0, line_gap = 0;
		stbtt_GetFontVMetrics(&info, &ascent, &descent, &line_gap);
		fonts_[fi].ascent = ascent * scale;
		fonts_[fi].line_h = (ascent - descent + line_gap) * scale;

		for(int gi = 0; gi < kGlyphCount; gi++)
		{
			const int cp = kGlyphFirst + gi;
			int adv = 0, lsb = 0;
			stbtt_GetCodepointHMetrics(&info, cp, &adv, &lsb);

			int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
			stbtt_GetCodepointBitmapBox(&info, cp, scale, scale, &x0, &y0, &x1, &y1);
			const unsigned gw = static_cast<unsigned>(x1 - x0);
			const unsigned gh = static_cast<unsigned>(y1 - y0);

			if(pen_x + gw + 2 >= kAtlasW)
			{
				pen_x = 4;
				pen_y += row_h + 2;
				row_h = 0;
			}
			if(pen_y + gh + 2 >= kAtlasH)
			{
				HAYAI_LOGE("ui: glyph atlas full");
				break;
			}

			if(gw && gh)
			{
				stbtt_MakeCodepointBitmap(&info, pixels + pen_y * kAtlasW + pen_x,
					static_cast<int>(gw), static_cast<int>(gh), kAtlasW, scale, scale, cp);
			}

			Glyph &g = fonts_[fi].glyphs[gi];
			g.u0 = static_cast<float>(pen_x) / kAtlasW;
			g.v0 = static_cast<float>(pen_y) / kAtlasH;
			g.u1 = static_cast<float>(pen_x + gw) / kAtlasW;
			g.v1 = static_cast<float>(pen_y + gh) / kAtlasH;
			g.xoff = static_cast<float>(x0);
			g.yoff = static_cast<float>(y0);
			g.xadv = adv * scale;
			g.w = static_cast<float>(gw);
			g.h = static_cast<float>(gh);

			pen_x += gw + 2;
			if(gh > row_h)
				row_h = gh;
		}
		pen_x = 4;
		pen_y += row_h + 2;
		row_h = 0;
	}
	plExit();
	g_atlas_cached = true;
	return upload_atlas(pixels);
}

bool Draw::upload_atlas(const uint8_t *pixels)
{
	// Upload: staging block -> tiled image via the copy engine.
	dk::Device dev = presenter_->device();
	dk::ImageLayout layout;
	dk::ImageLayoutMaker{ dev }
		.setType(DkImageType_2D)
		.setFormat(DkImageFormat_R8_Unorm)
		.setDimensions(kAtlasW, kAtlasH)
		.setFlags(DkImageFlags_UsageLoadStore | DkImageFlags_Usage2DEngine)
		.initialize(layout);

	if(!atlas_pool_.create(dev, static_cast<uint32_t>(layout.getSize()) + layout.getAlignment(),
			DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image))
		return false;
	gfx::Pool::Alloc img = atlas_pool_.allocate(static_cast<uint32_t>(layout.getSize()), layout.getAlignment());
	if(!img)
		return false;
	atlas_img_.initialize(layout, atlas_pool_.block(), img.offset);

	gfx::Pool staging;
	if(!staging.create(dev, kAtlasW * kAtlasH, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached))
		return false;
	gfx::Pool::Alloc st = staging.allocate(kAtlasW * kAtlasH, DK_IMAGE_LINEAR_STRIDE_ALIGNMENT);
	memcpy(st.cpu, pixels, kAtlasW * kAtlasH);

	dk::UniqueCmdBuf upload = dk::CmdBufMaker{ dev }.create();
	gfx::Pool upload_mem;
	if(!upload_mem.create(dev, 0x1000, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached))
		return false;
	gfx::Pool::Alloc um = upload_mem.allocate(0x1000, DK_CMDMEM_ALIGNMENT);
	upload.addMemory(upload_mem.block(), um.offset, um.size);

	dk::ImageView view{ atlas_img_ };
	upload.copyBufferToImage({ st.gpu }, view, { 0, 0, 0, kAtlasW, kAtlasH, 1 });
	presenter_->queue().submitCommands(upload.finishList());
	presenter_->queue().waitIdle();

	// Descriptors: one image, one linear-clamped sampler.
	auto *descs = static_cast<dk::ImageDescriptor *>(image_descs_.cpu);
	descs[0].initialize(view);

	dk::Sampler sampler;
	sampler.setFilter(DkFilter_Linear, DkFilter_Linear);
	sampler.setWrapMode(DkWrapMode_ClampToEdge, DkWrapMode_ClampToEdge, DkWrapMode_ClampToEdge);
	static_cast<dk::SamplerDescriptor *>(sampler_descs_.cpu)->initialize(sampler);

	return true;
}

bool Draw::create(gfx::Presenter &presenter)
{
	destroy();
	presenter_ = &presenter;
	dk::Device dev = presenter_->device();

	if(!code_pool_.create(dev, 128 * 1024,
			DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code))
		return false;
	if(!data_pool_.create(dev, sizeof(Vertex) * kMaxQuads * 4 + 64 * 1024,
			DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached))
		return false;
	if(!desc_pool_.create(dev, 16 * 1024, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached))
		return false;

	if(!load_shader("romfs:/shaders/ui_vsh.dksh", vsh_))
		return false;
	if(!load_shader("romfs:/shaders/ui_fsh.dksh", fsh_))
		return false;

	cmdbuf_ = dk::CmdBufMaker{ dev }.create();
	if(!cmdbuf_)
		return false;

	cmd_mem_ = data_pool_.allocate(0x2000 * gfx::Presenter::kMaxImages, DK_CMDMEM_ALIGNMENT);
	vertices_ = data_pool_.allocate(sizeof(Vertex) * kMaxQuads * 4, alignof(Vertex));
	uniform_ = data_pool_.allocate(sizeof(float) * 4, DK_UNIFORM_BUF_ALIGNMENT);
	image_descs_ = desc_pool_.allocate(sizeof(DkImageDescriptor) * 4, DK_IMAGE_DESCRIPTOR_ALIGNMENT);
	sampler_descs_ = desc_pool_.allocate(sizeof(DkSamplerDescriptor), DK_SAMPLER_DESCRIPTOR_ALIGNMENT);
	if(!cmd_mem_ || !vertices_ || !uniform_ || !image_descs_ || !sampler_descs_)
		return false;

	verts_ = static_cast<Vertex *>(vertices_.cpu);

	float *screen = static_cast<float *>(uniform_.cpu);
	screen[0] = 2.0f / static_cast<float>(presenter_->width());
	screen[1] = 2.0f / static_cast<float>(presenter_->height());
	screen[2] = 0.0f;
	screen[3] = 0.0f;

	return build_atlas();
}

void Draw::destroy()
{
	if(presenter_ && presenter_->queue())
		presenter_->queue().waitIdle();
	if(cmdbuf_)
	{
		cmdbuf_.destroy();
		cmdbuf_ = nullptr;
	}
	atlas_pool_.destroy();
	desc_pool_.destroy();
	data_pool_.destroy();
	code_pool_.destroy();
	verts_ = nullptr;
	presenter_ = nullptr;
}

void Draw::begin()
{
	quad_count_ = 0;
}

void Draw::push_quad(float x, float y, float w, float h,
	float u0, float v0, float u1, float v1, Color c)
{
	if(quad_count_ >= kMaxQuads)
		return;
	Vertex *v = verts_ + quad_count_ * 4;
	const float col[4] = { c.r, c.g, c.b, c.a };

	auto set = [&](int i, float px, float py, float pu, float pv) {
		v[i].pos[0] = px;
		v[i].pos[1] = py;
		v[i].uv[0] = pu;
		v[i].uv[1] = pv;
		memcpy(v[i].color, col, sizeof(col));
	};
	set(0, x, y, u0, v0);
	set(1, x, y + h, u0, v1);
	set(2, x + w, y + h, u1, v1);
	set(3, x + w, y, u1, v0);
	quad_count_++;
}

void Draw::rect(float x, float y, float w, float h, Color c)
{
	push_quad(x, y, w, h, white_u_, white_v_, white_u_, white_v_, c);
}

void Draw::gradient_v(float x, float y, float w, float h, Color top, Color bottom)
{
	// Two stacked halves keep the batch uniform; the eye cannot tell at these
	// alpha deltas, and it avoids a second shader path for per-vertex colour.
	const int steps = 12;
	for(int i = 0; i < steps; i++)
	{
		const float t0 = static_cast<float>(i) / steps;
		const float t1 = static_cast<float>(i + 1) / steps;
		const float tm = (t0 + t1) * 0.5f;
		Color c{
			top.r + (bottom.r - top.r) * tm,
			top.g + (bottom.g - top.g) * tm,
			top.b + (bottom.b - top.b) * tm,
			top.a + (bottom.a - top.a) * tm,
		};
		rect(x, y + h * t0, w, h * (t1 - t0) + 1.0f, c);
	}
}

void Draw::rounded_rect(float x, float y, float w, float h, float radius, Color c)
{
	if(radius <= 0.5f)
	{
		rect(x, y, w, h, c);
		return;
	}
	if(radius > w * 0.5f)
		radius = w * 0.5f;
	if(radius > h * 0.5f)
		radius = h * 0.5f;

	const float r = radius;
	// Centre cross
	rect(x + r, y, w - 2 * r, h, c);
	rect(x, y + r, r, h - 2 * r, c);
	rect(x + w - r, y + r, r, h - 2 * r, c);

	// Corners: the quarter disc, mirrored by flipping UVs.
	push_quad(x, y, r, r, corner_u0_, corner_v0_, corner_u1_, corner_v1_, c);
	push_quad(x + w - r, y, r, r, corner_u1_, corner_v0_, corner_u0_, corner_v1_, c);
	push_quad(x, y + h - r, r, r, corner_u0_, corner_v1_, corner_u1_, corner_v0_, c);
	push_quad(x + w - r, y + h - r, r, r, corner_u1_, corner_v1_, corner_u0_, corner_v0_, c);
}

float Draw::line_height(Font font) const
{
	return fonts_[static_cast<int>(font)].line_h;
}

float Draw::text_width(Font font, const char *str) const
{
	const FontData &f = fonts_[static_cast<int>(font)];
	float w = 0.0f;
	for(const char *p = str; *p; p++)
	{
		const int gi = static_cast<unsigned char>(*p) - kGlyphFirst;
		if(gi < 0 || gi >= kGlyphCount)
			continue;
		w += f.glyphs[gi].xadv;
	}
	return w;
}

void Draw::text(float x, float y, Font font, Color c, const char *str)
{
	const FontData &f = fonts_[static_cast<int>(font)];
	float pen = x;
	const float baseline = y + f.ascent;
	for(const char *p = str; *p; p++)
	{
		const int gi = static_cast<unsigned char>(*p) - kGlyphFirst;
		if(gi < 0 || gi >= kGlyphCount)
			continue;
		const Glyph &g = f.glyphs[gi];
		if(g.w > 0.0f && g.h > 0.0f)
			push_quad(pen + g.xoff, baseline + g.yoff, g.w, g.h, g.u0, g.v0, g.u1, g.v1, c);
		pen += g.xadv;
	}
}

void Draw::text_centered(float x, float y, float w, Font font, Color c, const char *str)
{
	text(x + (w - text_width(font, str)) * 0.5f, y, font, c, str);
}

void Draw::end(int slot)
{
	cmdbuf_.clear();
	cmdbuf_.addMemory(data_pool_.block(), cmd_mem_.offset + cmd_slice_ * 0x2000, 0x2000);
	cmd_slice_ = (cmd_slice_ + 1) % gfx::Presenter::kMaxImages;

	dk::ImageView target{ presenter_->image(slot) };
	cmdbuf_.bindRenderTargets(&target);
	cmdbuf_.setViewports(0, { { 0.0f, 0.0f, static_cast<float>(presenter_->width()),
								  static_cast<float>(presenter_->height()), 0.0f, 1.0f } });
	cmdbuf_.setScissors(0, { { 0, 0, presenter_->width(), presenter_->height() } });
	cmdbuf_.clearColor(0, DkColorMask_RGBA, theme::bg_bottom.r, theme::bg_bottom.g, theme::bg_bottom.b, 1.0f);

	cmdbuf_.bindShaders(DkStageFlag_GraphicsMask, { &vsh_, &fsh_ });
	cmdbuf_.bindImageDescriptorSet(image_descs_.gpu, 4);
	cmdbuf_.bindSamplerDescriptorSet(sampler_descs_.gpu, 1);
	cmdbuf_.bindTextures(DkStage_Fragment, 0, dkMakeTextureHandle(0, 0));
	cmdbuf_.bindUniformBuffer(DkStage_Vertex, 0, uniform_.gpu, uniform_.size);

	dk::RasterizerState rasterizer;
	dk::ColorState color;
	dk::ColorWriteState color_write;
	dk::BlendState blend;	// default is standard src-alpha over
	color.setBlendEnable(0, true);
	cmdbuf_.bindRasterizerState(rasterizer);
	cmdbuf_.bindColorState(color);
	cmdbuf_.bindColorWriteState(color_write);
	cmdbuf_.bindBlendStates(0, { blend });

	cmdbuf_.bindVtxBuffer(0, vertices_.gpu, vertices_.size);
	cmdbuf_.bindVtxAttribState(kAttribs);
	cmdbuf_.bindVtxBufferState(kBuffers);
	cmdbuf_.draw(DkPrimitive_Quads, quad_count_ * 4, 1, 0, 0);

	presenter_->queue().submitCommands(cmdbuf_.finishList());
	presenter_->present(slot);
}

} // namespace hayai::ui
