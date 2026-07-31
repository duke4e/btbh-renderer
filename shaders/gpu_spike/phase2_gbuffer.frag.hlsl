struct FragmentInput
{
    float4 position : SV_Position;
    float3 color : TEXCOORD0;
    float3 normal : TEXCOORD1;
};

struct GBufferOutput
{
    float4 albedo_ao : SV_Target0;
    float4 normal_material : SV_Target1;
    float4 emission_specular : SV_Target2;
};

GBufferOutput main(FragmentInput input)
{
    const float3 normal = normalize(input.normal);
    GBufferOutput output;
    output.albedo_ao = float4(input.color, 1.0f);
    output.normal_material = float4(
        normal.xy * 0.5f + 0.5f,
        0.42f,
        0.08f
    );
    output.emission_specular = float4(input.color * 0.04f, 0.5f);
    return output;
}
