// Aizawa Attractor - Godot 4 Particle Shader (GPUParticles2D)
//
// Setup in Godot:
//   - Add a GPUParticles2D node
//   - Set Amount to ~100000
//   - Set Lifetime to a very large value (e.g. 9999.0)
//   - Set One Shot to false, set Fixed FPS to 0
//   - Assign this shader as a new ShaderMaterial on the Process Material
//   - Option A (textured glow): assign a small particle texture in Texture,
//     and use aizawa_particle_glow_2d.gdshader on Material
//   - Option B (fully procedural disc): no texture needed,
//     use aizawa_particle_disc_2d.gdshader on Material
//   - This shader stores the attractor z-dimension in CUSTOM.x
//   - COLOR output maps speed to a fire-like ramp

shader_type particles;

// Aizawa attractor parameters (defaults match the original GL version)
uniform float step_dt   : hint_range(0.001, 0.05)  = 0.01;
uniform float param_a   : hint_range(0.0, 2.0)     = 0.95;
uniform float param_b   : hint_range(0.0, 2.0)     = 0.7;
uniform float param_c   : hint_range(0.0, 2.0)     = 0.6;
uniform float param_d   : hint_range(0.0, 6.0)     = 3.5;
uniform float param_e   : hint_range(0.0, 1.0)     = 0.25;
uniform float speed_scale : hint_range(0.0, 300.0) = 140.0;
uniform float init_spread : hint_range(0.01, 2.0)  = 0.1;

// Black-body radiation fire color ramp (matches render.vert)
// t in [0, 1]: 0 = black, 0.33 = red, 0.66 = orange/yellow, 1 = white
vec3 get_fire_color(float t) {
    t = clamp(t, 0.0, 1.0);
    if (t < 0.33) {
        return vec3(t / 0.33, 0.0, 0.0);
    } else if (t < 0.66) {
        return vec3(1.0, (t - 0.33) / 0.33, 0.0);
    } else {
        return vec3(1.0, 1.0, (t - 0.66) / 0.34);
    }
}

// Simple hash to spread initial positions differently per particle
float hash_f(uint n) {
    n = (n ^ 61u) ^ (n >> 16u);
    n *= 9u;
    n ^= n >> 4u;
    n *= 0x27d4eb2du;
    n ^= n >> 15u;
    return float(n) / float(0xFFFFFFFFu);
}

void process() {
    if (RESTART) {
        // Scatter particles near the attractor's basin of attraction
        float rx = hash_f(INDEX)              * 2.0 - 1.0;
        float ry = hash_f(INDEX + 100000u)    * 2.0 - 1.0;
        float rz = hash_f(INDEX + 200000u)    * 2.0 - 1.0;
        TRANSFORM[3].xy = vec2(rx, ry) * init_spread;
        CUSTOM.x = rz * init_spread;
        COLOR = vec4(1.0, 0.0, 0.0, 1.0);
        ACTIVE = true;
    } else {
        vec3 p = vec3(TRANSFORM[3].xy, CUSTOM.x);

        // Aizawa attractor equations (direct port from aizawa.comp)
        float dx = (p.z - param_b) * p.x - param_d * p.y;
        float dy = param_d * p.x + (p.z - param_b) * p.y;
        float dz = param_c + param_a * p.z
                 - (p.z * p.z * p.z) / 3.0
                 - (p.x * p.x + p.y * p.y) * (1.0 + param_e * p.z)
                 + 0.1 * p.z * (p.x * p.x * p.x);

        // Euler integration with fixed step (not DELTA, to keep attractor stable)
        vec3 delta = vec3(dx, dy, dz) * step_dt;

        // Fire color ramp based on instantaneous speed (matches render.vert)
        float heat = length(delta) * speed_scale;
        COLOR = vec4(get_fire_color(heat), 1.0);

        p += delta;
        TRANSFORM[3].xy = p.xy;
        CUSTOM.x = p.z;
        ACTIVE = true;
    }
}
