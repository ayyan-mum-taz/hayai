#version 460

// One atlas serves everything: a white texel for solid fills, a quarter-disc
// mask for rounded corners, and glyph coverage. All are single-channel, so the
// sampled value is alpha and the vertex colour supplies the rest.

layout (location = 0) in vec2 in_uv;
layout (location = 1) in vec4 in_color;
layout (location = 0) out vec4 out_color;

layout (binding = 0) uniform sampler2D atlas;

void main()
{
	float a = texture(atlas, in_uv).r;
	out_color = vec4(in_color.rgb, in_color.a * a);
}
