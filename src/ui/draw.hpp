// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
#pragma once

#include "gfx/pool.hpp"
#include "gfx/presenter.hpp"

#include <deko3d.hpp>

#include <cstdint>

namespace hayai::ui {

struct Color
{
	float r, g, b, a;
	constexpr Color with_alpha(float alpha) const { return { r, g, b, alpha }; }
};

// Dark, calm, one accent. The UI should read instantly in a lit room and
// disappear behind the game the moment a stream starts.
namespace theme {
constexpr Color bg_top{ 0.055f, 0.055f, 0.086f, 1.0f };
constexpr Color bg_bottom{ 0.027f, 0.027f, 0.047f, 1.0f };
constexpr Color card{ 1.0f, 1.0f, 1.0f, 0.05f };
constexpr Color accent{ 0.42f, 0.40f, 1.0f, 1.0f };
constexpr Color accent_dim{ 0.42f, 0.40f, 1.0f, 0.18f };
constexpr Color text{ 0.94f, 0.95f, 0.98f, 1.0f };
constexpr Color text_dim{ 0.60f, 0.62f, 0.70f, 1.0f };
constexpr Color good{ 0.35f, 0.85f, 0.55f, 1.0f };
constexpr Color warn{ 0.98f, 0.74f, 0.35f, 1.0f };
constexpr Color bad{ 0.98f, 0.42f, 0.45f, 1.0f };
} // namespace theme

enum class Font
{
	Body,	// 22 px
	Title,	// 34 px
	Small,	// 17 px
};

// Immediate-mode 2D renderer over deko3d: one atlas, one batched draw.
//
// Everything the UI needs is single-channel coverage -- a white texel, a
// quarter-disc for rounded corners, and glyphs -- so it all lives in one R8
// atlas and the whole frame collapses into a single draw call.
class Draw
{
public:
	Draw() = default;
	~Draw() { destroy(); }
	Draw(const Draw &) = delete;
	Draw &operator=(const Draw &) = delete;

	bool create(gfx::Presenter &presenter);
	void destroy();

	void begin();
	void end(int slot);	// records + submits + presents

	void rect(float x, float y, float w, float h, Color c);
	void gradient_v(float x, float y, float w, float h, Color top, Color bottom);
	void rounded_rect(float x, float y, float w, float h, float radius, Color c);

	void text(float x, float y, Font font, Color c, const char *str);
	float text_width(Font font, const char *str) const;
	float line_height(Font font) const;

	// Convenience: draw text centred in a box.
	void text_centered(float x, float y, float w, Font font, Color c, const char *str);

	unsigned width() const { return presenter_ ? presenter_->width() : 0; }
	unsigned height() const { return presenter_ ? presenter_->height() : 0; }

	struct Vertex
	{
		float pos[2];
		float uv[2];
		float color[4];
	};

private:
	static constexpr unsigned kMaxQuads = 4096;
	static constexpr unsigned kAtlasW = 1024;
	static constexpr unsigned kAtlasH = 1024;
	static constexpr unsigned kCornerPx = 48;	// quarter-disc mask resolution
	static constexpr unsigned kFontCount = 3;
	static constexpr int kGlyphFirst = 32;
	static constexpr int kGlyphCount = 96;	// printable ASCII

	struct Glyph
	{
		float u0, v0, u1, v1;
		float xoff, yoff;
		float xadv;
		float w, h;
	};

	struct FontData
	{
		Glyph glyphs[kGlyphCount]{};
		float ascent = 0.0f;
		float line_h = 0.0f;
	};

	bool build_atlas();
	bool upload_atlas(const uint8_t *pixels);
	bool load_shader(const char *path, dk::Shader &out);
	void push_quad(float x, float y, float w, float h,
		float u0, float v0, float u1, float v1, Color c);

	gfx::Presenter *presenter_ = nullptr;

	gfx::Pool code_pool_;
	gfx::Pool data_pool_;
	gfx::Pool desc_pool_;
	gfx::Pool atlas_pool_;

	dk::Shader vsh_;
	dk::Shader fsh_;
	dk::CmdBuf cmdbuf_ = nullptr;
	dk::Image atlas_img_;

	gfx::Pool::Alloc vertices_{};
	gfx::Pool::Alloc uniform_{};
	gfx::Pool::Alloc image_descs_{};
	gfx::Pool::Alloc sampler_descs_{};
	gfx::Pool::Alloc cmd_mem_{};
	unsigned cmd_slice_ = 0;

	Vertex *verts_ = nullptr;
	unsigned quad_count_ = 0;

	static FontData s_fonts_[kFontCount];
	FontData *fonts_ = s_fonts_;
	// White texel and corner mask locations in the atlas
	float white_u_ = 0.0f, white_v_ = 0.0f;
	float corner_u0_ = 0.0f, corner_v0_ = 0.0f, corner_u1_ = 0.0f, corner_v1_ = 0.0f;
};

} // namespace hayai::ui
