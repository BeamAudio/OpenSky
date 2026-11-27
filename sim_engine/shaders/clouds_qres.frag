#version 460 core

in vec2 v_uv;
layout(location = 0) out vec4 out_cloud; // Premultiplied RGB = cloud in-scattered radiance, A = opacity (1 - transmittance)

layout(binding = 0) uniform sampler2D u_cloud_noise;          // 256x256 seamless Perlin-Worley coverage & macro map
layout(binding = 1) uniform sampler3D u_cloud_vol3d;          // 64x64x64 isotropic 3D Perlin-Worley base cloud volume
layout(binding = 2) uniform sampler2D u_prev_cloud;           // Previous frame temporally reprojected cloud buffer
layout(binding = 3) uniform sampler2D u_terrain_depth;        // Reverse-Z static terrain depth
layout(binding = 4) uniform sampler2DArray u_atmo_field;      // 4 weather levels + rain (4) + cloud_sim (5) + dynamics (6)
layout(binding = 5) uniform sampler2D u_atmo_terrain;         // Regional solver terrain elevation
layout(binding = 6) uniform sampler3D u_cloud_detail;         // 64x64x64 4-octave tileable 3D Worley detail
layout(binding = 7) uniform sampler2DArray u_atmo_local_field;// Aircraft-following fine 8-16 km weather patch
layout(binding = 8) uniform sampler2D u_atmo_local_terrain;
layout(binding = 9) uniform sampler2D u_blue_noise;           // 64x64 tileable Void-and-Cluster blue noise

uniform mat4 u_inv_vp;
uniform mat4 u_prev_vp;
uniform vec3 u_camera_delta;
uniform vec3 u_sun_dir;
uniform vec2 u_cam_en;
uniform float u_cam_alt_m;
uniform float u_cloud_base_m;
uniform float u_cloud_thick_m;
uniform float u_coverage;
uniform float u_precip;
uniform float u_time;
uniform float u_temporal_blend;
uniform int u_steps;
uniform vec3 u_cloud_lit;
uniform vec3 u_cloud_shadow;
uniform vec3 u_haze_cool;
uniform vec3 u_haze_warm;
uniform vec4 u_atmo_domain;       // xy = min_e, min_n; zw = 1/span_e, 1/span_n
uniform vec4 u_atmo_local_domain; // zw=0 disables local patch
uniform vec4 u_wx_fx;             // x = valley_fog01, y = snow_factor01, z = lightning_flash01, w = storm_int01
uniform vec2 u_cloud_advect;      // Integrated rigid upwind offset (mid-cloud steering wind)
uniform vec2 u_field_advect;      // 60 Hz sub-step advection offset since last weather field publish
uniform float u_frame_phase;      // Monotonic golden-ratio sub-step phase [0..1)

const float R_EARTH = 6378137.0;
const float PI = 3.14159265;
const float MACRO_FREQ = 0.000112;

vec2 g_atmo_size = vec2(1.0);
vec2 g_local_size = vec2(1.0);

float remap01(float v, float lo, float hi) {
    return clamp((v - lo) / max(0.001, hi - lo), 0.0, 1.0);
}

// Normalized Henyey-Greenstein phase function multiplied by 4*pi (isotropic == 1.0).
float hg_phase_4pi(float cos_th, float g) {
    float g2 = g * g;
    return (1.0 - g2) / pow(max(0.03, 1.0 + g2 - 2.0 * g * cos_th), 1.5);
}

float intersect_shell_above(float dh, float vy) {
    float disc = max(0.0, vy * vy + (2.0 * dh) / R_EARTH);
    return (2.0 * dh) / max(0.0001, vy + sqrt(disc));
}

vec2 weather_uv(vec2 p_en) {
    vec2 q = clamp((p_en - u_atmo_domain.xy) * u_atmo_domain.zw, 0.0, 1.0);
    return (q * (g_atmo_size - 1.0) + 0.5) / g_atmo_size;
}

float weather_level(float agl) {
    if (agl < 550.0) return clamp((agl - 10.0) / 540.0, 0.0, 1.0);
    if (agl < 1800.0) return 1.0 + (agl - 550.0) / 1250.0;
    return 2.0 + clamp((agl - 1800.0) / 2700.0, 0.0, 1.0);
}

float local_weather_weight(vec2 p_en, out vec2 local_uv) {
    local_uv = vec2(0.5);
    if (u_atmo_local_domain.z <= 0.0 || u_atmo_local_domain.w <= 0.0) return 0.0;
    vec2 q = (p_en - u_atmo_local_domain.xy) * u_atmo_local_domain.zw;
    if (any(lessThan(q, vec2(0.0))) || any(greaterThan(q, vec2(1.0)))) return 0.0;
    local_uv = (q * (g_local_size - 1.0) + 0.5) / g_local_size;
    float edge = min(min(q.x, q.y), min(1.0 - q.x, 1.0 - q.y));
    return smoothstep(0.0, 0.16, edge);
}

float sample_regional_ground(vec2 p_en) {
    if (u_atmo_domain.z <= 0.0 || u_atmo_domain.w <= 0.0) return 0.0;
    vec2 local_uv;
    float weight = local_weather_weight(p_en, local_uv);
    if (weight >= 0.999) return textureLod(u_atmo_local_terrain, local_uv, 0.0).r;
    float coarse = textureLod(u_atmo_terrain, weather_uv(p_en), 0.0).r;
    if (weight <= 0.0) return coarse;
    return mix(coarse, textureLod(u_atmo_local_terrain, local_uv, 0.0).r, weight);
}

vec4 sample_regional_layer_fast(vec2 p_en, float layer, vec2 local_uv, float weight) {
    if (u_atmo_domain.z <= 0.0 || u_atmo_domain.w <= 0.0) return vec4(0.0);
    if (weight >= 0.999) return textureLod(u_atmo_local_field, vec3(local_uv, layer), 0.0);
    vec4 coarse = textureLod(u_atmo_field, vec3(weather_uv(p_en), layer), 0.0);
    if (weight <= 0.0) return coarse;
    return mix(coarse, textureLod(u_atmo_local_field, vec3(local_uv, layer), 0.0), weight);
}

vec4 sample_regional_layer(vec2 p_en, float layer) {
    vec2 local_uv;
    float weight = local_weather_weight(p_en, local_uv);
    return sample_regional_layer_fast(p_en, layer, local_uv, weight);
}

void sample_regional_column_hint(vec2 p_en, float altitude_msl, vec2 local_uv, float weight,
                                 out vec4 out_mf, out float out_rain, out vec4 out_sim, out vec4 out_dyn) {
    out_mf = vec4(0.0);
    out_rain = 0.0;
    out_sim = vec4(u_coverage, 0.68, 0.0, 0.0);
    out_dyn = vec4(0.0, 0.0, 0.0, 0.25);
    if (u_atmo_domain.z <= 0.0 || u_atmo_domain.w <= 0.0) return;

    vec4 fine = vec4(0.0), fine_sim = out_sim, fine_dyn = out_dyn;
    float fine_rain = 0.0;
    if (weight > 0.0) {
        float local_ground = textureLod(u_atmo_local_terrain, local_uv, 0.0).r;
        float lvl = weather_level(altitude_msl - local_ground);
        int li = clamp(int(floor(lvl + 0.5)), 0, 3);
        fine = textureLod(u_atmo_local_field, vec3(local_uv, float(li)), 0.0);
        fine_rain = textureLod(u_atmo_local_field, vec3(local_uv, 4.0), 0.0)[min(li, 3)];
        fine_sim = textureLod(u_atmo_local_field, vec3(local_uv, 5.0), 0.0);
        fine_dyn = textureLod(u_atmo_local_field, vec3(local_uv, 6.0), 0.0);
        if (weight >= 0.999) {
            out_mf = fine;
            out_rain = fine_rain;
            out_sim = fine_sim;
            out_dyn = fine_dyn;
            return;
        }
    }
    vec2 global_uv = weather_uv(p_en);
    float coarse_ground = textureLod(u_atmo_terrain, global_uv, 0.0).r;
    float clvl = weather_level(altitude_msl - coarse_ground);
    int ci = clamp(int(floor(clvl + 0.5)), 0, 3);
    vec4 coarse = textureLod(u_atmo_field, vec3(global_uv, float(ci)), 0.0);
    float coarse_rain = textureLod(u_atmo_field, vec3(global_uv, 4.0), 0.0)[min(ci, 3)];
    vec4 coarse_sim = textureLod(u_atmo_field, vec3(global_uv, 5.0), 0.0);
    vec4 coarse_dyn = textureLod(u_atmo_field, vec3(global_uv, 6.0), 0.0);
    out_mf   = (weight <= 0.0) ? coarse      : mix(coarse,      fine,      weight);
    out_rain = (weight <= 0.0) ? coarse_rain : mix(coarse_rain, fine_rain, weight);
    out_sim  = (weight <= 0.0) ? coarse_sim  : mix(coarse_sim,  fine_sim,  weight);
    out_dyn  = (weight <= 0.0) ? coarse_dyn  : mix(coarse_dyn,  fine_dyn,  weight);
}

float rain_extinction(vec2 p_en, float altitude, float depth_below_base) {
    if (u_atmo_domain.z <= 0.0 || u_atmo_domain.w <= 0.0) return 0.0;
    vec2 local_uv;
    float local_weight = local_weather_weight(p_en, local_uv);
    float local_ground = 0.0, local_rate = 0.0;
    if (local_weight > 0.0) {
        local_ground = textureLod(u_atmo_local_terrain, local_uv, 0.0).r;
        vec4 local_rain = textureLod(u_atmo_local_field, vec3(local_uv, 4.0), 0.0);
        float local_level = weather_level(altitude - local_ground);
        int local_low = min(int(local_level), 2);
        local_rate = mix(local_rain[local_low], local_rain[local_low + 1], local_level - float(local_low));
    }
    float ground = local_ground, rate = local_rate;
    if (local_weight < 0.999) {
        vec2 uv = weather_uv(p_en);
        float coarse_ground = textureLod(u_atmo_terrain, uv, 0.0).r;
        vec4 coarse_rain = textureLod(u_atmo_field, vec3(uv, 4.0), 0.0);
        float coarse_level = weather_level(altitude - coarse_ground);
        int coarse_low = min(int(coarse_level), 2);
        float coarse_rate = mix(coarse_rain[coarse_low], coarse_rain[coarse_low + 1], coarse_level - float(coarse_low));
        ground = local_weight <= 0.0 ? coarse_ground : mix(coarse_ground, local_ground, local_weight);
        rate = local_weight <= 0.0 ? coarse_rate : mix(coarse_rate, local_rate, local_weight);
    }
    if (altitude < ground || rate < 0.15) return 0.0;
    vec2 wind = sample_regional_layer_fast(p_en, 1.0, local_uv, local_weight).xy;
    float fall_spd = mix(7.5, 1.8, u_wx_fx.y);
    vec2 source = p_en - wind * (depth_below_base / fall_spd) + u_cloud_advect * 1.15;
    vec3 macro = textureLod(u_cloud_noise, source * MACRO_FREQ, 0.9).rgb;
    float shaft = smoothstep(0.46, 0.78, macro.r * 0.68 + macro.b * 0.32);
    vec3 streak = vec3(source * 0.00145, (altitude + u_time * fall_spd) * 0.00048);
    float bands = textureLod(u_cloud_vol3d, streak, 0.35).b;
    float flux = clamp(rate * 0.14, 0.0, 1.5);
    return flux * shaft * mix(0.28, 1.25, bands) * mix(0.00022, 0.00028, u_wx_fx.y);
}

float valley_fog_extinction(vec2 p_en, float altitude) {
    float ground = sample_regional_ground(p_en);
    float agl = altitude - ground;
    if (agl < -15.0 || agl > 460.0) return 0.0;
    float mist_cell = sample_regional_layer(p_en, 0.0).w;
    float h_falloff = exp(-max(0.0, agl) / 145.0) * (1.0 - smoothstep(260.0, 460.0, agl));
    vec2 advected_en = p_en + u_cloud_advect * 0.55;
    vec3 fog_n = textureLod(u_cloud_vol3d, vec3(advected_en * 0.00032, max(0.0, agl) * 0.0022), 0.4).rgb;
    float patch_d = smoothstep(0.26, 0.80, fog_n.r * 0.58 + fog_n.g * 0.32 + mist_cell * 0.48);
    return u_wx_fx.x * h_falloff * patch_d * 0.00115;
}

// Fast 3D Perlin-Worley density evaluator reusing precomputed macro weather_cov & top_h for near sun taps.
float eval_cloud_shape_fast(
    vec2 p_en, float alt_msl, float dist_m,
    float weather_cov, float top_h, float storm_amt, float sim_base_off,
    vec2 wind_base, vec2 local_shear, float inv_thick
) {
    float raw_h = (alt_msl - u_cloud_base_m) * inv_thick;
    float h_eff = (raw_h - sim_base_off) / max(0.35, 1.0 - sim_base_off);
    if (h_eff <= 0.001 || h_eff >= 0.999) return 0.0;
    float anvil = storm_amt * smoothstep(0.62, 0.92, h_eff) * (1.0 - smoothstep(0.92, 1.0, h_eff)) * 0.38;
    float h_grad = smoothstep(0.0, 0.11, h_eff) * clamp(1.0 - smoothstep(top_h * 0.30, top_h, h_eff) + anvil, 0.0, 1.0);
    if (h_grad <= 0.004) return 0.0;
    vec2 p_adv = p_en + wind_base - local_shear * (h_eff * h_eff);
    float base_lod = clamp(log2(max(1.0, dist_m * 0.00024)), 0.0, 2.5);
    vec3 shape_uv = vec3(p_adv * 0.00034, (alt_msl - u_cloud_base_m) * 0.00046);
    vec4 s3 = textureLod(u_cloud_vol3d, shape_uv, base_lod);
    float perlin_worley = s3.r * 0.48 + s3.g * 0.34 + s3.b * 0.18;
    float shaped = remap01(perlin_worley, 1.0 - weather_cov * h_grad * 0.84, 1.0);
    return clamp(shaped * mix(0.56, 1.24, smoothstep(0.05, 0.40, h_eff)), 0.0, 1.0);
}

// ============================================================================
// Unified 3D Volumetric Cloud Density Field (shared by primary & sun rays!)
// Guarantees 100% geometric alignment between clouds and their directional
// sunlight shadows, zero 2D vertical-pillar artifacts, and solid IMC cores.
// ============================================================================
float eval_cloud_density(
    vec2 p_en, float alt_msl, float dist_m,
    float eff_cov, float storm_amt, float sim_top, float sim_base_off,
    vec2 wind_base, vec2 local_shear, float inv_thick, bool with_detail,
    out float out_h01, out float out_wcov, out float out_toph
) {
    out_wcov = 0.0;
    out_toph = 0.5;
    float raw_h = (alt_msl - u_cloud_base_m) * inv_thick;
    float h_eff = (raw_h - sim_base_off) / max(0.35, 1.0 - sim_base_off);
    out_h01 = clamp(h_eff, 0.0, 1.0);
    if (h_eff <= 0.001 || h_eff >= 0.999) return 0.0;

    // Smooth wind-advected + altitude-sheared 3D world position.
    vec2 p_adv = p_en + wind_base - local_shear * (h_eff * h_eff);

    // 1. Macro 2D weather coverage (aperiodic 2-octave rotated lookup -> distinct 3-10 km cumulus fields).
    vec2 p_rot = mat2(0.80, -0.60, 0.60, 0.80) * p_adv;
    vec3 m0 = textureLod(u_cloud_noise, p_adv * 0.000082, 0.25).rgb;
    vec3 m1 = textureLod(u_cloud_noise, p_rot * 0.000185 + vec2(0.37, 0.71), 0.25).rgb;
    float macro_n = m0.r * 0.56 + m1.r * 0.30 + m0.b * 0.14;

    // Distinct broken cumulus fields with blue sky gaps in Fair/SCT/BKN; continuous sheet only in OVC/Storm (>80%).
    float cov_lo = mix(0.84, 0.28, eff_cov);
    float cov_hi = clamp(cov_lo + mix(0.20, 0.28, eff_cov), cov_lo + 0.08, 1.0);
    float weather_cov = remap01(macro_n, cov_lo, cov_hi);
    weather_cov = max(weather_cov, smoothstep(0.80, 0.96, eff_cov) * (0.45 + 0.55 * m0.b));
    out_wcov = weather_cov;
    if (weather_cov <= 0.004) return 0.0;

    // 2. Dramatic buoyant cumulus/cumulonimbus height envelope (flat LCL base, towering uneven convective pinnacles).
    float tower_seed = clamp(sim_top * 0.38 + m1.b * 0.44 + weather_cov * 0.34 + storm_amt * 0.20, 0.0, 1.0);
    float top_h = clamp(mix(0.22, 1.0, pow(tower_seed, 1.25)), 0.22, 0.99);
    out_toph = top_h;
    float anvil = storm_amt * smoothstep(0.62, 0.92, h_eff) * (1.0 - smoothstep(0.92, 1.0, h_eff)) * 0.38;
    float h_grad = smoothstep(0.0, 0.11, h_eff) * clamp(1.0 - smoothstep(top_h * 0.30, top_h, h_eff) + anvil, 0.0, 1.0);
    if (h_grad <= 0.004) return 0.0;

    // 3. True isotropic 3D Perlin-Worley base shape (bold sculpted 3D billowing cumulus towers!).
    float base_lod = clamp(log2(max(1.0, dist_m * 0.00020)), 0.0, 2.5);
    vec3 shape_uv = vec3(p_adv * 0.00034, (alt_msl - u_cloud_base_m) * 0.00046);
    vec4 s3 = textureLod(u_cloud_vol3d, shape_uv, base_lod);
    float perlin_worley = s3.r * 0.48 + s3.g * 0.34 + s3.b * 0.18;

    // Dimensional remap: sculpts distinct 3D billowing domes with deep crevices between puffs.
    float shaped = remap01(perlin_worley, 1.0 - weather_cov * h_grad * 0.84, 1.0);

    float density = shaped;
    float det_max_dist = (u_steps >= 37) ? 14000.0 : ((u_steps >= 27) ? 10000.0 : 7500.0);
    if (shaped > 0.003 && with_detail && dist_m < det_max_dist) {
        // 4. Crisp 3D Worley cauliflower erosion with distance-matched mip LOD (zero quarter-res stipple!).
        float det_lod = clamp(log2(max(1.0, dist_m * 0.00058)), 0.0, 3.0);
        vec2 curl = (s3.gb - 0.5) * (165.0 * (0.20 + 0.80 * h_eff));
        vec3 det_uv = vec3((p_adv + curl) * 0.00095, (alt_msl - u_cloud_base_m) * 0.00105);
        vec4 dw = textureLod(u_cloud_detail, det_uv, det_lod);
        float detail_fbm = dw.a * 0.48 + dw.b * 0.32 + dw.g * 0.20;
        float det_weight = (1.0 - smoothstep(det_max_dist * 0.55, det_max_dist, dist_m)) * 0.56 * (1.0 - 0.30 * shaped);
        float height_erode = mix(0.55, 1.20, smoothstep(0.08, 0.68, h_eff));
        density = remap01(shaped, (1.0 - detail_fbm) * det_weight * height_erode, 1.0);
    }

    // Localized near-camera IMC core fill (<200m only): guarantees zero-visibility whiteout inside dense clouds
    // without inflating external cloud silhouette or removing distant blue sky corridors.
    if (dist_m < 200.0) {
        float inside_fill = (1.0 - smoothstep(60.0, 200.0, dist_m))
                          * smoothstep(0.08, 0.38, weather_cov * h_grad)
                          * smoothstep(0.03, 0.14, h_eff);
        density = max(density, inside_fill * 0.90);
    }

    if (density <= 0.003) return 0.0;
    // Smooth vertical density ramp: lighter wisps at base, rich solid convective water content in mid/upper domes.
    density *= mix(0.56, 1.22, smoothstep(0.05, 0.40, h_eff));
    return clamp(density, 0.0, 1.0);
}

// Dynamic-quality directional sun light march driven by Settings weather_detail & FlightFrameBudget (u_steps):
//   - Low/Balanced (u_steps <= 24): 2-tap fast solver (locked 60 FPS on iGPU)
//   - Medium/High  (25..36):        3-tap balanced solver
//   - Ultra/Max    (u_steps >= 37): 4-tap deep convective solver
float march_sun_optical_depth(
    vec2 p_en, float alt_msl, float dist_m,
    float eff_cov, float weather_cov, float top_h, float storm_amt, float sim_top, float sim_base_off,
    vec2 wind_base, vec2 local_shear, float inv_thick,
    vec2 s_en, float s_y
) {
    float tau = 0.0;
    float cloud_top_msl = u_cloud_base_m + u_cloud_thick_m;

    if (u_steps <= 26) {
        float a0 = alt_msl + s_y * 75.0;
        if (a0 < cloud_top_msl) {
            tau += eval_cloud_shape_fast(p_en + s_en * 75.0, a0, dist_m + 75.0,
                                         weather_cov, top_h, storm_amt, sim_base_off,
                                         wind_base, local_shear, inv_thick) * 240.0;
        }
        float a1 = alt_msl + s_y * 420.0;
        if (a1 < cloud_top_msl) {
            tau += eval_cloud_shape_fast(p_en + s_en * 420.0, a1, dist_m + 420.0,
                                         weather_cov, top_h, storm_amt, sim_base_off,
                                         wind_base, local_shear, inv_thick) * 820.0;
        }
    } else if (u_steps <= 36) {
        float a0 = alt_msl + s_y * 55.0;
        if (a0 < cloud_top_msl) {
            tau += eval_cloud_shape_fast(p_en + s_en * 55.0, a0, dist_m + 55.0,
                                         weather_cov, top_h, storm_amt, sim_base_off,
                                         wind_base, local_shear, inv_thick) * 125.0;
        }
        float a1 = alt_msl + s_y * 210.0;
        if (a1 < cloud_top_msl) {
            tau += eval_cloud_shape_fast(p_en + s_en * 210.0, a1, dist_m + 210.0,
                                         weather_cov, top_h, storm_amt, sim_base_off,
                                         wind_base, local_shear, inv_thick) * 285.0;
        }
        float a2 = alt_msl + s_y * 700.0;
        if (a2 < cloud_top_msl) {
            float dh2 = 0.5, dwc2 = 0.0, dth2 = 0.5;
            tau += eval_cloud_density(p_en + s_en * 700.0, a2, dist_m + 700.0,
                                      eff_cov, storm_amt, sim_top, sim_base_off,
                                      wind_base, local_shear, inv_thick, false, dh2, dwc2, dth2) * 760.0;
        }
    } else {
        float a0 = alt_msl + s_y * 45.0;
        if (a0 < cloud_top_msl) {
            tau += eval_cloud_shape_fast(p_en + s_en * 45.0, a0, dist_m + 45.0,
                                         weather_cov, top_h, storm_amt, sim_base_off,
                                         wind_base, local_shear, inv_thick) * 95.0;
        }
        float a1 = alt_msl + s_y * 165.0;
        if (a1 < cloud_top_msl) {
            tau += eval_cloud_shape_fast(p_en + s_en * 165.0, a1, dist_m + 165.0,
                                         weather_cov, top_h, storm_amt, sim_base_off,
                                         wind_base, local_shear, inv_thick) * 190.0;
        }
        float a2 = alt_msl + s_y * 440.0;
        if (a2 < cloud_top_msl) {
            float dh2 = 0.5, dwc2 = 0.0, dth2 = 0.5;
            tau += eval_cloud_density(p_en + s_en * 440.0, a2, dist_m + 440.0,
                                      eff_cov, storm_amt, sim_top, sim_base_off,
                                      wind_base, local_shear, inv_thick, false, dh2, dwc2, dth2) * 380.0;
        }
        float a3 = alt_msl + s_y * 1000.0;
        if (a3 < cloud_top_msl) {
            float dh3 = 0.5, dwc3 = 0.0, dth3 = 0.5;
            tau += eval_cloud_density(p_en + s_en * 1000.0, a3, dist_m + 1000.0,
                                      eff_cov, storm_amt, sim_top, sim_base_off,
                                      wind_base, local_shear, inv_thick, false, dh3, dwc3, dth3) * 650.0;
        }
    }
    return tau * mix(0.0145, 0.0195, storm_amt);
}

void main() {
    g_atmo_size = vec2(textureSize(u_atmo_terrain, 0));
    if (u_atmo_local_domain.z > 0.0 && u_atmo_local_domain.w > 0.0)
        g_local_size = vec2(textureSize(u_atmo_local_terrain, 0));

    vec2 ndc = v_uv * 2.0 - 1.0;
    vec4 wp = u_inv_vp * vec4(ndc, 0.0001, 1.0);
    vec3 view_dir = normalize(wp.xyz / max(1e-5, wp.w));
    float vy = view_dir.y;

    float scene_d = textureLod(u_terrain_depth, v_uv, 0.0).r;
    float max_ray_t = 78000.0;
    if (scene_d > 0.000001) {
        vec4 gp = u_inv_vp * vec4(ndc, scene_d, 1.0);
        max_ray_t = min(max_ray_t, length(gp.xyz / max(1e-6, gp.w)));
    }

    float slab_base = u_cloud_base_m;
    float slab_top  = u_cloud_base_m + u_cloud_thick_m;
    float t0 = 0.0, t1 = 0.0;
    bool valid = true;
    if (u_cam_alt_m < slab_base) {
        if (vy < -0.004) valid = false;
        else {
            float vy_c = max(0.0, vy);
            t0 = intersect_shell_above(slab_base - u_cam_alt_m, vy_c);
            t1 = intersect_shell_above(slab_top  - u_cam_alt_m, vy_c);
        }
    } else if (u_cam_alt_m > slab_top) {
        if (vy >= -0.006) valid = false;
        else {
            float d_top = u_cam_alt_m - slab_top;
            float d_bot = u_cam_alt_m - slab_base;
            float disc0 = vy * vy - (2.0 * d_top) / R_EARTH;
            if (disc0 < 0.0) valid = false;
            else {
                t0 = (2.0 * d_top) / max(0.0001, -vy + sqrt(disc0));
                float disc1 = vy * vy - (2.0 * d_bot) / R_EARTH;
                t1 = (disc1 > 0.0) ? ((2.0 * d_bot) / max(0.0001, -vy + sqrt(disc1))) : (t0 + 22000.0);
            }
        }
    } else {
        t0 = 2.0;
        if (vy >= 0.0) {
            t1 = intersect_shell_above(slab_top - u_cam_alt_m, vy);
        } else {
            float d_bot = max(1.0, u_cam_alt_m - slab_base);
            float disc1 = vy * vy - (2.0 * d_bot) / R_EARTH;
            t1 = (disc1 > 0.0) ? ((2.0 * d_bot) / max(0.0001, -vy + sqrt(disc1))) : 18000.0;
        }
    }

    t0 = max(2.0, t0);
    t1 = min(t1, max_ray_t);

    float bn = texelFetch(u_blue_noise, ivec2(gl_FragCoord.xy) & 63, 0).r;
    float sub_jitter = fract(bn + u_frame_phase);

    vec3 sdir = normalize(u_sun_dir);
    float cos_th = dot(view_dir, sdir);
    vec2 ray_en = vec2(view_dir.x, -view_dir.z);
    float sun_vis = clamp((sdir.y + 0.06) / 0.20, 0.0, 1.0);

    float trans = 1.0;
    vec3 accum = vec3(0.0);
    float w_dist = 0.0, w_sum = 0.0;

    // 1. Rain shafts & valley radiation fog below cloud base.
    float rain_end = min(max_ray_t, 12000.0);
    if (u_cam_alt_m < u_cloud_base_m && vy > 0.002) {
        rain_end = min(rain_end, intersect_shell_above(u_cloud_base_m - u_cam_alt_m, vy));
    }
    float rain_strength = max(smoothstep(0.05, 0.14, u_precip), smoothstep(0.32, 0.54, u_wx_fx.w));
    float rain_visibility = rain_strength * (1.0 - smoothstep(200.0, 500.0, u_cam_alt_m - u_cloud_base_m));
    bool do_rain = rain_visibility > 0.0 && rain_end > 12.0;
    bool do_vfog = (u_wx_fx.x > 0.04) && rain_end > 18.0;
    if (do_rain || do_vfog) {
        float sun_scatter = pow(max(0.0, cos_th), 4.0) * sun_vis;
        vec3 rain_col = mix(u_cloud_shadow * 0.34, u_haze_cool * 0.54, 0.36) + u_cloud_lit * (sun_scatter * 0.32);
        vec3 snow_col = mix(u_haze_cool * 0.86, u_cloud_lit * 1.00, 0.58);
        vec3 precip_light = mix(rain_col, snow_col, u_wx_fx.y) + vec3(0.45, 0.58, 0.82) * (u_wx_fx.z * 0.55);
        vec3 fog_light = mix(u_haze_cool * 0.82, u_cloud_lit * 0.90, 0.48) + u_haze_warm * (sun_scatter * 0.22);
        for (int i = 0; i < 4; ++i) {
            if (trans < 0.04) break;
            float a = float(i) * 0.25, b = float(i + 1) * 0.25;
            float t = mix(a * a, b * b, sub_jitter) * rain_end;
            float alt = u_cam_alt_m + vy * t + (1.0 - vy * vy) * t * t / (2.0 * R_EARTH);
            float seg_len = rain_end * (b * b - a * a);
            float sig_r = 0.0;
            if (do_rain) {
                float depth = max(0.0, u_cloud_base_m - alt);
                sig_r = rain_extinction(u_cam_en + ray_en * t, alt, depth) * rain_visibility;
            }
            float sig_f = do_vfog ? valley_fog_extinction(u_cam_en + ray_en * t, alt) : 0.0;
            float sig_tot = sig_r + sig_f;
            if (sig_tot > 1e-6) {
                float alpha = 1.0 - exp(-sig_tot * seg_len);
                float min_t = (do_vfog && sig_f > sig_r) ? 0.10 : 0.48;
                float next_trans = max(min_t, trans * (1.0 - alpha));
                float w = max(0.0, trans - next_trans);
                vec3 col = (precip_light * sig_r + fog_light * sig_f) / sig_tot;
                accum += col * w;
                w_dist += t * w;
                w_sum += w;
                trans = next_trans;
            }
        }
    }

    // 2. Physically-Based Volumetric Cloud Raymarching with True Directional Sunlight Transport.
    if (valid && t1 > t0 && t0 < 72000.0) {
        vec2 wind_base = u_cloud_advect;
        float inv_thick = 1.0 / max(180.0, u_cloud_thick_m);
        vec2 s_en = vec2(sdir.x, -sdir.z);
        float s_y = max(0.05, sdir.y);

        // Normalized dual-lobe Mie scattering phase: bright crisp sunlit fronts + soft silver lining toward sun!
        float p_fwd  = hg_phase_4pi(cos_th,  0.68);
        float p_back = hg_phase_4pi(cos_th, -0.26);
        float phase_primary = mix(p_back, p_fwd, 0.58);
        float phase_ms      = mix(0.85, hg_phase_4pi(cos_th, 0.28), 0.45);
        float sun_glow_ang  = pow(max(0.0, cos_th), 5.0);

        bool have_atmo = (u_atmo_domain.z > 0.0 && u_atmo_domain.w > 0.0);
        vec3 hcol = mix(u_haze_cool, u_haze_warm * 1.12, pow(max(0.0, cos_th), 3.0) * 0.55 * sun_vis);
        // Synoptic vertical wind shear (L3 - L1) varies over tens of km; sample once at camera column for speed.
        vec2 cam_shear = have_atmo ? clamp(sample_regional_layer(u_cam_en, 6.0).gb, vec2(-28.0), vec2(28.0)) * 18.0 : vec2(0.0);

        // Smooth total step budget: 6 near-field steps when inside cloud layer + main volumetric march.
        int main_steps = clamp(u_steps, 16, 52);
        bool in_slab_near = (t0 < 120.0 && min(t1, 220.0) > t0 + 2.0);
        int total_steps = main_steps + (in_slab_near ? 6 : 0);
        float nf_end = in_slab_near ? min(t1, 220.0) : t0;
        float nf_span = nf_end - t0;
        float rem_len = max(0.0, min(t1 - nf_end, 26000.0));
        float m_focus = exp(-nf_end / 3500.0);
        float inv_main = 1.0 / float(main_steps);
        float prev_t = t0;

        for (int idx = 0; idx < 58; ++idx) {
            if (idx >= total_steps || trans < 0.022 || prev_t >= max_ray_t) break;

            float seg_hi;
            if (in_slab_near && idx < 6) {
                float nb = float(idx + 1) / 6.0;
                seg_hi = min(max_ray_t, t0 + nf_span * (nb * nb));
            } else {
                int mi = idx - (in_slab_near ? 6 : 0);
                float u = float(mi + 1) * inv_main;
                float wu = mix(0.55 * u * u + 0.45 * u, 0.40 * u * u + 0.60 * u, m_focus);
                seg_hi = min(max_ray_t, nf_end + rem_len * wu);
            }
            float dt_cur = seg_hi - prev_t;
            if (dt_cur <= 0.5) {
                prev_t = seg_hi;
                continue;
            }
            // Balanced [0.12..0.88] blue-noise sub-step jitter eliminates slice bands while keeping variance low for TAA.
            float t = mix(prev_t, seg_hi, mix(0.12, 0.88, sub_jitter));
            prev_t = seg_hi;

            vec3 rp = view_dir * t;
            float curv_rise = dot(rp.xz, rp.xz) / (2.0 * R_EARTH);
            float sample_alt = u_cam_alt_m + rp.y + curv_rise;
            vec2 p_en = u_cam_en + ray_en * t;

            // Sample solved cloud_sim field (layer 5) at 60 Hz continuously-advected p_field (zero 5 Hz stepping!).
            float rain01 = 0.0;
            float eff_cov = u_coverage;
            float storm_amt = clamp(u_precip * 0.75, 0.0, 1.0);
            float sim_top = 0.72;
            float sim_base_off = 0.0;
            vec2 local_shear = cam_shear;
            if (have_atmo) {
                vec2 p_field = p_en + u_field_advect;
                vec2 luv;
                float lw = local_weather_weight(p_field, luv);
                vec4 step_sim = sample_regional_layer_fast(p_field, 5.0, luv, lw);
                float sim_cov = clamp(step_sim.r, 0.0, 1.0);
                eff_cov = clamp(mix(u_coverage, sim_cov, 0.78), 0.04, 0.99);
                rain01 = clamp(step_sim.a * 1.15, 0.0, 1.0);
                storm_amt = clamp(max(u_precip * 0.75, step_sim.a * 0.92), 0.0, 1.0);
                sim_top = clamp(step_sim.g, 0.30, 1.0);
                sim_base_off = clamp(step_sim.b, 0.0, 0.18);
            }

            float h01 = 0.5, wcov = 0.0, toph = 0.5;
            float d = eval_cloud_density(
                p_en, sample_alt, t,
                eff_cov, storm_amt, sim_top, sim_base_off,
                wind_base, local_shear, inv_thick, true, h01, wcov, toph
            );
            if (d <= 0.003) continue;

            // Real directional light transport through the SAME 3D cloud volume toward u_sun_dir!
            float tau_sun = (d > 0.014) ? march_sun_optical_depth(
                p_en, sample_alt, t,
                eff_cov, wcov, toph, storm_amt, sim_top, sim_base_off,
                wind_base, local_shear, inv_thick, s_en, s_y
            ) : ((1.0 - h01) * 0.55);

            // Balanced multi-octave Beer-Powder direct sunlight + warm-key / cool-shadow sculpting!
            float powder = mix(1.0 - 0.58 * exp(-d * 4.8), 1.0, smoothstep(0.08, 0.78, cos_th));
            float direct_oct0 = exp(-tau_sun * 1.65) * phase_primary * powder;
            float direct_oct1 = 0.34 * exp(-tau_sun * 0.38) * phase_ms;
            float direct_oct2 = 0.11 * exp(-tau_sun * 0.10);
            float sun_strength = (direct_oct0 + direct_oct1 + direct_oct2) * sun_vis * mix(1.95, 1.02, storm_amt);
            vec3 direct_lit = u_cloud_lit * vec3(1.05, 1.01, 0.95) * sun_strength;
            // Warm backlit silver lining along thin sun-facing cloud rims.
            direct_lit += u_cloud_lit * vec3(1.22, 1.08, 0.88) * (sun_glow_ang * exp(-tau_sun * 0.62) * sun_vis * 0.54);

            // Natural atmospheric sky-dome & ground bounce ambient (clean cool-slate undersides without harsh black crush).
            float h_amb = smoothstep(0.05, 0.86, h01);
            float ao = mix(0.24, 0.92, h_amb) * mix(0.36, 1.0, exp(-tau_sun * 0.65));
            vec3 ground_bounce = u_cloud_shadow * vec3(0.76, 0.86, 1.10) * mix(0.34, 0.22, storm_amt);
            vec3 sky_fill = mix(u_haze_cool * vec3(0.80, 0.90, 1.12) * 0.78, u_cloud_lit * 0.38, 0.20);
            vec3 ambient = mix(ground_bounce, sky_fill, h_amb) * ao;

            // Inside-cloud isotropic multiple-scattering floor so zero-vis IMC reads as bright milky/overcast mist.
            float in_cloud_w = (1.0 - smoothstep(80.0, 240.0, t)) * smoothstep(0.08, 0.55, d);
            vec3 imc_mist = mix(u_cloud_lit * 0.48, mix(u_cloud_shadow, u_haze_cool, 0.55) * 0.62, storm_amt * 0.80) * (0.18 + 0.82 * sun_vis);
            vec3 step_col = ambient + direct_lit + imc_mist * (in_cloud_w * 0.55);

            // Storm tint & internal lightning volumetric flash.
            step_col *= mix(vec3(1.0), vec3(0.66, 0.73, 0.85), storm_amt * 0.62);
            if (u_wx_fx.z > 0.02) {
                float flash = u_wx_fx.z * smoothstep(0.14, 0.65, d) * (1.0 - abs(h01 - 0.45)) * (0.45 + 0.85 * rain01);
                step_col += vec3(0.76, 0.88, 1.24) * (flash * 2.4);
            }

            // Aerial perspective integrated directly per step (no distant color snap!).
            float aerial = smoothstep(14000.0, 46000.0, t) * 0.78;
            step_col = mix(step_col, hcol, aerial);

            // Dual-regime physical extinction: dense zero-vis IMC inside clouds (<180m) + soft ridge/crest fusion!
            float near_fade = smoothstep(1.5, 10.0, t);
            float terrain_fade = smoothstep(0.0, max(90.0, dt_cur * 0.95), max_ray_t - t);
            float sigma_t = mix(0.055, 0.0095, smoothstep(110.0, 320.0, t)) * near_fade * terrain_fade;
            float seg_tau = d * dt_cur * sigma_t * (1.0 + storm_amt * 0.35);
            float seg_trans = exp(-seg_tau);

            // Energy-conserving analytical step integration.
            float w = trans * (1.0 - seg_trans);
            accum += step_col * w;
            w_dist += t * w;
            w_sum += w;
            trans *= seg_trans;
        }

        // High-altitude sunlit Cirrus wisps at 7,600m MSL.
        if (trans > 0.10 && vy > 0.045 && u_cam_alt_m < 7500.0 && max_ray_t > 20000.0) {
            float tc = intersect_shell_above(7600.0 - u_cam_alt_m, vy);
            if (tc < min(90000.0, max_ray_t)) {
                vec2 c_en = u_cam_en + ray_en * tc + vec2(u_time * 36.0, u_time * 12.0);
                vec2 c_uv = vec2(c_en.x * 0.000018, c_en.y * 0.000046);
                vec2 c_warp = vec2(sin(c_uv.y * 6.0 + u_time * 0.02) * 0.04, 0.0);
                float cw = textureLod(u_cloud_noise, c_uv + c_warp, 0.0).g *
                           textureLod(u_cloud_noise, c_uv * 2.1 + vec2(0.37, 0.11) - c_warp, 0.0).r;
                float cirrus = smoothstep(0.44, 0.78, cw) * 0.26 * smoothstep(0.045, 0.18, vy) * (1.0 - smoothstep(45000.0, 90000.0, tc));
                if (cirrus > 0.005) {
                    vec3 cc = mix(u_cloud_lit * 0.92, u_haze_warm * 1.12, 0.35 + 0.35 * pow(max(0.0, cos_th), 4.0)) * (0.15 + 0.85 * sun_vis);
                    accum += cc * (cirrus * trans);
                    w_dist += tc * (cirrus * trans);
                    w_sum += cirrus * trans;
                    trans *= (1.0 - cirrus);
                }
            }
        }
    }

    float final_a = clamp(1.0 - trans, 0.0, 1.0);
    float avg_d = (w_sum > 0.001) ? (w_dist / w_sum) : 46000.0;
    vec4 cur_col = vec4(0.0);
    if (final_a > 0.001) {
        float far_fade = 1.0 - smoothstep(32000.0, 62000.0, avg_d);
        if (trans <= 0.025 && avg_d < 26000.0) final_a = 1.0;
        cur_col = vec4(accum * far_fade, final_a * far_fade);
    }

    // Motion-compensated temporal accumulation with relative tolerance (keeps clouds velvety smooth, ZERO speckle!).
    vec4 prev_clip = (w_sum > 0.001)
        ? (u_prev_vp * vec4(view_dir * avg_d + u_camera_delta, 1.0))
        : (u_prev_vp * vec4(view_dir, 0.0));
    vec2 prev_uv = (prev_clip.xy / max(1e-5, prev_clip.w)) * 0.5 + 0.5;
    float near_tb = mix(0.62, 1.0, smoothstep(60.0, 1200.0, avg_d));
    if (u_temporal_blend * near_tb > 0.01 && prev_clip.w > 0.0 &&
        prev_uv.x > 0.003 && prev_uv.x < 0.997 && prev_uv.y > 0.003 && prev_uv.y < 0.997) {
        vec4 hist = textureLod(u_prev_cloud, prev_uv, 0.0);
        // Motion-compensated history should converge full blue-noise jitter on high-contrast crests;
        // only reject large silhouette disocclusions so neither ghosting nor jitter grain appears.
        float alpha_diff = abs(hist.a - cur_col.a);
        float gate = 1.0 - smoothstep(0.55, 0.92, alpha_diff);
        out_cloud = mix(cur_col, hist, u_temporal_blend * near_tb * gate);
    } else {
        out_cloud = cur_col;
    }
}
