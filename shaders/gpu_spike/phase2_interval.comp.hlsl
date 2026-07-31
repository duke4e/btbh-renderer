StructuredBuffer<float4> interval_input : register(t0, space0);
RWTexture2D<float4> interval_output : register(u0, space1);

[numthreads(8, 8, 1)]
void main(uint3 dispatch_id : SV_DispatchThreadID)
{
    const uint interval_index = (dispatch_id.x + dispatch_id.y * 64u) & 15u;
    const float4 interval = interval_input[interval_index];
    const float center = (interval.x + interval.y) * 0.5f;
    const float radius = max(interval.y - interval.x, 0.0f) * 0.5f;
    interval_output[dispatch_id.xy] = float4(
        center,
        radius,
        interval.z,
        1.0f
    );
}
