RWTexture2D<float4> output_texture : register(u0, space1);

[numthreads(8, 8, 1)]
void main(uint3 dispatch_id : SV_DispatchThreadID)
{
    const float2 uv = (float2(dispatch_id.xy) + 0.5f) / float2(64.0f, 64.0f);
    output_texture[dispatch_id.xy] = float4(uv.x, uv.y, 0.25f, 1.0f);
}
