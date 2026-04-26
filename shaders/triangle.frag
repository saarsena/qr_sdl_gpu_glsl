#version 450

layout(set = 2, binding = 0) uniform sampler2D u_qr;

layout(set = 3, binding = 0) uniform U {
    vec2 resolution;
} u;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

const vec3 BG = vec3(0.06, 0.08, 0.11);

void main() {
    vec2 px = v_uv * u.resolution;
    float side = min(u.resolution.x, u.resolution.y) * 0.9;
    vec2 origin = (u.resolution - vec2(side)) * 0.5;
    vec2 qr_uv = (px - origin) / side;

    if (qr_uv.x < 0.0 || qr_uv.x > 1.0 || qr_uv.y < 0.0 || qr_uv.y > 1.0) {
        o_color = vec4(BG, 1.0);
        return;
    }

    float dark = texture(u_qr, qr_uv).r;
    vec3 col = mix(vec3(1.0), vec3(0.0), dark);
    o_color = vec4(col, 1.0);
}
