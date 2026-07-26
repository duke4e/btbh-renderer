struct FragmentInput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

float4 main(FragmentInput input) : SV_Target0
{
    const float radius = length(input.uv - 0.5f);
    const float alpha = saturate(0.55f - radius);
    return float4(0.15f, 0.65f, 1.0f, alpha);
}
