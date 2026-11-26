#version 460 core
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec3 a_nrm;
layout(location = 2) in vec4 a_col;
layout(location = 3) in vec2 a_uv;

uniform mat4 u_mvp;
uniform mat4 u_model;    // Camera-relative world transform for node (rotation + translation)
uniform mat4 u_ac_node;  // Node-to-aircraft local transform (X=right, Y=up, Z=aft)
uniform vec4 u_dmg_zones;// x=nose, y=wing_l, z=wing_r, w=tail
uniform vec4 u_dmg_extra;// x=airframe, y=windshield_crack, z=scorch, w=is_interior

out vec3 v_world_pos;
out vec3 v_nrm;
out vec4 v_col;
out vec2 v_uv;
out vec3 v_obj_pos;
out vec3 v_ac_pos;

float dmg_wave(vec3 p) {
    float a = sin(p.x * 13.7 + p.z * 9.3 + p.y * 7.1);
    float b = cos(p.z * 19.4 - p.x * 11.2 + p.y * 15.8);
    return a * 0.62 + b * 0.38;
}

void main() {
    vec3 lp = a_pos;
    vec3 ln = a_nrm;
    vec3 acp = (u_ac_node * vec4(a_pos, 1.0)).xyz;
    float max_d = max(max(u_dmg_zones.x, u_dmg_zones.w), max(u_dmg_zones.y, u_dmg_zones.z));
    if (max_d > 0.015 && (u_dmg_extra.w < 0.5 || abs(acp.x) > 0.85 || acp.z < -1.05)) {
        float w_nose = smoothstep(-1.45, -3.45, acp.z) * u_dmg_zones.x;
        float w_wl   = smoothstep(-0.75, -4.40, acp.x) * u_dmg_zones.y;
        float w_wr   = smoothstep( 0.75,  4.40, acp.x) * u_dmg_zones.z;
        float w_tail = smoothstep( 1.75,  3.75, acp.z) * u_dmg_zones.w;
        float w_bel  = smoothstep(-0.22, -1.05, acp.y) * u_dmg_extra.x * 0.60;
        float w = clamp(w_nose + w_wl + w_wr + w_tail + w_bel, 0.0, 1.50);
        if (w > 0.005) {
            float d1 = dmg_wave(acp * 1.35);
            float d2 = dmg_wave(acp * 3.10 + vec3(1.7, 0.9, 2.3));
            float crumple = (0.48 + 0.52 * abs(d1)) * w * 0.24;
            vec3 inward = normalize(vec3(-acp.x * 0.48, (acp.y < 0.0 ? 0.65 : -0.35), (acp.z < -0.5 ? 0.55 : -0.35)) + 1e-4);
            inward.y += (w_wl + w_wr) * 0.55;
            vec3 disp_ac = inward * crumple + vec3(d1, d2, d1 * 0.7) * (0.075 * w);
            lp += transpose(mat3(u_ac_node)) * disp_ac;
            ln = normalize(ln + vec3(d2, d1, d2 - d1) * (0.62 * w));
        }
    }
    vec4 wp = u_model * vec4(lp, 1.0);
    v_world_pos = wp.xyz;
    v_nrm = normalize(mat3(u_model) * ln);
    v_col = a_col;
    v_uv = a_uv;
    v_obj_pos = lp;
    v_ac_pos = (u_ac_node * vec4(lp, 1.0)).xyz;
    gl_Position = u_mvp * vec4(lp, 1.0);
}
