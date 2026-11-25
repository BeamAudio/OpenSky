#version 460 core

layout(binding = 0) uniform sampler2D u_atlas_albedo;
layout(binding = 1) uniform sampler2D u_atlas_normal;
layout(binding = 2) uniform sampler2D u_atlas_orm;
layout(binding = 3) uniform sampler2D u_interior_rooms;

uniform vec3 u_sun_dir;
uniform vec3 u_haze_cool;
uniform vec3 u_haze_warm;
uniform float u_haze_density;
uniform float u_cam_alt_m;
uniform float u_fade_start;
uniform float u_fade_end;

in vec3 v_world_pos;
in vec3 v_world_n;
in vec3 v_local_n;
in vec3 v_local_pos;
in vec3 v_bldg_size;
in vec2 v_world_en;
in vec2 v_uv;
in vec4 v_roof_ao;
in float v_arch;
in float v_bhash;
in float v_elev_m;
in float v_dist;

layout(location = 0) out vec4 frag_color;

float hash21(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

// Screen-space cotangent normal mapping using continuous metric UV for artifact-free relief!
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

// Sample a single tile (col 0..3, row 0..3) from our 4x4 2048x2048 atlas with seamless mipmap gradients & zero bleed!
void sample_atlas_tile(int col, int row, vec2 cont_uv, out vec3 alb, out vec3 map_n, out vec3 orm) {
    vec2 f = clamp(fract(cont_uv), vec2(0.008), vec2(0.992));
    vec2 tuv = (vec2(float(col), float(row)) + f) * 0.25;
    vec2 gx = dFdx(cont_uv) * 0.25;
    vec2 gy = dFdy(cont_uv) * 0.25;
    alb   = textureGrad(u_atlas_albedo, tuv, gx, gy).rgb;
    map_n = textureGrad(u_atlas_normal, tuv, gx, gy).rgb * 2.0 - 1.0;
    orm   = textureGrad(u_atlas_orm,    tuv, gx, gy).rgb;
}

// Subtle realistic architectural stucco/masonry tint per building so neighboring Spanish blocks never look cloned!
vec3 building_patina(float h, int style) {
    float t = fract(h * 13.37);
    if (style == 0 || style == 6) {
        // Aragon Ladrillo Caravista brick variation: terracotta-red, burnt sienna, warm rose-brick, ochre-brick
        int k = int(floor(t * 4.0));
        if (k == 0) return vec3(1.06, 0.94, 0.88);
        if (k == 1) return vec3(0.94, 0.84, 0.78);
        if (k == 2) return vec3(1.12, 1.02, 0.82);
        return vec3(0.98, 0.90, 0.86);
    }
    if (style == 1 || style == 4 || style == 5) {
        // Spanish ochre, cream, warm terracotta-salmon, apricot, sandstone, travertine
        int k = int(floor(t * 6.0));
        if (k == 0) return vec3(1.05, 0.98, 0.88);
        if (k == 1) return vec3(1.06, 0.88, 0.70);
        if (k == 2) return vec3(1.02, 0.79, 0.68);
        if (k == 3) return vec3(0.94, 0.93, 0.90);
        if (k == 4) return vec3(0.96, 0.86, 0.74);
        return vec3(0.89, 0.83, 0.76);
    }
    // Modern limestone/concrete/industrial subtle warm-cool drift
    return mix(vec3(0.90, 0.91, 0.92), vec3(1.06, 1.02, 0.95), t);
}

void main() {
    // Smooth dither transition into 22km Low-LOD Tier 1 at [u_fade_start .. u_fade_end] -> ZERO disappearing cities!
    float fade = 1.0 - smoothstep(u_fade_start, u_fade_end, v_dist);
    if (fade < 0.02) discard;
    if (fade < 0.98) {
        float dither = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
        if (fade < dither) discard;
    }

    int arch = int(v_arch + 0.5);
    float h1 = v_bhash;
    float h2 = fract(v_bhash * 7.13 + 0.29);
    bool is_roof = (v_uv.y < 0.50);

    vec3 geom_n = normalize(v_world_n);
    vec3 sdir = normalize(u_sun_dir);
    vec3 view_dir = normalize(v_world_pos);
    float day_t = clamp((sdir.y + 0.035) / 0.14, 0.0, 1.0);
    float night_t = clamp((0.06 - sdir.y) / 0.12, 0.0, 1.0);

    vec3 alb = vec3(0.5);
    vec3 map_n = vec3(0.0, 0.0, 1.0);
    vec3 orm = vec3(1.0, 0.85, 0.0);
    vec2 cont_uv = v_uv;
    int chosen_style = 1;
    bool is_metal_roof = false;
    bool is_parapet_trim = false;

    if (is_roof) {
        bool flat_surf = (v_uv.x >= 0.50);
        int r_col = 0;
        if (arch == 4) {
            r_col = 3; // Industrial ribbed galvanized steel
            is_metal_roof = true;
            cont_uv = v_local_pos.xz / 6.5;
        } else if (arch == 2 || flat_surf) {
            r_col = 2; // Spanish flat apartment terrace roof (pavers, skylights, HVAC, solar!)
            cont_uv = (v_local_pos.xz + v_bldg_size.xz * 0.5) / 14.0 + vec2(h1, h2);
        } else if (v_elev_m > 860.0 && (arch == 0 || h2 < 0.55)) {
            r_col = 3; // Pyrenean dark slate roofs at mountain elevations
            is_metal_roof = true;
            cont_uv = v_local_pos.xz / 5.0;
        } else {
            // Mix warm sun-baked Teja Arabe (0) and weathered pale ochre historic clay (1)!
            r_col = (h1 < 0.56) ? 0 : 1;
            cont_uv = (abs(v_local_n.x) > abs(v_local_n.z)) ? vec2(v_local_pos.z, v_local_pos.y * 1.4 + v_local_pos.x) / 4.8
                                                             : vec2(v_local_pos.x, v_local_pos.y * 1.4 + v_local_pos.z) / 4.8;
        }
        sample_atlas_tile(r_col, 0, cont_uv, alb, map_n, orm);
    } else {
        // WORLD-SPACE 1:1 METRIC FACADE TILING (exact Spanish ~3.1m stories & ~3.4m bays, ZERO stretched giant windows!)
        bool x_face = abs(v_local_n.x) > 0.5;
        float wall_len = x_face ? v_bldg_size.z : v_bldg_size.x;
        float u_m = x_face ? (v_local_pos.z + v_bldg_size.z * 0.5) : (v_local_pos.x + v_bldg_size.x * 0.5);
        float y_m = v_local_pos.y;
        float bh = max(3.2, v_bldg_size.y);

        // Select diverse authentic Spanish facade style (0..7) by archetype, elevation, height & per-building hash!
        if (arch == 4) {
            chosen_style = 7; // Industrial corrugated steel & concrete cladding
        } else if (arch == 0 || arch == 5 || arch == 3) {
            chosen_style = (v_elev_m > 740.0 || h1 < 0.55) ? 3 : ((h1 < 0.80) ? 5 : 1);
        } else if (arch == 2) {
            // Spanish mid-rise apartments (Zaragoza / Huesca): Ladrillo Caravista brick, modern limestone, Ensanche miradores, ochre, glass-brick!
            if (h1 < 0.30) chosen_style = 0;      // Red Ladrillo Caravista brick (signature Zaragoza!)
            else if (h1 < 0.54) chosen_style = 2; // Modern precast limestone & wide glass loggias
            else if (h1 < 0.72) chosen_style = 4; // Ensanche terracotta-salmon & white miradores
            else if (h1 < 0.86) chosen_style = 6; // Contemporary brick & dark bronze glass
            else chosen_style = 1;                // Classic warm ochre apartments
        } else {
            // Historic Aragonese townhouses (arch == 1): warm ochre, sandstone civic, Ensanche salmon, brick, stone!
            if (h1 < 0.28) chosen_style = 1;
            else if (h1 < 0.52) chosen_style = 5;
            else if (h1 < 0.72) chosen_style = 4;
            else if (h1 < 0.88) chosen_style = 0;
            else chosen_style = 3;
        }

        // Check if pixel is a parapet/penthouse/chimney cap (above main wall shaft -> clean matching masonry, NO floating windows!)
        float eave_h = (arch == 2) ? (bh * 0.90) : ((arch == 1) ? (bh * 0.83) : (bh * 0.77));
        if (y_m > eave_h || abs(v_local_n.y) > 0.65) {
            is_parapet_trim = true;
            // Clean masonry/plaster parapet coping tone matching building style
            vec3 trim_c = (chosen_style == 0 || chosen_style == 6) ? vec3(0.46, 0.25, 0.18)
                        : ((chosen_style == 2 || chosen_style == 7) ? vec3(0.60, 0.59, 0.57)
                        : ((chosen_style == 4) ? vec3(0.62, 0.45, 0.37) : vec3(0.58, 0.53, 0.44)));
            alb = trim_c * (0.90 + 0.16 * hash21(floor(v_local_pos.xz * 2.5)));
            orm = vec3(0.92, 0.86, 0.0);
        } else {
            // Quantize horizontal bays to integer repeats of 4-bay tile (~13.6m per full tile) so windows NEVER clip at corners!
            float n_bays = max(2.0, floor(wall_len / 3.4 + 0.5));
            float u_tile = (u_m / max(0.5, wall_len)) * (n_bays * 0.25);

            // Ground-floor commercial shopfront / stone plinth band (bottom 3.4m on urban buildings taller than 5.8m!)
            float gf_h = (bh > 5.8 && arch != 5) ? 3.4 : 0.0;
            if (gf_h > 0.5 && y_m < gf_h) {
                int gf_col = (arch == 4) ? 3 : ((chosen_style == 2 || chosen_style == 6) ? 2 : ((chosen_style == 3 || chosen_style == 5) ? 1 : 0));
                float v_gf = clamp(1.0 - (y_m / gf_h), 0.01, 0.99);
                cont_uv = vec2(u_tile, v_gf);
                sample_atlas_tile(gf_col, 1, cont_uv, alb, map_n, orm);
            } else {
                // Upper stories: exact 3.1m per story (2 stories per 512px tile => 6.2m per tile repeat!)
                float up_span = max(2.8, eave_h - gf_h);
                float n_stories = max(1.0, floor(up_span / 3.1 + 0.5));
                float y_frac = clamp((y_m - gf_h) / up_span, 0.0, 1.0);
                float v_up = (1.0 - y_frac) * (n_stories * 0.5);
                cont_uv = vec2(u_tile, v_up);
                int f_row = (chosen_style < 4) ? 2 : 3;
                int f_col = chosen_style & 3;
                sample_atlas_tile(f_col, f_row, cont_uv, alb, map_n, orm);
            }
        }
    }

    vec3 nrm = (v_dist < 2200.0 && !is_parapet_trim) ? perturb_normal(geom_n, v_world_pos, cont_uv, map_n, 0.85) : geom_n;
    vec3 col = alb;
    float rough = clamp(orm.g, 0.08, 0.96);
    float metal = 0.0;
    vec3 emiss = vec3(0.0);

    if (is_roof) {
        // Harmonize crisp 2K architectural roof with pre-baked real IGN PNOA rooftop RGB without muddying terracotta!
        vec3 pnoa_lin = pow(max(v_roof_ao.rgb, vec3(0.04)), vec3(2.2));
        float tile_luma = max(0.08, dot(alb, vec3(0.299, 0.587, 0.114)));
        vec3 harmonized = clamp(pnoa_lin * clamp(tile_luma * 2.05, 0.55, 1.55), vec3(0.04), vec3(0.88));
        col = mix(alb, harmonized, is_metal_roof ? 0.36 : 0.42);
        // Subtle per-building clay brightness/warmth drift so roofs form an organic Spanish mosaic!
        col *= mix(vec3(0.88, 0.86, 0.84), vec3(1.14, 1.06, 0.96), h2);
        metal = is_metal_roof ? clamp(orm.b, 0.0, 0.65) : 0.0;
    } else {
        float wall_bright = smoothstep(0.12, 0.30, dot(alb, vec3(0.3, 0.59, 0.11)));
        col = mix(col, col * building_patina(v_bhash, chosen_style), wall_bright * 0.82);

        if (!is_parapet_trim && orm.b > 0.45) {
            // 3D Interior Parallax Window Trick + Crisp Sky Reflection!
            vec2 win_cell = floor(cont_uv * vec2(4.0, 2.0));
            vec2 win_uv = fract(cont_uv * vec2(4.0, 2.0));
            float r_id = hash21(win_cell + floor(v_roof_ao.rg * 41.0 + v_bhash * 19.0));
            vec2 par = clamp(vec2(dot(view_dir, cross(geom_n, vec3(0.0, 1.0, 0.0))), view_dir.y) * 0.28, vec2(-0.22), vec2(0.22));
            float rcol = floor(fract(r_id * 4.0) * 4.0);
            float rrow = floor(fract(r_id * 16.0) * 4.0);
            vec2 room_uv = (vec2(rcol, rrow) + clamp(win_uv + par, vec2(0.05), vec2(0.95))) * 0.25;
            vec3 room = texture(u_interior_rooms, room_uv).rgb;
            col = room * 0.44;
            rough = 0.07;
            metal = 0.05;
            nrm = geom_n;
            float lit_room = step(0.42, r_id) * night_t;
            emiss += room * vec3(1.65, 1.08, 0.52) * (lit_room * 1.55);
        }
    }

    // Ground-contact sidewalk plinth AO (0..1.4m) + warm sunlit street bounce onto lower stories (0..7m) -> welds building into scene!
    float base_contact = mix(0.66, 1.0, smoothstep(0.0, 1.35, max(0.0, v_local_pos.y)));
    float street_bounce = (1.0 - smoothstep(0.5, 7.5, max(0.0, v_local_pos.y))) * (1.0 - abs(geom_n.y)) * day_t;
    float canyon_ao = mix(0.58, 1.0, v_roof_ao.a) * mix(0.52, 1.0, orm.r) * base_contact;

    // Terrain-synchronized Cook-Torrance GGX PBR + Warm Iberian Pavement Bounce + Sky IBL!
    vec3 ldir = (sdir.y >= -0.025) ? sdir : normalize(vec3(-sdir.x, 0.55, -sdir.z));
    vec3 sun_c = (sdir.y >= -0.025)
        // Match the terrain's calibrated daylight response so airport & city buildings blend cleanly.
        ? mix(vec3(1.0, 0.62, 0.30), vec3(1.0, 0.95, 0.86), clamp(sdir.y / 0.45, 0.0, 1.0)) * mix(0.55, 2.42, day_t)
        : vec3(0.32, 0.45, 0.72) * 0.36;
    vec3 sky_amb = mix(vec3(0.07, 0.11, 0.20), mix(vec3(0.34, 0.39, 0.45), u_haze_cool * 0.82, 0.52), day_t);
    vec3 gnd_amb = mix(vec3(0.04, 0.04, 0.05), vec3(0.28, 0.24, 0.19), day_t);
    vec3 amb_c = mix(gnd_amb, sky_amb, nrm.y * 0.5 + 0.5) * 0.54 + vec3(0.14, 0.12, 0.09) * street_bounce;

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
    vec3 ibl_env = mix(gnd_amb, mix(u_haze_cool, u_haze_warm, pow(max(0.0, dot(refl, sdir)), 4.0) * 0.60), smoothstep(-0.12, 0.35, refl.y));
    vec3 F_ibl = F0 + (max(vec3(1.0 - rough), F0) - F0) * pow(1.0 - ndv, 4.0);
    vec3 lit = kD * col * (amb_c * canyon_ao + sun_c * (ndl * mix(0.68, 1.0, canyon_ao))) +
               sun_c * (spec * ndl * 0.45) + ibl_env * F_ibl * (0.52 * day_t * canyon_ao);

    // EXACT Terrain-Matched 2-Layer Analytic Height Fog (Valley H_v=290m + Tropospheric H_a=2800m) + Rayleigh/Mie Scattering!
    float cos_th = max(0.0, dot(view_dir, sdir));
    float sun_vis = clamp((sdir.y + 0.08) / 0.24, 0.0, 1.0);
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
    float mist = clamp(1.0 - exp(-(tau_v + tau_a)), 0.0, 0.88);
    float valley_frac = clamp(tau_v / max(0.0001, tau_v + tau_a), 0.0, 1.0);
    float rayleigh = 0.75 * (1.0 + cos_th * cos_th);
    float hg_w = (1.0 - 0.3844) / (4.0 * 3.14159 * pow(max(0.04, 1.3844 - 1.24 * cos_th), 1.5));
    float mie = clamp(hg_w * 0.52 * sun_vis + pow(1.0 - clamp(abs(view_dir.y) * 2.4, 0.0, 1.0), 2.2) * 0.32 * sun_vis, 0.0, 1.35);
    vec3 ray_col = mix(u_haze_cool * (0.62 + 0.22 * rayleigh), mix(u_haze_cool, vec3(0.76, 0.84, 0.92), 0.38), valley_frac * 0.65);
    vec3 insc = mix(ray_col, u_haze_warm * 1.32, clamp(mie * 0.80, 0.0, 0.92));

    frag_color = vec4(mix(lit, insc, mist * 0.90) + emiss * (1.0 - mist * 0.6), 1.0);
}
