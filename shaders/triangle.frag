#version 450

layout(set = 2, binding = 0) uniform sampler2D u_qr;

layout(set = 3, binding = 0) uniform U {
    vec2 resolution;
    float time;
} u;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

const vec3 BG          = vec3(0.02, 0.04, 0.07);
const vec3 WATER_DEEP  = vec3(0.02, 0.06, 0.10);
const vec3 WATER_TINT  = vec3(0.45, 0.65, 0.85);
const vec3 STREAK      = vec3(0.70, 0.85, 1.00);

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

vec3 sample_qr(vec2 qr_uv) {
    if (qr_uv.x < 0.0 || qr_uv.x > 1.0 || qr_uv.y < 0.0 || qr_uv.y > 1.0) {
        return vec3(-1.0);
    }
    float dark = texture(u_qr, qr_uv).r;
    return mix(vec3(1.0), vec3(0.0), dark);
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

    // Two crossed sine ripples for the horizontal sample displacement.
    // Higher amplitude with depth — deeper water breaks the QR up more.
    float wave1 = sin(p.y * 0.055 - u.time * 2.2 + p.x * 0.012);
    float wave2 = sin(p.y * 0.140 + u.time * 1.4 - p.x * 0.020);
    float dx = (wave1 * 4.5 + wave2 * 2.0) * (0.35 + depth01 * 1.2);

    // A pinch of organic noise so the ripples aren't a perfect grid.
    float n = vnoise(vec2(p.x * 0.04, p.y * 0.05 - u.time * 0.6)) - 0.5;
    dx += n * 5.0 * (0.4 + depth01);

    // Sample the submerged QR directly (right-side up) with the displaced x.
    vec2 sample_p = vec2(p.x + dx, p.y);
    vec2 qr_uv = vec2((sample_p.x + qr_side * 0.5) / qr_side,
                     sample_p.y / qr_side);

    vec3 base;
    vec3 qr = sample_qr(qr_uv);
    if (qr.r < 0.0) {
        base = WATER_DEEP;
    } else {
        // Tint and darken the reflected QR with depth.
        base = mix(qr, qr * WATER_TINT, 0.55);
        base *= 1.0 - depth01 * 0.55;
        base = mix(base, WATER_DEEP, depth01 * 0.35);
    }

    // Bright specular streaks rolling across the water surface.
    float streak_phase = p.y * 0.35 - u.time * 3.5
                       + 8.0 * vnoise(vec2(p.x * 0.02, u.time * 0.4));
    float streak = pow(0.5 + 0.5 * sin(streak_phase), 12.0);
    streak *= smoothstep(qr_side * 0.9, 0.0, p.y);   // strongest near the surface
    base += STREAK * streak * 0.35;

    // Soft dark line right at the waterline so the surface reads as a surface.
    float surf = smoothstep(2.5, 0.0, p.y);
    base = mix(base, base * 0.55, surf * 0.6);

    o_color = vec4(base, 1.0);
}
