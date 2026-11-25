#version 460 core
layout(location = 0) in vec2 a_pos; // pixel coordinates [0..W, 0..H] top-left origin
layout(location = 1) in vec2 a_uv;  // u < 0 => solid UI quad; u >= 0 => TrueType font atlas UV
layout(location = 2) in vec4 a_col;

uniform vec2 u_screen_size;
out vec2 v_uv;
out vec4 v_col;

void main() {
    vec2 ndc = vec2(a_pos.x / u_screen_size.x * 2.0 - 1.0, 1.0 - a_pos.y / u_screen_size.y * 2.0);
    v_uv = a_uv;
    v_col = a_col;
    gl_Position = vec4(ndc, 0.0, 1.0);
}
