struct FragmentInput
{
    float4 position : SV_Position;
    float2 local_position : TEXCOORD0;
    float3 radiance : TEXCOORD1;
    nointerpolation uint shape : TEXCOORD2;
    nointerpolation float4 spot_parameters : TEXCOORD3;
};

float4 main(FragmentInput input) : SV_Target0
{
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
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
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
    return float4(input.radiance * coverage, coverage);
}
