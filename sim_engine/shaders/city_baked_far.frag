#version 460 core

layout(binding = 4) uniform sampler2D u_mtn_gi;

uniform vec3 u_sun_dir;
uniform vec3 u_haze_cool;
uniform vec3 u_haze_warm;
uniform float u_haze_density;
uniform float u_cam_alt_m;
uniform float u_far_start;
uniform float u_far_end;
uniform vec4 u_mtn_bounds;

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

void main() {
    // Zero discard keeps full AMD Hardware Early-Z / Hi-Z rejection across dense front-to-back city blocks.
    bool is_roof  = ((v_flags & 1u) != 0u);
    int style     = int((v_flags >> 2u) & 7u);
    int roof_kind = int((v_flags >> 5u) & 3u);

    vec3 nrm      = normalize(v_world_n);
    vec3 sdir     = normalize(u_sun_dir);
    vec3 view_dir = normalize(v_world_pos);
    float day_t   = clamp((sdir.y + 0.035) / 0.14, 0.0, 1.0);
    float night_t = clamp((0.06 - sdir.y) / 0.12, 0.0, 1.0);

    vec3 col;
    vec3 emiss = vec3(0.0);
    if (is_roof) {
        // ZERO-TEXTURE-FETCH per-building real IGN PNOA rooftop color harmonized with roof archetype!
        vec3 p = max(v_meta.rgb * (1.0 / 255.0), vec3(0.05));
        vec3 pnoa_lin = p * p;
        vec3 arch_r = (roof_kind == 0) ? vec3(0.32, 0.15, 0.09)
                    : ((roof_kind == 1) ? vec3(0.30, 0.21, 0.14)
                    : ((roof_kind == 2) ? vec3(0.28, 0.25, 0.22) : vec3(0.24, 0.25, 0.27)));
        col = mix(arch_r, pnoa_lin, 0.56);
    } else {
        float bhash = v_meta.x / 255.0;
        vec3 base_w = (style == 0 || style == 6) ? vec3(0.34, 0.18, 0.13)
                    : ((style == 2 || style == 7) ? vec3(0.42, 0.41, 0.39)
                    : ((style == 4) ? vec3(0.44, 0.28, 0.22) : vec3(0.41, 0.36, 0.28)));
        float story_band = 0.88 + 0.18 * step(0.42, fract(v_uv.y * 0.32));
        col = base_w * (0.86 + 0.26 * bhash) * story_band;
        if (night_t > 0.01) {
            vec2 wcell = floor(vec2(v_uv.x * 0.42, v_uv.y * 0.32));
            vec2 wf = fract(vec2(v_uv.x * 0.42, v_uv.y * 0.32));
            float wbox = step(0.22, wf.x) * step(wf.x, 0.78) * step(0.24, wf.y) * step(wf.y, 0.76);
            float wh = fract(sin(dot(wcell + vec2(bhash * 37.0, bhash * 19.0), vec2(12.9898, 78.233))) * 43758.5453);
            vec3 wcol = mix(vec3(1.45, 0.88, 0.36), vec3(1.25, 1.15, 0.92), fract(wh * 7.3));
            emiss = wcol * (wbox * step(0.66, wh) * night_t * 0.92);
        }
    }

    vec2 mtn_uv = clamp((v_world_en - u_mtn_bounds.xy) * u_mtn_bounds.zw, vec2(0.002), vec2(0.998));
    vec2 mtn_gi = texture(u_mtn_gi, mtn_uv).rg;
    float sun_comb = clamp(v_sun_vis, 0.0, 1.0) * mtn_gi.r;
    float ao_comb  = clamp(v_sky_ao, 0.0, 1.0) * mix(0.65, 1.0, mtn_gi.g);
    float baked_sun_vis = mix(0.12, 1.0, sun_comb);
    float baked_sky_ao  = mix(0.36, 1.0, ao_comb);

    vec3 ldir = (sdir.y >= -0.025) ? sdir : normalize(vec3(-sdir.x, 0.55, -sdir.z));
    vec3 sun_c = (sdir.y >= -0.025)
        ? mix(vec3(1.0, 0.62, 0.30), vec3(1.0, 0.95, 0.86), clamp(sdir.y / 0.45, 0.0, 1.0)) * mix(0.55, 2.42, day_t)
        : vec3(0.14, 0.20, 0.34) * 0.14;
    vec3 sky_amb = mix(vec3(0.012, 0.018, 0.042), mix(vec3(0.36, 0.42, 0.50), u_haze_cool * 0.86, 0.56), day_t);
    vec3 gnd_amb = mix(vec3(0.005, 0.006, 0.010), vec3(0.28, 0.24, 0.18), day_t);
    vec3 amb_c = mix(gnd_amb, sky_amb, nrm.y * 0.5 + 0.5) * 0.58;
    vec3 bounce_gi = vec3(0.30, 0.23, 0.15) * ((1.0 - ao_comb) * (1.0 - abs(nrm.y)) * day_t * 0.55);

    float ndl = max(0.0, dot(nrm, ldir));
    vec3 lit = col * (amb_c * baked_sky_ao + bounce_gi + sun_c * (baked_sun_vis * ndl));
    if (!is_roof && night_t > 0.01 && v_night_light > 0.02) {
        float nl = pow(clamp(v_night_light, 0.0, 1.0), 1.65) * exp(-max(0.0, v_uv.y) * 0.09);
        lit += col * vec3(2.40, 1.38, 0.46) * (nl * night_t);
    }

    float cos_th = max(0.0, dot(view_dir, sdir));
    float sun_vis_f = clamp((sdir.y + 0.08) / 0.24, 0.0, 1.0);
    float dy = v_elev_m - u_cam_alt_m;
    float h_cam_v = max(0.0, u_cam_alt_m - 320.0);
    float d_ov_hv = dy / 290.0;
    float int_v = exp(-h_cam_v / 290.0) * ((abs(d_ov_hv) < 0.02) ? (1.0 - 0.5 * d_ov_hv) : ((1.0 - exp(-d_ov_hv)) / d_ov_hv));
    float h_cam_a = max(0.0, u_cam_alt_m);
    float d_ov_ha = dy / 2800.0;
    float int_a = exp(-h_cam_a / 2800.0) * ((abs(d_ov_ha) < 0.02) ? (1.0 - 0.5 * d_ov_ha) : ((1.0 - exp(-d_ov_ha)) / d_ov_ha));
    float rho = max(u_haze_density, 0.000018);
    float tau = v_dist * rho * (1.28 * clamp(int_v, 0.0, 1.75) + 0.86 * clamp(int_a, 0.0, 1.45));
    float mist = clamp(1.0 - exp(-tau), 0.0, 0.90);
    float c2 = cos_th * cos_th;
    float mie = clamp((c2 * c2) * 0.55 * sun_vis_f, 0.0, 0.72);
    vec3 insc = mix(u_haze_cool * 0.88, u_haze_warm * 1.18, mie);

    frag_color = vec4(mix(lit, insc, mist * 0.90) + emiss * (1.0 - mist * 0.6), 1.0);
}
