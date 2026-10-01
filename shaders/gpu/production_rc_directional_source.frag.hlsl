struct FragmentInput
{
    float4 position : SV_Position;
    float2 local_position : TEXCOORD0;
    float3 radiance : TEXCOORD1;
    nointerpolation float angular_coverage : TEXCOORD2;
    nointerpolation uint is_spot : TEXCOORD3;
};

float4 main(FragmentInput input) : SV_Target0
{
    clip(float(input.is_spot) - 0.5f);
    const float radial_distance = length(input.local_position);
    const float radial_coverage =
        1.0f - smoothstep(0.72f, 1.0f, radial_distance);
    const float coverage = radial_coverage * input.angular_coverage;
    clip(coverage - 0.0001f);
    return float4(input.radiance * coverage, coverage);
}
