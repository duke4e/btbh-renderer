struct FragmentInput
{
    float4 position : SV_Position;
    float2 local_position : TEXCOORD0;
    float3 radiance : TEXCOORD1;
    nointerpolation uint shape : TEXCOORD2;
    nointerpolation float4 spot_parameters : TEXCOORD3;
    nointerpolation float top : TEXCOORD4;
};

struct FragmentOutput
{
    float4 source : SV_Target0;
    // RFC 0010: the source's top, blended with MAX into the R16F tops texture. -65504 (the lowest half float, what
    // the texture is cleared to) leaves it as it is.
    float top : SV_Target1;
};

FragmentOutput main(FragmentInput input)
{
    FragmentOutput output;
    output.top = input.top;
    float coverage = 1.0f;
    const float inner_cutoff = input.spot_parameters.x;
    const float outer_cutoff = input.spot_parameters.y;
    const bool is_spot =
        inner_cutoff > outer_cutoff &&
        outer_cutoff > 0.0001f &&
        inner_cutoff <= 1.0f;
    if (is_spot)
    {
        // Spots are emitted only into the angular source atlas. Keeping them
        // out of this additive isotropic field prevents a symmetric halo at
        // the source. A zero contribution also keeps generated MSL control
        // flow fully initialized after SPIR-V optimization.
        output.source = float4(0.0f, 0.0f, 0.0f, 0.0f);
        // Its top lies over the disk its directional light is drawn in (production_rc_directional_source).
        output.top = length(input.local_position) < 1.0f ? input.top : -65504.0f;
        return output;
    }
    else if (input.shape == 0u)
    {
        const float distance_from_center = length(input.local_position);
        coverage = 1.0f - smoothstep(0.82f, 1.0f, distance_from_center);
        clip(coverage - 0.0001f);
    }
    else if (input.shape == 2u)
    {
        const float end_distance = abs(input.local_position.x);
        const float side_distance = abs(input.local_position.y);
        coverage =
            (1.0f - smoothstep(0.86f, 1.0f, end_distance)) *
            (1.0f - smoothstep(0.72f, 1.0f, side_distance));
        clip(coverage - 0.0001f);
    }
    output.source = float4(input.radiance * coverage, coverage);
    return output;
}
