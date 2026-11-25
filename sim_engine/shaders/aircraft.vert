#version 460 core
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec3 a_nrm;
layout(location = 2) in vec4 a_col;
layout(location = 3) in vec2 a_uv;

uniform mat4 u_mvp;
uniform mat4 u_model; // Camera-relative world transform for node (rotation + translation)

out vec3 v_world_pos;
out vec3 v_nrm;
out vec4 v_col;
out vec2 v_uv;
out vec3 v_obj_pos;

void main() {
    vec4 wp = u_model * vec4(a_pos, 1.0);
    v_world_pos = wp.xyz;
    v_nrm = normalize(mat3(u_model) * a_nrm);
    v_col = a_col;
    v_uv = a_uv;
    v_obj_pos = a_pos;
    gl_Position = u_mvp * vec4(a_pos, 1.0);
}
