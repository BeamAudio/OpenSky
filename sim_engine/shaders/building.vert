#version 460 core

layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec3 a_nrm;
layout(location = 2) in vec2 a_uv;

struct BldgGpu {
    vec4 pos_yaw;
    vec4 scale_arch;
    vec4 roof_ao;
};

layout(std430, binding = 0) readonly buffer AllBldgs { BldgGpu bldgs[]; };
layout(std430, binding = 2) readonly buffer VisIndices { uint vis_idx[]; };

uniform mat4 u_vp;
uniform vec3 u_cam_enu; // x=east, y=alt, z=north

out vec3 v_world_pos;
out vec3 v_world_n;
out vec3 v_local_n;
out vec3 v_local_pos;
out vec3 v_bldg_size;
out vec2 v_world_en;
out vec2 v_uv;
out vec4 v_roof_ao;
out float v_arch;
out float v_bhash;
out float v_elev_m;
out float v_dist;

float hash21_v(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

void main() {
    uint id = vis_idx[gl_BaseInstance + gl_InstanceID];
    BldgGpu b = bldgs[id];

    float c = cos(b.pos_yaw.w), s = sin(b.pos_yaw.w);
    vec3 sp = a_pos * b.scale_arch.xyz;
    vec3 rp = vec3(sp.x * c + sp.z * s, sp.y, -sp.x * s + sp.z * c);
    vec3 rn = normalize(vec3(a_nrm.x * c + a_nrm.z * s, a_nrm.y, -a_nrm.x * s + a_nrm.z * c));

    float dx = b.pos_yaw.x - u_cam_enu.x;
    float dn = b.pos_yaw.z - u_cam_enu.z;
    float curv = (dx * dx + dn * dn) / (2.0 * 6378137.0);
    vec3 base_pos = vec3(dx, b.pos_yaw.y - u_cam_enu.y - curv, -dn);
    vec3 wpos = base_pos + rp;

    v_world_pos = wpos;
    v_world_n = rn;
    v_local_n = normalize(a_nrm);
    v_local_pos = sp;
    v_bldg_size = b.scale_arch.xyz;
    v_world_en = vec2(b.pos_yaw.x + rp.x, b.pos_yaw.z - rp.z);
    v_uv = a_uv;
    v_roof_ao = b.roof_ao;
    v_arch = b.scale_arch.w;
    v_bhash = hash21_v(floor(b.pos_yaw.xz * 0.35) + vec2(b.scale_arch.x * 1.7, b.pos_yaw.w * 11.3));
    v_elev_m = b.pos_yaw.y + sp.y;
    v_dist = length(wpos);
    gl_Position = u_vp * vec4(wpos, 1.0);
}
