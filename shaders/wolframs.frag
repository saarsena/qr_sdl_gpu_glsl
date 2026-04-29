#version 450
// Wolfram elementary CA — ping-pong feedback edition.
// Compiled twice: once with -DDOUBLE_BUFFER_0 (the propagation pass that writes
// the next state into the back buffer) and once without (the display pass that
// reads the front buffer and colorizes it).

layout(set = 2, binding = 0) uniform sampler2D u_doubleBuffer0;

layout(set = 3, binding = 0) uniform U {
    vec2 resolution;
    float time;
    float loop_period;
} u;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

#define u_resolution u.resolution
#define u_time u.time

const float RULE = 30.0;
const bool RANDOM_SEED = false;  // false = single center cell

float hash(float n) {
    return fract(sin(n * 91.3458) * 47453.5453);
}

float getBit(float rule, float n) {
    return mod(floor(rule / pow(2.0, n)), 2.0);
}

float applyRule(float rule, float l, float c, float r) {
    float idx = l * 4.0 + c * 2.0 + r;
    return getBit(rule, idx);
}

void main() {
    // v_uv has (0,0) at top-left under our vertex shader's convention, so row 0
    // is the seed row at the top of the screen and propagation moves downward
    // (toward larger v_uv.y). "Row above" is therefore at smaller V.
    vec2 uv = v_uv;
    vec2 pixel = uv * u_resolution;
    float dx = 1.0 / u_resolution.x;
    float dy = 1.0 / u_resolution.y;

#ifdef DOUBLE_BUFFER_0
    // ── Ping-pong propagation pass ──────────────────────
    // R = cell state, G = computed flag.
    // Reads previous-frame back-buffer; writes one new row each frame.
    float row = pixel.y;
    vec4 prev = texture(u_doubleBuffer0, uv);

    if (row < 1.0) {
        // Row 0: seed (pixel center sits at y=0.5 in Vulkan's top-origin coords)
        float state;
        if (RANDOM_SEED) {
            state = step(hash(pixel.x * 1.731), 0.5);
        } else {
            float mid = floor(u_resolution.x * 0.5);
            state = step(abs(pixel.x - mid), 0.5);
        }
        o_color = vec4(state, 1.0, 0.0, 1.0);
    } else if (prev.g > 0.5) {
        // Already computed — preserve.
        o_color = prev;
    } else {
        // Check if row above is ready.
        float above_y = uv.y - dy;
        vec4 aboveC = texture(u_doubleBuffer0, vec2(uv.x, above_y));

        if (aboveC.g < 0.5) {
            o_color = vec4(0.0);
        } else {
            // Clamp edges to 0 — prevent texture wrap-around.
            float l = (uv.x - dx < 0.0) ? 0.0
                    : step(0.5, texture(u_doubleBuffer0, vec2(uv.x - dx, above_y)).r);
            float c = step(0.5, aboveC.r);
            float r = (uv.x + dx > 1.0) ? 0.0
                    : step(0.5, texture(u_doubleBuffer0, vec2(uv.x + dx, above_y)).r);
            float state = applyRule(RULE, l, c, r);
            o_color = vec4(state, 1.0, 0.0, 1.0);
        }
    }

#else
    // ── Display pass ────────────────────────────────────
    vec4 data = texture(u_doubleBuffer0, uv);
    float state = data.r * data.g;
    float t = uv.y;  // 0 at top (newest seed), 1 at bottom (oldest rows)

    vec3 color;
    if (state < 0.5) {
        color = vec3(0.02, 0.02, 0.06);
    } else {
        vec3 a = vec3(1.0, 0.30, 0.05);
        vec3 b = vec3(0.85, 0.10, 0.55);
        vec3 c = vec3(0.15, 0.40, 1.0);
        color = (t < 0.5)
            ? mix(a, b, t * 2.0)
            : mix(b, c, (t - 0.5) * 2.0);
        color += 0.04 * sin(t * 25.0 + u_time * 2.0);
    }

    o_color = vec4(color, 1.0);
#endif
}
