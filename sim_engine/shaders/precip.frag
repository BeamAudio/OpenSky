#version 460 core

in vec2 v_uv;
in float v_alpha;
layout(location = 0) out vec4 frag_color;

uniform vec3 u_color;
uniform float u_snow;

void main() {
    vec2 d = (v_uv - 0.5) * 2.0;
    float r2 = dot(d, d);
    float shape = mix(
        (1.0 - smoothstep(0.15, 1.0, abs(d.x))) * (1.0 - smoothstep(0.25, 1.0, abs(d.y))),
        1.0 - smoothstep(0.15, 0.95, r2),
        u_snow
    );
    float a = v_alpha * shape;
    if (a < 0.006) discard;
    frag_color = vec4(u_color, a);
}
