// Fills an R32_UINT texture with one value: FSR 3.1's reconstructed previous depth (the bits of 1.0,
// the far plane, before the prepare-inputs pass keeps each texel's nearest depth with an atomic
// minimum) and its downsampler's atomic counter (0). SDL GPU clears only colour targets in render
// passes, whose clear value is a float.
[[vk::image_format("r32ui")]] RWTexture2D<uint> target_texture : register(u0, space1);

cbuffer ClearUniforms : register(b0, space2)
{
    // x the value, y and z the width and height to fill.
    uint4 clear_value_extent;
};

[numthreads(8, 8, 1)]
void main(uint2 texel : SV_DispatchThreadID)
{
    if (texel.x < clear_value_extent.y && texel.y < clear_value_extent.z)
    {
        target_texture[texel] = clear_value_extent.x;
    }
}
