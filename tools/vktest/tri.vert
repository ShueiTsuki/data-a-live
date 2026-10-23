#version 450
// vktest part 5: one triangle over the top-left half of the 64x64 target (Vulkan NDC, y down).
void main()
{
	vec2 p[3] = vec2[](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(-1.0, 1.0));
	gl_Position = vec4(p[gl_VertexIndex], 0.0, 1.0);
}
