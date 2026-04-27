// Soft radial glow shader for GPUParticles2D rendering.
// Assign as a ShaderMaterial to the GPUParticles2D node's Material.
// Works with particle COLOR coming from aizawa.gdshader process material.

shader_type canvas_item;
render_mode blend_add, unshaded;

uniform vec4 tint : source_color = vec4(1.0, 0.9, 0.7, 1.0);
uniform float radius : hint_range(0.1, 1.0) = 0.9;
uniform float softness : hint_range(0.01, 0.9) = 0.45;
uniform float glow_boost : hint_range(0.1, 5.0) = 1.8;

void fragment() {
    // Convert UV to centered coordinates in [-1, 1].
    vec2 p = UV * 2.0 - 1.0;
    float r = length(p);

    // Soft circular mask.
    float edge0 = max(0.0, radius - softness);
    float mask = 1.0 - smoothstep(edge0, radius, r);

    // Extra center bloom for a hotter core.
    float core = exp(-6.0 * r * r);

    // Particle color from process shader multiplied by optional texture and tint.
    vec4 texel = texture(TEXTURE, UV);
    vec4 particle = COLOR * texel * tint;

    float alpha = particle.a * mask;
    vec3 rgb = particle.rgb * (0.6 + core * glow_boost);

    COLOR = vec4(rgb, alpha);
}
