// Fully procedural disc shader for GPUParticles2D.
// No texture sampling required.
// Assign as a ShaderMaterial to GPUParticles2D -> Material.
// Color is driven by particle COLOR from aizawa.gdshader.

shader_type canvas_item;
render_mode blend_add, unshaded;

uniform vec4 tint : source_color = vec4(1.0, 1.0, 1.0, 1.0);
uniform float radius : hint_range(0.1, 1.0) = 0.85;
uniform float softness : hint_range(0.001, 0.9) = 0.3;
uniform float inner_bloom : hint_range(0.0, 8.0) = 2.2;
uniform float alpha_boost : hint_range(0.1, 5.0) = 1.0;

void fragment() {
    // UV is expected across the particle quad in [0, 1].
    vec2 p = UV * 2.0 - 1.0;
    float r = length(p);

    // Disc with soft edge.
    float edge0 = max(0.0, radius - softness);
    float disc = 1.0 - smoothstep(edge0, radius, r);

    // Hot center falloff.
    float core = exp(-8.0 * r * r);

    vec4 base = COLOR * tint;
    vec3 rgb = base.rgb * (disc + core * inner_bloom);
    float alpha = base.a * disc * alpha_boost;

    COLOR = vec4(rgb, alpha);
}
