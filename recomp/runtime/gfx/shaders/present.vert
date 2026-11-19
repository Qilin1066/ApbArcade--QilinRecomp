#version 450
// The window's picture: one triangle covering the viewport, with the frame's
// texture coordinates running 0..1 across it (top-left origin).

layout(location = 0) out vec2 uv;

void main() {
    const vec2 corner = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);   // (0,0) (2,0) (0,2)
    uv = corner;
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
