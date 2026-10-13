// Debug view for the temporal upscaler's inputs (BTBH_DEBUG_MOTION_VECTORS=1): the frame, dimmed, under
// its motion vectors at the output size. Hue is the direction a pixel moved since the previous frame
// (right red, left cyan, down green, up magenta), brightness its speed in render pixels; red marks the
// reactive mask. A held camera leaves the static city dark and the moving bodies and shots lit.
// BTBH_DEBUG_MOTION_VECTORS=2 shows instead how far each pixel's colour is from the previous frame's at the
// place its motion vector points to: black where the motion vectors are right (best without jitter,
// BTBH_FSR3_JITTER=off).
Texture2D<float4> color_texture : register(t0, space0);
SamplerState color_sampler : register(s0, space0);
Texture2D<float4> motion_texture : register(t1, space0);
SamplerState motion_sampler : register(s1, space0);
Texture2D<float> reactive_texture : register(t2, space0);
SamplerState reactive_sampler : register(s2, space0);
Texture2D<float4> previous_color_texture : register(t3, space0);
SamplerState previous_color_sampler : register(s3, space0);
[[vk::image_format("rgba16f")]] RWTexture2D<float4> output_texture : register(u0, space1);

cbuffer MotionViewUniforms : register(b0, space2)
{
    // 1 / width, 1 / height, width, height of the output.
    float4 output_metrics;
    // Render width and height (motion vectors are in UV units), pixels of motion at full brightness,
    // 1 for the reprojection error view.
    float4 motion_metrics;
};

[numthreads(8, 8, 1)]
void main(uint2 texel : SV_DispatchThreadID)
{
    if (texel.x >= uint(output_metrics.z) || texel.y >= uint(output_metrics.w))
    {
        return;
    }
    const float2 uv = (float2(texel) + 0.5f) * output_metrics.xy;
    const float3 color = color_texture.SampleLevel(color_sampler, uv, 0.0f).rgb;
    const float2 motion_uv = motion_texture.SampleLevel(motion_sampler, uv, 0.0f).xy;
    if (motion_metrics.w > 0.5f)
    {
        const float3 previous =
            previous_color_texture.SampleLevel(previous_color_sampler, uv + motion_uv, 0.0f).rgb;
        output_texture[texel] = float4(saturate(abs(color - previous) * 4.0f), 1.0f);
        return;
    }
    const float2 motion = motion_uv * motion_metrics.xy;
    const float reactive = reactive_texture.SampleLevel(reactive_sampler, uv, 0.0f);
    const float speed = length(motion);
    const float2 direction = speed > 0.0001f ? motion / speed : float2(0.0f, 0.0f);
    const float brightness = saturate(speed / max(motion_metrics.z, 0.0001f));
    const float3 hue = saturate(float3(
        0.5f + 0.5f * direction.x,
        0.5f + 0.5f * direction.y,
        0.5f - 0.5f * direction.y
    ));
    float3 result = color * 0.25f + hue * brightness;
    result = lerp(result, float3(1.0f, 0.1f, 0.1f), saturate(reactive) * 0.6f);
    output_texture[texel] = float4(result, 1.0f);
}
