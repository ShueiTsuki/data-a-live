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

// Part 5: a full-screen triangle that samples texture 0 with nearest filtering (an ASTC 4x4 image,
// bb27-bb29: the App Library's ASTC images come out transparent).
struct VertexUV {
	float4 position [[position]];
	float2 uv;
};

vertex VertexUV quad_vs(uint vid [[vertex_id]])
{
	const float2 p[3] = { float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0) };
	VertexUV v;
	v.position = float4(p[vid], 0.0, 1.0);
	v.uv = float2((p[vid].x + 1.0) * 0.5, (1.0 - p[vid].y) * 0.5);
	return v;
}

fragment float4 tex_fs(VertexUV in [[stage_in]], texture2d<float> t [[texture(0)]])
{
	constexpr sampler s(filter::nearest);
	return t.sample(s, in.uv);
}

// Part 6: the same with linear filtering, in pixel coordinates with clamp-to-zero, and at texture index 3.
fragment float4 tex_lin_fs(VertexUV in [[stage_in]], texture2d<float> t [[texture(0)]])
{
	constexpr sampler s(filter::linear, address::clamp_to_edge);
	return t.sample(s, in.uv);
}

fragment float4 tex_px_fs(VertexUV in [[stage_in]], texture2d<float> t [[texture(0)]])
{
	constexpr sampler s(coord::pixel, address::clamp_to_zero, filter::linear);
	return t.sample(s, in.uv * float2(t.get_width(), t.get_height()));
}

fragment float4 tex3_fs(VertexUV in [[stage_in]], texture2d<float> t [[texture(3)]])
{
	constexpr sampler s(filter::nearest);
	return t.sample(s, in.uv);
}
