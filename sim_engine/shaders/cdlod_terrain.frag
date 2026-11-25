#version 460 core

layout(binding = 1) uniform sampler2DArray u_ortho_arr;   // 256x256 SRGB8_ALPHA8 (linearized by hardware!)
layout(binding = 2) uniform sampler2DArray u_norm_arr;    // 65x65 R=nx, G=nz, B=cavity_ao
layout(binding = 3) uniform sampler2DArray u_surf_arr;    // 65x65 R=forest, G=prebaked sun shadow, B=IGN water, A=PMRF OSM buildings
layout(binding = 4) uniform sampler2DArray u_sat_arr;     // 16 layers x 256x256 real Northern Spain IGN PNOA patterns (hardware mipmaps + repeat!)
layout(binding = 5) uniform sampler2D u_rock_albedo;      // SRGB linearized
layout(binding = 6) uniform sampler2D u_rock_normal;
layout(binding = 7) uniform sampler2D u_cloud_noise;
layout(binding = 8) uniform sampler2D u_micro_detail;     // R=grass, G=sand, B=canopy height, A=leaf transmission
layout(binding = 9) uniform sampler2D u_grass_albedo;     // 1024x1024 CC0 Poly Haven close-up grass albedo (SRGB)
layout(binding = 10) uniform sampler2D u_grass_normal;    // 1024x1024 CC0 Poly Haven close-up grass normal
layout(binding = 11) uniform sampler2D u_gravel_normal;   // 1024x1024 CC0 Poly Haven close-up gravel/dirt normal
layout(binding = 12) uniform sampler2D u_gravel_albedo;   // 1024x1024 CC0 Poly Haven pebbles/soil/asphalt grit (SRGB)
layout(binding = 13) uniform sampler2D u_refl_env;        // Pre-baked 256x128 RGBA16F IBL reflection map + mip roughness chain!

uniform float u_cam_alt_m;
uniform vec3 u_sun_dir;
uniform vec3 u_haze_cool;
uniform vec3 u_haze_warm;
uniform float u_haze_density;
uniform float u_cloud_cov;
uniform float u_time;
uniform float u_detail_strength;
uniform vec2 u_wind_en; // Live RegionalAtmosphere local wind (East, North m/s) -> wind-driven water waves & chop!
uniform int u_quality; // F3 High uses baked light + compact material lookups at native resolution.
uniform int u_season; // 0 winter, 1 spring, 2 summer, 3 autumn; sourced from the regional weather library.
uniform float u_snowline_m; // Season- and weather-specific regional snowline above MSL.
uniform vec4 u_grass_zone;        // EN center, then runway direction (E,N).
uniform vec4 u_grass_zone_bounds; // half length, outer half width, runway half width, grass-surface flag.
uniform vec4 u_wx_env;            // x=ground_wetness01, y=snow_factor01, z=lightning_flash01, w=storm_int01

in vec3 v_world_pos;
in vec2 v_world_en;
in vec2 v_child_uv;
in vec2 v_parent_uv;
in float v_page_slice;
in float v_parent_slice;
in float v_morph;
in float v_fade_in;
in float v_elev_m;
in float v_dist;
in float v_node_size;
in float v_canopy_h;
in float v_crown_id;

layout(location = 0) out vec4 frag_color;

float hash21(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

float vnoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(
        mix(hash21(i), hash21(i + vec2(1.0, 0.0)), u.x),
        mix(hash21(i + vec2(0.0, 1.0)), hash21(i + vec2(1.0, 1.0)), u.x),
        u.y
    );
}

vec2 rot27(vec2 p) {
    return vec2(p.x * 0.8910 - p.y * 0.4540, p.x * 0.4540 + p.y * 0.8910);
}

// iGPU-tuned interlocking crown volume + smooth 2-tap LiDAR canopy normal perturbation.
// Uses the real PNOA orthophoto + existing micro_detail/surf textures (zero extra VRAM, 2-3 taps max!).
void apply_canopy_volume(inout vec3 albedo, inout vec3 normal, float strength, float canopy_h01,
                         vec2 aux_uv_c, float page_slice, vec3 view_dir, vec3 sun_dir) {
    if (strength <= 0.01) return;
    vec3 crown_n = normal;
    // 1. Smooth 2-tap forward-difference LiDAR canopy gradient (replaces 4-tap diff & faceted triangle normals)
    if (v_dist < 4200.0) {
        float near_grad = 1.0 - smoothstep(1800.0, 4200.0, v_dist);
        float step_u = 1.0 / 129.0;
        float h_e = textureLod(u_surf_arr, vec3(min(aux_uv_c.x + step_u, 0.996), aux_uv_c.y, page_slice), 0.0).r;
        float h_n = textureLod(u_surf_arr, vec3(aux_uv_c.x, min(aux_uv_c.y + step_u, 0.996), page_slice), 0.0).r;
        float tex_m = max(v_node_size / 128.0, 2.5);
        float gx = (h_e - canopy_h01) * 14.0 / tex_m;
        float gn = (h_n - canopy_h01) * 14.0 / tex_m;
        vec3 lidar_n = normalize(vec3(-gx, 1.0, gn));
        crown_n = normalize(mix(normal, lidar_n, strength * near_grad * 0.72));
    }

    // 2. Distance-selected interlocking forest-mass & tree-pack crown relief (1-2 fetches instead of 3!)
    vec2 dome_g;
    float dome_h;
    float leaf_mask;
    float dist_fade = 1.0 - smoothstep(7000.0, 22000.0, v_dist);
    if (v_dist < 3000.0) {
        vec2 uv_near = rot27(v_world_en) * 0.082;
        vec4 m_near = texture(u_micro_detail, uv_near);
        vec2 ray = rot27(vec2(view_dir.x, -view_dir.z)) / max(abs(view_dir.y), 0.42);
        vec2 crown_p = texture(u_micro_detail, uv_near - ray * (0.024 * m_near.b)).ba;
        float close_leaf = 1.0 - smoothstep(140.0, 850.0, v_dist);
        dome_g = (m_near.rg - vec2(0.5)) * (1.30 + 0.35 * close_leaf);
        dome_h = mix(m_near.b, crown_p.x, 0.72);
        leaf_mask = crown_p.y;
        // Ground-sight volumetric foliage shading (active out to 480 m across entire airfields!):
        // Uses 3D triplanar multi-shell leaf-cluster parallax + deep bough shadow cavities +
        // shaded vertical bark/trunk understory (NEVER brown dead-leaf grass_albedo.jpg!).
        float ground_foliage = (1.0 - smoothstep(140.0, 480.0, v_dist)) * strength;
        if (ground_foliage > 0.02) {
            float close_f = 1.0 - smoothstep(35.0, 220.0, v_dist);
            float side_w = smoothstep(0.05, 0.38, 1.0 - crown_n.y);
            // Triplanar projection along steep deformed tree walls eliminates top-down ortho stretch!
            vec2 top_p = rot27(v_world_en);
            vec2 side_p = vec2(v_world_en.x * 0.74 + v_world_en.y * 0.67, v_elev_m + v_canopy_h);
            vec2 proj_p = mix(top_p, side_p, side_w);
            vec2 par_dir = mix(rot27(vec2(view_dir.x, -view_dir.z)),
                               vec2(view_dir.x * 0.74 - view_dir.z * 0.67, view_dir.y), side_w);

            // 2-shell parallax-occluded leafy bough clusters (~2.2m boughs + ~0.55m leaf tufts)
            vec4 bough0 = texture(u_micro_detail, proj_p * 0.22);
            vec2 p_shell1 = proj_p - par_dir * ((1.0 - bough0.b) * (0.48 * (0.45 + 0.55 * close_f)));
            vec4 bough1 = texture(u_micro_detail, p_shell1 * 0.68 + vec2(0.17, 0.43));
            vec4 twig   = texture(u_micro_detail, rot27(p_shell1) * 1.95);

            // Distinct rounded leafy bough height & crisp shadow gaps between clusters
            float cluster_h = clamp(bough0.b * 0.48 + bough1.b * 0.36 + twig.b * 0.16, 0.0, 1.0);
            float cavity_ao = mix(0.28, 1.32, smoothstep(0.18, 0.82, cluster_h));
            // Per-bough 3D normal perturbation so sunlit bough faces pop vividly from dark foliage recesses!
            vec2 bough_n = (bough0.rg - 0.5) * 1.45 + (bough1.rg - 0.5) * 1.85 + (twig.rg - 0.5) * (1.25 * close_f);
            dome_g += bough_n * (ground_foliage * (1.0 + 0.55 * side_w));

            // Per-crown natural Northern Spain species tint (pine / olive oak / sunlit poplar) anchored to PNOA
            vec3 sp_tint = mix(vec3(0.82, 0.94, 0.88), vec3(1.16, 1.12, 0.82), v_crown_id);
            float sunlit_tip = smoothstep(0.55, 0.92, cluster_h) * clamp(dot(crown_n, sun_dir) * 0.6 + 0.4, 0.0, 1.0);
            vec3 leafy_col = albedo * sp_tint * cavity_ao * mix(vec3(0.88, 0.94, 0.96), vec3(1.12, 1.16, 0.88), sunlit_tip);

            // Shaded under-canopy skirt + dark vertical tree-trunk / woody stem bands on bottom 2.2 m!
            float skirt = (1.0 - smoothstep(0.35, 2.35, v_canopy_h)) * smoothstep(0.08, 0.45, v_canopy_h) * (0.35 + 0.65 * side_w);
            float trunk_wave = smoothstep(0.68, 0.90, vnoise(vec2((v_world_en.x + v_world_en.y) * 0.85, (v_elev_m + v_canopy_h) * 0.12)));
            vec3 understory = mix(leafy_col * 0.34, vec3(0.068, 0.050, 0.034), trunk_wave * 0.78);
            leafy_col = mix(leafy_col, understory, skirt * (0.65 + 0.30 * close_f));

            // Subtle fine leaf grain at close walkaround range (<90m)
            float fine_leaf = (1.0 - smoothstep(20.0, 95.0, v_dist)) * strength;
            if (fine_leaf > 0.02) {
                float leaf_speck = vnoise(p_shell1 * 6.5);
                leafy_col *= mix(1.0, 0.78 + 0.44 * leaf_speck, fine_leaf * 0.65);
            }
            albedo = mix(albedo, leafy_col, ground_foliage * 0.94);
        }
    } else {
        vec4 m_far = texture(u_micro_detail, v_world_en * 0.024);
        dome_g = (m_far.rg - vec2(0.5)) * 0.92;
        dome_h = m_far.b;
        leaf_mask = m_far.a;
    }
    crown_n = normalize(crown_n + vec3(dome_g.x, 0.0, dome_g.y) * (strength * 0.65 * dist_fade));
    normal = crown_n;

    // Soft canopy-skirt terrain blending + directional sunlit crown tops + backlit foliage scatter
    float crown_edge = smoothstep(0.05, 0.24, canopy_h01);
    float sun_crown = clamp(dot(crown_n, sun_dir) * 0.55 + 0.45, 0.20, 1.14);
    float under_ao = mix(0.76, 1.12, smoothstep(0.18, 0.86, dome_h) * (0.48 + 0.52 * crown_edge));
    float backlight = max(dot(view_dir, sun_dir), 0.0) * smoothstep(0.0, 0.42, sun_dir.y);
    float detail = clamp((under_ao * (0.86 + 0.28 * sun_crown) - 1.0) + leaf_mask * backlight * 0.11, -0.22, 0.22);
    albedo *= (1.0 + detail * strength * dist_fade);
}

// At eye level the ortho has no resolved blades or grit. Reuse the existing
// material atlas at a real surface scale, keeping the ortho's local color.
float airport_turf_mask() {
    if (u_grass_zone_bounds.x <= 0.0) return 0.0;
    vec2 axis = normalize(u_grass_zone.zw);
    vec2 delta = v_world_en - u_grass_zone.xy;
    float along = abs(dot(delta, axis));
    float across = abs(delta.x * axis.y - delta.y * axis.x);
    float along_mask = 1.0 - smoothstep(u_grass_zone_bounds.x - 8.0,
                                        u_grass_zone_bounds.x + 22.0, along);
    float across_mask = 1.0 - smoothstep(u_grass_zone_bounds.y - 14.0,
                                         u_grass_zone_bounds.y + 12.0, across);
    float runway_clear = mix(smoothstep(u_grass_zone_bounds.z + 8.0,
                                        u_grass_zone_bounds.z + 25.0, across),
                             1.0, u_grass_zone_bounds.w);
    return along_mask * across_mask * runway_clear;
}

void ground_relief(inout vec3 albedo, inout vec3 normal, float vegetation, float water, vec3 view_dir) {
    float airport_turf = airport_turf_mask();
    float natural_veg = smoothstep(.08, .32, vegetation);
    float grass_cover = max(natural_veg, airport_turf);
    float agl_here = max(0.0, u_cam_alt_m - v_elev_m);
    // Compute exact grass-runway strip coordinates when near a grass airfield (e.g. Castejon ES-0205)
    float grass_rwy = 0.0;
    float rwy_across = 999.0;
    float rwy_along = 999.0;
    if (u_grass_zone_bounds.x > 0.0 && u_grass_zone_bounds.w > 0.5) {
        vec2 axis = normalize(u_grass_zone.zw);
        vec2 delta = v_world_en - u_grass_zone.xy;
        rwy_along = abs(dot(delta, axis));
        rwy_across = abs(delta.x * axis.y - delta.y * axis.x);
        float al_m = 1.0 - smoothstep(u_grass_zone_bounds.x - 6.0, u_grass_zone_bounds.x + 12.0, rwy_along);
        float ac_m = 1.0 - smoothstep(u_grass_zone_bounds.z - 2.0, u_grass_zone_bounds.z + 3.5, rwy_across);
        grass_rwy = al_m * ac_m;
    }
    float near_ground = (1.0 - smoothstep(48.0, mix(195.0, 280.0, grass_rwy), v_dist)) *
                        (1.0 - smoothstep(12.0, mix(38.0, 68.0, grass_rwy), agl_here)) *
                        smoothstep(.52, .90, normal.y) * (1.0 - smoothstep(.08, .25, water));
    if (near_ground <= .01) return;

    // Eye-level walkaround & taxi zone (<115m, <28m AGL): crisp parallax grass tufts, mowed runway stripes,
    // worn main-gear tyre tracks, and velvety grazing grass sheen (0 KB extra VRAM, zero 3D grass cost!).
    float eye_w = (1.0 - smoothstep(18.0, 115.0, v_dist)) * (1.0 - smoothstep(5.0, 28.0, agl_here));
    float grass_w = smoothstep(.12, .42, max(grass_cover, grass_rwy));

    // Subtle 1-step blade-height parallax on close grass gives real velocity & depth cues during taxi/takeoff!
    vec2 par_en = v_world_en;
    if (grass_w > .5 && eye_w > .05) {
        float h_g = texture(u_micro_detail, v_world_en * 0.48).b;
        vec2 ray_en = vec2(view_dir.x, -view_dir.z) / max(abs(view_dir.y), 0.35);
        par_en -= clamp(ray_en, vec2(-2.2), vec2(2.2)) * ((1.0 - h_g) * 0.085 * eye_w);
    }
    vec2 uv_macro = rot27(par_en) * .42;
    vec2 uv_micro = par_en * 2.15;
    float break_tile = textureLod(u_cloud_noise, par_en * .018, 0.0).r;

    vec3 mat_col;
    vec3 mat_nrm;
    float ref_luma;
    if (grass_w > .5) {
        vec3 g0 = texture(u_grass_albedo, uv_macro).rgb;
        vec3 g1 = texture(u_grass_albedo, uv_micro).rgb;
        vec3 n0 = texture(u_grass_normal, uv_macro).rgb * 2.0 - 1.0;
        vec3 n1 = texture(u_grass_normal, uv_micro).rgb * 2.0 - 1.0;
        float mb = mix(.42, .72, break_tile);
        mat_col = mix(g0, g1, mb);
        mat_nrm = normalize(mix(n0, n1, .68));
        ref_luma = .244;
    } else {
        vec3 r0 = texture(u_gravel_albedo, uv_macro * .85).rgb;
        vec3 r1 = texture(u_gravel_albedo, uv_micro * .95).rgb;
        vec3 n0 = texture(u_gravel_normal, uv_macro * .85).rgb * 2.0 - 1.0;
        vec3 n1 = texture(u_gravel_normal, uv_micro * .95).rgb * 2.0 - 1.0;
        float mb = mix(.40, .70, break_tile);
        mat_col = mix(r0, r1, mb);
        mat_nrm = normalize(mix(n0, n1, .65));
        ref_luma = .145;
    }

    float src_luma = max(.015, dot(mat_col, vec3(.2126, .7152, .0722)));
    float lo_c = mix(.88, .58, eye_w);
    float hi_c = mix(1.12, 1.44, eye_w);
    float detail_gain = clamp(src_luma / ref_luma, lo_c, hi_c);
    // Crisp multi-scale grass-tuft & blade grain + velvety grazing sheen on close turf
    float blade_sheen = 0.0;
    if (grass_w > .5 && eye_w > .04) {
        float tuft = texture(u_micro_detail, par_en * 0.48).b;
        float blade = vnoise(par_en * 6.4);
        float fine_b = vnoise(rot27(par_en) * 14.0);
        detail_gain *= mix(1.0, (0.74 + 0.52 * tuft) * (0.84 + 0.22 * blade + 0.12 * fine_b), eye_w * 0.78);
        float graz = pow(1.0 - clamp(dot(normal, -view_dir), 0.0, 1.0), 2.8);
        blade_sheen = graz * eye_w * (0.14 + 0.12 * tuft);
    }
    vec3 pasture_tint = (grass_w > .5)
        ? mix(vec3(0.86, 0.96, 0.84), vec3(1.04, 1.09, 0.84), break_tile)
        : clamp(mat_col / vec3(src_luma), vec3(.82), vec3(1.20));
    vec3 textured_albedo = albedo * detail_gain * mix(vec3(1.0), pasture_tint, eye_w * .60);

    // Grass-runway visual landmarks: mowed 4.2m strips + worn C172 dual main-gear wheel ruts (+-1.35m)
    if (grass_rwy > 0.02) {
        float mow_band = 0.5 + 0.5 * sin(rwy_across * (3.14159265 / 4.2));
        float mow_gain = mix(0.92, 1.09, smoothstep(0.15, 0.85, mow_band));
        float rut_l = exp(-pow((rwy_across - 1.35) / 0.72, 2.0));
        float rut_c = exp(-pow(rwy_across / 0.55, 2.0)) * 0.45;
        float wear_noise = 0.65 + 0.35 * textureLod(u_cloud_noise, par_en * 0.035, 0.0).r;
        float rut_w = clamp((rut_l + rut_c) * wear_noise, 0.0, 1.0) * grass_rwy;
        // Touchdown zone wear slightly stronger near both ends
        float td_zone = smoothstep(u_grass_zone_bounds.x * 0.45, u_grass_zone_bounds.x * 0.88, rwy_along);
        rut_w = clamp(rut_w * (0.72 + 0.42 * td_zone), 0.0, 0.85);
        vec3 soil_rut = textured_albedo * vec3(1.16, 1.00, 0.76) * 0.86;
        textured_albedo = mix(textured_albedo * mow_gain, soil_rut, rut_w * 0.68);
        // Subtle crisp darker mowed border transition along grass runway edges
        float edge_cut = exp(-pow((rwy_across - u_grass_zone_bounds.z) / 1.2, 2.0)) * grass_rwy;
        textured_albedo *= (1.0 - edge_cut * 0.14);
    }
    textured_albedo += textured_albedo * vec3(0.32, 0.40, 0.18) * blade_sheen;
    albedo = mix(albedo, textured_albedo, near_ground);
    float nrm_strength = mix(.26, .88, eye_w);
    normal = normalize(normal + vec3(mat_nrm.x, 0.0, mat_nrm.y) * (near_ground * nrm_strength));
}

void main() {
    float n_macro = textureLod(u_cloud_noise, v_world_en * 0.00018 + vec2(0.17, 0.43), 0.0).r;
    float n_meso  = textureLod(u_cloud_noise, v_world_en * 0.00095 + vec2(n_macro * 0.15, -n_macro * 0.12), 0.0).r;

    // 1. Organic Noise-Modulated Stochastic CDLOD Parent <-> Child Dissolve (ZERO boxy or jaggy tile inclusions!)
    // New high-res tiles flow naturally across ridges & fields via Perlin-modulated smoothstep!
    float org_noise = (n_macro * 0.60 + n_meso * 0.40 - 0.50) * 0.28;
    float arrival_blend = smoothstep(0.0, 1.0, clamp((v_fade_in * 1.28) - (org_noise + 0.14), 0.0, 1.0));
    float tex_morph = clamp(max(smoothstep(0.0, 1.0, v_morph + org_noise * 0.55), 1.0 - arrival_blend), 0.0, 1.0);

    vec2 ortho_c_uv = (vec2(0.5) + v_child_uv * 511.0) / 512.0;
    vec2 ortho_p_uv = (vec2(0.5) + v_parent_uv * 511.0) / 512.0;
    vec2 aux_c_uv   = (vec2(0.5) + v_child_uv * 128.0) / 129.0;
    vec2 aux_p_uv   = (vec2(0.5) + v_parent_uv * 128.0) / 129.0;

    vec2 ortho_c_dx = dFdx(ortho_c_uv), ortho_c_dy = dFdy(ortho_c_uv);
    vec2 ortho_p_dx = dFdx(ortho_p_uv), ortho_p_dy = dFdy(ortho_p_uv);
    vec2 aux_c_dx = dFdx(aux_c_uv), aux_c_dy = dFdy(aux_c_uv);
    vec2 aux_p_dx = dFdx(aux_p_uv), aux_p_dy = dFdy(aux_p_uv);
    vec3 ortho;
    vec4 nao_v;
    vec4 surf;
    if (tex_morph <= 0.001) {
        ortho = textureGrad(u_ortho_arr, vec3(ortho_c_uv, v_page_slice), ortho_c_dx, ortho_c_dy).rgb;
        nao_v = textureGrad(u_norm_arr, vec3(aux_c_uv, v_page_slice), aux_c_dx, aux_c_dy);
        surf = textureGrad(u_surf_arr, vec3(aux_c_uv, v_page_slice), aux_c_dx, aux_c_dy);
    } else if (tex_morph >= 0.999) {
        ortho = textureGrad(u_ortho_arr, vec3(ortho_p_uv, v_parent_slice), ortho_p_dx, ortho_p_dy).rgb;
        nao_v = textureGrad(u_norm_arr, vec3(aux_p_uv, v_parent_slice), aux_p_dx, aux_p_dy);
        surf = textureGrad(u_surf_arr, vec3(aux_p_uv, v_parent_slice), aux_p_dx, aux_p_dy);
    } else {
        ortho = mix(textureGrad(u_ortho_arr, vec3(ortho_c_uv, v_page_slice), ortho_c_dx, ortho_c_dy).rgb,
                    textureGrad(u_ortho_arr, vec3(ortho_p_uv, v_parent_slice), ortho_p_dx, ortho_p_dy).rgb, tex_morph);
        nao_v = mix(textureGrad(u_norm_arr, vec3(aux_c_uv, v_page_slice), aux_c_dx, aux_c_dy),
                    textureGrad(u_norm_arr, vec3(aux_p_uv, v_parent_slice), aux_p_dx, aux_p_dy), tex_morph);
        surf = mix(textureGrad(u_surf_arr, vec3(aux_c_uv, v_page_slice), aux_c_dx, aux_c_dy),
                   textureGrad(u_surf_arr, vec3(aux_p_uv, v_parent_slice), aux_p_dx, aux_p_dy), tex_morph);
    }
    vec3 nao = nao_v.rgb;
    float rt_night = nao_v.a; // Pre-baked 3.9m/px 2.5D DSM ray-traced night local streetlamp irradiance!

    float nx = nao.r * 2.0 - 1.0;
    float nz = nao.g * 2.0 - 1.0;
    float ny = sqrt(max(0.02, 1.0 - nx * nx - nz * nz));
    vec3 world_n = normalize(vec3(nx, ny, nz));
    float cavity_ao = mix(0.44, 1.24, nao.b);
    float slope = 1.0 - world_n.y;

    vec3 sdir = normalize(u_sun_dir);
    vec3 view_dir = normalize(v_world_pos);
    vec3 pmrf_em = vec3(0.0);

    // Native-resolution iGPU path. Keep the real orthophoto and baked LiDAR/DSM
    // lighting as the primary signal, then add only one material-detail lookup,
    // one weather shadow LUT, and a close-range synthesized detail sample.
    // This avoids the full per-pixel multi-layer PBR/IBL cascade used by Ultra.
    if (u_quality == 2) {
        // Keep urban park & roadside LiDAR trees intact (only suppress over water).
        float forest_fast = smoothstep(0.05, 0.18, surf.r) *
                            (1.0 - smoothstep(0.18, 0.42, surf.b));
        float green_fast = clamp((ortho.g - ortho.r * 0.82) * 11.0 + (ortho.g - ortho.b) * 2.6, 0.0, 1.0);
        float slope_fast = 1.0 - world_n.y;
        float rock_fast = smoothstep(0.20, 0.38, slope_fast) * smoothstep(480.0, 720.0, v_elev_m);
        float veg_fast = clamp(max(forest_fast, green_fast * (1.0 - rock_fast)), 0.0, 1.0);
        vec3 fast_albedo = ortho;
        // Skip u_sat_arr lookup on dense forest canopy (ortho + LiDAR canopy volume supplies the detail).
        if (forest_fast < 0.82) {
            int layer_fast = (rock_fast > 0.42) ? ((v_elev_m > 1650.0) ? 13 : 12) :
                             (forest_fast > 0.28) ? ((v_elev_m > 1080.0) ? 9 : 6) :
                             (green_fast > 0.42) ? 2 : 4;
            float layer_scale = mix(180.0, 420.0, smoothstep(900.0, 12000.0, v_dist));
            vec3 baked_detail = textureLod(u_sat_arr, vec3(v_world_en / layer_scale, float(layer_fast)),
                                          clamp(log2(max(1.0, v_dist / 260.0)) * 0.72, 0.0, 4.2)).rgb;
            fast_albedo *= clamp(vec3(1.0) + (baked_detail - vec3(0.5)) * 0.24, vec3(0.88), vec3(1.12));
        }
        vec3 meadow_tint_fast = u_season == 0 ? vec3(0.92, 0.95, 0.99) :
                                u_season == 1 ? vec3(0.98, 1.035, 0.96) :
                                u_season == 2 ? vec3(1.035, 0.99, 0.88) : vec3(1.08, 0.92, 0.78);
        float veg_tint_fast = clamp((green_fast * 0.16 + forest_fast * 0.10) * (1.0 - rock_fast), 0.0, 0.20);
        fast_albedo *= mix(vec3(1.0), meadow_tint_fast, veg_tint_fast);
        float snow_h_fast = v_elev_m + smoothstep(-0.35, 0.55, world_n.z) * 150.0 + (n_meso - 0.5) * 100.0;
        float snow_fast = smoothstep(u_snowline_m - 170.0, u_snowline_m + 120.0, snow_h_fast) *
                          (1.0 - smoothstep(0.10, 0.28, slope_fast)) * (1.0 - smoothstep(0.08, 0.38, surf.b));
        fast_albedo = mix(fast_albedo, vec3(0.72, 0.78, 0.84), snow_fast * 0.88);
        vec3 normal_fast = world_n;

        // Water slope-gate & ortho aqua cue (prevents painting flat blue over Rio Esera gravel bars!)
        float aqua_fast = smoothstep(0.01, 0.08, ortho.g - ortho.r) * smoothstep(0.74, 0.98, ortho.b / max(0.015, ortho.r));
        float water_slope_fast = smoothstep(0.84, 0.94, world_n.y);
        float water_raw_fast = max(surf.b * water_slope_fast, smoothstep(0.02, 0.22, surf.b) * aqua_fast * water_slope_fast * 0.95);
        float water_mask_fast = smoothstep(0.32, 0.50, water_raw_fast);

        float near_w_fast = 1.0 - smoothstep(60.0, 1400.0, v_dist);
        if (near_w_fast > 0.01 && water_mask_fast < 0.15) {
            vec3 close_tex = (veg_fast > 0.38)
                ? texture(u_grass_albedo, v_world_en * 0.28).rgb
                : texture(u_gravel_albedo, v_world_en * 0.28).rgb;
            float close_luma = dot(close_tex, vec3(0.2126, 0.7152, 0.0722));
            float close_grain = clamp(1.0 + (close_luma - 0.48) * 0.26, 0.88, 1.12);
            fast_albedo *= mix(1.0, close_grain, near_w_fast * 0.76);
        }

        ground_relief(fast_albedo, normal_fast, green_fast * (1.0 - forest_fast), water_mask_fast, view_dir);
        // Preserve authentic PNOA crown hue while applying volumetric LiDAR canopy relief & shading
        float canopy_w_fast = forest_fast * (1.0 - snow_fast);
        fast_albedo = mix(fast_albedo, ortho, canopy_w_fast * 0.85);
        vec2 aux_c_use = (tex_morph >= 0.5) ? aux_p_uv : aux_c_uv;
        float slice_use = (tex_morph >= 0.5) ? v_parent_slice : v_page_slice;
        apply_canopy_volume(fast_albedo, normal_fast, canopy_w_fast, surf.r, aux_c_use, slice_use, view_dir, sdir);

        // 1-tap LiDAR canopy contact shadow on adjacent sunlit ground/streets (iGPU friendly, zero shadow map!)
        float canopy_vis_fast = 1.0;
        if (v_dist < 2200.0 && sdir.y > 0.10 && water_mask_fast < 0.15 && surf.r < 0.12) {
            float parallax_m = clamp(6.0 / max(sdir.y, 0.14), 3.0, 40.0);
            float parent_page = (tex_morph >= 0.5) ? 2.0 : 1.0;
            vec2 sun_en = vec2(sdir.x, -sdir.z);
            vec2 p_uv_s = aux_c_use + sun_en * (parallax_m / max(v_node_size * parent_page, 1.0)) * (128.0 / 129.0);
            if (all(greaterThanEqual(p_uv_s, vec2(0.004))) && all(lessThanEqual(p_uv_s, vec2(0.996)))) {
                float caster = textureLod(u_surf_arr, vec3(p_uv_s, slice_use), 0.0).r;
                float fade_p = 1.0 - smoothstep(1400.0, 2200.0, v_dist);
                canopy_vis_fast = 1.0 - smoothstep(0.08, 0.34, caster) * 0.34 * fade_p;
            }
        }

        float cloud_shadow_fast = 1.0;
        float sun_vis_fast = clamp((sdir.y + 0.08) / 0.24, 0.0, 1.0);
        if (sun_vis_fast > 0.01) {
            // Match clouds_qres.frag wind_base & MACRO_FREQ so terrain shadows track overhead cumulus & storm cells!
            vec2 flow_fast = vec2(u_time * 9.5, u_time * 4.2);
            vec2 shadow_uv_fast = (v_world_en + vec2(sdir.x, -sdir.z) * (1200.0 / max(sdir.y, 0.22)) + flow_fast) * 0.000112;
            float shadow_noise_fast = textureLod(u_cloud_noise, shadow_uv_fast, 0.5).r;
            float threshold_fast = mix(0.75, 0.34, clamp(u_cloud_cov, 0.06, 0.96));
            float sh_depth = mix(0.46, 0.68, clamp(u_wx_env.w, 0.0, 1.0));
            cloud_shadow_fast = 1.0 - smoothstep(threshold_fast - 0.04, threshold_fast + 0.18, shadow_noise_fast) *
                                      sh_depth * sun_vis_fast;
        }
        // Rain-wet surface darkening (asphalt runways, aprons, roads & rock darken realistically when soaked!)
        float wet01 = clamp(u_wx_env.x, 0.0, 1.0) * (1.0 - water_mask_fast) * (1.0 - snow_fast);
        float puddle_mask = wet01 * smoothstep(0.86, 0.97, world_n.y) * (1.0 - forest_fast * 0.85);
        fast_albedo *= mix(1.0, 0.58, wet01 * (1.0 - forest_fast * 0.45));

        float baked_shadow_fast = mix(0.46, 1.0, surf.g) * canopy_vis_fast;
        float wrapped_fast = clamp((dot(normal_fast, sdir) + mix(0.08, 0.25, veg_fast)) /
                                   (1.0 + mix(0.08, 0.25, veg_fast)), 0.0, 1.0);
        float day_fast = clamp((sdir.y + 0.035) / 0.14, 0.0, 1.0);
        vec3 sun_fast = (sdir.y >= -0.025)
            ? mix(vec3(1.0, 0.62, 0.30), vec3(1.0, 0.96, 0.88), clamp(sdir.y / 0.45, 0.0, 1.0)) * mix(0.55, 2.38, day_fast)
            : vec3(0.14, 0.20, 0.34) * 0.14;
        vec3 sky_amb_fast = mix(vec3(0.014, 0.020, 0.044), mix(vec3(0.32, 0.37, 0.44), u_haze_cool * 0.80, 0.50), day_fast) * mix(0.36, 0.54, u_cloud_cov);
        vec3 gnd_amb_fast = mix(vec3(0.006, 0.007, 0.011), vec3(0.16, 0.14, 0.10), day_fast);
        vec3 ambient_fast = mix(gnd_amb_fast, sky_amb_fast, normal_fast.y * 0.5 + 0.5);
        float ao_fast = mix(cavity_ao, 1.0, water_mask_fast);
        vec3 fast_lit = fast_albedo * (ambient_fast * ao_fast + sun_fast * (wrapped_fast * baked_shadow_fast * cloud_shadow_fast * ao_fast));
        // Wet rain-slicked runway/road/ground Fresnel sky reflection & puddle sheen!
        if (puddle_mask > 0.02) {
            float ndv_p = clamp(dot(normal_fast, -view_dir), 0.0, 1.0);
            float fres_p = 0.03 + 0.55 * pow(1.0 - ndv_p, 4.0);
            float p_patch = smoothstep(0.34, 0.72, textureLod(u_cloud_noise, v_world_en * 0.045, 0.0).r);
            float wet_spec = pow(max(0.0, dot(reflect(view_dir, normal_fast), sdir)), 38.0) * sun_vis_fast * cloud_shadow_fast;
            vec3 sky_r = mix(u_haze_cool * 0.65, vec3(0.70, 0.78, 0.88), 0.35) * max(0.15, day_fast);
            fast_lit = mix(fast_lit, sky_r, fres_p * puddle_mask * (0.28 + 0.42 * p_patch)) + sun_fast * (wet_spec * puddle_mask * 0.25);
        }
        // Synchronized Poisson storm lightning illumination across terrain & valley walls!
        if (u_wx_env.z > 0.02) {
            fast_lit += fast_albedo * vec3(0.68, 0.80, 1.15) * (u_wx_env.z * (0.35 + 0.65 * max(0.0, normal_fast.y)));
        }
        float sss_fast = pow(max(0.0, dot(-normal_fast, sdir)), 1.8) * veg_fast * day_fast;
        fast_lit += fast_albedo * vec3(0.22, 0.34, 0.14) * (sss_fast * 0.34);

        if (water_mask_fast > 0.01) {
            float ndv_w = clamp(dot(world_n, -view_dir), 0.0, 1.0);
            float fres_w = 0.04 + 0.48 * pow(1.0 - ndv_w, 3.5);
            vec3 water_deep = mix(vec3(0.020, 0.048, 0.030), vec3(0.022, 0.070, 0.068), clamp((v_elev_m - 360.0) / 480.0, 0.0, 1.0));
            vec3 water_fast_col = mix(water_deep, u_haze_cool * 0.62, fres_w * (0.24 + 0.32 * day_fast));
            // Keep shallow braided riverbars natural using PNOA ortho blended into deep water channels.
            float deep_w = smoothstep(0.42, 0.84, water_raw_fast) * (0.55 + 0.45 * aqua_fast);
            float glint_fast = pow(max(0.0, dot(reflect(view_dir, world_n), sdir)), 52.0) * (1.0 - u_cloud_cov * 0.50);
            fast_lit = mix(fast_lit, water_fast_col + u_haze_warm * glint_fast * 0.34, water_mask_fast * clamp(deep_w + 0.38, 0.0, 1.0));
        }

        float night_fast = clamp((0.06 - sdir.y) / 0.12, 0.0, 1.0);
        fast_lit += vec3(1.0, 0.48, 0.14) * smoothstep(0.08, 0.82, rt_night) * night_fast * 0.70;

        // Elevation-aware 2-layer exponential height fog (preserves clear downward/mid-range PNOA color!)
        float dy_f = v_elev_m - u_cam_alt_m;
        float h_cam_vf = max(0.0, u_cam_alt_m - 320.0);
        float d_ov_hvf = dy_f / 290.0;
        float int_vf = exp(-h_cam_vf / 290.0) * ((abs(d_ov_hvf) < 0.02) ? (1.0 - 0.5 * d_ov_hvf) : ((1.0 - exp(-d_ov_hvf)) / d_ov_hvf));
        float h_cam_af = max(0.0, u_cam_alt_m);
        float d_ov_haf = dy_f / 2800.0;
        float int_af = exp(-h_cam_af / 2800.0) * ((abs(d_ov_haf) < 0.02) ? (1.0 - 0.5 * d_ov_haf) : ((1.0 - exp(-d_ov_haf)) / d_ov_haf));
        float rho_f = max(u_haze_density, 0.000012);
        float tau_f = v_dist * rho_f * (0.58 * clamp(int_vf, 0.0, 1.45) + 0.55 * clamp(int_af, 0.0, 1.35));
        float fog_fast = clamp(1.0 - exp(-tau_f), 0.0, 0.76);
        float cos_th_f = max(0.0, dot(view_dir, sdir));
        float mie_f = clamp(pow(cos_th_f, 4.0) * 0.48 * sun_vis_fast, 0.0, 0.65);
        vec3 haze_f = mix(u_haze_cool * 0.88, u_haze_warm * 1.16, mie_f);
        fast_lit = mix(fast_lit, haze_f, fog_fast * 0.84);
        frag_color = vec4(fast_lit, 1.0);
        return;
    }

    // 2. Linear-space Regional Land-Cover Classification
    float ortho_luma = max(0.015, dot(ortho, vec3(0.2126, 0.7152, 0.0722)));
    float ortho_max = max(ortho.r, max(ortho.g, ortho.b));
    float ortho_min = min(ortho.r, min(ortho.g, ortho.b));
    float ortho_sat = (ortho_max - ortho_min) / max(0.04, ortho_max);

    float ridge_crest = smoothstep(0.72, 0.90, nao.b) * smoothstep(0.26, 0.44, slope);
    float alpine_rock_zone = smoothstep(1680.0, 2120.0, v_elev_m) * smoothstep(0.16, 0.32, slope);
    float w_cliff = clamp(max(smoothstep(0.30, 0.50, slope), max(ridge_crest * 0.65, alpine_rock_zone * 0.88)), 0.0, 1.0);
    float w_shingle = smoothstep(0.20, 0.42, ortho_luma) * (1.0 - smoothstep(0.14, 0.30, ortho_sat)) * (1.0 - smoothstep(0.02, 0.09, slope)) * 0.70;
    float w_rock = clamp(max(w_cliff, w_shingle), 0.0, 1.0);

    float w_forest = smoothstep(0.12, 0.48, surf.r) * (1.0 - w_rock);
    float green_dom = clamp((ortho.g - ortho.r * 0.80) * 14.0 + (ortho.g - ortho.b) * 3.5, 0.0, 1.0);
    float rem = max(0.0, 1.0 - w_rock - w_forest);
    float w_meadow = rem * green_dom;
    float w_farm = rem * (1.0 - green_dom);

    float n_fine  = (v_dist < 3600.0) ? texture(u_micro_detail, v_world_en * 0.012).r : n_meso;
    float n_ultra = (v_dist < 1400.0) ? texture(u_micro_detail, v_world_en * 0.065).g : n_fine;

    // 3. Early IGN Hydrography water classification
    float aqua_hint = smoothstep(0.01, 0.08, ortho.g - ortho.r) * smoothstep(0.74, 0.98, ortho.b / max(0.015, ortho.r));
    float water_slope_gate = smoothstep(0.84, 0.94, world_n.y);
    float shore_jitter = (n_fine - 0.5) * 0.10 + (n_ultra - 0.5) * 0.05;
    float water_raw = max(surf.b * water_slope_gate, smoothstep(0.02, 0.22, surf.b) * aqua_hint * water_slope_gate * 0.95);
    float wet_bank = smoothstep(0.18, 0.38, water_raw + shore_jitter);
    float water_mask = smoothstep(0.34, 0.50, water_raw + shore_jitter);

    // One height-field parallax lookup approximates the short contact shadow
    // cast by a nearby crown. The lookup runs only on close, sunlit ground;
    // it uses the baked LiDAR canopy heights instead of a live shadow map.
    float canopy_parallax_vis = 1.0;
    if (v_dist < 2200.0 && sdir.y > 0.10 && water_mask < 0.15 && surf.r < 0.12) {
        float parallax_m = clamp(6.0 / max(sdir.y, 0.14), 3.0, 42.0);
        float parent_page = (tex_morph >= 0.5) ? 2.0 : 1.0;
        vec2 sun_en = vec2(sdir.x, -sdir.z);
        vec2 parallax_uv = sun_en * (parallax_m / max(v_node_size * parent_page, 1.0)) * (128.0 / 129.0);
        vec2 sample_uv = (tex_morph >= 0.5) ? (aux_p_uv + parallax_uv) : (aux_c_uv + parallax_uv);
        float in_page = step(0.004, sample_uv.x) * step(sample_uv.x, 0.996) *
                        step(0.004, sample_uv.y) * step(sample_uv.y, 0.996);
        vec4 blocker;
        if (tex_morph >= 0.5)
            blocker = textureGrad(u_surf_arr, vec3(clamp(sample_uv, 0.004, 0.996), v_parent_slice), aux_p_dx, aux_p_dy);
        else
            blocker = textureGrad(u_surf_arr, vec3(clamp(sample_uv, 0.004, 0.996), v_page_slice), aux_c_dx, aux_c_dy);
        float crown_height_m = blocker.r * 25.0;
        float crown_cover = smoothstep(0.025, 0.11, blocker.r) * (1.0 - smoothstep(0.08, 0.35, blocker.a));
        float projected_contact = smoothstep(3.8, 7.2, crown_height_m);
        float sun_visibility = clamp((sdir.y + 0.08) / 0.24, 0.0, 1.0);
        float shadow_strength = 0.25 * sun_visibility * (1.0 - u_cloud_cov * 0.55);
        canopy_parallax_vis = 1.0 - in_page * crown_cover * projected_contact * shadow_strength;
    }

    // 4. 16-Pattern Real Northern Spain / Aragon Distance-Locked Texture2DArray Cascade (0m .. 64km!)
    // Native GL_TEXTURE_2D_ARRAY hardware mipmaps + GL_REPEAT -> ZERO manual atlas margins or seams!
    float sat_fade     = clamp(1.0 - v_dist / 64000.0, 0.18, 1.0) * u_detail_strength;
    float mid_fade     = clamp(1.0 - v_dist / 3600.0,  0.0,  1.0) * u_detail_strength;
    float ultra_fade   = clamp(1.0 - v_dist / 1200.0,  0.0,  1.0) * u_detail_strength;
    float far_nrm_fade = clamp(1.0 - v_dist / 58000.0, 0.16, 1.0) * u_detail_strength;

    vec3 base_col = ortho;
    vec3 virt_nrm = vec3(0.0, 0.0, 1.0);
    float detail_ratio = 1.0;
    vec3 crag_n_add = vec3(0.0);
    float snow_mask = 0.0;
    float pat_scale_active = 200.0;
    vec2 uv_warp = vec2(n_macro - 0.5, n_meso - 0.5) * 0.09;

    {
        int slot_a = 0, slot_b = 1;
        float mix_ab = 0.0;
        float pat_scale = 230.0;

        bool near_water = (surf.b > 0.015) || (w_shingle > 0.35);
        bool is_urban   = (slope < 0.055) && (w_forest < 0.12) && (ortho_sat < 0.155) && (ortho_luma > 0.08) && (ortho_luma < 0.42);
        bool is_badland = (v_elev_m < 680.0) && (slope > 0.11) && (ortho_luma > 0.22) && (ortho_sat < 0.28);
        bool is_talus   = (v_elev_m > 750.0) && (nao.b < 0.44) && (slope > 0.16) && (w_forest < 0.20);

        if (near_water) {
            slot_a = 8; slot_b = (v_elev_m > 620.0) ? 7 : 2; mix_ab = clamp(1.0 - surf.b * 4.5, 0.0, 0.55); pat_scale = 175.0;
        } else if (v_elev_m > 2020.0 && slope < 0.28) {
            slot_a = 14; slot_b = 13; mix_ab = smoothstep(0.14, 0.28, slope); pat_scale = 220.0;
        } else if (v_elev_m > 1950.0 && slope >= 0.22) {
            slot_a = 13; slot_b = 12; mix_ab = (1.0 - smoothstep(0.34, 0.48, nao.b)) * 0.65; pat_scale = 240.0;
        } else if (is_talus) {
            slot_a = 12; slot_b = 10; mix_ab = smoothstep(0.24, 0.44, slope); pat_scale = 200.0;
        } else if (is_badland) {
            slot_a = 4; slot_b = 5; mix_ab = smoothstep(0.10, 0.24, ortho_sat) * 0.65; pat_scale = 210.0;
        } else if (w_cliff > 0.28) {
            slot_a = 10; slot_b = (v_elev_m > 1650.0) ? 13 : 12; mix_ab = (1.0 - smoothstep(0.30, 0.46, nao.b)) * 0.55; pat_scale = 225.0;
        } else if (w_forest > 0.20) {
            slot_a = 6; slot_b = 9; mix_ab = smoothstep(820.0, 1180.0, v_elev_m); pat_scale = 190.0;
        } else if (v_elev_m > 1580.0) {
            slot_a = 11; slot_b = 10; mix_ab = smoothstep(0.16, 0.32, slope); pat_scale = 205.0;
        } else if (is_urban) {
            slot_a = 15; slot_b = 0; mix_ab = smoothstep(0.11, 0.155, ortho_sat) * 0.5; pat_scale = 165.0;
        } else if (v_elev_m > 620.0 && (w_meadow >= w_farm || green_dom > 0.42)) {
            slot_a = 7; slot_b = 5; mix_ab = smoothstep(0.08, 0.22, slope); pat_scale = 215.0;
        } else if (v_elev_m <= 620.0 && green_dom > 0.42) {
            slot_a = 2; slot_b = 0; mix_ab = 1.0 - smoothstep(0.42, 0.68, green_dom); pat_scale = 200.0;
        } else if (v_elev_m > 420.0 && slope > 0.065) {
            slot_a = 5; slot_b = (v_elev_m < 850.0) ? 3 : 6; mix_ab = smoothstep(0.06, 0.20, w_forest) * 0.65; pat_scale = 185.0;
        } else if (v_elev_m > 320.0 && v_elev_m < 880.0 && (slope > 0.022 || n_macro > 0.56)) {
            slot_a = 3; slot_b = (ortho.r - ortho.g > 0.042) ? 1 : 0; mix_ab = clamp(0.65 - n_macro * 0.75, 0.0, 0.70); pat_scale = 190.0;
        } else if (ortho.r - ortho.g > 0.042 * max(0.3, ortho_luma * 2.5)) {
            slot_a = 1; slot_b = 0; mix_ab = smoothstep(0.35, 0.75, n_macro) * 0.45; pat_scale = 210.0;
        } else {
            slot_a = 0; slot_b = 3; mix_ab = smoothstep(0.58, 0.85, n_macro) * 0.45; pat_scale = 225.0;
        }

        float oct = clamp(log2(max(220.0, v_dist) * 0.004545), 0.0, 6.0);
        float oct0 = floor(oct);
        float oct_f = fract(oct);
        float scale0 = pat_scale * exp2(oct0);
        float scale1 = scale0 * 2.0;
        pat_scale_active = scale0;

        vec2 uv0 = v_world_en * (1.0 / scale0) + uv_warp * 0.30;
        vec2 uv1 = rot27(v_world_en * (1.0 / scale1)) - uv_warp * 0.45;
        int slot_hi = (mix_ab > 0.25) ? slot_b : slot_a;
        vec3 s0 = textureLod(u_sat_arr, vec3(uv0, float(slot_a)),  0.30 + oct_f * 0.65).rgb;
        vec3 s1 = textureLod(u_sat_arr, vec3(uv1, float(slot_hi)), 0.15 + (1.0 - oct_f) * 0.55).rgb;
        vec3 sat_hp = mix(s0, s1, mix(0.28, 0.72, oct_f));

        // SATELLITE-FIRST PRINCIPLE: Real IGN PNOA & Z17/Z18 ortho is the primary albedo everywhere (0..64km)!
        // u_sat_arr provides ONLY subtle sub-texel grain (+/-7%) — NEVER overwrites or embosses fake autogen blotches!
        float sat_luma = dot(sat_hp, vec3(0.299, 0.587, 0.114));
        float urban_tame = clamp(max(smoothstep(0.02, 0.18, surf.a), is_urban ? 0.90 : 0.0), 0.0, 1.0) * (1.0 - w_rock) * (1.0 - w_forest);
        float sat_gain = mix(0.10, 0.24, smoothstep(600.0, 14000.0, v_dist)) * mix(1.0, 0.12, urban_tame);
        detail_ratio = clamp(1.0 + (sat_luma - 0.50) * (sat_gain * 1.15), 0.88, 1.14);
        vec3 sat_mult = clamp(vec3(1.0) + (sat_hp - vec3(0.50)) * sat_gain, vec3(0.88), vec3(1.12));
        base_col = mix(base_col, base_col * sat_mult, sat_fade * 0.45);

        float dx_n = (s0.r - s1.r) * 0.35 + (sat_hp.r - 0.50) * 0.28;
        float dy_n = (s0.g - s1.g) * 0.35 + (sat_hp.g - 0.50) * 0.28;
        float slope_n_boost = 0.45 + 0.55 * smoothstep(0.08, 0.30, slope);
        virt_nrm = normalize(vec3(dx_n * slope_n_boost, dy_n * slope_n_boost, 1.0));
    }

    // 4b. Subtle, Luminance-Preserving < 1km Close-Up Micro-Detail (lets real Z17/Z18 satellite ortho shine without fake autogen grit!)
    float pbr_rough = 0.84;
    if (v_dist < 1050.0 && water_mask < 0.95) {
        float close_w = clamp(1.0 - v_dist / 1000.0, 0.0, 1.0) * u_detail_strength * (1.0 - water_mask);
        if (close_w > 0.01) {
            float near_w = 1.0 - smoothstep(60.0, 380.0, v_dist);
            float urban_zone = clamp(smoothstep(0.03, 0.18, surf.a), 0.0, 1.0) * (1.0 - w_rock) * (1.0 - w_forest);
            float veg_mix = clamp(w_meadow * 1.15 + w_forest * 0.95 + green_dom * 0.45, 0.0, 1.0) * (1.0 - w_rock) * (1.0 - urban_zone * 0.85);

            vec2 uv_a = v_world_en * 0.22 + uv_warp * 0.04;
            vec2 uv_b = rot27(v_world_en * 0.72);
            bool use_grass = (veg_mix > 0.35 && urban_zone < 0.45);
            vec3 det_alb = use_grass ? texture(u_grass_albedo, uv_a).rgb : texture(u_gravel_albedo, uv_a).rgb;
            vec3 micro_n = (use_grass ? texture(u_grass_normal, uv_a).rgb : texture(u_gravel_normal, uv_a).rgb) * 2.0 - 1.0;

            if (near_w > 0.01) {
                vec3 alb_b = use_grass ? texture(u_grass_albedo, uv_b).rgb : texture(u_gravel_albedo, uv_b).rgb;
                vec3 nrm_b = (use_grass ? texture(u_grass_normal, uv_b).rgb : texture(u_gravel_normal, uv_b).rgb) * 2.0 - 1.0;
                det_alb = mix(det_alb, alb_b, 0.45 * near_w);
                micro_n = normalize(mix(micro_n, nrm_b, 0.45 * near_w));
            }

            // Gentle zero-mean luminance micro-grain (+/-12% max) preserves 100% of real aerial photograph colors & field textures!
            float det_luma = dot(det_alb, vec3(0.30, 0.59, 0.11)) / (use_grass ? 0.195 : 0.205);
            float micro_mod = clamp(1.0 + (det_luma - 1.0) * 0.24, 0.88, 1.12);
            float det_strength = close_w * (0.22 + 0.18 * near_w) * (1.0 - urban_zone * 0.65);
            base_col *= mix(1.0, micro_mod, det_strength);

            virt_nrm.xy += micro_n.xy * (close_w * (0.28 + 0.24 * near_w) * (1.0 - urban_zone * 0.70)) * (1.0 - w_cliff * 0.60);
            pbr_rough = mix(pbr_rough, mix(0.72, 0.86, veg_mix), close_w);
        }
    }

    // Preserve the orthophoto as the main signal while giving meadows, fields,
    // and mixed Pyrenean canopy a low-amplitude annual color cycle.
    vec3 meadow_season_tint = u_season == 0 ? vec3(0.91, 0.94, 0.98) :
                              u_season == 1 ? vec3(0.98, 1.045, 0.94) :
                              u_season == 2 ? vec3(1.045, 0.99, 0.87) : vec3(1.09, 0.91, 0.75);
    vec3 field_season_tint = u_season == 0 ? vec3(0.95, 0.93, 0.90) :
                            u_season == 1 ? vec3(1.00, 1.025, 0.92) :
                            u_season == 2 ? vec3(1.08, 0.96, 0.80) : vec3(1.10, 0.88, 0.70);
    float vegetated_season_weight = (w_meadow * 0.32 + w_farm * 0.18) * (1.0 - water_mask);
    base_col = mix(base_col, base_col * meadow_season_tint, vegetated_season_weight);
    base_col = mix(base_col, base_col * field_season_tint, w_farm * (1.0 - water_mask) * 0.14);
    if (u_season == 3 && w_forest > 0.01) {
        float autumn_patch = smoothstep(0.30, 0.76, n_macro * 0.62 + n_meso * 0.38);
        vec3 autumn_canopy_tint = mix(vec3(1.055, 0.94, 0.82), vec3(1.12, 0.83, 0.62), autumn_patch);
        base_col = mix(base_col, base_col * autumn_canopy_tint, w_forest * 0.22 * (1.0 - water_mask));
    } else if (u_season == 0 && w_forest > 0.01) {
        base_col = mix(base_col, base_col * vec3(0.93, 0.97, 1.02), w_forest * 0.12 * (1.0 - water_mask));
    }

    // 5. Natural Ortho-Anchored Pyrenean Limestone Cliffs (subtle rock strata relief on steep faces, zero fake painted stripes!)
    float crag_dist_w = clamp(1.0 - v_dist / 58000.0, 0.0, 1.0);
    float mtn_rock_w = max(w_cliff, smoothstep(0.18, 0.38, slope)) * crag_dist_w;
    if (mtn_rock_w > 0.01) {
        float rib1 = n_meso;
        float rib_dx = 0.0, rib_dz = 0.0;
        if (v_dist < 42000.0) {
            float f_sc = (v_dist > 8000.0) ? 0.0085 : 0.016;
            vec2 fuv = vec2(v_world_en.x * f_sc + v_world_en.y * (f_sc * 0.65), v_elev_m * (f_sc * 0.85) + n_macro * 1.8);
            rib1 = vnoise(fuv);
            rib_dx = (vnoise(fuv + vec2(0.16, 0.0)) - rib1) * 2.8;
            rib_dz = (vnoise(fuv + vec2(0.0, 0.16)) - rib1) * 2.2;
        }
        crag_n_add = vec3(rib_dx, 0.0, rib_dz) * mtn_rock_w * 0.65;

        if (w_cliff > 0.06) {
            float cliff_lod = clamp(log2(max(300.0, v_dist) * 0.0033), 0.0, 3.2);
            float cliff_sc = exp2(floor(cliff_lod * 0.65));
            vec2 cwarp = vec2(n_macro - 0.5, n_meso - 0.5) * 0.16;
            vec2 cliff_uv = vec2((v_world_en.x * 0.78 + v_world_en.y * 0.62) * (0.016 / cliff_sc), v_elev_m * (0.024 / cliff_sc)) + cwarp;
            vec3 cliff_tex = textureLod(u_rock_albedo, cliff_uv, fract(cliff_lod * 0.65)).rgb;
            vec3 cliff_nrm = textureLod(u_rock_normal, cliff_uv, fract(cliff_lod * 0.65)).rgb * 2.0 - 1.0;
            pbr_rough = mix(pbr_rough, 0.64, w_cliff);
            float cliff_luma = clamp(dot(cliff_tex, vec3(0.2126, 0.7152, 0.0722)) / 0.161, 0.72, 1.32);

            float strata_warm = smoothstep(0.28, 0.74, vnoise(vec2(v_elev_m * 0.018 + n_macro * 1.2, v_world_en.x * 0.001)));
            vec3 weathered_rock = mix(vec3(0.41, 0.42, 0.43), vec3(0.50, 0.42, 0.31), strata_warm * 0.45) * cliff_luma;
            crag_n_add += vec3(cliff_nrm.x, 0.0, -cliff_nrm.y) * (w_cliff * crag_dist_w * 0.85);

            // Blend gently with real IGN PNOA rock orthophoto so cliffs never look artificially painted!
            vec3 harmonized_rock = mix(ortho * cliff_luma, weathered_rock, 0.28 * smoothstep(0.26, 0.52, slope));
            base_col = mix(base_col, harmonized_rock, w_cliff * crag_dist_w * 0.55 * smoothstep(0.22, 0.46, slope));
        }
        base_col *= mix(1.0, clamp(0.92 + (rib1 - 0.5) * 0.22, 0.82, 1.14), mtn_rock_w * 0.45);
    }

    // 6. Subtle Far Forest Canopy Bump beyond Far-LOD 3D Impostors (3.5..58km) + Horizon Village Hints beyond 18km!
    float far_mtn_fade = clamp(1.0 - v_dist / 58000.0, 0.0, 1.0);
    float dist_mtn_w = smoothstep(2100.0, 4600.0, v_dist) * far_mtn_fade;
    float rock_macro = n_macro * 0.26 + n_meso * 0.08;
    if (dist_mtn_w > 0.01) {
        float treeline = clamp((2240.0 - v_elev_m) / 280.0, 0.0, 1.0);
        float forest_mass = clamp(w_forest * 1.45, 0.0, 1.0) * treeline * (1.0 - w_rock * 0.88);
        if (forest_mass > 0.03) {
            float sun_tilt = clamp(dot(virt_nrm.xy, vec2(sdir.x, -sdir.z)) * 0.85, -0.35, 0.45);
            float crown_h = clamp((detail_ratio - 0.78) * 1.85 + (n_meso - 0.5) * 0.30, 0.0, 1.0);
            float crown_sh = mix(0.76, 1.18, smoothstep(0.18, 0.82, crown_h)) * (1.0 + sun_tilt * 0.28);
            // Modulate REAL IGN PNOA forest satellite imagery gently (NEVER paint solid green over it!)
            base_col = mix(base_col, base_col * crown_sh, forest_mass * dist_mtn_w * 0.68);
            virt_nrm.xy += vec2((crown_h - 0.5) * 1.8, (n_meso - 0.5) * 1.4) * (forest_mass * dist_mtn_w);
        }
    }

    // Real 3D GPU LOD1 Buildings now render all the way to 22km -> PMRF only gently blends horizon towns > 17km!
    float far_bldg_w = smoothstep(17000.0, 21500.0, v_dist) * smoothstep(0.08, 0.30, surf.a) * (1.0 - water_mask);
    if (far_bldg_w > 0.01) {
        float b_ang = floor(n_macro * 6.0) * 0.5236;
        float ca = cos(b_ang), sb = sin(b_ang);
        mat2 b_rot = mat2(vec2(ca, -sb), vec2(sb, ca));
        float cell_m_b = mix(15.0, 48.0, clamp((v_dist - 2000.0) / 36000.0, 0.0, 1.0));
        float b_h = 8.5 * clamp(surf.a * 1.35, 0.35, 1.15);
        vec2 par_b = clamp(-view_dir.xz / max(0.32, abs(view_dir.y)), vec2(-1.4), vec2(1.4)) * b_h;
        vec2 buv_r = (b_rot * (v_world_en + par_b)) / cell_m_b;
        vec2 buv_b = (b_rot * v_world_en) / cell_m_b;
        vec2 buv_s = (b_rot * (v_world_en + vec2(sdir.x, -sdir.z) * (b_h * 1.35))) / cell_m_b;
        vec2 fr = fract(buv_r), fb = fract(buv_b), fs = fract(buv_s);
        float id_h = hash21(floor(buv_r));
        float in_r = step(0.14, fr.x) * step(fr.x, 0.86) * step(0.16, fr.y) * step(fr.y, 0.84) * step(0.14, id_h);
        float in_b = step(0.14, fb.x) * step(fb.x, 0.86) * step(0.16, fb.y) * step(fb.y, 0.84);
        float in_s = step(0.14, fs.x) * step(fs.x, 0.86) * step(0.16, fs.y) * step(fs.y, 0.84);
        base_col *= (1.0 - in_s * (1.0 - in_r) * 0.52 * far_bldg_w);
        float is_w = in_b * (1.0 - in_r);
        if (is_w > 0.5) {
            vec3 wcol = mix(vec3(0.46, 0.40, 0.31), vec3(0.32, 0.31, 0.29), smoothstep(750.0, 1150.0, v_elev_m));
            base_col = mix(base_col, wcol * 0.85, far_bldg_w * 0.85);
        }
        if (in_r > 0.5) {
            float ps = (fr.y < 0.5) ? 1.0 : -1.0;
            float rlit = clamp(0.42 + 0.92 * max(0.0, dot(normalize(vec3(0.0, 0.88, ps * 0.47)), sdir)), 0.30, 1.35);
            vec3 rbase = mix(mix(vec3(0.38, 0.19, 0.12), vec3(0.28, 0.16, 0.11), fract(id_h * 7.3)), vec3(0.14, 0.16, 0.19), smoothstep(820.0, 1180.0, v_elev_m));
            base_col = mix(base_col, mix(ortho * 1.05, rbase, 0.58) * rlit, far_bldg_w * 0.92);
            virt_nrm.xy += vec2(0.0, ps * 1.4) * far_bldg_w;
        }
        float night_vis = clamp((0.08 - sdir.y) / 0.14, 0.0, 1.0);
        if (night_vis > 0.01 && (in_r > 0.5 || is_w > 0.5)) {
            pmrf_em += vec3(1.15, 0.72, 0.32) * (step(0.45, fract(id_h * 13.7)) * night_vis * far_bldg_w * 0.85);
        }
    }

    float ridge_convex = smoothstep(0.52, 0.80, nao.b);
    float north_chill = smoothstep(-0.35, 0.55, world_n.z) * 185.0;
    float snow_strata = (n_meso - 0.5) * 230.0 + (n_fine - 0.5) * 100.0;
    float snow_height = v_elev_m + north_chill + (rock_macro - 0.17) * 300.0 + snow_strata - ridge_convex * 240.0;
    snow_mask = smoothstep(u_snowline_m - 270.0, u_snowline_m + 170.0, snow_height) *
                (1.0 - smoothstep(0.10, 0.26, slope));
    if (snow_mask > 0.01) {
        vec3 alpine_r = vec3(0.22, 0.23, 0.25) * (0.76 + 0.48 * detail_ratio);
        float sf = clamp(0.84 + (detail_ratio - 1.0) * 0.32, 0.65, 1.24);
        vec3 s_col = mix(vec3(0.46, 0.58, 0.76), vec3(0.86, 0.90, 0.95), clamp(0.42 + 0.75 * max(0.0, dot(world_n, sdir)), 0.0, 1.0)) * sf;
        base_col = mix(mix(base_col, alpine_r, smoothstep(0.08, 0.22, slope) * 0.72), s_col, snow_mask * 0.92);
    }

    float dry_aqua = aqua_hint * (1.0 - water_mask) * (1.0 - smoothstep(535.0, 565.0, v_elev_m)) * water_slope_gate;
    if (dry_aqua > 0.01) {
        base_col = mix(base_col, vec3(0.30, 0.29, 0.26) * (0.85 + 0.30 * n_fine), dry_aqua * 0.80);
    }

    // Keep the photographed crown colours and supply near/far 3D relief from LiDAR gradients & crown domes.
    float forest_floor = smoothstep(0.05, 0.18, surf.r) *
                         (1.0 - smoothstep(0.18, 0.42, surf.b));
    base_col = mix(base_col, ortho, forest_floor * (1.0 - snow_mask) * 0.85);

    float nrm_str = clamp(far_nrm_fade * 1.25 + mid_fade * 0.65 + ultra_fade * 0.55, 0.0, 2.05) * (1.0 - water_mask);
    vec3 final_n = normalize(vec3(
        world_n.x + virt_nrm.x * nrm_str + crag_n_add.x,
        world_n.y,
        world_n.z - virt_nrm.y * nrm_str + crag_n_add.z
    ));
    ground_relief(base_col, final_n, green_dom * (1.0 - forest_floor), water_mask, view_dir);
    vec2 aux_c_u = (tex_morph >= 0.5) ? aux_p_uv : aux_c_uv;
    float slice_u = (tex_morph >= 0.5) ? v_parent_slice : v_page_slice;
    apply_canopy_volume(base_col, final_n, forest_floor * (1.0 - snow_mask), surf.r, aux_c_u, slice_u, view_dir, sdir);

    // 7. Pebble Shingle Riverbanks & Glacial Turquoise Embalse de Mediano Water (ZERO ring seams!)
    vec3 water_glint = vec3(0.0);
    if (wet_bank > 0.01) {
        vec3 peb = texture(u_rock_albedo, v_world_en * 0.38).rgb;
        vec3 sh_c = mix(vec3(0.38, 0.37, 0.34), vec3(0.62, 0.60, 0.55), clamp(peb.r * 3.0, 0.0, 1.0));
        sh_c *= mix(1.0, 0.68, smoothstep(0.28, 0.42, water_raw + shore_jitter));
        base_col = mix(base_col, sh_c, wet_bank * (1.0 - water_mask) * 0.86);
    }
    if (water_mask > 0.01) {
        // Wind-driven water ripples & wave chop scaled directly by RegionalAtmosphere wind speed!
        float wspd = length(u_wind_en);
        vec2 wdir = (wspd > 0.25) ? (u_wind_en / wspd) : vec2(0.82, 0.57);
        float wvel = 0.25 + clamp(wspd * 0.055, 0.0, 0.95);
        float chop = mix(0.085, 0.26, clamp(wspd / 16.0, 0.0, 1.0));
        vec3 wn1 = texture(u_rock_normal, v_world_en * 0.14 - wdir * (u_time * wvel)).rgb * 2.0 - 1.0;
        vec3 wn2 = texture(u_rock_normal, rot27(v_world_en * 0.30) - rot27(wdir) * (u_time * wvel * 0.72)).rgb * 2.0 - 1.0;
        vec3 wave_n = normalize(vec3((wn1.x + wn2.x) * chop, 1.0, (wn1.y + wn2.y) * chop));
        final_n = normalize(mix(final_n, wave_n, water_mask));

        float ndv = clamp(dot(final_n, -view_dir), 0.0, 1.0);
        float fresnel = 0.03 + 0.56 * pow(1.0 - ndv, 3.6);
        float shallow_t = 1.0 - smoothstep(0.45, 0.88, water_raw);
        // True linear Iberian water reflectance: deep olive-emerald Ebro lowlands -> deep glacial alpine teal in Pyrenees!
        vec3 deep_low  = vec3(0.020, 0.048, 0.028);
        vec3 deep_pyre = vec3(0.014, 0.072, 0.076);
        vec3 shal_col  = mix(vec3(0.038, 0.068, 0.042), vec3(0.032, 0.118, 0.105), clamp((v_elev_m - 360.0) / 480.0, 0.0, 1.0));
        vec3 body = mix(
            mix(deep_low, deep_pyre, clamp((v_elev_m - 360.0) / 480.0, 0.0, 1.0)),
            shal_col, shallow_t * 0.65
        );
        vec3 refl = reflect(view_dir, final_n);
        vec2 ruv = vec2(atan(refl.z, refl.x) * (0.5 / 3.14159265) + 0.5, asin(clamp(refl.y, -1.0, 1.0)) * (1.0 / 3.14159265) + 0.5);
        vec3 sky_refl = textureLod(u_refl_env, ruv, 0.8 + clamp(wspd * 0.08, 0.0, 1.2)).rgb;
        vec2 rcld_uv = (v_world_en + refl.xz * (1800.0 / max(0.18, abs(refl.y))) + vec2(u_time * 22.0, u_time * 8.0)) * 0.000118;
        float rcld = smoothstep(0.62 - u_cloud_cov * 0.36, 0.82 - u_cloud_cov * 0.36, texture(u_cloud_noise, rcld_uv).r);
        sky_refl = mix(sky_refl, mix(u_haze_warm, vec3(0.95, 0.94, 0.92), 0.55), rcld * 0.32);
        float mtn_occ = clamp((1.0 - surf.g) * 0.78 + (1.0 - smoothstep(0.45, 0.92, water_raw)) * 0.42, 0.0, 0.85);
        sky_refl = mix(sky_refl, mix(vec3(0.015, 0.042, 0.038), u_haze_cool * 0.28, 0.35), mtn_occ * 0.68);

        vec3 wcol = mix(body, sky_refl, fresnel * 0.64);
        float sun_spec = pow(max(0.0, dot(refl, sdir)), 78.0) * (1.0 - u_cloud_cov * 0.45) * (1.0 - mtn_occ * 0.75);
        water_glint = u_haze_warm * sun_spec * 1.25 * water_mask;
        base_col = mix(base_col, wcol, water_mask);
    }

    // 8. Physically-Based Cook-Torrance GGX PBR + Pre-Baked 50km Horizon Shadows + Sky/Ground IBL!
    // Lively, crisp IGN PNOA satellite color fidelity with punchy sun-vs-shadow relief!
    float bc_luma = dot(base_col, vec3(0.2126, 0.7152, 0.0722));
    vec3 sat_bc = max(vec3(0.0), mix(vec3(bc_luma), base_col, 1.20)) * 0.96;
    base_col = mix(sat_bc, base_col, water_mask);
    float sun_vis = clamp((sdir.y + 0.08) / 0.24, 0.0, 1.0);
    float day_t = clamp((sdir.y + 0.035) / 0.14, 0.0, 1.0);
    float mtn_shadow = 1.0 - (1.0 - surf.g) * 0.74 * clamp(1.15 - u_cloud_cov * 0.48, 0.42, 1.0);
    // Moving cloud-shadow LUT: one lookup from the same procedural weather field
    // used by the volumetric cloud pass, advected by the local wind.
    vec2 shadow_light_en = vec2(sdir.x, -sdir.z) * (1350.0 / max(sdir.y, 0.22));
    vec2 cloud_flow_en = vec2(u_time * 22.0, u_time * 8.0) - u_wind_en * (u_time * 1.45);
    vec2 shadow_uv = (v_world_en + shadow_light_en + cloud_flow_en) * 0.000112 + vec2(0.21, 0.47);
    float shadow_cloud_noise = textureLod(u_cloud_noise, shadow_uv, 0.0).r;
    float shadow_threshold = mix(0.71, 0.27, clamp(u_cloud_cov, 0.06, 0.96));
    float cloud_umbra = smoothstep(shadow_threshold - 0.02, shadow_threshold + 0.15, shadow_cloud_noise);
    float cld_sh = 1.0 - u_cloud_cov * 0.035 - cloud_umbra * 0.40 * sun_vis;

    // Physically-calibrated material roughness across snow, wet shingle, limestone rock, soil & vegetation!
    pbr_rough = mix(pbr_rough, 0.22, wet_bank * (1.0 - water_mask));
    pbr_rough = mix(pbr_rough, 0.36, snow_mask);
    pbr_rough = mix(pbr_rough, 0.06, water_mask);

    vec3 light_dir = (sdir.y >= -0.025) ? sdir : normalize(vec3(-sdir.x, max(0.45, -sdir.y * 2.5), -sdir.z));
    vec3 sun_col = (sdir.y >= -0.025)
        ? mix(vec3(1.0, 0.62, 0.30), vec3(1.0, 0.95, 0.86), clamp(sdir.y / 0.45, 0.0, 1.0)) * mix(0.55, 2.46, day_t)
        : vec3(0.14, 0.20, 0.34) * 0.14;
    float veg_cov = clamp(w_meadow + w_forest, 0.0, 1.0) * (1.0 - w_rock);
    float wrap = mix(0.08, 0.26, veg_cov);
    float ndl = clamp((dot(final_n, light_dir) + wrap) / (1.0 + wrap), 0.0, 1.0);
    float ndl_raw = max(0.0, dot(final_n, light_dir));
    float ndv_l = max(0.001, dot(final_n, -view_dir));
    vec3 H_l = normalize(-view_dir + light_dir);
    float ndh_l = max(0.0, dot(final_n, H_l));

    // Cook-Torrance GGX direct specular (wet river shingle glint, rock mineral specular, snow micro-sparkle!)
    float a_r = max(0.04, pbr_rough * pbr_rough);
    float a2_r = a_r * a_r;
    float dn_r = ndh_l * ndh_l * (a2_r - 1.0) + 1.0;
    float D_r = a2_r / (3.14159265 * dn_r * dn_r + 1e-5);
    float k_r = (pbr_rough + 1.0) * (pbr_rough + 1.0) * 0.125;
    float G_r = (ndv_l / (ndv_l * (1.0 - k_r) + k_r)) * (max(0.001, ndl_raw) / (max(0.001, ndl_raw) * (1.0 - k_r) + k_r));
    float F_r = 0.035 + 0.965 * pow(1.0 - max(0.0, dot(H_l, -view_dir)), 5.0);
    float ggx_s = clamp((D_r * G_r * F_r) / (4.0 * ndv_l * max(0.001, ndl_raw) + 1e-4), 0.0, 2.8) * (1.0 - water_mask);

    vec3 sky_amb = mix(vec3(0.012, 0.018, 0.042), mix(vec3(0.30, 0.35, 0.42), u_haze_cool * 0.78, 0.52), day_t) * mix(0.34, 0.52, u_cloud_cov);
    vec3 gnd_amb = mix(vec3(0.005, 0.006, 0.010), vec3(0.14, 0.12, 0.09), day_t);
    vec3 amb_col = mix(gnd_amb, sky_amb, final_n.y * 0.5 + 0.5);

    float sh_tot = mtn_shadow * cld_sh * canopy_parallax_vis;
    float ao_eff = mix(cavity_ao, 1.0, water_mask);

    vec3 lit = base_col * (amb_col * ao_eff + sun_col * (ndl * sh_tot * ao_eff)) +
               sun_col * (ggx_s * ndl_raw * sh_tot * 0.65);
    // Grazing vegetation velvet scatter + pre-baked IBL reflection sheen on wet rocks, snow & urban streets (skip on matte grass/dirt!)
    lit += base_col * vec3(0.24, 0.34, 0.16) * (pow(1.0 - ndv_l, 2.8) * veg_cov * (1.0 - water_mask) * day_t * 0.28);
    if (pbr_rough < 0.78 && water_mask < 0.99) {
        vec3 l_refl = reflect(view_dir, final_n);
        vec2 l_ruv = vec2(atan(l_refl.z, l_refl.x) * (0.5 / 3.14159265) + 0.5, asin(clamp(l_refl.y, -1.0, 1.0)) * (1.0 / 3.14159265) + 0.5);
        vec3 land_env = textureLod(u_refl_env, l_ruv, pbr_rough * 5.2).rgb;
        lit += land_env * ((1.0 - pbr_rough) * pow(1.0 - ndv_l, 3.5) * (1.0 - water_mask) * 0.26 * mix(0.25, 1.0, day_t));
    }

    // 3-Layer (0m Ground + 5m & 10m Elevated Volumetric Canyon Layers) Pre-Baked 2.5D DSM Ray-Traced Night Lights!
    float night_t = clamp((0.06 - sdir.y) / 0.12, 0.0, 1.0);
    if (night_t > 0.01 && rt_night > 0.030 && water_mask < 0.85) {
        // Layer 0 (0m ground): sharp streetlamp pools; Layers 1 & 2 (5m + 10m): soft volumetric air glow above streets!
        float nl_0m  = pow(smoothstep(0.08, 0.98, rt_night), 1.85);
        float nl_vol = pow(smoothstep(0.03, 0.92, rt_night), 1.20);
        vec2 lamp_cell = fract(v_world_en * (1.0 / 12.0));
        float d_pt = length(lamp_cell - vec2(0.5));
        float lamp_spot = exp(-d_pt * d_pt * 14.0);
        float pinpoint = mix(0.42 + 0.98 * lamp_spot, 0.88, smoothstep(900.0, 3400.0, v_dist));
        float pool_i = nl_0m * pinpoint * (1.0 - water_mask);
        vec3 lamp_c = mix(vec3(1.00, 0.52, 0.14), vec3(1.00, 0.84, 0.48), clamp(pool_i * 1.10, 0.0, 1.0));
        float hotspot = pow(max(0.0, nl_0m * lamp_spot), 2.2) * 0.55;
        vec3 vol_bloom = mix(vec3(1.00, 0.50, 0.12), vec3(1.00, 0.78, 0.38), nl_vol) * (nl_vol * 0.22 * (1.0 - water_mask));
        pmrf_em += ((base_col * (3.85 * pool_i) + lamp_c * (0.10 * pool_i + hotspot)) * lamp_c + vol_bloom) * night_t;
    }

    // 9. True 3-Layer Analytic Exponential Height Fog + Orographic Peak Mist + Natural Peak Light-Bending!
    //    Layer A: Low-altitude Pyrenean Valley & River Mist (H_v = 290m -> pools luminous mist in valleys & basins!)
    //    Layer B: Tropospheric Regional Aerial Perspective (H_a = 2800m -> crisp multi-range mountain depth separation!)
    //    Layer C: Orographic Mountain Peak & Saddle Fog (1150m..2950m) + Natural Light Bending Around Ridges!
    float dy = v_elev_m - u_cam_alt_m;
    float H_v = 290.0;
    float h_cam_v = max(0.0, u_cam_alt_m - 320.0);
    float d_ov_hv = dy / H_v;
    float int_v = exp(-h_cam_v / H_v) * ((abs(d_ov_hv) < 0.02) ? (1.0 - 0.5 * d_ov_hv) : ((1.0 - exp(-d_ov_hv)) / d_ov_hv));

    float H_a = 2800.0;
    float h_cam_a = max(0.0, u_cam_alt_m);
    float d_ov_ha = dy / H_a;
    float int_a = exp(-h_cam_a / H_a) * ((abs(d_ov_ha) < 0.02) ? (1.0 - 0.5 * d_ov_ha) : ((1.0 - exp(-d_ov_ha)) / d_ov_ha));

    float basin_hum = 1.0 + 0.45 * smoothstep(0.02, 0.35, surf.b) + 0.35 * smoothstep(0.0, 0.55, 1.0 - nao.b);
    float rho = max(u_haze_density, 0.000018);
    float tau_valley = v_dist * (rho * 1.45) * clamp(int_v, 0.0, 1.85) * basin_hum;
    float tau_atmo   = v_dist * (rho * 0.92) * clamp(int_a, 0.0, 1.50);

    // Layer C: Drifting translucent orographic mist hugging Pyrenean peaks, saddles & upper flanks!
    float peak_elev = smoothstep(1150.0, 2150.0, v_elev_m);
    float peak_dist = smoothstep(850.0, 4800.0, v_dist);
    float peak_wisp = 0.58 + 0.84 * textureLod(u_cloud_noise, (v_world_en + vec2(u_time * 12.0, -u_time * 5.0)) * 0.00014, 0.0).r;
    float crest_wrap = smoothstep(0.42, 0.88, nao.b) * smoothstep(0.10, 0.40, slope);
    float peak_band = peak_elev * peak_dist * mix(0.68, 1.38, crest_wrap) * peak_wisp;
    float tau_peak = v_dist * (rho * 1.55) * peak_band * (0.68 + 0.52 * u_cloud_cov);

    float fog_a = clamp(1.0 - exp(-(tau_valley + tau_atmo + tau_peak)), 0.0, 0.90);
    float valley_frac = clamp((tau_valley + tau_peak * 0.72) / max(0.0001, tau_valley + tau_atmo + tau_peak), 0.0, 1.0);

    float cos_th = max(0.0, dot(view_dir, sdir));
    float rayleigh = 0.75 * (1.0 + cos_th * cos_th);
    // Dual-lobe Henyey-Greenstein Mie forward-scattering + Natural Ridge-Wrap Light Bending around mountain peaks!
    float hg_wide  = (1.0 - 0.3844) / (4.0 * 3.14159 * pow(max(0.04, 1.3844 - 1.24 * cos_th), 1.5));
    float hg_tight = (1.0 - 0.7744) / (4.0 * 3.14159 * pow(max(0.02, 1.7744 - 1.76 * cos_th), 1.5));
    float horizon_lift = pow(1.0 - clamp(abs(view_dir.y) * 2.4, 0.0, 1.0), 2.2) * 0.38 * sun_vis;
    float ridge_graz = pow(1.0 - ndv_l, 2.2) * peak_elev * peak_dist;
    float light_bend = (hg_wide * 0.45 + hg_tight * 0.34) * (0.35 + 1.15 * ridge_graz + 0.65 * crest_wrap) * peak_band * sun_vis;
    float sun_shaft = mix(0.46, 1.28, sh_tot);
    float mie_w = clamp((hg_wide * 0.52 + hg_tight * 0.14) * sun_vis * sun_shaft + light_bend + horizon_lift, 0.0, 1.65);

    vec3 ray_col = mix(u_haze_cool * (0.62 + 0.22 * rayleigh), mix(u_haze_cool, vec3(0.78, 0.86, 0.94), 0.42), valley_frac * 0.68);
    vec3 inscatter = mix(ray_col, u_haze_warm * (1.36 * mix(sun_shaft, 1.25, peak_band * 0.55)), clamp(mie_w * 0.82, 0.0, 0.94));
    // Silky luminous diffraction glow bending across backlit & grazing Pyrenean peak silhouettes!
    vec3 peak_rim_bend = u_haze_warm * (light_bend * 0.42 * (1.0 - water_mask));

    vec3 out_col = mix(lit, inscatter, fog_a * 0.90) + peak_rim_bend + (water_glint + pmrf_em) * (1.0 - fog_a * 0.80);
    frag_color = vec4(out_col, 1.0);
}
