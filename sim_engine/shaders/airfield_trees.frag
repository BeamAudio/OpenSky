#version 460 core
// Reverse-Z (1=near, 0=far) front-tangent proxy quad guarantees true ray hit depth <= quad depth!
layout(depth_less) out float gl_FragDepth;

layout(binding = 0) uniform sampler2D u_leaf_albedo;
layout(binding = 1) uniform sampler2D u_leaf_normal;
layout(binding = 2) uniform sampler2D u_bark_albedo;

uniform mat4 u_vp;
uniform float u_time;
uniform vec2 u_wind_en;
uniform vec3 u_sun;
uniform vec3 u_haze_cool;
uniform vec3 u_haze_warm;
uniform float u_haze_density;
uniform int u_season;
uniform float u_snowline_m;

in vec3 v_ray_dir;
flat in vec3 v_base_cam;
flat in vec4 v_meta;
flat in vec3 v_params; // x=height, y=fade, z=min_hit_world_t
flat in float v_ground_msl;

layout(location = 0) out vec4 frag_color;

// Lightweight 3D trigonometric bough-cluster turbulence (2 octaves, pure FMA ALU).
float bough_noise(vec3 p, float seed) {
    vec3 s1 = vec3(seed * 6.283, seed * 11.73, seed * 17.31);
    float a = sin(p.x * 9.5 + s1.x) * cos(p.y * 11.0 + s1.y) * sin(p.z * 9.5 + s1.z);
    float b = sin((p.x + p.z) * 19.0 - s1.z) * cos((p.y - p.x) * 22.0 + s1.x);
    return a * 0.62 + b * 0.38;
}

vec2 hit_envelope(vec3 ro, vec3 rd, vec3 center, vec3 inv_rad) {
    vec3 q = (ro - center) * inv_rad;
    vec3 d = rd * inv_rad;
    float a = dot(d, d);
    float b = dot(q, d);
    float c = dot(q, q) - 1.0;
    float disc = b * b - a * c;
    if (disc < 0.0) return vec2(1e9, -1.0);
    float sq = sqrt(disc);
    float inv_a = 1.0 / max(a, 1e-6);
    float t0 = (-b - sq) * inv_a;
    float t1 = (-b + sq) * inv_a;
    if (t1 <= 0.0) return vec2(1e9, -1.0);
    return vec2(max(t0, 0.0), t1);
}

bool hit_trunk(vec3 ro, vec3 rd, float y_top, float r_base, float r_top,
               out float out_t, out vec3 out_n, out vec2 out_uv) {
    float r_mid = 0.5 * (r_base + r_top);
    float a = dot(rd.xz, rd.xz);
    if (a < 1e-6) return false;
    float b = dot(ro.xz, rd.xz);
    float c = dot(ro.xz, ro.xz) - r_mid * r_mid;
    float disc = b * b - a * c;
    if (disc < 0.0) return false;
    float t = (-b - sqrt(disc)) / a;
    if (t <= 0.0) return false;
    vec3 p = ro + rd * t;
    if (p.y < -0.02 || p.y > y_top) return false;
    float h01 = clamp(p.y / max(y_top, 0.01), 0.0, 1.0);
    float r_y = mix(r_base, r_top, h01) * (1.0 + 0.32 * exp(-h01 * 12.0));
    float c2 = dot(ro.xz, ro.xz) - r_y * r_y;
    float disc2 = b * b - a * c2;
    if (disc2 < 0.0) return false;
    t = (-b - sqrt(disc2)) / a;
    if (t <= 0.0) return false;
    p = ro + rd * t;
    if (p.y < -0.02 || p.y > y_top) return false;
    out_t = t;
    out_n = normalize(vec3(p.x, 0.08 * r_base, p.z));
    out_uv = vec2(atan(p.z, p.x) * 0.31831 + 0.5, p.y * 2.6);
    return true;
}

// Irregular 3D botanical crown density (>0 inside jagged bough clusters, <0 in open sky gaps).
float crown_field(vec3 p, int sp, float seed, float width_scale) {
    vec2 lean = vec2(sin(seed * 23.1), cos(seed * 19.7)) * 0.06 * p.y;
    vec3 q = vec3((p.x - lean.x) / width_scale, p.y, (p.z - lean.y) / width_scale);
    float r_xz = length(q.xz);
    float ang = atan(q.z, q.x);
    float bn = bough_noise(q, seed);

    if (sp == 1) { // Pyrenean Scots Pine: broken radiating conifer whorls
        if (q.y < 0.18 || q.y > 1.02) return -1.0;
        float h01 = clamp((q.y - 0.20) / 0.80, 0.0, 1.0);
        float base_rad = mix(0.48, 0.09, pow(h01, 0.85)) * (1.0 - 0.22 * exp(-h01 * 10.0));
        float whorl = 0.58 + 0.42 * sin(q.y * 19.0 + r_xz * 4.5 + sin(ang * 3.0 + seed * 6.28));
        float arms = 0.80 + 0.26 * sin(ang * 5.0 + floor(q.y * 7.0) * 2.1 + seed * 12.0)
                          + 0.15 * cos(ang * 8.0 - q.y * 11.0);
        float eff_r = base_rad * arms * (0.76 + 0.34 * whorl) + bn * 0.12;
        return (eff_r - r_xz) * 2.8 + bn * 0.34;
    } else if (sp == 0) { // Holm Oak: broad gnarled Mediterranean crown
        if (q.y < 0.20 || q.y > 1.02) return -1.0;
        vec3 c_off = vec3(sin(seed * 13.7) * 0.08, 0.60, cos(seed * 17.3) * 0.08);
        vec3 d = (q - c_off) / vec3(0.58, 0.37, 0.58);
        float bough = 0.24 * sin(ang * 4.0 + q.y * 6.0 + seed * 9.0)
                    + 0.16 * cos(ang * 7.0 - q.y * 9.0 + seed * 5.0);
        float under = smoothstep(0.22, 0.40, q.y + bn * 0.09);
        return (1.0 - length(d) + bough * 0.55 + bn * 0.38) * under - (1.0 - under) * 0.5;
    } else if (sp == 2) { // Valley Poplar: tall feathery spire
        if (q.y < 0.16 || q.y > 1.02) return -1.0;
        float h01 = clamp((q.y - 0.18) / 0.82, 0.0, 1.0);
        float plume_r = 0.32 * sin(pow(h01, 0.72) * 3.14159) + 0.05 * (1.0 - h01);
        float tufts = 0.80 + 0.30 * sin(ang * 5.0 + q.y * 14.0 + seed * 8.0);
        return (plume_r * tufts - r_xz) * 3.4 + bn * 0.38;
    } else { // Maquis Shrub: low irregular Mediterranean bush
        if (q.y < -0.02 || q.y > 0.96) return -1.0;
        vec3 d = (q - vec3(0.0, 0.38, 0.0)) / vec3(0.74, 0.48, 0.72);
        float scrub = 0.26 * sin(ang * 5.0 + seed * 7.0) + 0.18 * cos(q.y * 12.0 + ang * 3.0);
        return (1.0 - length(d)) + scrub * 0.42 + bn * 0.42;
    }
}

// Fast 1-tap cylindrical-projected botanical leaf-cluster UV inside species quadrant.
vec2 leaf_atlas_uv(vec3 p, int sp, float seed) {
    float freq = (sp == 3) ? 1.80 : ((sp == 1) ? 2.40 : 2.10);
    // Stable 3D projection: combine angle-around-trunk with vertical & radial depth so there is zero stretching.
    float u = fract((atan(p.z, p.x) * 0.48 + length(p.xz) * 0.95) * freq + seed * 3.7);
    float v = fract((p.y * 1.35 + (p.x - p.z) * 0.25) * freq + seed * 2.1);
    vec2 q_off = vec2(float(sp & 1) * 0.5, float((sp >> 1) & 1) * 0.5);
    return q_off + (0.014 + 0.472 * vec2(u, v));
}

void main() {
    float H = v_params.x;
    float fade = v_params.y;
    float min_world_t = v_params.z;

    // Screen-space interleaved gradient noise: drives BOTH smooth 5-step volume jitter AND
    // seamless pixel-by-pixel LOD dissolve into the rising terrain canopy (zero hard LOD ring!).
    float ign = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    if (fade < 0.99 && ign > fade) discard;

    int sp = int(floor(v_meta.w));
    float seed = fract(v_meta.w);
    float yaw = seed * 6.2831853;
    float cy = cos(yaw), sy = sin(yaw);
    mat2 inv_rot = mat2(cy, sy, -sy, cy);
    mat2 fwd_rot = mat2(cy, -sy, sy, cy);

    float width_scale = 0.85 + fract(seed * 17.31) * 0.34;
    vec3 rd_world = normalize(v_ray_dir);
    vec3 ro_w = -v_base_cam / H;
    vec3 ro = vec3(inv_rot * ro_w.xz, ro_w.y).xzy;
    vec3 rd = vec3(inv_rot * rd_world.xz, rd_world.y).xzy;

    float wspd = length(u_wind_en);
    float wphase = u_time * (1.2 + clamp(wspd * 0.12, 0.0, 2.2)) + seed * 6.283;
    vec2 sway = inv_rot * ((u_wind_en * 0.004 + vec2(0.006, 0.004)) * sin(wphase) * clamp(0.3 + wspd * 0.12, 0.3, 1.3));

    vec3 env_c = (sp == 3) ? vec3(0.0, 0.42, 0.0) : vec3(0.0, 0.56, 0.0);
    vec3 env_r = (sp == 3) ? vec3(0.84, 0.52, 0.84)
               : ((sp == 0) ? vec3(0.70, 0.48, 0.70)
               : ((sp == 1) ? vec3(0.58, 0.50, 0.58) : vec3(0.44, 0.50, 0.44)));
    env_r.xz *= width_scale;
    vec2 env_t = hit_envelope(ro, rd, env_c, 1.0 / env_r);

    float trunk_top = (sp == 1) ? 0.56 : ((sp == 0) ? 0.46 : ((sp == 2) ? 0.48 : 0.0));
    float trunk_rb  = (sp == 0) ? 0.044 : ((sp == 1) ? 0.036 : 0.032);
    float trunk_rt  = (sp == 0) ? 0.020 : 0.013;
    float t_tr = 1e9;
    vec3 n_tr = vec3(0.0, 1.0, 0.0);
    vec2 uv_tr = vec2(0.0);
    bool has_trunk = (trunk_top > 0.05) && hit_trunk(ro, rd, trunk_top, trunk_rb, trunk_rt, t_tr, n_tr, uv_tr);

    if (env_t.y < 0.0 && !has_trunk) discard;

    // Ultra-lean 5-step volumetric raymarch (1 texture tap per step, zero extra gradient loops!).
    bool hit = false;
    bool hit_is_trunk = false;
    float t_hit = 1e9;
    vec3 leaf_tex = vec3(0.0);
    vec3 n_local = vec3(0.0, 1.0, 0.0);
    float interior_ao = 1.0;

    if (env_t.y > env_t.x) {
        const int STEPS = 5;
        float t_span = env_t.y - env_t.x;
        float dt = t_span / float(STEPS);
        float t = env_t.x + dt * (0.12 + 0.76 * ign);

        for (int s = 0; s < STEPS; ++s) {
            if (has_trunk && t > t_tr) break;
            vec3 p = ro + rd * t;
            p.xz -= sway * p.y;
            float d = crown_field(p, sp, seed, width_scale);
            if (d > -0.06) {
                vec2 auv = leaf_atlas_uv(p, sp, seed);
                vec4 lsample = texture(u_leaf_albedo, auv);
                float dens_w = smoothstep(-0.06, 0.34, d);
                if (lsample.a * mix(0.65, 1.32, dens_w) >= 0.46) {
                    hit = true;
                    t_hit = t;
                    leaf_tex = lsample.rgb;
                    // Analytic radial + upward crown macro-normal perturbed by HQ leaf normal map (1 tap!).
                    vec3 radial = normalize(vec3(p.x, (p.y - 0.46) * 0.85 + 0.18, p.z) + vec3(0.0, 0.001, 0.0));
                    vec3 ln = texture(u_leaf_normal, auv).rgb * 2.0 - 1.0;
                    vec3 ref = (abs(radial.y) < 0.95) ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
                    vec3 T = normalize(cross(ref, radial));
                    vec3 B = cross(radial, T);
                    n_local = normalize(radial + (T * ln.x + B * ln.y) * 0.85);
                    float depth_in = clamp((t - env_t.x) / max(t_span, 0.1), 0.0, 1.0);
                    interior_ao = mix(1.04, 0.46, smoothstep(0.06, 0.72, depth_in));
                    break;
                }
            }
            t += dt;
        }
    }

    if (!hit && has_trunk) {
        hit = true;
        hit_is_trunk = true;
        t_hit = t_tr;
        n_local = n_tr;
        leaf_tex = texture(u_bark_albedo, uv_tr).rgb * 0.52;
        float h_y = (ro + rd * t_tr).y;
        interior_ao = mix(0.42, 0.86, smoothstep(0.0, 0.14, h_y)) * mix(1.0, 0.34, smoothstep(0.20, trunk_top, h_y));
    }

    if (!hit) discard;

    float t_world = max(t_hit * H, min_world_t);
    vec3 hit_cam = rd_world * t_world;
    vec4 clip = u_vp * vec4(hit_cam, 1.0);
    gl_FragDepth = clamp(clip.z / max(clip.w, 1e-6), 0.0, 1.0);

    vec3 p_local = ro + rd * t_hit;
    vec3 n_world = normalize(vec3(fwd_rot * n_local.xz, n_local.y).xzy);
    vec3 sdir = normalize(u_sun);

    // 1-tap volumetric sun self-shadowing inside the crown.
    float self_sun = 1.0;
    vec3 col = leaf_tex;
    if (!hit_is_trunk) {
        vec3 s_local = vec3(inv_rot * sdir.xz, sdir.y).xzy;
        vec3 p_sh = p_local - vec3(sway.x * p_local.y, 0.0, sway.y * p_local.y) + s_local * 0.16;
        self_sun = 1.0 - smoothstep(-0.02, 0.42, crown_field(p_sh, sp, seed, width_scale)) * 0.62;

        // Strict satellite-first PNOA color anchoring so every tree matches the real valley/mountainside!
        // Extract neutral leaf-cluster micro-luminance contrast (0.65..1.35) and apply it to the
        // real PNOA orthophoto color + subtle botanical species hue tint.
        float leaf_luma = max(0.015, dot(leaf_tex, vec3(0.2126, 0.7152, 0.0722)));
        float leaf_detail = clamp(leaf_luma / 0.105, 0.58, 1.38);

        // Clamp PNOA sample into realistic wooded linear-RGB range (protects against bright road-edge bleed).
        vec3 pnoa = clamp(v_meta.rgb, vec3(0.020, 0.028, 0.016), vec3(0.16, 0.19, 0.12));
        // Blend 78% real PNOA terrain color + 22% dark botanical leaf hue for seamless scenery match!
        vec3 sp_hue = (sp == 1) ? vec3(0.84, 0.96, 0.88)   // cooler Pyrenean Scots pine
                    : ((sp == 2) ? vec3(1.06, 1.08, 0.82)  // warmer riverine poplar
                    : ((sp == 3) ? vec3(0.98, 0.96, 0.84)  // dusty maquis shrub
                    : vec3(0.92, 0.97, 0.86)));            // Mediterranean holm oak
        vec3 base_pnoa = pnoa * sp_hue * leaf_detail;
        col = mix(leaf_tex, base_pnoa, 0.78);

        // Exact same 4-season response as cdlod_terrain.frag!
        if (u_season == 0) {
            col *= (sp == 2) ? vec3(0.82, 0.78, 0.68) : vec3(0.90, 0.94, 0.99);
            float snow_w = smoothstep(u_snowline_m - 170.0, u_snowline_m + 120.0, v_ground_msl) *
                           smoothstep(0.35, 0.82, n_world.y);
            col = mix(col, vec3(0.72, 0.78, 0.84), snow_w * 0.65);
        } else if (u_season == 1) {
            col *= vec3(0.98, 1.035, 0.96);
        } else if (u_season == 2) {
            col *= vec3(1.035, 0.99, 0.88);
        } else {
            col *= (sp == 2) ? vec3(1.18, 0.94, 0.52) : vec3(1.04, 0.93, 0.80);
        }
    }

    // Match cdlod_terrain.frag sun, ambient & atmospheric haze identically!
    float day = clamp((sdir.y + 0.035) / 0.14, 0.0, 1.0);
    float wrap = hit_is_trunk ? 0.10 : 0.25;
    float diff = clamp((dot(n_world, sdir) + wrap) / (1.0 + wrap), 0.0, 1.0);
    float vert_ao = mix(0.46, 1.06, smoothstep(0.14, 0.88, p_local.y));
    float vol_ao = interior_ao * (hit_is_trunk ? 1.0 : vert_ao);

    vec3 sun_col = (sdir.y >= -0.025)
        ? mix(vec3(1.0, 0.62, 0.30), vec3(1.0, 0.96, 0.88), clamp(sdir.y / 0.45, 0.0, 1.0)) * mix(0.55, 2.30, day)
        : vec3(0.14, 0.20, 0.34) * 0.14;
    vec3 sky_amb = mix(vec3(0.014, 0.020, 0.044), mix(vec3(0.32, 0.37, 0.44), u_haze_cool * 0.80, 0.50), day) * 0.45;
    vec3 gnd_amb = mix(vec3(0.006, 0.007, 0.011), vec3(0.16, 0.14, 0.10), day);
    vec3 ambient = mix(gnd_amb, sky_amb, n_world.y * 0.5 + 0.5);

    float back = pow(max(0.0, dot(rd_world, sdir)), 3.2) * day * (hit_is_trunk ? 0.0 : 1.0);
    vec3 sss = col * vec3(0.35, 0.50, 0.16) * (back * 0.40 * self_sun * vol_ao);
    vec3 lit = col * (ambient * vol_ao + sun_col * (diff * self_sun * mix(0.56, 1.04, vol_ao))) + sss;

    float cos_th = max(0.0, dot(rd_world, sdir));
    vec3 haze_c = mix(u_haze_cool * 0.88, u_haze_warm * 1.15, clamp(pow(cos_th, 4.0) * 0.55 * day, 0.0, 0.72));
    float haze = clamp(1.0 - exp(-t_world * max(u_haze_density, 0.000018) * 1.15), 0.0, 0.88);
    frag_color = vec4(mix(lit, haze_c, haze), 1.0);
}
