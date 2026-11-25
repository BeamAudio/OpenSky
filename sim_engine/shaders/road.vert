#version 460 core
layout(location = 0) in vec3 a_pos;       // anchor-relative (East, Elev_m, North)
layout(location = 1) in vec2 a_uv;        // x=across [0..1], y=length_m
layout(location = 2) in float a_kind;     // 0=motorway, 1=primary/avenue, 2=secondary/street, 3=residential
layout(location = 3) in uint a_bake_u32;  // 10-TOD packed GI: bits 0..19 = 10x2-bit sun vis, bits 20..27 = 8-bit Sky AO

uniform mat4 u_vp;
uniform vec3 u_cam_enu; // (cam_east, cam_alt, cam_north)
uniform float u_sun_phase10;

out vec3 v_world_pos;
out vec2 v_world_en;
out vec2 v_uv;
out float v_sun_vis;
out float v_sky_ao;
out float v_night_light;
out float v_kind;
out float v_elev_m;
out float v_dist;

void main() {
    float dx = a_pos.x - u_cam_enu.x;
    float dn = a_pos.z - u_cam_enu.z;
    float curv = (dx * dx + dn * dn) / (2.0 * 6378137.0);
    vec3 wpos = vec3(dx, a_pos.y - u_cam_enu.y - curv, -dn);

    float p = clamp(u_sun_phase10, 0.0, 9.0);
    uint k0 = uint(floor(p));
    uint k1 = min(9u, k0 + 1u);
    float s0 = float((a_bake_u32 >> (k0 * 2u)) & 3u) * (1.0 / 3.0);
    float s1 = float((a_bake_u32 >> (k1 * 2u)) & 3u) * (1.0 / 3.0);
    v_sun_vis = mix(s0, s1, p - float(k0));
    v_sky_ao = float((a_bake_u32 >> 20u) & 255u) * (1.0 / 255.0);
    v_night_light = float((a_bake_u32 >> 28u) & 15u) * (1.0 / 15.0);

    v_world_pos = wpos;
    v_world_en = vec2(a_pos.x, a_pos.z);
    v_uv = a_uv;
    v_kind = a_kind;
    v_elev_m = a_pos.y;
    v_dist = length(wpos);
    gl_Position = u_vp * vec4(wpos, 1.0);
}
