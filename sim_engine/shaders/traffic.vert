#version 460 core
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec3 a_nrm;
layout(location = 2) in float a_part; // 0=body paint, 1=glass, 2=headlight, 3=taillight, 4=wheel/trim

struct CarGpu {
    vec4 pos_yaw;    // x=east_m, y=ground_m, z=north_m, w=yaw_rad
    vec4 scale_col;  // x=width, y=height, z=length, w=color_id
};

layout(std430, binding = 0) readonly buffer CarBuf { CarGpu cars[]; };

uniform mat4 u_vp;
uniform vec3 u_cam_enu;

out vec3 v_world_pos;
out vec3 v_nrm;
out float v_part;
out float v_col_id;
out float v_elev_m;
out float v_dist;

void main() {
    CarGpu car = cars[gl_InstanceID];
    float c = cos(car.pos_yaw.w), s = sin(car.pos_yaw.w);
    vec3 sp = a_pos * car.scale_col.xyz;
    vec3 rp = vec3(sp.x * c + sp.z * s, sp.y, -sp.x * s + sp.z * c);
    vec3 rn = normalize(vec3(a_nrm.x * c + a_nrm.z * s, a_nrm.y, -a_nrm.x * s + a_nrm.z * c));
    float dx = car.pos_yaw.x - u_cam_enu.x;
    float dn = car.pos_yaw.z - u_cam_enu.z;
    float curv = (dx * dx + dn * dn) / (2.0 * 6378137.0);
    vec3 wpos = vec3(dx, car.pos_yaw.y - u_cam_enu.y - curv, -dn) + rp;

    v_world_pos = wpos;
    v_nrm = rn;
    v_part = a_part;
    v_col_id = car.scale_col.w;
    v_elev_m = car.pos_yaw.y;
    v_dist = length(wpos);
    gl_Position = u_vp * vec4(wpos, 1.0);
}
