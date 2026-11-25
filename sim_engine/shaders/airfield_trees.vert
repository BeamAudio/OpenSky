#version 460 core

struct TreeVolGpu {
    vec4 pos_h;     // x=east, y=ground_y, z=north, w=height_m
    vec4 meta_col;  // rgb=pnoa_linear, w=species_int + seed_frac
};

layout(std430, binding = 0) readonly buffer TreeVolBuf {
    TreeVolGpu trees[];
};

uniform mat4 u_vp;
uniform vec3 u_camera;   // (cam_east, cam_alt_m, cam_north)
uniform float u_cam_agl;

out vec3 v_ray_dir;
flat out vec3 v_base_cam;
flat out vec4 v_meta;
flat out vec3 v_params; // x=height, y=fade, z=min_hit_world_t
flat out float v_ground_msl;

void main() {
    TreeVolGpu t = trees[gl_InstanceID];
    vec3 base_cam = vec3(t.pos_h.x - u_camera.x, t.pos_h.y - u_camera.y, u_camera.z - t.pos_h.z);
    float d2_xz = dot(base_cam.xz, base_cam.xz);
    base_cam.y -= d2_xz / 12756274.0; // Spherical Earth curvature drop matching terrain

    float dist = length(base_cam);
    // Wide complementary crossfade (360..720 m distance, 160..340 m AGL) matching cdlod_terrain.vert!
    float dist_fade = 1.0 - smoothstep(360.0, 720.0, dist);
    float agl_fade = 1.0 - smoothstep(160.0, 340.0, u_cam_agl);
    float fade = dist_fade * agl_fade;
    if (fade <= 0.01) {
        gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    int sp = int(floor(t.meta_col.w));
    // Gently sink distant crowns into the rising terrain-canopy height as they dissolve out.
    float H = t.pos_h.w * mix(0.68, 1.0, smoothstep(0.0, 0.65, fade));
    // Tight species bounding radius in unit tree space (covers irregular boughs & width variation).
    float rad_u = (sp == 3) ? 0.94 : ((sp == 0) ? 0.80 : ((sp == 1) ? 0.68 : 0.50));
    float cy_u  = (sp == 3) ? 0.44 : 0.54;
    float bound_r = length(vec2(rad_u, 0.56)) * H;

    vec3 vol_c = base_cam + vec3(0.0, cy_u * H, 0.0);
    float d_c = length(vol_c);
    vec3 fwd = vol_c / max(d_c, 0.001);
    vec3 ref_up = (abs(fwd.y) < 0.98) ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 right = normalize(cross(ref_up, fwd));
    vec3 up = cross(fwd, right);

    // Place proxy quad on the front tangent plane of the bounding sphere so all internal
    // ray hits have strictly smaller Reverse-Z depth (enables hardware Early-Z / Hi-Z!).
    float plane_d = max(0.18, d_c - bound_r);
    float quad_half;
    if (d_c > bound_r * 1.06) {
        float sin_a = clamp(bound_r / d_c, 0.0, 0.97);
        float tan_a = sin_a / sqrt(max(0.04, 1.0 - sin_a * sin_a));
        quad_half = plane_d * tan_a * 1.06;
    } else {
        quad_half = bound_r * 1.45;
    }

    vec2 corner = vec2((gl_VertexID & 1) != 0 ? 1.0 : -1.0, (gl_VertexID & 2) != 0 ? 1.0 : -1.0);
    vec3 quad_pos = fwd * plane_d + (right * corner.x + up * corner.y) * quad_half;

    v_ray_dir = quad_pos;
    v_base_cam = base_cam;
    v_meta = t.meta_col;
    v_params = vec3(H, fade, plane_d);
    v_ground_msl = t.pos_h.y;
    gl_Position = u_vp * vec4(quad_pos, 1.0);
}
