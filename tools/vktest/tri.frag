#version 450
// vktest part 5: green, so a covered pixel reads 0xff00ff00 (RGBA8 little-endian).
layout(location = 0) out vec4 color;
void main()
{
	color = vec4(0.0, 1.0, 0.0, 1.0);
}
