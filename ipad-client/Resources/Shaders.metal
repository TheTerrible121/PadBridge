#include <metal_stdlib>
using namespace metal;

struct RasterData {
    float4 position [[position]];
    float2 textureCoordinate;
};

vertex RasterData padbridgeVertex(uint vertexID [[vertex_id]]) {
    constexpr float2 positions[] = {
        float2(-1.0, -1.0), float2(1.0, -1.0),
        float2(-1.0,  1.0), float2(1.0,  1.0)
    };
    constexpr float2 coordinates[] = {
        float2(0.0, 1.0), float2(1.0, 1.0),
        float2(0.0, 0.0), float2(1.0, 0.0)
    };
    RasterData out;
    out.position = float4(positions[vertexID], 0.0, 1.0);
    out.textureCoordinate = coordinates[vertexID];
    return out;
}

fragment half4 padbridgeNV12Fragment(RasterData in [[stage_in]],
                                      texture2d<float> luma [[texture(0)]],
                                      texture2d<float> chroma [[texture(1)]]) {
    constexpr sampler videoSampler(coord::normalized, address::clamp_to_edge,
                                   filter::linear);
    const float y = luma.sample(videoSampler, in.textureCoordinate).r;
    const float2 uv = chroma.sample(videoSampler, in.textureCoordinate).rg - float2(0.5);

    // BT.709 video-range YCbCr to RGB.
    const float scaledY = 1.164383 * (y - 0.0625);
    const float3 rgb = float3(
        scaledY + 1.792741 * uv.y,
        scaledY - 0.213249 * uv.x - 0.532909 * uv.y,
        scaledY + 2.112402 * uv.x
    );
    return half4(half3(saturate(rgb)), 1.0h);
}

