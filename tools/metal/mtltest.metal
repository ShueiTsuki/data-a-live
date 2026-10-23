// mtltest's shaders, compiled to a metallib by build_plugin.sh (xcrun metal / metallib) and run
// through NativeMetal (metal2vulkan -> SPIR-V -> Turnip) on the Adreno 840.
#include <metal_stdlib>
using namespace metal;

// Part 2: every thread writes its index times 3 plus 0x1000 (as vktest's fill.comp).
kernel void fill(device uint *words [[buffer(0)]], uint i [[thread_position_in_grid]])
{
	words[i] = i * 3u + 0x1000u;
}

// Part 3: one triangle over the top-left half of a 64x64 target (Metal NDC: y up, texture row 0 at top).
struct Vertex {
	float4 position [[position]];
};

vertex Vertex tri_vs(uint vid [[vertex_id]])
{
	const float2 p[3] = { float2(-1.0, 1.0), float2(1.0, 1.0), float2(-1.0, -1.0) };
	Vertex v;
	v.position = float4(p[vid], 0.0, 1.0);
	return v;
}

fragment float4 tri_fs()
{
	return float4(0.0, 1.0, 0.0, 1.0);
}
