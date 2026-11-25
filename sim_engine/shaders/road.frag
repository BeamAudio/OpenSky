#version 460 core

layout(binding = 0) uniform sampler2D u_asphalt_alb;
layout(binding = 1) uniform sampler2D u_asphalt_nrm;
layout(binding = 2) uniform sampler2D u_gravel_alb;
layout(binding = 3) uniform sampler2D u_gravel_nrm;
layout(binding = 4) uniform sampler2D u_refl_env;

uniform vec3 u_sun_dir;
uniform vec3 u_haze_cool;
uniform vec3 u_haze_warm;
uniform float u_haze_density;
uniform float u_cam_alt_m;

in vec3 v_world_pos;
in vec2 v_world_en;
in vec2 v_uv;
in float v_sun_vis;
in float v_sky_ao;
in float v_night_light;
in float v_kind;
in float v_elev_m;
in float v_dist;

layout(location = 0) out vec4 frag_color;

float ggx_spec(vec3 N, vec3 V, vec3 L, float rough, float f0) {
    vec3 H = normalize(V + L);
    float ndh = max(0.0, dot(N, H));
    float ndv = max(0.001, dot(N, V));
    float ndl = max(0.001, dot(N, L));
    float a = max(0.04, rough * rough);
    float a2 = a * a;
    float d = ndh * ndh * (a2 - 1.0) + 1.0;
    float D = a2 / (3.14159265 * d * d + 1e-5);
    float k = (rough + 1.0) * (rough + 1.0) * 0.125;
    float G = (ndv / (ndv * (1.0 - k) + k)) * (ndl / (ndl * (1.0 - k) + k));
    float F = f0 + (1.0 - f0) * pow(1.0 - max(0.0, dot(H, V)), 5.0);
    return clamp((D * G * F) / (4.0 * ndv * ndl + 1e-4), 0.0, 4.0);
}

void main() {
    float dist_fade = 1.0 - smoothstep(2400.0, 3600.0, v_dist);
    if (dist_fade < 0.02) discard;

    vec3 sdir = normalize(u_sun_dir);
    vec3 view_dir = normalize(v_world_pos);
    float day_t = clamp((sdir.y + 0.035) / 0.14, 0.0, 1.0);
    float night_t = clamp((0.08 - sdir.y) / 0.14, 0.0, 1.0);

    float across = abs(v_uv.x - 0.5) * 2.0; // 0=center, 1=outer shoulder/verge
    bool is_highway = (v_kind < 1.5);
    float verge_edge = is_highway ? 0.78 : 0.72;

    // Multi-scale asphalt + organic dusty gravel shoulder/verge PBR textures
    vec2 auv = vec2(v_uv.x * 2.6, v_uv.y * 0.14);
    vec2 guv = v_world_en * 0.24;
    vec3 asph = texture(u_asphalt_alb, auv).rgb * mix(0.36, 0.48, step(1.5, v_kind)) * vec3(1.04, 1.00, 0.95);
    vec3 anrm = texture(u_asphalt_nrm, auv).rgb * 2.0 - 1.0;
    vec3 grav = texture(u_gravel_alb, guv).rgb * vec3(0.92, 0.88, 0.78);
    vec3 gnrm = texture(u_gravel_nrm, guv).rgb * 2.0 - 1.0;

    // Crisp < 320m close-up aggregate & pebble PBR micro-detail (zero flat grey tape!)
    float close_r = 1.0 - smoothstep(80.0, 320.0, v_dist);
    if (close_r > 0.01) {
        vec3 a_hi = texture(u_asphalt_alb, auv * 4.5).rgb;
        vec3 n_hi = texture(u_asphalt_nrm, auv * 4.5).rgb * 2.0 - 1.0;
        vec3 g_hi = texture(u_gravel_alb, guv * 3.4).rgb;
        vec3 gn_hi = texture(u_gravel_nrm, guv * 3.4).rgb * 2.0 - 1.0;
        float hi_a = clamp(dot(a_hi, vec3(0.3, 0.59, 0.11)) / 0.16, 0.64, 1.42);
        float hi_g = clamp(dot(g_hi, vec3(0.3, 0.59, 0.11)) / 0.20, 0.62, 1.45);
        asph *= mix(1.0, hi_a, close_r * 0.78);
        grav *= mix(1.0, hi_g, close_r * 0.75);
        anrm = normalize(mix(anrm, n_hi, close_r * 0.62));
        gnrm = normalize(mix(gnrm, gn_hi, close_r * 0.60));
    }

    // Tire-worn twin wheel tracks (darker & smoother specular asphalt!) + dusty center strip
    float lane_pos = abs(across - 0.36);
    float tire_track = 1.0 - smoothstep(0.02, 0.18, lane_pos);
    float dust_edge = smoothstep(verge_edge - 0.22, verge_edge + 0.02, across + (gnrm.x * 0.08));
    asph *= mix(1.0, 0.80, tire_track * 0.68);
    asph = mix(asph, grav * 0.82, dust_edge * 0.42);

    // Subtle realistic Spanish road paint (crisp on highways, weathered/soft on secondary, none on village tracks!)
    float dash_c = step(0.45, fract(v_uv.y / 14.0));
    float center_mark = (1.0 - smoothstep(0.010, 0.028, across)) * (v_kind < 0.5 ? 1.0 : dash_c) * (is_highway ? 0.82 : 0.30 * step(v_kind, 2.2));
    float lane_mark   = (v_kind < 0.5) ? ((1.0 - smoothstep(0.007, 0.022, abs(across - 0.42))) * dash_c * 0.72) : 0.0;
    float edge_mark   = (1.0 - smoothstep(0.008, 0.024, abs(across - (verge_edge - 0.05)))) * (is_highway ? 0.75 : 0.22 * step(v_kind, 2.2));
    float paint = clamp(center_mark + lane_mark + edge_mark, 0.0, 1.0);
    asph = mix(asph, vec3(0.72, 0.70, 0.66), paint * 0.85);

    // Organic dusty-gravel verge & weathered curb blend (melts seamlessly into terrain grass/plazas!)
    float is_verge = smoothstep(verge_edge - 0.04, verge_edge + 0.08, across + gnrm.x * 0.09);
    float is_gutter = (1.0 - smoothstep(0.006, 0.045, abs(across - verge_edge))) * (is_highway ? 0.28 : 0.22);
    vec3 base = mix(asph, grav * 0.88, is_verge) * (1.0 - is_gutter);

    vec3 surf_n = normalize(mix(anrm, gnrm, is_verge));
    vec3 nrm = normalize(vec3(surf_n.x * 0.42, 1.0, surf_n.y * 0.42));
    float rough = mix(mix(0.68, 0.36, tire_track), 0.88, is_verge);
    rough = mix(rough, 0.52, paint);

    // PRE-BAKED FIXED-SUN 10-TIME-OF-DAY SHADOWS + HEMISPHERE SKY AO + WARM STREET-CANYON BOUNCE GI!
    // Matches cdlod_terrain.frag & city_baked.frag identically so roads NEVER pop bright inside shadows!
    float baked_sun = mix(0.14, 1.0, clamp(v_sun_vis, 0.0, 1.0));
    float baked_ao  = mix(0.38, 1.0, clamp(v_sky_ao, 0.0, 1.0));

    vec3 ldir = (sdir.y >= -0.025) ? sdir : normalize(vec3(-sdir.x, 0.55, -sdir.z));
    vec3 sun_c = (sdir.y >= -0.025)
        ? mix(vec3(1.0, 0.62, 0.30), vec3(1.0, 0.95, 0.86), clamp(sdir.y / 0.45, 0.0, 1.0)) * mix(0.55, 2.42, day_t)
        : vec3(0.14, 0.20, 0.34) * 0.14;
    vec3 sky_amb = mix(vec3(0.012, 0.018, 0.042), mix(vec3(0.34, 0.40, 0.48), u_haze_cool * 0.85, 0.54), day_t);
    vec3 gnd_amb = mix(vec3(0.005, 0.006, 0.010), vec3(0.24, 0.20, 0.15), day_t);
    vec3 amb_c = mix(gnd_amb, sky_amb, nrm.y * 0.5 + 0.5) * 0.54;
    // Warm sunlit wall-to-pavement multi-bounce GI in shaded town/village alleys
    vec3 bounce_gi = vec3(0.26, 0.20, 0.14) * ((1.0 - v_sky_ao) * day_t * 0.48);

    float ndl = max(0.0, dot(nrm, ldir));
    float ndv = max(0.001, dot(nrm, -view_dir));
    float spec = ggx_spec(nrm, -view_dir, ldir, rough, 0.045);
    float fres = 0.04 + (1.0 - rough) * 0.34 * pow(1.0 - ndv, 4.0);
    vec3 refl = reflect(view_dir, nrm);
    vec2 ruv = vec2(atan(refl.z, refl.x) * (0.5 / 3.14159265) + 0.5, asin(clamp(refl.y, -1.0, 1.0)) * (1.0 / 3.14159265) + 0.5);
    vec3 env_r = textureLod(u_refl_env, ruv, rough * 5.2).rgb;
    vec3 lit = base * (amb_c * baked_ao + bounce_gi + sun_c * (ndl * baked_sun * mix(0.75, 1.0, baked_ao))) +
               sun_c * (spec * ndl * baked_sun * 0.72) +
               env_r * (fres * baked_ao * 0.45 * mix(0.25, 1.0, day_t));

    // RAZOR-SHARP PRE-BAKED RAY-TRACED NIGHT LOCAL LIGHTS (towns/villages) + Focused Periodic Streetlamp Cones & Cat's-Eyes on ALL Roads!
    vec3 emiss = vec3(0.0);
    if (night_t > 0.01) {
        float rt_nl = pow(clamp(v_night_light, 0.0, 1.0), 1.45);
        // Periodic focused streetlamp cones (28m spacing in towns/highways, 38m on rural roads) so dark asphalt contrasts sharply between lamps!
        float spacing = (v_kind < 1.5 || rt_nl > 0.15) ? 28.0 : 38.0;
        float lp_along = abs(fract(v_uv.y / spacing) - 0.5) * 2.0;
        float lp_cone  = exp(-lp_along * lp_along * 8.5);
        float lp_core  = exp(-lp_along * lp_along * 38.0) * smoothstep(0.32, 0.82, across);
        float edge_w   = mix(0.52, 1.0, smoothstep(0.18, 0.76, across));
        // Inside towns, building-ray-traced occlusion modulates periodic streetlamp cones; on rural roads, roadside lamps illuminate the route!
        float rural_w  = mix(0.92, 0.68, smoothstep(1.5, 3.0, v_kind));
        float pool     = max(rt_nl * (0.22 + 0.98 * lp_cone * edge_w), rural_w * lp_cone * edge_w);
        vec3 lamp_col  = mix(vec3(1.00, 0.56, 0.16), vec3(1.00, 0.86, 0.54), clamp(pool * 0.85 + lp_core * 0.5, 0.0, 1.0));
        // Drive illumination through textured asphalt/paint albedo + crisp specular tire-track glints (zero flat cream wash!)
        float track_glint = (1.0 - rough) * (0.30 + 0.70 * tire_track) * lp_cone * (1.0 - is_verge);
        emiss += (base * (3.10 * pool) + lamp_col * (0.09 * pool + 0.48 * track_glint + 0.65 * lp_core)) * lamp_col * night_t;
        // Crisp retro-reflective white/amber roadside cat's-eye studs on interurban highways & rural roads!
        float stud_d = (1.0 - smoothstep(0.006, 0.024, abs(across - (verge_edge - 0.05)))) *
                       (1.0 - smoothstep(0.0, 0.06, abs(fract(v_uv.y / 18.0) - 0.5)));
        emiss += vec3(1.35, 1.24, 0.96) * (stud_d * night_t * (1.0 - smoothstep(220.0, 850.0, v_dist)));
    }

    // Unified 3-Layer Analytic Height Fog (Valley + Tropospheric + Mountain Peak Mist)
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
    float mist = clamp(1.0 - exp(-(tau_v + tau_a)), 0.0, 0.85);
    float valley_frac = clamp(tau_v / max(0.0001, tau_v + tau_a), 0.0, 1.0);
    float rayleigh = 0.75 * (1.0 + cos_th * cos_th);
    float hg_w = (1.0 - 0.3844) / (4.0 * 3.14159 * pow(max(0.04, 1.3844 - 1.24 * cos_th), 1.5));
    float mie = clamp(hg_w * 0.52 * sun_vis + pow(1.0 - clamp(abs(view_dir.y) * 2.4, 0.0, 1.0), 2.2) * 0.32 * sun_vis, 0.0, 1.35);
    vec3 ray_col = mix(u_haze_cool * (0.62 + 0.22 * rayleigh), mix(u_haze_cool, vec3(0.76, 0.84, 0.92), 0.38), valley_frac * 0.65);
    vec3 insc = mix(ray_col, u_haze_warm * 1.32, clamp(mie * 0.80, 0.0, 0.92));

    // Soft organic noisy gravel edge feather so road blends invisibly into terrain!
    float edge_alpha = (1.0 - smoothstep(0.76, 0.98, across + gnrm.y * 0.10)) * dist_fade;
    if (edge_alpha < 0.04) discard;
    frag_color = vec4(mix(lit, insc, mist * 0.88) + emiss * (1.0 - mist * 0.5), edge_alpha);
}
