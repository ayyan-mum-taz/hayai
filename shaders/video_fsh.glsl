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

// Interleaved gradient noise (Jimenez). One dot product and a fract, and it
// decorrelates far better than a hash at this cost.
float ign(vec2 p)
{
	return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

void main()
{
	float y = texture(luma_tex, in_uv).r;
	vec2 uv = texture(chroma_tex, in_uv).rg;

	vec3 yuv = vec3(y, uv.x, uv.y) - offset.xyz;
	mat3 m = mat3(mat_col0.xyz, mat_col1.xyz, mat_col2.xyz);
	vec3 rgb = m * yuv;

	// Dither by slightly less than one 8-bit step, zero-mean.
	//
	// The stream is 8-bit YUV and we resolve to an 8-bit surface, so smooth
	// gradients -- skies, dark interiors, fog -- quantise into visible bands.
	// This is most obvious at lower stream resolutions, where each source pixel
	// covers more of the panel. Sub-LSB noise converts the banding into
	// dither that the eye integrates away, and costs about three instructions.
	const float amount = 0.6 / 255.0;
	rgb += amount * ign(gl_FragCoord.xy) - (amount * 0.5);

	out_color = vec4(rgb, 1.0);
}
