#version 460 core
layout(binding = 0) uniform sampler2D u_font_tex;
uniform int u_tex_mode; // 0 = HUD UI/font, 1 = false-color GPU windshield PDE heatmap
in vec2 v_uv;
in vec4 v_col;
layout(location = 0) out vec4 frag_color;

void main() {
    if (v_uv.x < 0.0) {
        frag_color = v_col;
    } else if (u_tex_mode == 1) {
        vec4 s = texture(u_font_tex, v_uv);
        float h = clamp(s.r * 1.85, 0.0, 1.0);
        float spd = clamp(length(s.gb) * 2.8, 0.0, 1.0);
        float wet = clamp(s.a, 0.0, 1.0);
        vec3 base = mix(vec3(0.02, 0.045, 0.095), vec3(0.06, 0.28, 0.62), wet);
        vec3 film = mix(base, vec3(0.16, 0.90, 1.00), pow(h, 0.68));
        film = mix(film, vec3(0.98, 1.00, 1.00), smoothstep(0.52, 0.92, h));
        film += vec3(0.95, 0.24, 0.68) * (spd * 0.52 * (0.25 + 0.75 * h));
        vec2 g = abs(fract(v_uv * vec2(8.0, 5.0)) - 0.5);
        float grid = 1.0 - smoothstep(0.46, 0.50, max(g.x, g.y));
        film += vec3(0.06, 0.13, 0.20) * grid;
        frag_color = vec4(film, 0.96 * v_col.a);
    } else {
        vec4 g = texture(u_font_tex, v_uv);
        frag_color = vec4(g.rgb * v_col.rgb, g.a * v_col.a);
    }
}
