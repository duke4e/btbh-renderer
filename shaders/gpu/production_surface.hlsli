struct BTBHSurfaceVertexResult
{
    float3 position;
    float3 normal;
    // x = dissolve noise or water height
    // y = surface alpha
    // z = dissolve threshold or normalized animation time
    // w = surface variant
    float4 data;
};

float btbh_surface_hash11(float value)
{
    value = frac(value * 0.1031f);
    value *= value + 33.33f;
    value *= value + value;
    return frac(value);
}

float btbh_surface_hash21(float2 value)
{
    return frac(
        sin(dot(value, float2(127.1f, 311.7f))) * 43758.5453f
    );
}

float2 btbh_surface_hash22(float2 value)
{
    value = float2(
        dot(value, float2(127.1f, 311.7f)),
        dot(value, float2(269.5f, 183.3f))
    );
    return frac(sin(value) * 43758.5453f);
}

// Value noise with smooth interpolation: continuous in `value`, two channels in [0, 1).
float2 btbh_surface_smooth_noise22(float2 value)
{
    const float2 cell = floor(value);
    const float2 fraction = value - cell;
    const float2 blend = fraction * fraction * (3.0f - 2.0f * fraction);
    const float2 corner00 = btbh_surface_hash22(cell);
    const float2 corner10 = btbh_surface_hash22(cell + float2(1.0f, 0.0f));
    const float2 corner01 = btbh_surface_hash22(cell + float2(0.0f, 1.0f));
    const float2 corner11 = btbh_surface_hash22(cell + float2(1.0f, 1.0f));
    return lerp(
        lerp(corner00, corner10, blend.x),
        lerp(corner01, corner11, blend.x),
        blend.y
    );
}

float btbh_surface_value_noise(float2 value)
{
    const float2 cell = floor(value);
    const float2 fraction = frac(value);
    const float2 curve =
        fraction * fraction * (3.0f - 2.0f * fraction);
    const float a = btbh_surface_hash21(cell);
    const float b = btbh_surface_hash21(cell + float2(1.0f, 0.0f));
    const float c = btbh_surface_hash21(cell + float2(0.0f, 1.0f));
    const float d = btbh_surface_hash21(cell + float2(1.0f, 1.0f));
    return lerp(lerp(a, b, curve.x), lerp(c, d, curve.x), curve.y);
}

// Integer hash for the surface detail noise: stable at the large world coordinates of a city,
// where the sin-based hashes above lose precision.
float btbh_detail_hash(float2 cell)
{
    const int2 integer_cell = (int2)cell;
    uint hash = (uint)integer_cell.x * 0x8da6b343u ^ (uint)integer_cell.y * 0xd8163841u;
    hash ^= hash >> 15;
    hash *= 0x2c1b3c6du;
    hash ^= hash >> 12;
    hash *= 0x297a2d39u;
    hash ^= hash >> 15;
    return (float)(hash & 0x00ffffffu) * (1.0f / 16777216.0f);
}

float btbh_detail_noise(float2 value)
{
    const float2 cell = floor(value);
    const float2 fraction = value - cell;
    const float2 curve = fraction * fraction * (3.0f - 2.0f * fraction);
    const float a = btbh_detail_hash(cell);
    const float b = btbh_detail_hash(cell + float2(1.0f, 0.0f));
    const float c = btbh_detail_hash(cell + float2(0.0f, 1.0f));
    const float d = btbh_detail_hash(cell + float2(1.0f, 1.0f));
    return lerp(lerp(a, b, curve.x), lerp(c, d, curve.x), curve.y);
}

// Three octaves around 0.5; each octave fades out once its cells shrink below about two pixels
// (footprint = noise cells per pixel of the first octave).
float btbh_detail_fbm(float2 value, float footprint)
{
    float result = 0.0f;
    float amplitude = 0.5f;
    float scale = 1.0f;
    [unroll]
    for (int octave = 0; octave < 3; ++octave)
    {
        const float fade = saturate((0.6f - footprint * scale) * 2.5f);
        result += (btbh_detail_noise(value * scale + octave * 17.31f) - 0.5f) * amplitude * fade;
        amplitude *= 0.5f;
        scale *= 2.03f;
    }
    return result;
}

float btbh_surface_fbm(float2 value)
{
    float result = 0.0f;
    float amplitude = 0.5f;
    float frequency = 1.0f;
    [unroll]
    for (int index = 0; index < 3; ++index)
    {
        result +=
            btbh_surface_value_noise(value * frequency) * amplitude;
        amplitude *= 0.5f;
        frequency *= 2.1f;
    }
    return result;
}

float2 btbh_surface_wave_dx(
    float2 position,
    float2 direction,
    float frequency,
    float time_shift
)
{
    const float phase =
        dot(direction, position) * frequency + time_shift;
    const float wave = exp(sin(phase) - 1.0f);
    return float2(wave, -wave * cos(phase));
}

float btbh_surface_waves(
    float2 position,
    int iterations,
    float time
)
{
    const float wave_phase_shift = length(position) * 0.1f;
    float iteration = 0.0f;
    float frequency = 0.32f;
    float time_multiplier = 2.0f;
    float weight = 1.0f;
    float value_sum = 0.0f;
    float weight_sum = 0.0f;

    [loop]
    for (int index = 0; index < iterations; ++index)
    {
        const float2 direction =
            float2(sin(iteration), cos(iteration));
        const float2 wave = btbh_surface_wave_dx(
            position,
            direction,
            frequency,
            time * time_multiplier + wave_phase_shift
        );
        position += direction * wave.y * weight * 0.38f;
        value_sum += wave.x * weight;
        weight_sum += weight;
        weight = lerp(weight, 0.0f, 0.22f);
        frequency *= 1.18f;
        time_multiplier *= 1.07f;
        iteration += 1232.399963f;
    }
    return value_sum / max(0.001f, weight_sum);
}

float3 btbh_surface_wave_normal(
    float2 position,
    float epsilon,
    float depth,
    float time
)
{
    const float2 offset = float2(epsilon, 0.0f);
    const float height =
        btbh_surface_waves(position, 8, time) * depth;
    const float3 center =
        float3(position.x, height, position.y);
    const float3 dx = center - float3(
        position.x - epsilon,
        btbh_surface_waves(position - offset, 8, time) * depth,
        position.y
    );
    const float3 dz = center - float3(
        position.x,
        btbh_surface_waves(position + offset.yx, 8, time) * depth,
        position.y + epsilon
    );
    return normalize(cross(dx, dz));
}

float3 btbh_surface_rotate_axis_angle(
    float3 value,
    float3 axis,
    float angle
)
{
    const float cosine = cos(angle);
    const float sine = sin(angle);
    return
        value * cosine +
        cross(axis, value) * sine +
        axis * dot(axis, value) * (1.0f - cosine);
}

BTBHSurfaceVertexResult btbh_apply_surface_vertex(
    float3 position,
    float3 normal,
    float4 instance_custom,
    uint surface_variant,
    float surface_time,
    float4 surface_parameters0,
    float4 surface_parameters1
)
{
    BTBHSurfaceVertexResult result;
    result.position = position;
    result.normal = normal;
    result.data = float4(0.0f, 1.0f, 0.0f, (float)surface_variant);

    if (surface_variant == 2u)
    {
        const float2 scaled_position = position.xz * 7.5f;
        const float2 warp = float2(
            sin(scaled_position.y * 0.23f + surface_time * 0.20f),
            cos(scaled_position.x * 0.17f + surface_time * 0.28f)
        );
        const float wave =
            btbh_surface_waves(
                scaled_position + warp * 2.6f,
                5,
                surface_time
            ) -
            0.42f;
        result.position.y += wave * 0.08f;
        result.data.x = wave;
    }
    else if (surface_variant == 3u && instance_custom.x > 0.0f)
    {
        const float duration = max(0.01f, instance_custom.z);
        const float time = saturate(instance_custom.x / duration);
        const float2 noise_uv =
            position.xz * 2.5f +
            float2(
                instance_custom.y * 7.3f,
                instance_custom.y * 13.1f
            );
        const float dissolve_noise = btbh_surface_fbm(noise_uv);
        const float drift_seed = btbh_surface_hash11(
            instance_custom.y +
            position.x * 3.7f +
            position.z * 5.1f
        );
        const float drift_angle = drift_seed * 6.2832f;
        const float drift_amount = 0.3f + drift_seed * 0.4f;
        const float drift_time = time * time;
        result.position.x +=
            cos(drift_angle) * drift_amount * drift_time * 0.6f;
        result.position.z +=
            sin(drift_angle) * drift_amount * drift_time * 0.6f;
        result.position.y +=
            time * 0.4f * (0.5f + drift_seed * 0.5f);
        result.data.x = dissolve_noise;
        result.data.y =
            1.0f - smoothstep(0.55f, 1.0f, time);
        result.data.z = time * 1.2f - 0.1f;
    }
    else if (surface_variant == 4u && instance_custom.y > 0.0f)
    {
        // Collapse: the structure pancakes onto its base (local y = 0), sinks into the ground
        // and crumbles along a smooth noise field. Every term is continuous over the model, so
        // faces bend instead of tearing. Shard cells per vertex used to send the corners of a
        // large low-poly facade different ways, stretching it into slabs as big as the building
        // that rose toward the camera and threw huge shadows over the map.
        const float duration = max(0.01f, surface_parameters0.x);
        const float time = saturate(instance_custom.y / duration);
        const float local_scale = max(1.0f, instance_custom.w);
        const float cell_size = max(
            0.01f,
            surface_parameters0.y * local_scale
        );
        const float spread_time =
            1.0f - (1.0f - time) * (1.0f - time);
        const float drop =
            -0.5f *
            surface_parameters0.z *
            instance_custom.y *
            instance_custom.y *
            local_scale;
        // Upper floors crumble further than the base; 4 local units is the reference extent.
        const float height_share =
            0.3f + 0.7f * saturate(position.y / (4.0f * local_scale));
        const float2 crumble =
            (btbh_surface_smooth_noise22(
                position.xz / (cell_size * 2.0f) +
                instance_custom.z * 0.37f
            ) - 0.5f) *
            cell_size *
            surface_parameters0.w *
            height_share *
            spread_time;
        result.position = float3(
            position.x + crumble.x,
            position.y * (1.0f - 0.55f * spread_time) + drop,
            position.z + crumble.y
        );
        result.data.y =
            1.0f - smoothstep(0.6f, 1.0f, time);
        result.data.z = time;
    }

    return result;
}

// Ordered coverage keeps fading debris in the opaque RC/depth path.
bool btbh_surface_coverage_visible(float coverage, float2 pixel_position)
{
    const uint2 pixel = uint2(pixel_position) & 3u;
    const uint low = ((pixel.x & 1u) ^ (pixel.y & 1u)) * 2u + (pixel.y & 1u);
    const uint high = ((pixel.x >> 1u) ^ (pixel.y >> 1u)) * 2u + (pixel.y >> 1u);
    const float threshold = (float(low * 4u + high) + 0.5f) / 16.0f;
    return coverage >= threshold;
}

bool btbh_surface_shadow_visible(float4 surface_data, float2 pixel_position)
{
    const uint variant = (uint)(surface_data.w + 0.5f);
    if (variant == 3u)
    {
        return
            surface_data.x >= surface_data.z &&
            surface_data.y >= 0.01f;
    }
    if (variant == 4u)
    {
        return btbh_surface_coverage_visible(surface_data.y, pixel_position);
    }
    return true;
}
