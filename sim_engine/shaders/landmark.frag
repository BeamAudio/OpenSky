#version 460 core
uniform vec3 u_sun_dir;
uniform vec3 u_haze_cool;
uniform vec3 u_haze_warm;
uniform float u_haze_density;
uniform float u_cam_alt_m;

in vec3 v_world_pos;
in vec3 v_nrm;
in vec4 v_col;
in vec2 v_meta;
in float v_elev_m;
in float v_dist;

layout(location = 0) out vec4 frag_color;

void main() {
    vec3 nrm = normalize(v_nrm);
    vec3 sdir = normalize(u_sun_dir);
    vec3 view_dir = normalize(v_world_pos);
    float day_t = clamp((sdir.y + 0.035) / 0.14, 0.0, 1.0);
    float night_t = clamp((0.08 - sdir.y) / 0.14, 0.0, 1.0);

    // Procedural Aragonese masonry courses, arched facade windows & ceramic dome ribs (eliminates flat plastic faces!)
    vec3 wabs = v_world_pos;
    float is_wall = 1.0 - smoothstep(0.45, 0.75, abs(nrm.y));
    float horz = (abs(nrm.x) > abs(nrm.z)) ? (wabs.z + v_elev_m * 0.01) : (wabs.x + v_elev_m * 0.01);
    float course = 1.0 - smoothstep(0.0, 0.10, abs(fract(v_elev_m * 0.55) - 0.5));
    float block_g = fract(sin(dot(floor(vec2(horz * 0.45, v_elev_m * 0.55)), vec2(12.9898, 78.233))) * 43758.5453);
    float fine_g = fract(sin(dot(floor(vec2(horz * 2.4, v_elev_m * 2.4)), vec2(39.346, 11.135))) * 43758.5453);
    // Classical recessed arched bay openings along stone/brick walls (not on glass Torre del Agua!)
    float is_masonry = step(v_meta.x, 0.25) * is_wall;
    float win_x = abs(fract(horz * 0.145) - 0.5);
    float win_y = fract(v_elev_m * 0.165);
    float arch_win = is_masonry * (1.0 - smoothstep(0.13, 0.18, win_x)) * smoothstep(0.18, 0.26, win_y) * (1.0 - smoothstep(0.72, 0.82, win_y));
    // Subtle weathering & stone shadow banding
    float stone_tex = mix(1.0, (0.86 + 0.22 * block_g + 0.08 * fine_g) * (1.0 - course * 0.16), is_masonry);
    vec3 base = v_col.rgb * stone_tex;
    base = mix(base, base * 0.26 + vec3(0.02, 0.025, 0.035), arch_win * 0.85);
    float rough = mix(clamp(v_col.a, 0.06, 0.96), 0.14, arch_win);
    float metal = clamp(v_meta.x, 0.0, 1.0);

    // Full Cook-Torrance GGX PBR + Sky/Ground Hemisphere IBL
    vec3 ldir = (sdir.y >= -0.025) ? sdir : normalize(vec3(-sdir.x, 0.55, -sdir.z));
    vec3 sun_c = (sdir.y >= -0.025)
        ? mix(vec3(1.0, 0.62, 0.30), vec3(1.0, 0.95, 0.86), clamp(sdir.y / 0.45, 0.0, 1.0)) * mix(0.55, 2.44, day_t)
        : vec3(0.14, 0.20, 0.34) * 0.14;
    vec3 sky_amb = mix(vec3(0.012, 0.018, 0.042), u_haze_cool * 0.92, day_t);
    vec3 gnd_amb = mix(vec3(0.005, 0.006, 0.010), vec3(0.24, 0.21, 0.17), day_t);
    vec3 amb_c = mix(gnd_amb, sky_amb, nrm.y * 0.5 + 0.5) * 0.52;

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
    float G = (ndv / (ndv * (1.0 - k) + k)) * (ndl / (ndl * (1.0 - k) + k));
    vec3 F0 = mix(vec3(0.04), base, metal);
    vec3 F = F0 + (1.0 - F0) * pow(1.0 - max(0.0, dot(H, V)), 5.0);
    vec3 spec = (D * G * F) / (4.0 * ndv * ndl + 1e-4);
    vec3 kD = (vec3(1.0) - F) * (1.0 - metal);

    vec3 refl = reflect(view_dir, nrm);
    vec3 ibl_env = mix(gnd_amb, mix(u_haze_cool, u_haze_warm, pow(max(0.0, dot(refl, sdir)), 4.0) * 0.65), smoothstep(-0.15, 0.35, refl.y));
    vec3 F_ibl = F0 + (max(vec3(1.0 - rough), F0) - F0) * pow(1.0 - ndv, 4.0);
    vec3 lit = kD * base * (amb_c + sun_c * ndl) + sun_c * (spec * ndl) + ibl_env * F_ibl * (0.65 * day_t);

    // Textured architectural monument floodlighting at dusk & night (preserves stone courses, brick & blue/gold domes!)
    float flood_dir = (0.28 + 0.72 * (1.0 - smoothstep(0.55, 0.95, nrm.y))) * (0.65 + 0.35 * abs(nrm.x + nrm.z * 0.7));
    vec3 emiss = (base * vec3(1.45, 0.96, 0.48) * flood_dir + vec3(1.15, 0.76, 0.32) * (arch_win * 0.65)) * (v_meta.y * night_t);

    // EXACT Terrain-Matched 2-Layer Analytic Height Fog + Rayleigh/Mie Scattering
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
