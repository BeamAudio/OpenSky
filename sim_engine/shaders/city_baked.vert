#version 460 core

layout(location = 0) in vec3 a_pos;       // anchor-relative (East_m, Elev_m, North_m)
layout(location = 1) in vec3 a_nrm;       // normalized GL_BYTE outward normal in OpenGL (X=E, Y=Up, Z=-N)
layout(location = 2) in float a_flags;    // bit0=is_roof, bit1=is_parapet, bits2..4=style(0..7), bits5..7=roof_kind(0..3)
layout(location = 3) in vec2 a_uv;        // walls=(u_tile_bays, y_local_m), roofs=(wall_aligned_u, wall_aligned_v)
layout(location = 4) in uint a_bake_u32;  // 10-TOD packed GI: bits 0..19 = 10x2-bit sun vis, bits 20..27 = 8-bit Sky AO
layout(location = 5) in vec4 a_meta;      // walls=(bhash,eave_b,arch,0), roofs=(pnoa_r,pnoa_g,pnoa_b,bhash)

uniform mat4 u_vp;
uniform vec3 u_cam_enu; // x=east, y=alt, z=north
uniform float u_sun_phase10;
uniform vec2 u_reg_offset = vec2(0.0, 0.0);
uniform vec2 u_reg_scale  = vec2(1.0, 1.0);

out vec3 v_world_pos;
out vec3 v_world_n;
out vec2 v_world_en;
out vec2 v_uv;
out float v_sun_vis;
out float v_sky_ao;
out float v_night_light;
out vec4 v_meta;
flat out uint v_flags;
out float v_elev_m;
out float v_dist;

void main() {
    vec2 en_w = vec2(a_pos.x, a_pos.z) * u_reg_scale + u_reg_offset;
    float dx = en_w.x - u_cam_enu.x;
    float dn = en_w.y - u_cam_enu.z;
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
    v_world_n = normalize(a_nrm);
    v_world_en = en_w;
    v_uv = a_uv;
    v_meta = a_meta;
    v_flags = uint(a_flags + 0.5);
    v_elev_m = a_pos.y;
    v_dist = length(wpos);
    gl_Position = u_vp * vec4(wpos, 1.0);
}
