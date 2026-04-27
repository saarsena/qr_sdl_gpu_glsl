#version 430 core

// We don't use vertex inputs (layout location) because we read directly from SSBO
// This allows us to treat the SSBO as a pool of lines.

struct Particle {
    vec4 pos;
    vec4 prev_pos;
};

layout(std430, binding = 0) buffer ParticleBuffer {
    Particle particles[];
};

uniform mat4 view;
uniform mat4 projection;
uniform float speed_scale; // To tune the color intensity

out vec3 v_color;

// PHYSICS BASED COLOR RAMP (Black Body Radiation approximation)
// t = 0..1
vec3 get_fire_color(float t) {
    t = clamp(t, 0.0, 1.0);
    vec3 color = vec3(0.0);

    if (t < 0.33) {
        float local_t = t / 0.33;
        color = vec3(local_t, 0.0, 0.0); // Black to Red
    } else if (t < 0.66) {
        float local_t = (t - 0.33) / 0.33;
        color = vec3(1.0, local_t, 0.0); // Red to Orange/Yellow
    } else {
        float local_t = (t - 0.66) / 0.34;
        color = vec3(1.0, 1.0, local_t); // Yellow to White
    }
    return color;
}

void main() {
    // We are drawing GL_LINES. 
    // Each particle is 2 vertices: 
    // VertexID 2*i     -> particles[i].pos
    // VertexID 2*i + 1 -> particles[i].prev_pos
    
    uint particle_idx = gl_VertexID / 2;
    uint endpoint = gl_VertexID % 2; // 0 = curr, 1 = prev

    Particle p = particles[particle_idx];
    
    vec3 pos_curr = p.pos.xyz;
    vec3 pos_prev = p.prev_pos.xyz;

    // Pick position based on endpoint
    vec3 final_pos = (endpoint == 0) ? pos_curr : pos_prev;

    // Calculate speed for coloring
    float dist = distance(pos_curr, pos_prev);
    // Speed is dist / dt, but dt is constant, so just scale dist
    float heat = dist * speed_scale;

    v_color = get_fire_color(heat);
    
    gl_Position = projection * view * vec4(final_pos, 1.0);
}
