#version 460 core

layout(location = 0) in vec2 a_corner; // (-1,-1), (1,-1), (1,1), (-1,1)

struct BldgGpu {
    vec4 pos_yaw;    // x=east_m, y=ground_m, z=north_m, w=yaw_rad
    vec4 scale_arch; // x=width_m, y=height_m, z=depth_m, w=archetype
    vec4 roof_ao;
};

layout(std430, binding = 0) readonly buffer AllBldgs { BldgGpu bldgs[]; };
layout(std430, binding = 2) readonly buffer VisIndices { uint vis_idx[]; };

uniform mat4 u_vp;
uniform vec3 u_cam_enu;
uniform vec3 u_sun_dir;

out vec2 v_local_xz;
out vec2 v_half_ext;
out vec2 v_sun_off;
out float v_dist;

void main() {
    uint id = vis_idx[gl_BaseInstance + gl_InstanceID];
    BldgGpu b = bldgs[id];

    float c = cos(b.pos_yaw.w), s = sin(b.pos_yaw.w);
    vec2 he = b.scale_arch.xz * 0.5;
    float bh = b.scale_arch.y;

    // Project building height along -sun_dir onto ground plane, rotated into building-local XZ!
    vec3 sdir = normalize(u_sun_dir);
    float proj_len = min(bh / max(0.32, sdir.y), min(bh * 2.4, 34.0));
    vec2 w_off = vec2(-sdir.x, -sdir.z) * proj_len;
    // Inverse of (x*c + z*s, -x*s + z*c) is (x*c - z*s, x*s + z*c)
    vec2 l_off = vec2(w_off.x * c - w_off.y * s, w_off.x * s + w_off.y * c);

    float pad = 2.6;
    vec2 bmin = min(-he, -he + l_off) - vec2(pad);
    vec2 bmax = max( he,  he + l_off) + vec2(pad);
    vec2 lp = mix(bmin, bmax, a_corner * 0.5 + 0.5);

    vec2 rp = vec2(lp.x * c + lp.y * s, -lp.x * s + lp.y * c);
    float dx = b.pos_yaw.x - u_cam_enu.x;
    float dn = b.pos_yaw.z - u_cam_enu.z;
    float curv = (dx * dx + dn * dn) / (2.0 * 6378137.0);
    // Sit flush slightly above ground & sidewalks with polygon offset so shadows wrap cleanly onto streets & plazas
    vec3 wpos = vec3(dx + rp.x, b.pos_yaw.y - u_cam_enu.y - curv + 0.26, -dn + rp.y);

    v_local_xz = lp;
    v_half_ext = he;
    v_sun_off = l_off;
    v_dist = length(wpos);
    gl_Position = u_vp * vec4(wpos, 1.0);
}
