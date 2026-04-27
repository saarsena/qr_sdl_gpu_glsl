#version 450

layout(set = 2, binding = 0) uniform sampler2D u_qr;

layout(set = 3, binding = 0) uniform U {
    vec2 resolution;
    float time;
    float loop_period;
} u;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

const vec3 BG          = vec3(0.02, 0.04, 0.07);
const vec3 WATER_DEEP  = vec3(0.02, 0.06, 0.10);
const vec3 WATER_TINT  = vec3(0.45, 0.65, 0.85);
const vec3 STREAK      = vec3(0.70, 0.85, 1.00);
const vec3 GOLD        = vec3(1.00, 0.78, 0.30);
const float TAU = 6.2831853;

float hash21(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

float vnoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 w = f * f * (3.0 - 2.0 * f);
    float a = hash21(i);
    float b = hash21(i + vec2(1.0, 0.0));
    float c = hash21(i + vec2(0.0, 1.0));
    float d = hash21(i + vec2(1.0, 1.0));
    return mix(mix(a, b, w.x), mix(c, d, w.x), w.y);
}

bool in_qr(vec2 qr_uv) {
    return qr_uv.x >= 0.0 && qr_uv.x <= 1.0
        && qr_uv.y >= 0.0 && qr_uv.y <= 1.0;
}

// Returns the dark factor at qr_uv (0 = light module, 1 = dark module),
// using the pixel-perfect smoothstep so the value transitions over ~1 screen
// pixel at module boundaries — exactly what fwidth needs for the gold edge.
// Out-of-bounds returns 0 so neighbour samples for glow/edge stay well-defined.
float qr_dark(vec2 qr_uv) {
    if (!in_qr(qr_uv)) return 0.0;
    vec2 tex_size = vec2(textureSize(u_qr, 0));
    vec2 px = qr_uv * tex_size;
    vec2 module_uv = floor(px) + smoothstep(0.0, 1.0, fract(px) / fwidth(px));
    return texture(u_qr, module_uv / tex_size).r;
}

void main() {
    vec2 px = v_uv * u.resolution;
    vec2 center = u.resolution * 0.5;
    float min_dim = min(u.resolution.x, u.resolution.y);

    // Waterline is at the top of the screen — the entire view is below water.
    float qr_side = min_dim * 0.55;
    float waterline = 0.0;

    // p is centered horizontally; p.y is depth below the waterline.
    vec2 p = vec2(px.x - center.x, px.y - waterline);

    float depth = p.y / qr_side;          // 0 at the surface, grows downward
    float depth01 = clamp(depth, 0.0, 1.0);

    // All animated terms below pin their temporal frequencies to integer
    // harmonics of omega = 2π / loop_period — so the entire scene returns
    // to itself at t = loop_period and gif loops are seamless.
    float omega = TAU / max(u.loop_period, 0.001);
    float ct = cos(u.time * omega);
    float st = sin(u.time * omega);

    // Two crossed sine ripples for the horizontal sample displacement.
    // Higher amplitude with depth — deeper water breaks the QR up more.
    float wave1 = sin(p.y * 0.055 - u.time * omega * 2.0 + p.x * 0.012);
    float wave2 = sin(p.y * 0.140 + u.time * omega * 1.0 - p.x * 0.020);
    float dx = (wave1 * 4.5 + wave2 * 2.0) * (0.35 + depth01 * 1.2);

    // A pinch of organic noise so the ripples aren't a perfect grid.
    // Sampling on a circle in noise-space (radius NOISE_R) instead of a
    // straight line in time keeps the noise periodic at loop_period.
    const float NOISE_R = 0.7;
    float n = vnoise(vec2(p.x * 0.04 + ct * NOISE_R,
                          p.y * 0.05 + st * NOISE_R)) - 0.5;
    dx += n * 5.0 * (0.4 + depth01);

    // Sample the submerged QR directly (right-side up) with the displaced x.
    vec2 sample_p = vec2(p.x + dx, p.y);
    vec2 qr_uv = vec2((sample_p.x + qr_side * 0.5) / qr_side,
                     sample_p.y / qr_side);

    float dark = qr_dark(qr_uv);
    bool in_bounds = in_qr(qr_uv);

    // ---- Base water-tinted QR ----
    vec3 base;
    if (!in_bounds) {
        base = WATER_DEEP;
    } else {
        vec3 qr = mix(vec3(1.0), vec3(0.0), dark);
        base = mix(qr, qr * WATER_TINT, 0.55);
        base *= 1.0 - depth01 * 0.55;
        base = mix(base, WATER_DEEP, depth01 * 0.35);
    }

    // ---- Sharp thin gold outline ----
    // fwidth on the smoothstep'd dark factor peaks within ~1 screen pixel at
    // every module boundary — naturally a 1-px-wide line, no gain tweaking.
    if (in_bounds) {
        float edge = fwidth(dark);
        float outline = clamp(edge * 1.20, 0.0, 1.0);
        base += GOLD * outline * (1.0 - depth01 * 0.45) * 0.85;
    }

    // ---- Soft gold glow halo ----
    // Accumulate dark-module presence in a small radial kernel; each bar
    // gets a warm halo that survives the water tint without smearing.
    float glow = 0.0;
    const int   GLOW_TAPS      = 12;
    const float GLOW_RADIUS_PX = 7.0;
    for (int i = 0; i < GLOW_TAPS; i++) {
        float a = (float(i) + 0.5) / float(GLOW_TAPS) * 6.2831853;
        vec2 off = vec2(cos(a), sin(a)) * GLOW_RADIUS_PX / qr_side;
        glow += qr_dark(qr_uv + off);
    }
    glow /= float(GLOW_TAPS);
    base += GOLD * glow * (1.0 - depth01 * 0.40) * 0.30;

    // ---- Underwater specular streaks rolling across the surface ----
    // Outer phase speed is 3 cycles per loop; inner noise jitter samples a
    // circle in noise-space so the streaks repeat exactly at loop_period.
    const float STREAK_NOISE_R = 1.2;
    float streak_phase = p.y * 0.35 - u.time * omega * 3.0
                       + 8.0 * vnoise(vec2(p.x * 0.02 + ct * STREAK_NOISE_R,
                                           st * STREAK_NOISE_R));
    float streak = pow(0.5 + 0.5 * sin(streak_phase), 12.0);
    streak *= smoothstep(qr_side * 0.9, 0.0, p.y);   // strongest near the surface
    base += STREAK * streak * 0.35;

    // Soft dark line right at the waterline so the surface reads as a surface.
    float surf = smoothstep(2.5, 0.0, p.y);
    base = mix(base, base * 0.55, surf * 0.6);

    o_color = vec4(base, 1.0);
}
