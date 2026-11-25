#version 460 core

uniform vec3 u_sun_dir;
uniform float u_fade_start;
uniform float u_fade_end;

in vec2 v_local_xz;
in vec2 v_half_ext;
in vec2 v_sun_off;
in float v_dist;

layout(location = 0) out vec4 frag_color;

float sd_box(vec2 p, vec2 b) {
    vec2 d = abs(p) - b;
    return length(max(d, 0.0)) + min(max(d.x, d.y), 0.0);
}

void main() {
    float fade = 1.0 - smoothstep(u_fade_start * 0.72, u_fade_end * 0.85, v_dist);
    if (fade < 0.02) discard;

    vec3 sdir = normalize(u_sun_dir);
    float day_t = clamp((sdir.y + 0.035) / 0.16, 0.0, 1.0);

    // 1. Soft dark ground-contact AO crevice hugging every building wall
    float d_base = sd_box(v_local_xz, v_half_ext);
    float contact_ao = (1.0 - smoothstep(-0.2, 2.3, d_base)) * 0.44;

    // 2. Analytic swept-footprint directional sun cast shadow (zero overlap seams!)
    float off2 = dot(v_sun_off, v_sun_off);
    float t = (off2 > 0.001) ? clamp(dot(v_local_xz, v_sun_off) / off2, 0.0, 1.0) : 0.0;
    float d_cast = sd_box(v_local_xz - v_sun_off * t, v_half_ext);
    float penumbra = 0.8 + 1.2 * t;
    float cast_sh = (1.0 - smoothstep(-0.25, penumbra, d_cast)) * (0.48 * day_t);

    float sh = max(contact_ao, cast_sh) * fade;
    if (sh < 0.015) discard;

    // Natural sky-tinted cool shadow color (matches terrain & Spanish sunlit pavement shadows!)
    frag_color = vec4(0.06, 0.08, 0.12, sh);
}
