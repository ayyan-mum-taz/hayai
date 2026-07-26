#version 460

// NV12 -> RGB. Both samplers alias the NVDEC output surface directly: luma is
// the R8 plane, chroma is the interleaved RG8 plane at half resolution. The
// GPU's texture units do the chroma upsample for free, which is why nothing
// here needs a scaling pass.

layout (location = 0) in vec2 in_uv;
layout (location = 0) out vec4 out_color;

layout (binding = 0) uniform sampler2D luma_tex;
layout (binding = 1) uniform sampler2D chroma_tex;

layout (std140, binding = 0) uniform ColorConv
{
	vec4 mat_col0;
	vec4 mat_col1;
	vec4 mat_col2;
	vec4 offset;
};

void main()
{
	float y = texture(luma_tex, in_uv).r;
	vec2 uv = texture(chroma_tex, in_uv).rg;

	vec3 yuv = vec3(y, uv.x, uv.y) - offset.xyz;
	mat3 m = mat3(mat_col0.xyz, mat_col1.xyz, mat_col2.xyz);

	out_color = vec4(m * yuv, 1.0);
}
