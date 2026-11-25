#version 460 core

in vec2 v_uv;
layout(location = 0) out vec4 out_cloud; // RGB = cloud lit color, A = opacity (1 - transmittance)

layout(binding = 0) uniform sampler2D u_cloud_noise;
layout(binding = 1) uniform sampler3D u_cloud_vol3d; // Native hardware 128x128x16 GL_TEXTURE_3D (1-tap trilinear wrap!)
layout(binding = 2) uniform sampler2D u_prev_cloud;  // Previous frame quarter-res temporal history!
layout(binding = 3) uniform sampler2D u_terrain_depth; // Opaque terrain depth copied before moving objects draw.
layout(binding = 4) uniform sampler2DArray u_atmo_field; // Four weather levels plus local rain rates
layout(binding = 5) uniform sampler2D u_atmo_terrain;     // Terrain elevation for terrain-following AGL layer selection
layout(binding = 6) uniform sampler3D u_cloud_detail;     // Mip-filtered meter-scale 3D cellular detail

uniform mat4 u_inv_vp;
uniform mat4 u_prev_vp;
uniform vec3 u_camera_delta; // Current aircraft origin minus previous origin in ENU render axes
uniform vec3 u_sun_dir;
uniform vec2 u_cam_en;
uniform float u_cam_alt_m;
uniform float u_cloud_base_m;
uniform float u_cloud_thick_m;
uniform float u_coverage;
uniform float u_precip;
uniform float u_time;
uniform int u_frame_idx;
uniform int u_sparse_period; // 1=full rays, 2=half, 4=one 2x2 Bayer phase.
uniform float u_temporal_blend;
uniform int u_steps;
uniform vec3 u_cloud_lit;
uniform vec3 u_cloud_shadow;
uniform vec3 u_haze_cool;
uniform vec3 u_haze_warm;
uniform vec4 u_atmo_domain; // xy = min_e, min_n; zw = 1/span_e, 1/span_n
uniform vec4 u_wx_fx;       // x = valley_fog01, y = snow_factor01, z = lightning_flash01, w = storm_int01

const float R_EARTH = 6378137.0;
const float MACRO_FREQ = 0.000112;

float ign_noise(vec2 px) {
    return fract(52.9829189 * fract(dot(px, vec2(0.06711056, 0.00583715))));
}

float remap01(float v, float lo, float hi) {
    return clamp((v - lo) / max(0.001, hi - lo), 0.0, 1.0);
}

float hg_phase(float cos_th, float g) {
    float g2 = g * g;
    return (1.0 - g2) / (4.0 * 3.14159265 * pow(max(0.02, 1.0 + g2 - 2.0 * g * cos_th), 1.5));
}

float intersect_shell_above(float dh, float vy) {
    float disc = max(0.0, vy * vy + (2.0 * dh) / R_EARTH);
    return (2.0 * dh) / max(0.0001, vy + sqrt(disc));
}

vec2 weather_uv(vec2 p_en) {
    vec2 size = vec2(textureSize(u_atmo_terrain, 0));
    vec2 q = clamp((p_en - u_atmo_domain.xy) * u_atmo_domain.zw, 0.0, 1.0);
    return (q * (size - 1.0) + 0.5) / size;
}

float weather_level(float agl) {
    if (agl < 550.0) return clamp((agl - 10.0) / 540.0, 0.0, 1.0);
    if (agl < 1800.0) return 1.0 + (agl - 550.0) / 1250.0;
    return 2.0 + clamp((agl - 1800.0) / 2700.0, 0.0, 1.0);
}

vec4 sample_regional_atmo(vec2 p_en, float altitude_msl) {
    vec2 auv = weather_uv(p_en);
    float ground_msl = textureLod(u_atmo_terrain, auv, 0.0).r;
    float level = weather_level(altitude_msl - ground_msl);
    float l0 = min(floor(level), 2.0);
    return mix(textureLod(u_atmo_field, vec3(auv, l0), 0.0),
               textureLod(u_atmo_field, vec3(auv, l0 + 1.0), 0.0), level - l0);
}

vec4 cloud_cells(vec2 pos_en, float altitude, vec3 flow, float distance_m, float step_m) {
    // The texture spans 64 m and the smallest cellular features span 2 m.
    // Use the ray's cross section and segment length to filter unresolved detail.
    float footprint = max(distance_m * 0.0027, step_m * 0.62);
    float lod = clamp(log2(max(1.0, footprint)), 0.0, 6.0);
    vec3 p = vec3(pos_en.x, altitude, pos_en.y) - flow.xzy * u_time;
    p.xy = mat2(0.80, 0.60, -0.60, 0.80) * p.xy;
    p.yz = mat2(0.923, -0.385, 0.385, 0.923) * p.yz;
    return textureLod(u_cloud_detail, p * (1.0 / 64.0), lod);
}

float erode_cloud(float shape, vec4 cells, float strength) {
    float carving = dot(cells, vec4(0.34, 0.28, 0.22, 0.16));
    float fringe = 1.0 - smoothstep(0.26, 0.90, shape);
    return max(0.0, shape + (carving - 0.54) * strength * fringe);
}

float rain_extinction(vec2 p_en, float altitude, float depth_below_base) {
    vec2 uv = weather_uv(p_en);
    float ground = textureLod(u_atmo_terrain, uv, 0.0).r;
    if (altitude < ground) return 0.0;
    vec4 rain = textureLod(u_atmo_field, vec3(uv, 4.0), 0.0);
    float level = weather_level(altitude - ground);
    int low = min(int(level), 2);
    float rate = mix(rain[low], rain[low + 1], level - float(low));
    if (rate < 0.15) return 0.0;
    vec2 wind = textureLod(u_atmo_field, vec3(uv, 1.0), 0.0).xy;
    // Backtrace falling rain/snow to its parent storm cell; streaks slant with wind shear!
    float fall_spd = mix(7.5, 1.8, u_wx_fx.y);
    vec2 source = p_en - wind * (depth_below_base / fall_spd + u_time * 1.45);
    vec3 macro = textureLod(u_cloud_noise, source * MACRO_FREQ, 0.9).rgb;
    // Discrete localized rain/snow curtains hanging under dense storm cells (never an opaque gray sky lid!)
    float shaft = smoothstep(0.48, 0.78, macro.r * 0.68 + macro.b * 0.32);
    vec3 streak = vec3(source * 0.00145, (altitude + u_time * fall_spd) * 0.00048);
    float bands = textureLod(u_cloud_vol3d, streak, 0.0).g;
    float flux = clamp(rate * 0.14, 0.0, 1.5);
    return flux * shaft * mix(0.28, 1.25, bands) * mix(0.00022, 0.00028, u_wx_fx.y);
}

float valley_fog_extinction(vec2 p_en, float altitude) {
    vec2 uv = weather_uv(p_en);
    float ground = textureLod(u_atmo_terrain, uv, 0.0).r;
    float agl = altitude - ground;
    if (agl < -15.0 || agl > 460.0) return 0.0;
    float mist_cell = textureLod(u_atmo_field, vec3(uv, 0.0), 0.0).w;
    float h_falloff = exp(-max(0.0, agl) / 145.0) * (1.0 - smoothstep(260.0, 460.0, agl));
    vec3 fog_n = textureLod(u_cloud_vol3d, vec3(p_en * 0.00032 + vec2(u_time * 0.004, 0.0), max(0.0, agl) * 0.0022), 0.0).rgb;
    float patch_d = smoothstep(0.24, 0.78, fog_n.r * 0.68 + fog_n.g * 0.32 + mist_cell * 0.85);
    return u_wx_fx.x * h_falloff * patch_d * 0.00115;
}

// Cheap macro envelope (2D taps only) so empty-air raymarch steps skip 3D Worley & sun cone taps!
float sample_cloud_macro(vec2 p_en, float h01, vec2 wind_base, float eff_cov, vec4 mf, out vec2 out_p, out vec3 out_ma, out vec3 out_mb, out float out_heff) {
    out_p = p_en + wind_base - mf.xy * (18.0 * h01 * h01);
    out_ma = textureLod(u_cloud_noise, out_p * MACRO_FREQ, 0.25).rgb;
    out_mb = textureLod(u_cloud_noise, (out_p + vec2(-u_time * 3.0, u_time * 5.0)) * 0.000105 + vec2(0.31, 0.67), 0.35).rgb;
    float base_lift = (out_mb.g - 0.36) * 0.08;
    out_heff = clamp((h01 - base_lift) / max(0.32, 1.0 - base_lift), 0.0, 1.0);
    if (out_heff <= 0.001) return 0.0;

    float raw_cov = out_ma.r * 0.62 + out_mb.b * 0.38;
    float local_lo = clamp(mix(0.73, 0.32, eff_cov) - mf.w * 0.30, 0.20, 0.78);
    float cover = remap01(raw_cov, local_lo, local_lo + 0.24);
    float ctype = clamp(out_ma.b * 0.55 + out_mb.r * 0.45 + eff_cov * 0.22 + u_precip * 0.28 + mf.z * 0.055, 0.0, 1.0);
    float top = mix(0.32, 0.98, ctype);
    float anvil = smoothstep(0.68, 0.94, out_heff) * smoothstep(0.24, 0.72, u_precip + max(0.0, mf.z) * 0.10) * 0.24;
    float profile = smoothstep(0.0, 0.16, out_heff) * clamp(1.0 - smoothstep(top * 0.50, top, out_heff) + anvil, 0.0, 1.0);
    float lenticular = remap01(out_mb.r * 0.60 + out_ma.g * 0.40,
                               0.64 - eff_cov * 0.14, 0.86 - eff_cov * 0.12) *
                       smoothstep(0.78, 0.86, h01) * (1.0 - smoothstep(0.89, 0.98, h01)) * 0.20;
    return max(cover * profile, lenticular);
}

// Fast 2-tap density helper used by the distant steps of the 3-step logarithmic sun cone raymarch.
float sample_cloud_shadow(vec2 p_en, float h01, vec2 wind_base, float eff_cov, float w_lod) {
    if (h01 <= 0.0 || h01 >= 1.0) return 0.0;
    vec2 p = p_en + wind_base;
    vec3 ma = textureLod(u_cloud_noise, p * MACRO_FREQ, 0.8).rgb;
    float local_lo = clamp(mix(0.73, 0.32, eff_cov), 0.24, 0.78);
    float cover = remap01(ma.r * 0.74 + ma.b * 0.26, local_lo, local_lo + 0.24);
    float top = mix(0.36, 0.98, clamp(ma.b * 0.75 + eff_cov * 0.25 + u_precip * 0.25, 0.0, 1.0));
    float base_shape = cover * smoothstep(0.0, 0.08, h01) * (1.0 - smoothstep(top * 0.50, top, h01));
    if (base_shape <= 0.02) return 0.0;
    vec3 w_coord = vec3(p * 0.00070, h01 * (u_cloud_thick_m * 0.00078) - u_time * 0.0022);
    vec4 w = textureLod(u_cloud_detail, w_coord, min(w_lod, 1.8));
    float worley_lo = w.a * 0.62 + w.b * 0.38;
    return clamp(remap01(base_shape, (1.0 - worley_lo) * 0.54, 1.0), 0.0, 1.0);
}

// Modern Schneider Perlin-Worley 3D cumulus sampler with crisp cauliflower turrets.
float sample_cloud_modern(vec2 p_en, float h01, float altitude_msl, vec2 wind_base, float eff_cov, float dist_m,
                          out float out_ero, out float out_billow, out vec3 out_wcoord, out float out_wlod, out float out_base) {
    out_ero = 0.5;
    out_billow = 0.5;
    out_wcoord = vec3(0.0);
    out_wlod = 0.0;
    out_base = 0.0;
    if (h01 <= 0.0 || h01 >= 1.0) return 0.0;
    vec4 mf = sample_regional_atmo(p_en, altitude_msl);
    vec2 p; vec3 macro_a, macro_b; float h_eff;
    float base_shape = sample_cloud_macro(p_en, h01, wind_base, eff_cov, mf, p, macro_a, macro_b, h_eff);
    out_base = base_shape;
    if (base_shape <= 0.015) return 0.0;

    // Crisp multi-scale 3D Worley cauliflower lobes (1.4km primary turrets + 540m crisp bubbling ridges)
    vec2 curl = vec2(macro_b.g - 0.5, macro_a.g - 0.5) * (160.0 * (0.20 + 0.80 * h_eff));
    vec3 w_coord = vec3((p + curl) * 0.00070, (altitude_msl - u_cloud_base_m) * 0.00078 - u_time * 0.0022);
    float w_lod = clamp(log2(max(1.0, dist_m * 0.00014)), 0.0, 1.05);
    vec4 w = textureLod(u_cloud_detail, w_coord, w_lod);
    vec3 w2_coord = vec3(mat2(0.80, 0.60, -0.60, 0.80) * p * 0.00185 + vec2(0.19, 0.73), w_coord.z * 2.1);
    vec4 w2 = textureLod(u_cloud_detail, w2_coord, clamp(w_lod + 0.20, 0.0, 1.35));

    // Solid dense cumulus core + sharp cauliflower edge carving (zero milky fog, zero hollow cores!)
    float worley_lo = w.a * 0.52 + w.b * 0.30 + w2.a * 0.18;
    float worley_hi = w.b * 0.28 + w.g * 0.42 + w2.b * 0.22 + w.r * 0.08;
    float base_d = remap01(base_shape, (1.0 - worley_lo) * 0.58, 1.0);
    float edge = 1.0 - base_d;
    float fine_carve = (1.0 - worley_hi) * mix(0.28, 0.52, h_eff) * (edge * edge + 0.18);
    float density = smoothstep(0.08, 0.26, max(0.0, base_d - fine_carve) * 1.60);

    out_billow = clamp(worley_lo * 0.52 + worley_hi * 0.48, 0.0, 1.0);
    out_ero = worley_hi;
    out_wcoord = w_coord;
    out_wlod = w_lod;
    return density;
}

void main() {
    // Reconstruct world-space view ray for this quarter-res pixel
    vec2 ndc = v_uv * 2.0 - 1.0;
    vec4 wp = u_inv_vp * vec4(ndc, 0.0001, 1.0);
    vec3 view_dir = normalize(wp.xyz / max(1e-5, wp.w));
    float vy = view_dir.y;

    // Exact center Reverse-Z terrain depth (no 5-tap max dilation, which previously
    // carved a bright halo outside mountain ridges!) + soft terrain-wrap fade below.
    float scene_d = textureLod(u_terrain_depth, v_uv, 0.0).r;
    float max_ray_t = 78000.0;
    if (scene_d > 0.000001) {
        vec4 gp = u_inv_vp * vec4(ndc, scene_d, 1.0);
        max_ray_t = min(max_ray_t, length(gp.xyz / max(1e-6, gp.w)));
    }

    float slab_top = u_cloud_base_m + u_cloud_thick_m;
    float t0 = 0.0, t1 = 0.0;
    bool valid = true;
    bool in_slab = false;
    if (u_cam_alt_m < u_cloud_base_m) {
        if (vy < -0.004) valid = false;
        else {
            float vy_c = max(0.0, vy);
            t0 = intersect_shell_above(u_cloud_base_m - u_cam_alt_m, vy_c);
            t1 = intersect_shell_above(slab_top - u_cam_alt_m, vy_c);
        }
    } else if (u_cam_alt_m > slab_top) {
        if (vy >= -0.006) valid = false;
        else {
            float d_top = u_cam_alt_m - slab_top;
            float d_bot = u_cam_alt_m - u_cloud_base_m;
            float disc0 = vy * vy - (2.0 * d_top) / R_EARTH;
            if (disc0 < 0.0) valid = false;
            else {
                t0 = (2.0 * d_top) / max(0.0001, -vy + sqrt(disc0));
                float disc1 = vy * vy - (2.0 * d_bot) / R_EARTH;
                t1 = (disc1 > 0.0) ? ((2.0 * d_bot) / max(0.0001, -vy + sqrt(disc1))) : (t0 + 18000.0);
            }
        }
    } else {
        // Aircraft is flying INSIDE the volumetric cloud layer: start 4m ahead & intersect exact top/bottom shell!
        in_slab = true;
        t0 = 2.0;
        if (vy >= 0.0) {
            t1 = intersect_shell_above(slab_top - u_cam_alt_m, vy);
        } else {
            float d_bot = max(1.0, u_cam_alt_m - u_cloud_base_m);
            float disc1 = vy * vy - (2.0 * d_bot) / R_EARTH;
            t1 = (disc1 > 0.0) ? ((2.0 * d_bot) / max(0.0001, -vy + sqrt(disc1))) : 16000.0;
        }
    }

    t0 = max(2.0, t0);
    t1 = min(t1, max_ray_t);

    vec4 cur_col = vec4(0.0);
    float avg_d = 46000.0;

    vec3 sdir = normalize(u_sun_dir);
    float cos_th = dot(view_dir, sdir);
    vec2 ray_en = vec2(view_dir.x, -view_dir.z);
    float trans = 1.0;
    vec3 accum = vec3(0.0);
    float w_dist = 0.0, w_sum = 0.0;
    float rain_end = min(max_ray_t, 12000.0);
    if (u_cam_alt_m < u_cloud_base_m && vy > 0.002) {
        rain_end = min(rain_end, intersect_shell_above(u_cloud_base_m - u_cam_alt_m, vy));
    }
    // Volumetric rain shafts, snow curtains & low valley radiation fog in front of mountains!
    bool do_rain = (u_precip > 0.045 || u_wx_fx.w > 0.35) && u_cam_alt_m < (u_cloud_base_m + 350.0) && rain_end > 12.0;
    bool do_vfog = (u_wx_fx.x > 0.04) && rain_end > 18.0;
    if (do_rain || do_vfog) {
        float jitter = 0.5;
        float sun_scatter = pow(max(0.0, cos_th), 4.0) * clamp((sdir.y + 0.05) / 0.25, 0.0, 1.0);
        vec3 rain_col = mix(u_cloud_shadow * 0.28, u_haze_cool * 0.56, 0.34) + u_cloud_lit * (sun_scatter * 0.35);
        vec3 snow_col = mix(u_haze_cool * 0.86, u_cloud_lit * 1.05, 0.62);
        vec3 precip_light = mix(rain_col, snow_col, u_wx_fx.y) + vec3(0.45, 0.58, 0.82) * (u_wx_fx.z * 0.55);
        vec3 fog_light = mix(u_haze_cool * 0.82, u_cloud_lit * 0.95, 0.52) + u_haze_warm * (sun_scatter * 0.24);
        for (int i = 0; i < 5; ++i) {
            if (trans < 0.04) break;
            float a = float(i) * 0.20, b = float(i + 1) * 0.20;
            float t = mix(a * a, b * b, jitter) * rain_end;
            float alt = u_cam_alt_m + vy * t + (1.0 - vy * vy) * t * t / (2.0 * R_EARTH);
            float seg_len = rain_end * (b * b - a * a);
            float sig_r = 0.0;
            if (do_rain) {
                float depth = max(0.0, u_cloud_base_m - alt);
                sig_r = rain_extinction(u_cam_en + ray_en * t, alt, depth);
            }
            float sig_f = do_vfog ? valley_fog_extinction(u_cam_en + ray_en * t, alt) : 0.0;
            float sig_tot = sig_r + sig_f;
            if (sig_tot > 1e-6) {
                float alpha = 1.0 - exp(-sig_tot * seg_len);
                // Keep foreground rain/snow curtains semi-translucent so sculpted 3D storm clouds remain visible behind them!
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

    // Interleave expensive cloud rays with a 2x2 Bayer phase. One quarter of
    // pixels gets a fresh volume march each frame; the first frame remains full
    // rate so every pixel has valid history before sparse reprojection begins.
    ivec2 bayer_xy = ivec2(gl_FragCoord.xy) & ivec2(1);
    int bayer_rank = (bayer_xy.y == 0)
        ? ((bayer_xy.x == 0) ? 0 : 2)
        : ((bayer_xy.x == 0) ? 3 : 1);
    bool march_cloud = u_sparse_period <= 1 ||
        (u_sparse_period == 2 && ((bayer_rank & 1) == (u_frame_idx & 1))) ||
        (u_sparse_period >= 4 && bayer_rank == (u_frame_idx & 3));
    if (!march_cloud) {
        // Reproject previous converged cloud+precip frame directly (avoid double-blending
        // foreground rain onto history_cloud, which caused 2x2 Bayer checkerboard dots!)
        if (u_temporal_blend > 0.01) {
            float ref_t = (valid && t1 > t0 && t0 < 74000.0) ? mix(t0, t1, in_slab ? 0.48 : 0.36) : min(max_ray_t * 0.5, 4500.0);
            vec4 prev_clip = u_prev_vp * vec4(view_dir * ref_t + u_camera_delta, 1.0);
            vec2 prev_uv = (prev_clip.xy / max(1e-5, prev_clip.w)) * 0.5 + 0.5;
            if (prev_clip.w > 0.0 && prev_uv.x > 0.005 && prev_uv.x < 0.995 &&
                prev_uv.y > 0.005 && prev_uv.y < 0.995) {
                out_cloud = textureLod(u_prev_cloud, prev_uv, 0.0);
                return;
            }
        }
        out_cloud = vec4(accum, clamp(1.0 - trans, 0.0, 0.995));
        return;
    }

    if (valid && t1 > t0 && t0 < 74000.0) {
        vec2 wind_base = vec2(u_time * 9.5, u_time * 4.2);
        float eff_cov = clamp(u_coverage, 0.06, 0.96);
        float max_m = in_slab ? 9500.0 : mix(8200.0, 16000.0, smoothstep(0.015, 0.26, vy));
        float march_len = min(t1 - t0, max_m);

        {
            float sun_vis = clamp((sdir.y + 0.08) / 0.24, 0.0, 1.0);
            float hg_fwd = mix(hg_phase(cos_th, -0.22), hg_phase(cos_th, 0.74), 0.72);
            float hg_ms1 = mix(0.25, hg_phase(cos_th, 0.32), 0.55);
            float storm_tint = smoothstep(0.12, 0.78, u_precip);
            float overcast_tint = smoothstep(0.56, 0.92, u_coverage);
            vec2 s_en = vec2(sdir.x, -sdir.z);
            float s_y = max(0.06, sdir.y);
            float inv_thick = 1.0 / max(150.0, u_cloud_thick_m);

            bool near_cloud = t0 < 680.0;
            int max_steps = near_cloud ? 20 : clamp(u_steps + 6, 16, 20);
            // Adaptive raymarch with static per-pixel IGN start phase (eliminates horizontal slicing
            // bands on distant cloud bases with ZERO temporal flicker!)
            float t_end = t0 + march_len;
            float base_dt = march_len / float(max(12, u_steps + 2));
            float t = t0 + base_dt * 0.45;
            bool in_detail = false;
            int zero_streak = 0;
            for (int i = 0; i < max_steps; i++) {
                if (trans < 0.025 || t >= t_end) break;
                float dt_cur = in_detail ? (base_dt * 0.52) : base_dt;
                if (near_cloud) dt_cur = max(35.0, (t - t0) * 0.24 + 42.0);
                vec3 rp = view_dir * t;
                float curv_rise = dot(rp.xz, rp.xz) / (2.0 * R_EARTH);
                float sample_alt_msl = u_cam_alt_m + rp.y + curv_rise;
                float h01 = (sample_alt_msl - u_cloud_base_m) * inv_thick;
                vec2 p_en = u_cam_en + ray_en * t;
                float ero = 0.5, billow = 0.5, w_lod = 0.0, base_g = 0.0;
                vec3 w_coord = vec3(0.0);
                float d = sample_cloud_modern(p_en, h01, sample_alt_msl, wind_base, eff_cov, t, ero, billow, w_coord, w_lod, base_g);
                if (base_g > 0.015) in_detail = true;
                if (d > 0.012) {
                    zero_streak = 0;
                    // Fast 3-step logarithmic sun cone raymarch:
                    // Step 0 uses local 3D Worley gradient (1 tap) for crisp cauliflower ridge self-shadowing!
                    vec4 sw0 = textureLod(u_cloud_detail, w_coord + vec3(s_en * 0.085, s_y * 0.11), w_lod);
                    float billow_s0 = sw0.a * 0.52 + sw0.b * 0.30 + sw0.g * 0.18;
                    float d_s0 = clamp(billow_s0 * base_g * 1.35, 0.0, 1.0);
                    float d_s1 = sample_cloud_shadow(p_en + s_en * 440.0,  h01 + s_y * (440.0  * inv_thick), wind_base, eff_cov, w_lod + 0.50);
                    float d_s2 = sample_cloud_shadow(p_en + s_en * 1250.0, h01 + s_y * (1250.0 * inv_thick), wind_base, eff_cov, w_lod + 0.90);
                    float tau_sun = (d_s0 * 0.45 + d_s1 * 1.05 + d_s2 * 1.90) * (1.0 + storm_tint * 0.72);

                    // Calibrated Hillaire/Wrenninge 2-octave multiple scattering: bright snow-white
                    // sunlit cauliflower peaks + luminous creamy multi-scatter + cool slate-blue bases!
                    float powder = mix(0.50, 1.0, 1.0 - exp(-d * 4.8 * (0.38 + 0.62 * (1.0 - h01))));
                    float puff_relief = clamp((billow - billow_s0) * 2.4 + 0.78, 0.34, 1.48);
                    float direct_0 = exp(-tau_sun * 1.15) * hg_fwd * powder * puff_relief * 2.55;
                    float multi_1  = 0.46 * exp(-tau_sun * 0.28) * hg_ms1 * 1.65;
                    float multi_2  = 0.20 * exp(-tau_sun * 0.08) * 0.78;
                    float sun_energy = (direct_0 + multi_1 + multi_2) * sun_vis * mix(1.0, 0.46, storm_tint);

                    // Vertical sky-to-ground ambient bounce: cool slate undersides -> bright sky-lit domes
                    float h_amb = smoothstep(0.0, 0.80, h01);
                    float amb_occ = mix(0.42, 1.0, 0.50 * h_amb + 0.50 * ero);
                    vec3 gnd_amb = u_cloud_shadow * mix(0.36, 0.14, storm_tint) + u_haze_warm * (0.040 * sun_vis * (1.0 - storm_tint));
                    vec3 sky_amb = mix(u_haze_cool, vec3(0.92, 0.95, 1.0), 0.50);
                    vec3 ambient = mix(gnd_amb, sky_amb, h_amb) * amb_occ * (1.0 - storm_tint * 0.36);

                    vec3 step_c = ambient + u_cloud_lit * sun_energy;
                    float silver = pow(1.0 - ero, 2.0) * pow(max(0.0, cos_th), 5.0) * exp(-tau_sun * 0.60) * sun_vis;
                    step_c += u_cloud_lit * vec3(1.16, 1.04, 0.86) * (silver * 0.78);

                    vec3 weather_tint = mix(vec3(1.0), vec3(0.60, 0.68, 0.78),
                                            clamp(storm_tint * 0.76 + overcast_tint * 0.18, 0.0, 0.80));
                    step_c *= weather_tint;
                    if (in_slab) {
                        float rim = pow(max(0.0, cos_th), 7.5) * (1.0 - d * 0.55) * (0.32 + 0.68 * h01);
                        step_c += u_cloud_lit * vec3(1.22, 0.96, 0.72) * (rim * 0.88);
                    }
                    if (u_wx_fx.z > 0.02) {
                        float flash = u_wx_fx.z * smoothstep(0.16, 0.65, d) * (1.0 - abs(h01 - 0.45));
                        step_c += vec3(0.74, 0.86, 1.18) * (flash * 2.0);
                    }

                    // Solid volumetric cumulus extinction + soft 180m mountain-wrap fade
                    float terrain_wrap = smoothstep(0.0, 180.0, max_ray_t - t);
                    float extinction = mix(0.0115, 0.0052, smoothstep(100.0, 3800.0, t)) * terrain_wrap;
                    float seg_tau = d * dt_cur * extinction * (1.0 + u_precip * 0.35);
                    float seg_trans = exp(-seg_tau);
                    float w = trans * (1.0 - seg_trans);
                    accum += step_c * w;
                    w_dist += t * w;
                    w_sum += w;
                    trans *= seg_trans;
                } else if (in_detail) {
                    zero_streak++;
                    if (zero_streak >= 3) in_detail = false;
                }
                t += dt_cur;
            }
        }

        // High-altitude dynamically sheared Cirrus wisps at 7,600m MSL
        if (trans > 0.10 && vy > 0.045 && u_cam_alt_m < 7500.0 && max_ray_t > 20000.0) {
            float tc = intersect_shell_above(7600.0 - u_cam_alt_m, vy);
            if (tc < min(90000.0, max_ray_t)) {
                vec2 c_en = u_cam_en + ray_en * tc + vec2(u_time * 42.0, u_time * 15.0);
                vec2 c_uv = vec2(c_en.x * 0.000018, c_en.y * 0.000048);
                vec2 c_warp = vec2(sin(c_uv.y * 6.0 + u_time * 0.02) * 0.04, 0.0);
                float cw = textureLod(u_cloud_noise, c_uv + c_warp, 0.0).g *
                           textureLod(u_cloud_noise, c_uv * 2.1 + vec2(0.37, 0.11) - c_warp, 0.0).r;
                float cirrus = smoothstep(0.42, 0.78, cw) * 0.28 * smoothstep(0.045, 0.18, vy) * (1.0 - smoothstep(45000.0, 90000.0, tc));
                if (cirrus > 0.005) {
                    vec3 cc = mix(u_cloud_lit, u_haze_warm * 1.16, 0.35 + 0.35 * pow(max(0.0, cos_th), 4.0));
                    accum += cc * (cirrus * trans);
                    w_dist += tc * (cirrus * trans);
                    w_sum += cirrus * trans;
                    trans *= (1.0 - cirrus);
                }
            }
        }

    }
    float final_a = clamp(1.0 - trans, 0.0, 0.995);
    if (final_a > 0.001) {
        avg_d = w_dist / max(0.001, w_sum);
        vec3 fc = accum / max(0.001, w_sum);
        float sun_vis = clamp((sdir.y + 0.08) / 0.24, 0.0, 1.0);
        vec3 hcol = mix(u_haze_cool, u_haze_warm * 1.15, pow(max(0.0, cos_th), 3.0) * 0.65 * sun_vis);
        fc = mix(fc, hcol, smoothstep(28000.0, 74000.0, avg_d) * 0.74);
        final_a *= 1.0 - smoothstep(58000.0, 76000.0, avg_d);
        cur_col = vec4(fc * final_a, final_a);
    }

    // Temporal Reprojection: smoothly accumulate distant clouds, taper blend at close range when flying INSIDE clouds!
    vec4 prev_clip = u_prev_vp * vec4(view_dir * avg_d + u_camera_delta, 1.0);
    vec2 prev_uv = (prev_clip.xy / max(1e-5, prev_clip.w)) * 0.5 + 0.5;
    // Always accumulate a small amount to suppress sample noise. Keep
    // fast-moving detail close to the aircraft responsive.
    float near_tb = mix(0.40, 1.0, smoothstep(80.0, 1800.0, avg_d));
    if (u_temporal_blend * near_tb > 0.01 && prev_clip.w > 0.0 && prev_uv.x > 0.005 && prev_uv.x < 0.995 && prev_uv.y > 0.005 && prev_uv.y < 0.995) {
        vec4 hist = textureLod(u_prev_cloud, prev_uv, 0.0);
        float diff = abs(hist.a - cur_col.a);
        float terrain_gate = 1.0 - smoothstep(0.28, 0.72, diff);
        out_cloud = mix(cur_col, hist, u_temporal_blend * near_tb * terrain_gate);
    } else {
        out_cloud = cur_col;
    }
}
