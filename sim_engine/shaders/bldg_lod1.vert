#version 460 core

layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec3 a_nrm;

struct BldgGpu {
    vec4 pos_yaw;    // x=east_m, y=ground_m, z=north_m, w=yaw_rad
    vec4 scale_arch; // x=width_m, y=height_m, z=depth_m, w=archetype
    vec4 roof_ao;    // x=roof_r, y=roof_g, z=roof_b, w=canyon_ao
};

layout(std430, binding = 0) readonly buffer AllBldgs { BldgGpu bldgs[]; };
layout(std430, binding = 5) readonly buffer Lod1Indices { uint lod1_idx[]; };

uniform mat4 u_vp;
uniform vec3 u_cam_enu;

out vec3 v_world_pos;
out vec3 v_world_n;
out vec4 v_roof_ao;
out float v_is_roof;
out float v_elev_m;
out float v_dist;

void main() {
    uint id = lod1_idx[gl_InstanceID];
    BldgGpu b = bldgs[id];

    float c = cos(b.pos_yaw.w), s = sin(b.pos_yaw.w);
    vec3 sp = a_pos * b.scale_arch.xyz;
    vec3 rp = vec3(sp.x * c + sp.z * s, sp.y, -sp.x * s + sp.z * c);
    vec3 rn = normalize(vec3(a_nrm.x * c + a_nrm.z * s, a_nrm.y, -a_nrm.x * s + a_nrm.z * c));

    float dx = b.pos_yaw.x - u_cam_enu.x;
    float dn = b.pos_yaw.z - u_cam_enu.z;
    float curv = (dx * dx + dn * dn) / (2.0 * 6378137.0);
    vec3 wpos = vec3(dx + rp.x, b.pos_yaw.y - u_cam_enu.y - curv + rp.y, -dn + rp.z);

    v_world_pos = wpos;
    v_world_n = rn;
    v_roof_ao = b.roof_ao;
    v_is_roof = step(0.5, a_nrm.y);
    v_elev_m = b.pos_yaw.y + sp.y;
    v_dist = length(wpos);
    gl_Position = u_vp * vec4(wpos, 1.0);
}
