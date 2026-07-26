#version 460

// Full-screen (or aspect-fitted) quad. The vertices are computed on the CPU
// when the stream geometry changes, so there is no transform to apply here.

layout (location = 0) in vec2 in_pos;
layout (location = 1) in vec2 in_uv;

layout (location = 0) out vec2 out_uv;

void main()
{
	gl_Position = vec4(in_pos, 0.0, 1.0);
	out_uv = in_uv;
}
