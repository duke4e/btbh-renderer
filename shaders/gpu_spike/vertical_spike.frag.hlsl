struct FragmentInput
{
    float4 position : SV_Position;
    float3 color : TEXCOORD0;
};

struct GBufferOutput
{
    float4 albedo : SV_Target0;
    float4 normal_roughness : SV_Target1;
    float4 emissive_hdr : SV_Target2;
};

GBufferOutput main(FragmentInput input)
{
    GBufferOutput output;
    output.albedo = float4(input.color, 1.0f);
    output.normal_roughness = float4(0.5f, 0.5f, 1.0f, 0.35f);
    output.emissive_hdr = float4(input.color * 2.0f, 1.0f);
    return output;
}
