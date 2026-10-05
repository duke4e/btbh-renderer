// How a ray crossing the RC source field treats an emitter sample.
//
// An emitter adds its light and absorbs what lies behind it only in proportion to how bright
// it is against that background. A source that stopped every ray outright, however dim, cast a
// dark tail away from the sun and sky: fading impacts, sparks, shot discs and budget-dimmed
// clusters flickered across the ground as moving shadows. The background is the sky and sun
// this ray would reach. A source at least twice as bright as it stays opaque, as before, so its
// light is still counted once; a source as bright as the background leaves it unchanged, and a
// dimmer one darkens it by at most about 4%.

static const float hrc_source_background_floor = 0.05f;
static const float hrc_source_transmittance_cutoff = 0.004f;

float hrc_source_luminance(float3 value)
{
    return dot(value, float3(0.2126f, 0.7152f, 0.0722f));
}

// `premultiplied` is the sample's radiance times its coverage, as the source passes write it.
float hrc_source_opacity(
    float3 premultiplied,
    float coverage,
    float background_luminance
)
{
    if (coverage <= 0.001f)
    {
        return 0.0f;
    }
    const float emitted = hrc_source_luminance(premultiplied) / coverage;
    const float ratio =
        emitted / max(background_luminance, hrc_source_background_floor);
    return coverage * saturate((ratio - 0.5f) / 1.5f);
}

// Composites one source sample over the radiance gathered so far along the ray. Returns true
// once the ray is effectively opaque and the march can stop.
bool hrc_source_composite(
    float4 source,
    float4 directed_source,
    float background_luminance,
    inout float3 radiance,
    inout float transmittance
)
{
    const float coverage = saturate(max(source.a, directed_source.a));
    const float3 premultiplied = source.rgb + directed_source.rgb;
    const float opacity =
        hrc_source_opacity(premultiplied, coverage, background_luminance);
    if (opacity <= 0.0f)
    {
        return false;
    }
    radiance += transmittance * (opacity / coverage) * premultiplied;
    transmittance *= 1.0f - opacity;
    if (transmittance <= hrc_source_transmittance_cutoff)
    {
        transmittance = 0.0f;
        return true;
    }
    return false;
}
