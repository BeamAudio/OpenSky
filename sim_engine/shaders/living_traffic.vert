#version 460 core
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec3 a_nrm;
layout(location = 2) in float a_part; // 0=primary, 1=accent/containers, 2=glass, 3=red nav/beacon, 4=green nav, 5=white strobe/mast, 6=landing/deck light, 7=ship wake

struct EntityGpu {
    vec4 pos_yaw;   // x=east_m, y=alt_m, z=north_m, w=yaw_rad (0 = North/-Z, +CW = East/+X)
    vec4 pr_scale;  // x=pitch_rad (+nose up), y=roll_rad (+right wing down), z=scale, w=livery_id
    vec4 lights;    // x=beacon_phase, y=strobe_on, z=landing_on, w=wake_speed01
};

layout(std430, binding = 0) readonly buffer EntityBuf { EntityGpu entities[]; };

uniform mat4 u_vp;
uniform vec3 u_cam_enu;
uniform int u_base_instance;

out vec3 v_world_pos;
out vec3 v_nrm;
out float v_part;
out float v_livery;
out vec4 v_lights;
out vec2 v_local_xz;
out float v_dist;

void main() {
    EntityGpu e = entities[u_base_instance + gl_InstanceID];
    vec3 sp = a_pos * e.pr_scale.z;
    // Scale wake length smoothly with vessel speed (0 when docked in port!).
    if (a_part > 6.5) {
        sp.z *= (0.15 + 0.85 * e.lights.w);
        sp.x *= (0.45 + 0.55 * e.lights.w);
    }

    // Apply local roll (around -Z forward), pitch (around +X right), then yaw (around +Y up).
    float cr = cos(e.pr_scale.y), sr = sin(e.pr_scale.y);
    vec3 p1 = vec3(sp.x * cr - sp.y * sr, sp.x * sr + sp.y * cr, sp.z);
    vec3 n1 = vec3(a_nrm.x * cr - a_nrm.y * sr, a_nrm.x * sr + a_nrm.y * cr, a_nrm.z);

    float cp = cos(e.pr_scale.x), sp_p = sin(e.pr_scale.x);
    vec3 p2 = vec3(p1.x, p1.y * cp - p1.z * sp_p, p1.y * sp_p + p1.z * cp);
    vec3 n2 = vec3(n1.x, n1.y * cp - n1.z * sp_p, n1.y * sp_p + n1.z * cp);

    float cy = cos(e.pos_yaw.w), sy = sin(e.pos_yaw.w);
    vec3 rp = vec3(p2.x * cy - p2.z * sy, p2.y, p2.x * sy + p2.z * cy);
    vec3 rn = normalize(vec3(n2.x * cy - n2.z * sy, n2.y, n2.x * sy + n2.z * cy));

    float dx = e.pos_yaw.x - u_cam_enu.x;
    float dn = e.pos_yaw.z - u_cam_enu.z;
    float curv = (dx * dx + dn * dn) / (2.0 * 6378137.0);
    vec3 wpos = vec3(dx, e.pos_yaw.y - u_cam_enu.y - curv, -dn) + rp;

    v_world_pos = wpos;
    v_nrm = rn;
    v_part = a_part;
    v_livery = e.pr_scale.w;
    v_lights = e.lights;
    v_local_xz = a_pos.xz;
    v_dist = length(wpos);
    gl_Position = u_vp * vec4(wpos, 1.0);
}
