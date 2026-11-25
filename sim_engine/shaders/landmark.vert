#version 460 core
layout(location = 0) in vec3 a_pos;   // anchor-relative (East, Elev_m, North)
layout(location = 1) in vec3 a_nrm;
layout(location = 2) in vec4 a_col;   // RGB linear base albedo, A=roughness
layout(location = 3) in vec2 a_meta;  // x=metallic, y=emissive_night

uniform mat4 u_vp;
uniform vec3 u_cam_enu;

out vec3 v_world_pos;
out vec3 v_nrm;
out vec4 v_col;
out vec2 v_meta;
out float v_elev_m;
out float v_dist;

void main() {
    float dx = a_pos.x - u_cam_enu.x;
    float dn = a_pos.z - u_cam_enu.z;
    float curv = (dx * dx + dn * dn) / (2.0 * 6378137.0);
    vec3 wpos = vec3(dx, a_pos.y - u_cam_enu.y - curv, -dn);
    v_world_pos = wpos;
    v_nrm = normalize(a_nrm);
    v_col = a_col;
    v_meta = a_meta;
    v_elev_m = a_pos.y;
    v_dist = length(wpos);
    gl_Position = u_vp * vec4(wpos, 1.0);
}
