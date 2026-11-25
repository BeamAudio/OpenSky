#version 460 core

uniform mat4 u_vp;
uniform vec3 u_cam_world;   // World position (east_m, alt_m, -north_m) for seamless box wrap
uniform vec3 u_rel_vel;     // Particle velocity relative to aircraft in render axes (east, up, -north)
uniform vec3 u_cam_right;
uniform vec3 u_cam_up;
uniform float u_time;
uniform float u_snow;       // 0 = stretched rain streak, 1 = drifting snowflake
uniform float u_intensity;  // [0..1] precip intensity

out vec2 v_uv;
out float v_alpha;

float hash1(float n) {
    return fract(sin(n * 127.1 + 311.7) * 43758.5453123);
}

void main() {
    int id = gl_VertexID / 6;
    int corner = gl_VertexID % 6;
    const vec2 kQuad[6] = vec2[6](
        vec2(-1.0, -1.0), vec2( 1.0, -1.0), vec2( 1.0,  1.0),
        vec2(-1.0, -1.0), vec2( 1.0,  1.0), vec2(-1.0,  1.0)
    );
    vec2 q = kQuad[corner];
    v_uv = q * 0.5 + 0.5;

    float fid = float(id);
    vec3 seed = vec3(hash1(fid * 1.13 + 0.7), hash1(fid * 2.41 + 3.1), hash1(fid * 3.77 + 7.9));
    // Active particle density scales smoothly with rain/snow intensity (zero pop-in!)
    float active_gate = smoothstep(0.0, 0.12, u_intensity - seed.x * 0.78);
    if (active_gate <= 0.001) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        v_alpha = 0.0;
        return;
    }

    vec3 box = vec3(18.0, 13.0, 18.0);
    // Gentle lateral flutter on snowflakes
    vec3 flutter = vec3(sin(u_time * 2.4 + fid * 1.7), 0.0, cos(u_time * 2.1 + fid * 2.3)) * (0.85 * u_snow);
    vec3 fall = vec3(0.0, mix(-8.5, -1.7, u_snow), 0.0);
    vec3 vel = u_rel_vel + fall + flutter;

    vec3 p = fract((seed * box + vel * u_time - u_cam_world) / box) * box - box * 0.5;
    float dist = length(p);
    // Keep rain/snow cleanly outside the cockpit interior (~1.65 m clearance)
    float near_fade = smoothstep(1.55, 2.55, dist);
    float far_fade = 1.0 - smoothstep(6.5, 10.5, dist);

    float spd = length(vel);
    vec3 axis = (spd > 0.25) ? (vel / spd) : vec3(0.0, -1.0, 0.0);
    vec3 view_dir = normalize(p + vec3(1e-5));
    vec3 side = cross(axis, view_dir);
    float side_len = length(side);
    side = (side_len > 0.02) ? (side / side_len) : u_cam_right;

    float streak_len = mix(clamp(0.26 + spd * 0.015, 0.26, 1.15), 0.052, u_snow);
    float streak_w   = mix(0.011, 0.048, u_snow);
    vec3 world_pos = p + side * (q.x * streak_w) + mix(axis, u_cam_up, u_snow * 0.7) * (q.y * streak_len);

    gl_Position = u_vp * vec4(world_pos, 1.0);
    v_alpha = active_gate * near_fade * far_fade * mix(0.50, 0.78, u_snow) * (0.50 + 0.50 * u_intensity);
}
