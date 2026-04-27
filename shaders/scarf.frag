#version 450

layout(set = 2, binding = 0) uniform sampler2D u_image;

layout(set = 3, binding = 0) uniform U {
    vec2 resolution;
    float time;
    float loop_period;  // seconds in one full cycle of every animated effect
} u;

const float TAU = 6.2831853;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

const vec3 BG = vec3(0.0);

float hash21(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

// Returns ~1 for warm, saturated, mid-bright pixels (the gold leaf), and
// 0 for whites/blacks/cool colors. Tuned for the SolidarityScarf palette.
float goldness(vec3 c) {
    float yb = (c.r + c.g) * 0.5 - c.b;          // yellow vs blue
    float l  = luma(c);
    float warm = smoothstep(0.12, 0.40, yb);
    float mid  = smoothstep(0.18, 0.40, l) * (1.0 - smoothstep(0.85, 0.98, l));
    return warm * mid;
}

void main() {
    // Aspect-correct fit (letterbox / pillarbox).
    vec2 win = u.resolution;
    vec2 img = vec2(textureSize(u_image, 0));
    float win_aspect = win.x / win.y;
    float img_aspect = img.x / img.y;

    vec2 uv;
    if (win_aspect > img_aspect) {
        float scale = img_aspect / win_aspect;
        uv = vec2((v_uv.x - 0.5) / scale + 0.5, v_uv.y);
    } else {
        float scale = win_aspect / img_aspect;
        uv = vec2(v_uv.x, (v_uv.y - 0.5) / scale + 0.5);
    }

    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
        o_color = vec4(BG, 1.0);
        return;
    }

    vec3 base = texture(u_image, uv).rgb;
    vec3 col  = base;

    // ---- Subtle gold glow ----
    // Two rings of taps around this pixel; accumulate any gold contribution
    // weighted by distance falloff. Radius is in screen pixels so the bloom
    // looks consistent across window sizes.
    vec3  glow = vec3(0.0);
    float wsum = 0.0;
    const int RAYS = 12;
    const float RADIUS_PX = 9.0;
    for (int i = 0; i < RAYS; i++) {
        float a = (float(i) + 0.5) / float(RAYS) * 6.2831853;
        vec2 dir = vec2(cos(a), sin(a)) / win;
        for (int r = 1; r <= 2; r++) {
            float rr = float(r) * 0.5;            // 0.5, 1.0
            vec2 off = dir * RADIUS_PX * rr;
            vec3 s   = texture(u_image, uv + off).rgb;
            float g  = goldness(s);
            float w  = 1.0 - rr;                  // closer = more weight
            glow += s * g * w;
            wsum += w;
        }
    }
    glow /= max(wsum, 1.0);
    // Pulse the glow gently so it feels alive rather than baked-in.
    // 1 cycle per loop_period = always loops cleanly.
    float omega = TAU / max(u.loop_period, 0.001);
    float pulse = 0.85 + 0.15 * sin(u.time * omega);
    col += glow * 0.55 * pulse;

    // ---- Sun-ray shimmer ----
    // The rays radiate from a point above her head. We compute the polar angle
    // from that source and modulate the brightness of the mid-gray ray strokes
    // by an angular wave that drifts over time — looks like the rays breathe.
    // RAY_CENTER must sit ABOVE her head; near the origin theta sweeps so fast
    // per-pixel that the shimmer collapses into a visible starburst, so the
    // donut mask also excludes the convergence point.
    vec2  RAY_CENTER = vec2(0.5, 0.18);
    vec2  to_p   = uv - RAY_CENTER;
    float r_dist = length(to_p);
    float theta  = atan(to_p.y, to_p.x);

    // Donut mask: fade in past the inner exclusion zone, fade out at the rim.
    float ray_region = smoothstep(0.12, 0.22, r_dist)
                     * smoothstep(0.45, 0.25, r_dist);

    // "Ray-ness": pick out the mid-gray ray strokes — exclude pure black
    // (background) and exclude gold (border / accents).
    float l_ray = luma(base);
    float ray_ness = smoothstep(0.20, 0.42, l_ray)
                   * (1.0 - smoothstep(0.65, 0.88, l_ray));
    ray_ness *= 1.0 - goldness(base);

    // Two angular waves at different harmonics of the loop frequency so the
    // shimmer drifts/turbulates without ever drifting OUT of phase — at
    // t = loop_period everything is back where it started.
    float w1 = sin(theta * 18.0 - u.time * omega * 1.0);  // 1 cycle/loop
    float w2 = sin(theta *  7.0 + u.time * omega * 2.0);  // 2 cycles/loop
    float shimmer = w1 * 0.6 + w2 * 0.4;

    col += vec3(0.85, 0.80, 0.60) * ray_region * ray_ness * shimmer * 0.30;

    // ---- Star twinkle ----
    // Stars: bright pixels surrounded by a dark neighborhood. We measure the
    // local mean luminance from a small ring of samples; if the center pixel
    // is much brighter than the ring AND the ring itself is dark, this pixel
    // is a star and gets a per-pixel-hash modulated boost.
    float center_l = luma(base);
    float ring_l   = 0.0;
    const int RING = 8;
    const float RING_PX = 3.5;
    for (int i = 0; i < RING; i++) {
        float a = float(i) / float(RING) * 6.2831853;
        vec2 off = vec2(cos(a), sin(a)) * RING_PX / win;
        ring_l += luma(texture(u_image, uv + off).rgb);
    }
    ring_l /= float(RING);

    float star_score = smoothstep(0.04, 0.20, center_l - ring_l);
    float dark_bg    = 1.0 - smoothstep(0.10, 0.28, ring_l);
    float twinkle_mask = star_score * dark_bg;

    // Per-star phase from pixel-quantized hash so each star twinkles independently.
    // Quantize the per-star speed to an integer harmonic of the loop frequency
    // (1..5 cycles per loop) — keeps stars varied AND the whole field loops.
    vec2  star_id = floor(uv * img);
    float h       = hash21(star_id);
    float harmonic = floor(1.0 + h * 4.999);  // 1, 2, 3, 4, or 5
    float speed   = harmonic * omega;
    float phase   = h * TAU;
    float t       = sin(u.time * speed + phase) * 0.5 + 0.5;
    t             = pow(t, 2.5);

    col += vec3(0.85, 0.92, 1.0) * twinkle_mask * t * 0.55;

    o_color = vec4(col, 1.0);
}
