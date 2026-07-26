#version 460

// 2D UI batch. Positions arrive in pixels; the uniform converts to clip space
// so layout code never thinks about NDC.

layout (location = 0) in vec2 in_pos;
layout (location = 1) in vec2 in_uv;
layout (location = 2) in vec4 in_color;

layout (std140, binding = 0) uniform Screen
{
	vec4 inv_size;	// xy = 2/width, 2/height
};

layout (location = 0) out vec2 out_uv;
layout (location = 1) out vec4 out_color;

void main()
{
	gl_Position = vec4(in_pos.x * inv_size.x - 1.0, 1.0 - in_pos.y * inv_size.y, 0.0, 1.0);
	out_uv = in_uv;
	out_color = in_color;
}
