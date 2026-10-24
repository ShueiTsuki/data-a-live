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

// Part 7: the sweep samples texture 0 with the sampler state bound at 0, at uv * scale (1 for normalized
// coordinates, the texture's size for pixel coordinates).
fragment float4 sweep_fs(VertexUV in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]],
    constant float2 &scale [[buffer(0)]])
{
	return t.sample(s, in.uv * scale);
}

// Part 7: one 1x1 texture at each of the 16 indices; `which` picks the one to read.
fragment float4 index_fs(VertexUV in [[stage_in]], constant uint &which [[buffer(0)]],
    texture2d<float> t0 [[texture(0)]], texture2d<float> t1 [[texture(1)]], texture2d<float> t2 [[texture(2)]],
    texture2d<float> t3 [[texture(3)]], texture2d<float> t4 [[texture(4)]], texture2d<float> t5 [[texture(5)]],
    texture2d<float> t6 [[texture(6)]], texture2d<float> t7 [[texture(7)]], texture2d<float> t8 [[texture(8)]],
    texture2d<float> t9 [[texture(9)]], texture2d<float> t10 [[texture(10)]], texture2d<float> t11 [[texture(11)]],
    texture2d<float> t12 [[texture(12)]], texture2d<float> t13 [[texture(13)]], texture2d<float> t14 [[texture(14)]],
    texture2d<float> t15 [[texture(15)]])
{
	constexpr sampler s(filter::nearest);
	switch (which) {
	case 0: return t0.sample(s, in.uv);
	case 1: return t1.sample(s, in.uv);
	case 2: return t2.sample(s, in.uv);
	case 3: return t3.sample(s, in.uv);
	case 4: return t4.sample(s, in.uv);
	case 5: return t5.sample(s, in.uv);
	case 6: return t6.sample(s, in.uv);
	case 7: return t7.sample(s, in.uv);
	case 8: return t8.sample(s, in.uv);
	case 9: return t9.sample(s, in.uv);
	case 10: return t10.sample(s, in.uv);
	case 11: return t11.sample(s, in.uv);
	case 12: return t12.sample(s, in.uv);
	case 13: return t13.sample(s, in.uv);
	case 14: return t14.sample(s, in.uv);
	default: return t15.sample(s, in.uv);
	}
}
