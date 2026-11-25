#version 460 core

layout(location = 0) in vec2 a_uv; // [0..1] on 65x65 patch grid (64x64 quads = 4x sharper ridges & cliffs!)

struct NodeInstance {
    float min_e_rel;
    float min_n_rel;
    float size_m;
    float morph_start;
    float morph_end;
    float page_slice;
    float parent_slice;
    float fade_in;
    float parent_uv_off_x;
    float parent_uv_off_y;
    float world_min_e;
    float world_min_n;
};

layout(std430, binding = 0) readonly buffer NodeBuf {
    NodeInstance nodes[];
};

layout(binding = 0) uniform sampler2DArray u_height_arr; // 67x67 (65x65 + 1-texel border gutter)
layout(binding = 2) uniform sampler2DArray u_norm_arr;   // 129x129 R=nx, G=nz, B=cavity_ao
layout(binding = 3) uniform sampler2DArray u_surf_arr;

uniform mat4 u_vp;
uniform float u_cam_alt_m;
uniform float u_cam_agl_m;
uniform float u_time;
uniform vec2 u_wind_en;

out vec3 v_world_pos;   // camera-relative world position (+X East, +Y Up, -Z North)
out vec2 v_world_en;    // anchor-relative world (East, North) meters
out vec2 v_child_uv;    // [0..1] in child page
out vec2 v_parent_uv;   // [0..1] in parent page
out float v_page_slice;
out float v_parent_slice;
out float v_morph;      // [0..1] smooth geometric CDLOD morph factor
out float v_fade_in;    // [0..1] randomized per-page arrival progress for organic noise-dissolve in frag!
out float v_elev_m;
out float v_dist;
out float v_node_size;
out float v_canopy_h;
out float v_crown_id;

vec2 hash22_v(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * vec3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.xx + p3.yz) * p3.zy);
}

// Map patch UV [0..1] cleanly into 67x67 heightmap texel centers [1.5/67 .. 65.5/67] (100% crack-free shared edges!)
vec2 height_uv(vec2 uv) {
    return (vec2(1.5) + uv * 64.0) / 67.0;
}

// Map patch UV [0..1] cleanly into 129x129 normal/surface texel centers [0.5/129 .. 128.5/129] (100% seam-free edges!)
vec2 aux_uv(vec2 uv) {
    return (vec2(0.5) + uv * 128.0) / 129.0;
}

void main() {
    NodeInstance nd = nodes[gl_InstanceID];

    // 1. Estimate distance at un-morphed vertex for continuous cubic-smoothed CDLOD geomorph factor
    vec2 raw_en_rel = vec2(nd.min_e_rel, nd.min_n_rel) + a_uv * nd.size_m;
    float h_probe = textureLod(u_height_arr, vec3(height_uv(a_uv), nd.page_slice), 0.0).r;
    float est_dist = length(vec3(raw_en_rel.x, h_probe - u_cam_alt_m, -raw_en_rel.y));
    float morph_lin = clamp((est_dist - nd.morph_start) / max(1.0, nd.morph_end - nd.morph_start), 0.0, 1.0);
    float morph_d = morph_lin * morph_lin * (3.0 - 2.0 * morph_lin);
    float morph = clamp(max(morph_d, 1.0 - nd.fade_in), 0.0, 1.0);

    // 2. CDLOD Vertex Geomorphing (65x65 grid -> 32 parent cells): odd verts slide smoothly onto even parent verts!
    vec2 frac_odd = fract(a_uv * 32.0);
    vec2 uv_m = a_uv - frac_odd * (morph / 32.0);
    vec2 p_uv = vec2(nd.parent_uv_off_x, nd.parent_uv_off_y) + uv_m * 0.5;

    float h_child  = textureLod(u_height_arr, vec3(height_uv(uv_m), nd.page_slice), 0.0).r;
    float h_parent = textureLod(u_height_arr, vec3(height_uv(p_uv), nd.parent_slice), 0.0).r;
    float h = mix(h_child, h_parent, morph);

    // Drape the existing orthophoto over the LiDAR canopy. Use the same
    // parent/child morph as the DEM so the canopy follows streamed detail.
    // Keep urban/roadside LiDAR trees intact (strict building/road/runway masks
    // are already applied in surface.r on the CPU page builder); only suppress on water.
    vec4 surf_child = textureLod(u_surf_arr, vec3(aux_uv(uv_m), nd.page_slice), 0.0);
    vec4 surf_parent = textureLod(u_surf_arr, vec3(aux_uv(p_uv), nd.parent_slice), 0.0);
    vec4 surface = mix(surf_child, surf_parent, morph);
    vec2 world_en = vec2(nd.world_min_e, nd.world_min_n) + uv_m * nd.size_m;
    float canopy_m = min(surface.r * 25.0, 13.5);
    // Crisp shoulder ramp so hedgerows & forest walls rise cleanly instead of melting into flat clay ramps.
    float canopy_cover = smoothstep(0.85, 2.65, canopy_m) *
                         (1.0 - smoothstep(0.18, 0.42, surface.b));
    float forest_lod_weight = 1.0 - smoothstep(5500.0, 24000.0, est_dist);

    // Cellular individual tree-crown domes (7.2 m spacing) break smooth LiDAR blobs into distinct
    // interlocking tree crowns with per-tree species variation (v_crown_id) and zero 3D region baking!
    vec2 cell_p = world_en * (1.0 / 7.2);
    vec2 cell_i = floor(cell_p);
    vec2 cell_f = fract(cell_p);
    float min_d2 = 4.0;
    float crown_id = 0.5;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            vec2 nb = vec2(float(dx), float(dy));
            vec2 rnd = hash22_v(cell_i + nb);
            vec2 diff = nb + (0.18 + 0.64 * rnd) - cell_f;
            float d2_c = dot(diff, diff);
            if (d2_c < min_d2) {
                min_d2 = d2_c;
                crown_id = rnd.x;
            }
        }
    }
    float dome = sqrt(max(0.0, 1.0 - min(1.0, min_d2 * 1.45)));
    float bough_jag = 0.5 + 0.5 * sin(world_en.x * 1.35 + crown_id * 6.28) * cos(world_en.y * 1.28 - crown_id * 4.1);
    // Live wind-driven tree sway & gust bending directly from RegionalAtmosphere u_wind_en!
    float wind_spd = length(u_wind_en);
    float wind_freq = 1.2 + clamp(wind_spd * 0.14, 0.0, 2.8);
    float wind_amp = (0.015 + clamp(wind_spd * 0.0085, 0.0, 0.16)) * smoothstep(1.8, 8.5, canopy_m);
    float wind_wave = sin(u_time * wind_freq + world_en.x * 0.28 + world_en.y * 0.24 + crown_id * 6.28);
    float sway = wind_wave * wind_amp;
    float near_crown = 1.0 - smoothstep(140.0, 620.0, est_dist);
    float crown_billow = mix(0.84 + 0.16 * sin(world_en.x * 0.23) * cos(world_en.y * 0.21),
                             (0.46 + 0.54 * dome) * (0.86 + 0.22 * bough_jag) * (0.84 + 0.32 * crown_id),
                             near_crown) + sway;
    float lift_gain = mix(0.54, 0.80, smoothstep(85.0, 420.0, est_dist));
    // Wide seamless crossfade (320..720 m distance, 160..340 m AGL) between ground-level volumetric
    // trees and distant LiDAR terrain canopy so there is zero abrupt LOD wall or pop-in.
    float vol_veg_mask = (1.0 - smoothstep(320.0, 720.0, est_dist)) * (1.0 - smoothstep(160.0, 340.0, u_cam_agl_m));
    float canopy_relief = canopy_m * lift_gain * canopy_cover * forest_lod_weight * crown_billow * (1.0 - vol_veg_mask);

    vec2 en_rel = vec2(nd.min_e_rel, nd.min_n_rel) + uv_m * nd.size_m;
    // Physically bend tall tree tops downwind in strong Cierzo / storm gusts!
    en_rel += u_wind_en * (0.028 * clamp(canopy_relief / 6.0, 0.0, 1.4) * (0.65 + 0.35 * wind_wave) * near_crown);

    v_node_size = nd.size_m;

    // 4. Camera-relative position with Earth curvature drop (exact double-precision origin handled on CPU!)
    vec3 pos = vec3(en_rel.x, h + canopy_relief - u_cam_alt_m, -en_rel.y);
    float d2 = dot(pos.xz, pos.xz);
    pos.y -= d2 / (2.0 * 6378137.0);

    v_world_pos = pos;
    v_world_en = world_en;
    v_child_uv = uv_m;
    v_parent_uv = p_uv;
    v_page_slice = nd.page_slice;
    v_parent_slice = nd.parent_slice;
    v_morph = morph_d;
    v_fade_in = nd.fade_in;
    v_elev_m = h;
    v_dist = length(pos);
    v_canopy_h = canopy_relief;
    v_crown_id = crown_id;

    gl_Position = u_vp * vec4(pos, 1.0);
}
