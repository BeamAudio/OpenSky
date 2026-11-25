#version 460 core

layout(binding = 0) uniform sampler2D u_atlas_albedo;
layout(binding = 1) uniform sampler2D u_atlas_normal;
layout(binding = 2) uniform sampler2D u_atlas_orm;
layout(binding = 3) uniform sampler2D u_interior_rooms;
layout(binding = 4) uniform sampler2D u_mtn_gi;   // 512x384 RG8: R=50km mountain 10-TOD shadow, G=valley Sky AO
layout(binding = 5) uniform sampler2D u_refl_env; // Pre-baked 256x128 RGBA16F equirectangular reflection map + mip chain!

uniform vec3 u_sun_dir;
uniform vec3 u_haze_cool;
uniform vec3 u_haze_warm;
uniform float u_haze_density;
uniform float u_cam_alt_m;
uniform vec4 u_mtn_bounds; // (min_e, min_n, inv_span_e, inv_span_n)

in vec3 v_world_pos;
in vec3 v_world_n;
in vec2 v_world_en;
in vec2 v_uv;
in float v_sun_vis;
in float v_sky_ao;
in float v_night_light;
in vec4 v_meta;
flat in uint v_flags;
in float v_elev_m;
in float v_dist;

layout(location = 0) out vec4 frag_color;

float hash21(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

float vnoise2(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(
        mix(hash21(i), hash21(i + vec2(1.0, 0.0)), u.x),
        mix(hash21(i + vec2(0.0, 1.0)), hash21(i + vec2(1.0, 1.0)), u.x),
        u.y
    );
}

vec3 perturb_normal(vec3 N, vec3 pos, vec2 cont_uv, vec3 map_n, float strength) {
    vec3 dp1 = dFdx(pos);
    vec3 dp2 = dFdy(pos);
    vec2 duv1 = dFdx(cont_uv);
    vec2 duv2 = dFdy(cont_uv);
    vec3 dp2perp = cross(dp2, N);
    vec3 dp1perp = cross(N, dp1);
    vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;
    float invmax = inversesqrt(max(dot(T, T), dot(B, B)) + 1e-8);
    mat3 TBN = mat3(T * invmax, B * invmax, N);
    vec3 tn = normalize(vec3(map_n.xy * strength, max(0.15, map_n.z)));
    return normalize(TBN * tn);
}

void sample_atlas_tile(int col, int row, vec2 cont_uv, out vec3 alb, out vec3 map_n, out vec3 orm) {
    vec2 f = clamp(fract(cont_uv), vec2(0.008), vec2(0.992));
    vec2 tuv = (vec2(float(col), float(row)) + f) * 0.25;
    vec2 gx = dFdx(cont_uv) * 0.25;
    vec2 gy = dFdy(cont_uv) * 0.25;
    alb   = textureGrad(u_atlas_albedo, tuv, gx, gy).rgb;
    map_n = textureGrad(u_atlas_normal, tuv, gx, gy).rgb * 2.0 - 1.0;
    orm   = textureGrad(u_atlas_orm,    tuv, gx, gy).rgb;
}

vec3 building_patina(float h, int style) {
    float t = fract(h * 13.37);
    if (style == 0 || style == 6) {
        int k = int(floor(t * 4.0));
        if (k == 0) return vec3(1.08, 0.96, 0.90);
        if (k == 1) return vec3(0.96, 0.86, 0.80);
        if (k == 2) return vec3(1.14, 1.04, 0.84);
        return vec3(1.00, 0.92, 0.88);
    }
    if (style == 1 || style == 4 || style == 5) {
        int k = int(floor(t * 6.0));
        if (k == 0) return vec3(1.07, 1.00, 0.90);
        if (k == 1) return vec3(1.08, 0.90, 0.72);
        if (k == 2) return vec3(1.04, 0.82, 0.70);
        if (k == 3) return vec3(0.96, 0.95, 0.92);
        if (k == 4) return vec3(0.98, 0.88, 0.76);
        return vec3(0.92, 0.86, 0.79);
    }
    return mix(vec3(0.92, 0.93, 0.94), vec3(1.08, 1.04, 0.97), t);
}

void main() {
    // Near-field city pass only draws chunks <= near_cut (~720-2500 m); keeping zero discard preserves Hardware Early-Z.
    bool is_roof    = ((v_flags & 1u) != 0u);
    bool flag_par   = ((v_flags & 2u) != 0u);
    int style       = int((v_flags >> 2u) & 7u);
    int roof_kind   = int((v_flags >> 5u) & 3u);

    vec3 geom_n   = normalize(v_world_n);
    vec3 sdir     = normalize(u_sun_dir);
    vec3 view_dir = normalize(v_world_pos);
    float day_t   = clamp((sdir.y + 0.035) / 0.14, 0.0, 1.0);
    float night_t = clamp((0.06 - sdir.y) / 0.12, 0.0, 1.0);

    vec3 col = vec3(0.45);
    vec3 map_n = vec3(0.0, 0.0, 1.0);
    vec3 orm = vec3(1.0, 0.84, 0.0);
    vec2 cont_uv = v_uv;
    float rough = 0.84;
    float metal = 0.0;
    vec3 emiss = vec3(0.0);
    bool is_parapet = flag_par;
    float y_local = is_roof ? 10.0 : v_uv.y;

    // Subtle close-up masonry / plaster / terracotta micro-grain (< 340m) keeps buildings razor-sharp at 30m AGL!
    float close_g = 1.0 - smoothstep(90.0, 340.0, v_dist);
    float micro_g = (close_g > 0.01) ? (vnoise2(v_world_en * 2.2 + vec2(v_elev_m * 1.9)) * 0.65 + vnoise2(v_world_en * 7.5 + vec2(v_elev_m * 6.5)) * 0.35) : 0.5;

    if (is_roof) {
        // COHERENT PER-BUILDING ROOF MATERIAL (ZERO camo patches!) + Wall-Aligned Crisp 2K Tile/Terrace/Slate PBR!
        vec3 pnoa_lin = pow(max(v_meta.rgb / 255.0, vec3(0.05)), vec3(2.2));
        float bhash = v_meta.a / 255.0;
        bool is_pyr_slate = (roof_kind == 3 && v_elev_m > 780.0);
        int sample_rk = is_pyr_slate ? 1 : roof_kind; // Pyrenean slate uses real shingle pattern (col 1), industrial uses metal (col 3)!
        float tile_scale = (roof_kind == 2) ? 12.5 : (is_pyr_slate ? 3.8 : ((roof_kind == 3) ? 6.2 : 4.2));
        cont_uv = v_uv / tile_scale + vec2(bhash * 0.73, fract(bhash * 5.3));

        vec3 tile_alb;
        sample_atlas_tile(sample_rk, 0, cont_uv, tile_alb, map_n, orm);
        if (is_pyr_slate) {
            // Authentic dark blue-charcoal Pyrenean slate stone shingles (Castejon de Sos, Benasque, Jaca!)
            float sl_luma = clamp(dot(tile_alb, vec3(0.299, 0.587, 0.114)) / 0.24, 0.55, 1.45);
            tile_alb = vec3(0.11, 0.125, 0.15) * sl_luma;
        }
        float tile_luma = max(0.08, dot(tile_alb, vec3(0.299, 0.587, 0.114)));
        vec3 pnoa_harm = clamp(pnoa_lin * clamp(tile_luma * 2.15, 0.58, 1.55), vec3(0.04), vec3(0.82));
        float harm_w = is_pyr_slate ? 0.22 : ((roof_kind == 3) ? 0.42 : ((roof_kind == 2) ? 0.46 : 0.38));
        col = mix(tile_alb, pnoa_harm, harm_w);
        col *= mix(vec3(0.90, 0.88, 0.85), vec3(1.12, 1.05, 0.96), fract(bhash * 7.13));
        col *= mix(1.0, 0.86 + 0.28 * micro_g, close_g * 0.65);
        rough = is_pyr_slate ? 0.68 : clamp(orm.g, 0.26, 0.94);
        metal = (roof_kind == 3 && !is_pyr_slate) ? clamp(orm.b * 0.65, 0.0, 0.55) : 0.0;
    } else {
        float bhash  = v_meta.x / 255.0;
        float eave_h = max(3.2, (v_meta.y / 255.0) * 64.0);
        int arch     = int(v_meta.z + 0.5);
        float y_m    = v_uv.y;
        float u_tile = v_uv.x;

        if (flag_par || y_m > eave_h) {
            is_parapet = true;
            vec3 trim_c = (style == 0 || style == 6) ? vec3(0.45, 0.26, 0.19)
                        : ((style == 2 || style == 7) ? vec3(0.58, 0.57, 0.54)
                        : ((style == 4) ? vec3(0.60, 0.44, 0.36) : vec3(0.56, 0.51, 0.42)));
            col = trim_c * (0.90 + 0.20 * micro_g);
            orm = vec3(0.92, 0.86, 0.0);
            rough = 0.86;
        } else {
            float gf_h = (eave_h > 5.8 && arch != 5) ? 3.4 : 0.0;
            vec3 alb;
            if (gf_h > 0.5 && y_m < gf_h) {
                int gf_col = (arch == 4) ? 3 : ((style == 2 || style == 6) ? 2 : ((style == 3 || style == 5) ? 1 : 0));
                cont_uv = vec2(u_tile, clamp(1.0 - (y_m / gf_h), 0.01, 0.99));
                sample_atlas_tile(gf_col, 1, cont_uv, alb, map_n, orm);
            } else {
                float up_span = max(2.8, eave_h - gf_h);
                float n_stories = max(1.0, floor(up_span / 3.1 + 0.5));
                float y_frac = clamp((y_m - gf_h) / up_span, 0.0, 1.0);
                cont_uv = vec2(u_tile, (1.0 - y_frac) * (n_stories * 0.5));
                sample_atlas_tile(style & 3, (style < 4) ? 2 : 3, cont_uv, alb, map_n, orm);
            }
            col = alb;
            rough = clamp(orm.g, 0.08, 0.96);
            float wall_bright = smoothstep(0.12, 0.30, dot(alb, vec3(0.3, 0.59, 0.11)));
            col = mix(col, col * building_patina(bhash, style), wall_bright * 0.80);
            // Crisp close-up stucco/brick micro-grain + subtle eaves shadow right beneath roofline!
            col *= mix(1.0, 0.88 + 0.24 * micro_g, close_g * wall_bright * 0.62);
            float eave_sh = 1.0 - smoothstep(eave_h - 0.55, eave_h, y_m) * 0.26;
            col *= eave_sh;

            if (orm.b > 0.45 && v_dist < 4000.0) {
                vec2 win_cell = floor(cont_uv * vec2(4.0, 2.0));
                vec2 win_uv = fract(cont_uv * vec2(4.0, 2.0));
                float r_id = hash21(win_cell + vec2(bhash * 71.0, float(style) * 13.0));
                vec2 par = clamp(vec2(dot(view_dir, cross(geom_n, vec3(0.0, 1.0, 0.0))), view_dir.y) * 0.28, vec2(-0.22), vec2(0.22));
                float rcol = floor(fract(r_id * 4.0) * 4.0);
                float rrow = floor(fract(r_id * 16.0) * 4.0);
                vec2 room_uv = (vec2(rcol, rrow) + clamp(win_uv + par, vec2(0.05), vec2(0.95))) * 0.25;
                vec3 room = texture(u_interior_rooms, room_uv).rgb;
                col = room * 0.46;
                rough = 0.07;
                metal = 0.06;
                map_n = vec3(0.0, 0.0, 1.0);
                emiss += room * vec3(1.65, 1.08, 0.52) * (step(0.42, r_id) * night_t * 1.55);
            } else if (orm.b > 0.45) {
                col *= 0.44;
                rough = 0.12;
                emiss += vec3(1.30, 0.82, 0.38) * (step(0.45, fract(bhash * 17.3)) * night_t * 0.65);
            }
        }
    }

    vec3 nrm = (v_dist < 2200.0 && !is_parapet && (map_n.x != 0.0 || map_n.y != 0.0))
             ? perturb_normal(geom_n, v_world_pos, cont_uv, map_n, is_roof ? 0.96 : 1.02)
             : geom_n;
    // Ultra-crisp < 260m tactile micro-relief normal perturbation on stucco, brick, terracotta & Pyrenean slate!
    if (close_g > 0.02 && rough > 0.25) {
        vec2 mpos = v_world_en * 2.8 + vec2(v_elev_m * 2.3);
        float gx_m = vnoise2(mpos + vec2(0.18, 0.0)) - vnoise2(mpos - vec2(0.18, 0.0));
        float gy_m = vnoise2(mpos + vec2(0.0, 0.18)) - vnoise2(mpos - vec2(0.0, 0.18));
        nrm = normalize(nrm + vec3(gx_m, gy_m * (is_roof ? 0.2 : 0.45), gy_m) * (close_g * 0.22));
    }

    // PRE-BAKED 10-TIME-OF-DAY RAY-TRACED SHADOWS (Local Buildings x 50km Pyrenees Mountains!) + HEMISPHERE SKY AO + BOUNCE GI!
    vec2 mtn_uv = clamp((v_world_en - u_mtn_bounds.xy) * u_mtn_bounds.zw, vec2(0.002), vec2(0.998));
    vec2 mtn_gi = texture(u_mtn_gi, mtn_uv).rg;
    float sun_comb = clamp(v_sun_vis, 0.0, 1.0) * mtn_gi.r;
    float ao_comb  = clamp(v_sky_ao, 0.0, 1.0) * mix(0.65, 1.0, mtn_gi.g);
    float baked_sun_vis = mix(0.12, 1.0, sun_comb);
    float baked_sky_ao  = mix(0.36, 1.0, ao_comb);

    float base_contact = is_roof ? 1.0 : mix(0.62, 1.0, smoothstep(0.0, 1.8, max(0.0, y_local)));
    float ao_tot = baked_sky_ao * mix(0.60, 1.0, orm.r) * base_contact;

    float canyon_bounce = (1.0 - ao_comb) * (1.0 - abs(geom_n.y)) * day_t;
    vec3 bounce_gi = vec3(0.30, 0.23, 0.15) * canyon_bounce * 0.62;

    vec3 ldir = (sdir.y >= -0.025) ? sdir : normalize(vec3(-sdir.x, 0.55, -sdir.z));
    vec3 sun_c = (sdir.y >= -0.025)
        // Match the terrain's calibrated daylight response so cities and ortho blend seamlessly.
        ? mix(vec3(1.0, 0.62, 0.30), vec3(1.0, 0.95, 0.86), clamp(sdir.y / 0.45, 0.0, 1.0)) * mix(0.55, 2.42, day_t)
        : vec3(0.14, 0.20, 0.34) * 0.14;
    vec3 sky_amb = mix(vec3(0.012, 0.018, 0.042), mix(vec3(0.36, 0.42, 0.50), u_haze_cool * 0.86, 0.56), day_t);
    vec3 gnd_amb = mix(vec3(0.005, 0.006, 0.010), vec3(0.28, 0.24, 0.18), day_t);
    vec3 amb_c = mix(gnd_amb, sky_amb, nrm.y * 0.5 + 0.5) * 0.58;

    vec3 V = -view_dir;
    vec3 H = normalize(V + ldir);
    float ndl = max(0.0, dot(nrm, ldir));
    float ndv = max(0.001, dot(nrm, V));
    float ndh = max(0.0, dot(nrm, H));
    float a = max(0.04, rough * rough);
    float a2 = a * a;
    float dn = ndh * ndh * (a2 - 1.0) + 1.0;
    float D = a2 / (3.14159265 * dn * dn + 1e-5);
    float k = (rough + 1.0) * (rough + 1.0) * 0.125;
    float G = (ndv / (ndv * (1.0 - k) + k)) * (max(0.001, ndl) / (max(0.001, ndl) * (1.0 - k) + k));
    vec3 F0 = mix(vec3(0.04), col, metal);
    vec3 F = F0 + (1.0 - F0) * pow(1.0 - max(0.0, dot(H, V)), 5.0);
    vec3 spec = clamp((D * G * F) / (4.0 * ndv * max(0.001, ndl) + 1e-4), vec3(0.0), vec3(4.5));
    vec3 kD = (vec3(1.0) - F) * (1.0 - metal);

    vec3 refl = reflect(view_dir, nrm);
    vec2 ruv = vec2(atan(refl.z, refl.x) * (0.5 / 3.14159265) + 0.5, asin(clamp(refl.y, -1.0, 1.0)) * (1.0 / 3.14159265) + 0.5);
    vec3 ibl_env = textureLod(u_refl_env, ruv, rough * 5.6).rgb;
    vec3 F_ibl = F0 + (max(vec3(1.0 - rough), F0) - F0) * pow(1.0 - ndv, 4.0);

    float direct_vis = baked_sun_vis * ndl;
    vec3 lit = kD * col * (amb_c * ao_tot + bounce_gi + sun_c * direct_vis) +
               sun_c * (spec * direct_vis * 0.45) +
               ibl_env * F_ibl * (0.64 * ao_tot * mix(0.25, 1.0, day_t));
    // 3-Layer (0m Ground, 5m Mid-Canyon, 10m Upper-Canyon) Pre-Baked 2.5D DSM Ray-Traced Volumetric Night Light!
    if (!is_roof && night_t > 0.01 && v_night_light > 0.02) {
        float h_agl = max(0.0, y_local);
        // Reconstruct 0m, 5m, and 10m volumetric light bands + aerial scatter inside the street canyon
        float w_0m  = exp(-h_agl * 0.26);
        float w_5m  = exp(-pow((h_agl - 5.0) / 3.4, 2.0));
        float w_10m = exp(-pow((h_agl - 10.0) / 4.2, 2.0));
        float layer_comb = clamp(0.52 * w_0m + 0.32 * w_5m + 0.20 * w_10m, 0.18, 1.15);
        float nl = pow(clamp(v_night_light, 0.0, 1.0), 1.45) * layer_comb;
        vec3 nl_col = mix(vec3(1.0, 0.54, 0.15), vec3(1.0, 0.84, 0.48), clamp(nl * 1.1, 0.0, 1.0));
        // Direct facade illumination + 3-layer (0m/5m/10m) volumetric street-canyon air bloom!
        float vol_air = pow(clamp(v_night_light, 0.0, 1.0), 1.15) * (0.45 * w_0m + 0.38 * w_5m + 0.25 * w_10m);
        lit += kD * col * nl_col * (nl * night_t * 3.15) + nl_col * (F_ibl * nl * night_t * 0.40);
        emiss += nl_col * (vol_air * night_t * 0.28);
    }

    // EXACT Terrain-Matched 3-Layer Analytic Height Fog (Valley + Tropospheric + Orographic Mountain Peak Mist!)
    float cos_th = max(0.0, dot(view_dir, sdir));
    float sun_vis_f = clamp((sdir.y + 0.08) / 0.24, 0.0, 1.0);
    float dy = v_elev_m - u_cam_alt_m;
    float H_v = 290.0;
    float h_cam_v = max(0.0, u_cam_alt_m - 320.0);
    float d_ov_hv = dy / H_v;
    float int_v = exp(-h_cam_v / H_v) * ((abs(d_ov_hv) < 0.02) ? (1.0 - 0.5 * d_ov_hv) : ((1.0 - exp(-d_ov_hv)) / d_ov_hv));
    float H_a = 2800.0;
    float h_cam_a = max(0.0, u_cam_alt_m);
    float d_ov_ha = dy / H_a;
    float int_a = exp(-h_cam_a / H_a) * ((abs(d_ov_ha) < 0.02) ? (1.0 - 0.5 * d_ov_ha) : ((1.0 - exp(-d_ov_ha)) / d_ov_ha));
    float rho = max(u_haze_density, 0.000018);
    float tau_v = v_dist * (rho * 1.45) * clamp(int_v, 0.0, 1.85);
    float tau_a = v_dist * (rho * 0.92) * clamp(int_a, 0.0, 1.50);
    float peak_band = smoothstep(1150.0, 2150.0, v_elev_m) * smoothstep(850.0, 4800.0, v_dist);
    float tau_p = v_dist * (rho * 1.45) * peak_band;
    float mist = clamp(1.0 - exp(-(tau_v + tau_a + tau_p)), 0.0, 0.90);
    float valley_frac = clamp((tau_v + tau_p * 0.72) / max(0.0001, tau_v + tau_a + tau_p), 0.0, 1.0);
    float rayleigh = 0.75 * (1.0 + cos_th * cos_th);
    float hg_w = (1.0 - 0.3844) / (4.0 * 3.14159 * pow(max(0.04, 1.3844 - 1.24 * cos_th), 1.5));
    float mie = clamp(hg_w * (0.52 + 0.55 * peak_band) * sun_vis_f + pow(1.0 - clamp(abs(view_dir.y) * 2.4, 0.0, 1.0), 2.2) * 0.32 * sun_vis_f, 0.0, 1.55);
    vec3 ray_col = mix(u_haze_cool * (0.62 + 0.22 * rayleigh), mix(u_haze_cool, vec3(0.76, 0.84, 0.92), 0.40), valley_frac * 0.65);
    vec3 insc = mix(ray_col, u_haze_warm * (1.32 + 0.22 * peak_band), clamp(mie * 0.80, 0.0, 0.92));

    frag_color = vec4(mix(lit, insc, mist * 0.90) + emiss * (1.0 - mist * 0.6), 1.0);
}
