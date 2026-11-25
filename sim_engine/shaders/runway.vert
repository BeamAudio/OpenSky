#version 460 core
layout(location = 0) in vec3 a_pos; // anchor-relative (east, elev, north)
layout(location = 1) in vec3 a_nrm;
layout(location = 2) in vec2 a_uv;  // u=width[0..1], v=length_m
layout(location = 3) in float a_paved;
layout(location = 4) in float a_is_runway;

uniform mat4 u_vp;
uniform vec3 u_cam_enu;

out vec3 v_world_pos;
out vec3 v_nrm;
out vec2 v_uv;
out float v_paved;
out float v_is_runway;
out float v_dist;

void main() {
    float dx = a_pos.x - u_cam_enu.x;
    float dn = a_pos.z - u_cam_enu.z;
    float curv = (dx * dx + dn * dn) / (2.0 * 6378137.0);
    vec3 wpos = vec3(dx, a_pos.y - u_cam_enu.y - curv, -dn);
    v_world_pos = wpos;
    v_nrm = a_nrm;
    v_uv = a_uv;
    v_paved = a_paved;
    v_is_runway = a_is_runway;
    v_dist = length(wpos);
    gl_Position = u_vp * vec4(wpos, 1.0);
}
