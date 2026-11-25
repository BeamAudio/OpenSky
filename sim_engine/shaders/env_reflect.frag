#version 460 core
in vec2 v_uv;
layout(location = 0) out vec4 frag_color;

uniform vec3 u_sun_dir;
uniform vec3 u_haze_cool;
uniform vec3 u_haze_warm;
uniform vec3 u_cloud_lit;
uniform vec3 u_cloud_shadow;
uniform float u_cloud_cov;
uniform float u_precip;

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

// Pre-Baked Equirectangular IBL Reflection Map (256x128 RGBA16F + hardware mipmap roughness chain!)
// Refreshed at <=0.5 Hz when sun, cloud color/coverage, precipitation, or haze changes.
void main() {
    float phi = (v_uv.x - 0.5) * 6.28318530;
    float theta = (v_uv.y - 0.5) * 3.14159265;
    float ct = cos(theta);
    vec3 dir = normalize(vec3(ct * cos(phi), sin(theta), ct * sin(phi)));
    vec3 sdir = normalize(u_sun_dir);

    float day_t = clamp((sdir.y + 0.06) / 0.22, 0.0, 1.0);
    float night_t = clamp((0.06 - sdir.y) / 0.14, 0.0, 1.0);
    float cos_th = max(0.0, dot(dir, sdir));
    float y_pos = max(0.0, dir.y);

    // 1. Upper Hemisphere: Rayleigh Zenith + Golden Horizon + Mie Sun Corona + Soft Cloud Band Reflection!
    vec3 zenith = mix(vec3(0.015, 0.032, 0.075), mix(vec3(0.06, 0.20, 0.58), vec3(0.11, 0.28, 0.68), clamp(sdir.y * 1.8, 0.0, 1.0)), day_t);
    vec3 horiz  = mix(vec3(0.03, 0.05, 0.10), mix(u_haze_cool * 1.15, u_haze_warm * 1.18, pow(cos_th, 2.2) * 0.65), day_t);
    vec3 sky = mix(horiz, zenith, pow(y_pos, 0.58));
    float rain_dim = smoothstep(0.06, 0.85, u_precip);
    sky = mix(sky, sky * vec3(0.69, 0.76, 0.84), rain_dim * 0.52);

    // Solar Mie corona + crisp specular sun disc (drives natural sun glints on water, windows & aircraft!)
    float mie_halo = pow(cos_th, 10.0) * 0.65 + pow(cos_th, 96.0) * 2.2 + smoothstep(0.9985, 0.9997, cos_th) * 9.0;
    sky += u_haze_warm * (mie_halo * day_t * (1.0 - rain_dim * 0.72));

    // Subtle reflected cumulus cloud band overhead (keeps aircraft glass & water alive with sky clouds!)
    if (dir.y > 0.035 && u_cloud_cov > 0.08) {
        vec2 cuv = dir.xz / max(0.14, dir.y) * 0.42;
        float n = vnoise(cuv * 2.2) * 0.62 + vnoise(cuv * 5.4 + vec2(3.1, 1.7)) * 0.38;
        float cm = smoothstep(0.68 - u_cloud_cov * 0.42, 0.86 - u_cloud_cov * 0.38, n) * smoothstep(0.035, 0.22, dir.y);
        vec3 ccol = mix(u_cloud_shadow, u_cloud_lit * 1.25, clamp(0.35 + 0.65 * pow(cos_th, 2.0), 0.0, 1.0));
        ccol = mix(ccol, ccol * vec3(0.78, 0.84, 0.91), rain_dim * 0.44);
        sky = mix(sky, ccol, cm * 0.72);
    }

    // 2. Horizon Pyrenean Silhouette Band + Lower Hemisphere Iberian Ground Bounce!
    float ridge_h = 0.025 + 0.045 * vnoise(vec2(phi * 2.8, 1.3)) * smoothstep(-0.3, 0.4, -dir.z);
    vec3 mtn_col = mix(u_haze_cool * 0.38, mix(vec3(0.10, 0.12, 0.14), u_haze_cool * 0.68, 0.62), day_t);
    vec3 gnd_col = mix(vec3(0.015, 0.018, 0.024), vec3(0.18, 0.16, 0.12), day_t);
    // Warm subtle Spanish town glow on night horizon
    vec3 night_glow = vec3(0.14, 0.08, 0.035) * (night_t * exp(-abs(dir.y) * 14.0));

    float below_ridge = 1.0 - smoothstep(ridge_h - 0.015, ridge_h + 0.02, dir.y);
    float below_gnd = 1.0 - smoothstep(-0.14, 0.01, dir.y);
    vec3 col = mix(sky, mtn_col, below_ridge * 0.82);
    col = mix(col, gnd_col, below_gnd) + night_glow;
    frag_color = vec4(max(col, vec3(0.0)), 1.0);
}
