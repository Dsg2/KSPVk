#version 450
// Fullscreen triangle.
void main() { vec2 t = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2); gl_Position = vec4(t * 2 - 1, 0, 1); }
