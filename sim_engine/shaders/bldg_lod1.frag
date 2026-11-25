#version 460 core

uniform vec3 u_sun_dir;
uniform vec3 u_haze_cool;
uniform vec3 u_haze_warm;
uniform float u_haze_density;
uniform float u_cam_alt_m;
uniform float u_lod_in_start;
uniform float u_lod_in_end;
uniform float u_far_start;
uniform float u_far_end;

in vec3 v_world_pos;
in vec3 v_world_n;
in vec4 v_roof_ao;
in float v_is_roof;
in float v_elev_m;
in float v_dist;

layout(location = 0) out vec4 frag_color;

void main() {
    // Seamless dither cross-fade in from LOD0 (b_rad*0.80..b_rad) & smooth fade out into horizon haze (17..22km)!
    float fin  = smoothstep(u_lod_in_start, u_lod_in_end, v_dist);
    float fout = 1.0 - smoothstep(u_far_start, u_far_end, v_dist);
    float vis = fin * fout;
    if (vis < 0.02) discard;
    if (vis < 0.98) {
        float dither = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
        if (vis < dither) discard;
    }

    vec3 nrm = normalize(v_world_n);
    vec3 sdir = normalize(u_sun_dir);
    vec3 view_dir = normalize(v_world_pos);
    float day_t = clamp((sdir.y + 0.035) / 0.14, 0.0, 1.0);
    float night_t = clamp((0.06 - sdir.y) / 0.12, 0.0, 1.0);

    // Real pre-baked IGN PNOA rooftop RGB on top + warm Aragonese stone/brick/stucco walls!
    vec3 roof_lin = pow(max(v_roof_ao.rgb, vec3(0.05)), vec3(2.2));
    vec3 wall_lin = mix(vec3(0.36, 0.30, 0.24), roof_lin * 1.15, 0.35);
    vec3 col = mix(wall_lin, roof_lin, v_is_roof);

    vec3 ldir = (sdir.y >= -0.025) ? sdir : normalize(vec3(-sdir.x, 0.55, -sdir.z));
    vec3 sun_c = (sdir.y >= -0.025)
        ? mix(vec3(1.0, 0.62, 0.30), vec3(1.0, 0.95, 0.86), clamp(sdir.y / 0.45, 0.0, 1.0)) * mix(0.55, 1.38, day_t)
        : vec3(0.32, 0.45, 0.72) * 0.36;
    vec3 sky_amb = mix(vec3(0.07, 0.11, 0.20), mix(vec3(0.34, 0.39, 0.45), u_haze_cool * 0.82, 0.52), day_t);
    vec3 gnd_amb = mix(vec3(0.04, 0.04, 0.05), vec3(0.28, 0.24, 0.19), day_t);
    vec3 amb_c = mix(gnd_amb, sky_amb, nrm.y * 0.5 + 0.5) * 0.52;
    float ao = mix(0.62, 1.0, v_roof_ao.a);
    float ndl = max(0.0, dot(nrm, ldir));
    vec3 lit = col * (amb_c * ao + sun_c * (ndl * mix(0.72, 1.0, ao)));

    // Warm distant Spanish city glow at twilight & Short Night!
    vec3 emiss = vec3(1.25, 0.78, 0.36) * ((1.0 - v_is_roof) * night_t * 0.55);

    // EXACT Terrain-Matched 2-Layer Analytic Height Fog (H_v=290m + H_a=2800m)
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
