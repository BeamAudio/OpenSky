#version 460 core
layout(binding = 0) uniform sampler2D u_font_tex;
in vec2 v_uv;
in vec4 v_col;
layout(location = 0) out vec4 frag_color;

void main() {
    if (v_uv.x < 0.0) {
        frag_color = v_col;
    } else {
        vec4 g = texture(u_font_tex, v_uv);
        frag_color = vec4(g.rgb * v_col.rgb, g.a * v_col.a);
    }
}
