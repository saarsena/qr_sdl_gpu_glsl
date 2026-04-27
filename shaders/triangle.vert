#version 450

layout(location = 0) out vec2 v_uv;

// Fullscreen triangle. v_uv is set so that (0,0) is the top-left of the screen
// and (1,1) is the bottom-right, matching the natural PNG row order (row 0 at
// top). This requires flipping Y relative to clip space, which in SDL_GPU has
// the OpenGL-style Y-up convention.
void main() {
    vec2 pos = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    v_uv = vec2(pos.x, 1.0 - pos.y);
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
