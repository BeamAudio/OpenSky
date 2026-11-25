#version 460 core

in vec2 v_uv;
layout(location = 0) out vec4 frag_color;

uniform mat4 u_inv_vp;
uniform vec3 u_sun_dir;
uniform vec3 u_haze_cool;
uniform vec3 u_haze_warm;
uniform float u_cloud_cov;

float hash21(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

void main() {
    vec2 ndc = v_uv * 2.0 - 1.0;
    vec4 wp = u_inv_vp * vec4(ndc, 0.0001, 1.0);
    vec3 dir = normalize(wp.xyz / max(1e-5, wp.w));
    vec3 sdir = normalize(u_sun_dir);

    float day_t = clamp((sdir.y + 0.04) / 0.16, 0.0, 1.0);
    float high_t = clamp(sdir.y / 0.45, 0.0, 1.0);
    float elev = clamp(dir.y, 0.0, 1.0);

    // Physically-inspired Mediterranean Rayleigh zenith-to-horizon sky gradient + Henyey-Greenstein Mie solar corona!
    vec3 zenith_day = mix(vec3(0.048, 0.18, 0.58), vec3(0.052, 0.23, 0.76), high_t);
    vec3 zenith_night = vec3(0.0018, 0.0035, 0.011);
    float overcast = smoothstep(0.42, 0.92, u_cloud_cov);
    vec3 zenith = mix(zenith_night, zenith_day, day_t);
    zenith = mix(zenith, vec3(0.22, 0.27, 0.34) * mix(0.68, 1.0, day_t), overcast * 0.78);
    vec3 horiz_col = mix(u_haze_cool * 0.72, mix(u_haze_cool, u_haze_warm, 0.28 * day_t), day_t);
    horiz_col = mix(horiz_col, vec3(0.42, 0.46, 0.52) * mix(0.72, 1.0, day_t), overcast * 0.64);

    float ray_t = pow(1.0 - elev, 2.8);
    vec3 sky = mix(zenith, horiz_col * mix(0.85, 1.18, day_t), ray_t);

    // Warm sunlit Henyey-Greenstein Mie aureole + crisp solar disk
    float cos_th = max(0.0, dot(dir, sdir));
    float sun_vis = clamp((sdir.y + 0.06) / 0.18, 0.0, 1.0);
    float mie_wide = pow(cos_th, 5.0) * 0.55 + pow(cos_th, 32.0) * 1.25;
    float sun_disk = smoothstep(0.99945, 0.99982, dot(dir, sdir)) * 18.0 * sun_vis * (1.0 - u_cloud_cov * 0.75);
    sky += u_haze_warm * (mie_wide * sun_vis * (0.65 + 0.55 * ray_t)) + vec3(1.0, 0.94, 0.82) * sun_disk;

    // Subtle crisp stars during the compressed Short Night phase!
    float night_w = clamp((-sdir.y - 0.02) / 0.10, 0.0, 1.0) * smoothstep(0.04, 0.25, elev);
    if (night_w > 0.01) {
        vec2 sc = floor(dir.xz / max(0.15, dir.y) * 260.0);
        float st = hash21(sc);
        float star = smoothstep(0.991, 0.9992, st) * night_w * (1.0 - u_cloud_cov * 0.85);
        sky += vec3(0.86, 0.91, 1.0) * (star * 2.4);
    }

    // Ground bounce color below horizon
    if (dir.y < 0.0) {
        sky = mix(horiz_col, horiz_col * 0.45, clamp(-dir.y * 4.0, 0.0, 1.0));
    }
    frag_color = vec4(sky, 1.0);
}
