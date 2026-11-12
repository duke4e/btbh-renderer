#ifndef BTBH_DIG_MASK_HLSLI
#define BTBH_DIG_MASK_HLSLI

// RFC 0007: the dig mask of an SDF terrain (BTBHGPUSceneGeometryBatch.dug_by). One texel per cell column of the
// terrain: the height (in cells above its floor) of the patch's top surface where a patch has taken over from the
// host's ground, 65504 elsewhere. A fragment of a dug batch is hidden when it doesn't lie more than a threshold
// under that surface, at its flat world position (before btbh_world_bend), so a curved world keeps the hole under
// its patch.
//
// origin: x, z the mask's corner (world), y the terrain's floor (world y), w the cell size.
// scale: x, z 1 / the mask's extent along x and z (0: no mask, the batch draws as before), y 1 / the cell size,
//        w the threshold in cells.
bool btbh_dig_mask_hidden(Texture2D<float> mask, SamplerState mask_sampler, float4 origin, float4 scale, float3 world)
{
    if (scale.x <= 0.0f)
    {
        return false;
    }
    const float2 uv = (world.xz - origin.xy) * scale.xz;
    if (uv.x < 0.0f || uv.y < 0.0f || uv.x > 1.0f || uv.y > 1.0f)
    {
        return false;
    }
    const float top = mask.SampleLevel(mask_sampler, uv, 0.0f);
    return (world.y - origin.z) * scale.y > top - scale.w;
}

#endif
