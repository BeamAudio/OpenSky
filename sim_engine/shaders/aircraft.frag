#version 460 core
layout(binding = 0) uniform sampler2D u_albedo_tex;
layout(binding = 1) uniform sampler2D u_normal_tex;
layout(binding = 2) uniform sampler2D u_mr_tex;
layout(binding = 3) uniform sampler2D u_refl_env; // Pre-baked 256x128 RGBA16F IBL reflection map + mip roughness chain!
layout(binding = 4) uniform sampler3D u_windscreen_rain_front; // Four physically simulated rain-rate relief layers
layout(binding = 5) uniform sampler3D u_windscreen_rain_side;
layout(binding = 6) uniform sampler2D u_output_lut;
layout(binding = 7) uniform sampler2D u_backdrop_tex; // Resolved linear HDR world+sky for true optical raindrop refraction!

uniform vec3 u_sun_dir;
uniform vec3 u_haze_cool;
uniform vec3 u_haze_warm;
uniform vec4 u_base_tint;
uniform vec4 u_pbr_factors; // x=metallic, y=roughness, z=has_nrm, w=has_mr
uniform vec3 u_emissive_col;
uniform float u_is_interior;
uniform float u_alpha_mul;
uniform float u_livery;
uniform int u_unlit;
uniform int u_cabin_glass;
uniform float u_baked_reflection;
uniform vec3 u_glass_weather; // x=local rain mm/h, y=simulation seconds, z=aircraft speed m/s
uniform vec4 u_glass_flow_front; // xy=integrated uv offset, zw=flow velocity vector
uniform vec4 u_glass_flow_side;  // xy=integrated uv offset, zw=flow velocity vector
uniform vec4 u_glass_fx;      // x=frost_ice01, y=cabin_mist01, z=grime01, w=snow_factor01
uniform vec4 u_plane_wx;      // x=airframe_ice01, y=lightning_flash01, z=defrost_clear01, w=wet_sheen01
uniform vec4 u_dmg_zones;     // x=nose, y=wing_l, z=wing_r, w=tail
uniform vec4 u_dmg_extra;     // x=airframe, y=windshield_crack, z=scorch, w=is_interior
uniform int u_native_output;
uniform int u_aircraft_detail;
uniform int u_rain_luts;
uniform float u_output_exposure;

in vec3 v_world_pos;
in vec3 v_nrm;
in vec4 v_col;
in vec2 v_uv;
in vec3 v_obj_pos;
in vec3 v_ac_pos;
in vec4 v_clip_pos;

layout(location = 0) out vec4 frag_color;

vec3 agx_tonemap(vec3 val) {
    val = max(val, vec3(0.0));
    const mat3 agx_mat = mat3(
        vec3(0.84247906, 0.04232824, 0.04237565),
        vec3(0.07843360, 0.87846864, 0.07843360),
        vec3(0.07922375, 0.07916613, 0.87914297)
    );
    const mat3 agx_mat_inv = mat3(
        vec3( 1.19687900, -0.05289685, -0.05297185),
        vec3(-0.09802088,  1.15190313, -0.09804345),
        vec3(-0.09902975, -0.09896118,  1.15107367)
    );
    vec3 v = agx_mat * val;
    v = clamp((log2(max(v, vec3(1e-6))) - (-12.47393)) / (4.026069 - (-12.47393)), 0.0, 1.0);
    vec3 v2 = v * v;
    vec3 v4 = v2 * v2;
    vec3 c = +15.5 * v4 * v2 - 40.14 * v4 * v + 31.96 * v4 - 6.868 * v2 * v + 0.4298 * v2 + 0.1191 * v - 0.00232;
    return clamp(agx_mat_inv * c, 0.0, 1.0);
}

vec3 native_display_color(vec3 linear_color) {
    if (u_native_output == 0) return linear_color;
    vec3 srgb = agx_tonemap(linear_color * u_output_exposure);
    srgb = max(srgb, vec3(0.0));
    srgb = srgb * (0.97 + 0.03 * srgb);
    srgb = mix(srgb, smoothstep(vec3(0.008), vec3(0.992), srgb), 0.14);
    float luma = dot(srgb, vec3(0.2126, 0.7152, 0.0722));
    float max_c = max(srgb.r, max(srgb.g, srgb.b));
    float min_c = min(srgb.r, min(srgb.g, srgb.b));
    float cur_sat = (max_c - min_c) / max(0.001, max_c);
    float vib_boost = 1.12 + 0.08 * (1.0 - cur_sat);
    srgb = clamp(mix(vec3(luma), srgb, vib_boost), 0.0, 1.0);
    return vec3(
        textureLod(u_output_lut, vec2(srgb.r, 0.5), 0.0).r,
        textureLod(u_output_lut, vec2(srgb.g, 0.5), 0.0).g,
        textureLod(u_output_lut, vec2(srgb.b, 0.5), 0.0).b
    );
}

vec3 perturb_normal(vec3 N, vec3 pos, vec2 uv, vec3 map_n, float strength) {
    vec3 dp1 = dFdx(pos);
    vec3 dp2 = dFdy(pos);
    vec2 duv1 = dFdx(uv);
    vec2 duv2 = dFdy(uv);
    vec3 dp2perp = cross(dp2, N);
    vec3 dp1perp = cross(N, dp1);
    vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;
    float invmax = inversesqrt(max(dot(T, T), dot(B, B)) + 1e-8);
    mat3 TBN = mat3(T * invmax, B * invmax, N);
    vec3 tn = normalize(vec3(map_n.xy * strength, max(0.15, map_n.z)));
    return normalize(TBN * tn);
}

float rain_hash11(float p) {
    p = fract(p * 0.1031);
    p *= p + 33.33;
    p *= p + p;
    return fract(p);
}
vec3 rain_hash32(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * vec3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yxz + 33.33);
    return fract((p3.xxy + p3.yzz) * p3.zyx);
}

// Clinging raindrop impact beads strictly bounded inside each cell (zero edge clipping or rectangles).
// Returns xy = dome normal in uv space (+u right/aft, +v down), z = drop mask, w = dome relief.
vec4 rain_static_beads(vec2 uv, float scale, float t, float dens, vec2 dir, float stretch) {
    vec2 st = uv * scale;
    // Smooth quad derivative BEFORE floor/branch so cell boundaries never trigger fwidth spikes!
    float px_cell = max(length(vec2(dFdx(st.x), dFdy(st.x))), length(vec2(dFdx(st.y), dFdy(st.y))));
    float aa = clamp(px_cell * 1.15, 0.010, 0.055);
    vec2 id = floor(st);
    vec2 gv = fract(st) - 0.5;
    vec2 perp = vec2(-dir.y, dir.x);

    vec3 h = rain_hash32(id + vec2(scale * 7.31, 1.93));
    if (h.x > dens) return vec4(0.0);

    float phase = t * mix(0.85, 1.85, h.z) + h.y * 31.7;
    float epoch = floor(phase);
    float c = fract(phase);
    vec3 rnd = rain_hash32(id + vec2(epoch * 13.7, epoch * 29.1));
    if (rnd.x > dens) return vec4(0.0);

    float fade = smoothstep(0.0, 0.04, c) * smoothstep(1.0, 0.58, c);
    if (fade < 0.01) return vec4(0.0);

    // Offset + radius * stretch <= 0.43 < 0.50: mathematically cannot clip at cell borders!
    vec2 p0 = (rnd.yz - 0.5) * 0.46 + perp * (sin(c * 22.0 + h.z * 6.2831) * 0.014 * smoothstep(0.0, 0.2, c));
    float r = mix(0.064, 0.118, h.z * h.z) * (0.66 + 0.34 * smoothstep(0.0, 0.14, c));

    vec2 d = gv - p0;
    vec2 d_loc = vec2(dot(d, perp), dot(d, dir) / stretch);
    float dist = length(d_loc);
    float m = smoothstep(r + aa, r - aa, dist) * fade;
    vec2 n_loc = clamp(d_loc / max(r, 1e-4), vec2(-1.0), vec2(1.0));
    vec2 n_uv = (perp * n_loc.x + dir * n_loc.y) * m;
    return vec4(n_uv, m, sqrt(max(0.0, 1.0 - dot(n_loc, n_loc))) * m);
}

// Seamless 3x3-neighborhood flowing teardrop droplets driven by integrated slipstream + gravity flow.
// Returns xy = dome normal in uv space, z = teardrop mask, w = dome relief.
vec4 rain_rivulet_layer(vec2 uv, vec2 flow_shift, vec2 dir, float scale, float dens, float t, float speed01) {
    vec2 st = uv * scale - flow_shift * scale;
    // Continuous screen-space pixel width in cell units (immune to floor() quads and early-continue!).
    float px_cell = max(length(vec2(dFdx(st.x), dFdy(st.x))), length(vec2(dFdx(st.y), dFdy(st.y))));
    float aa = clamp(px_cell * 1.15, 0.010, 0.055);
    vec2 id = floor(st);
    vec2 gv = fract(st) - 0.5;
    vec2 perp = vec2(-dir.y, dir.x);

    vec2 best_n = vec2(0.0);
    float best_m = 0.0;
    float best_h = 0.0;
    float tail_len = mix(0.14, 0.48, speed01);

    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            vec2 off = vec2(float(dx), float(dy));
            vec2 cid = id + off;
            vec2 lgv = gv - off;
            vec3 h = rain_hash32(cid + vec2(scale * 3.17, 11.43));
            if (h.x > dens) continue;

            float wobble = 0.036 * sin(dot(cid, dir) * 2.1 + t * (2.6 + 1.8 * h.y) + h.z * 6.2831);
            vec2 p0 = (h.yz - 0.5) * 0.52 + perp * wobble;
            float r = mix(0.044, 0.086, h.z * h.z);

            vec2 d = lgv - p0;
            float along = dot(d, dir);
            float across = dot(d, perp);

            float dist_uv;
            vec2 n_loc;
            float m_fade = 1.0;
            float aa_eff = aa;
            if (along >= 0.0) {
                float head_y = along / 1.15;
                dist_uv = length(vec2(across, head_y));
                n_loc = clamp(vec2(across, head_y) / max(r, 1e-4), vec2(-1.0), vec2(1.0));
            } else {
                float u = clamp(-along / tail_len, 0.0, 1.0);
                float tail_w = max(0.22, (1.0 - 0.78 * u * u) * (1.0 - 0.10 * u));
                float end_cap = max(0.0, (-along) - tail_len);
                dist_uv = length(vec2(across, end_cap * tail_w));
                float r_eff = r * tail_w;
                float m_tail = smoothstep(r_eff + aa_eff * 0.65, max(0.0, r_eff - aa_eff * 0.65), dist_uv);
                float n_y = (end_cap > 1e-4) ? (-end_cap / max(r, 1e-4)) : (-0.32 * (1.0 - u));
                n_loc = clamp(vec2(across / max(r_eff, 1e-4), n_y), vec2(-1.0), vec2(1.0));
                float m = m_tail * (1.0 - 0.55 * u * u) * smoothstep(tail_len * 0.94, tail_len * 0.35, -along);
                if (m > best_m) {
                    best_m = m;
                    best_n = (perp * n_loc.x + dir * n_loc.y) * m;
                    best_h = sqrt(max(0.0, 1.0 - dot(n_loc, n_loc) * 0.7)) * m;
                }
                continue;
            }

            float m = smoothstep(r + aa_eff, r - aa_eff, dist_uv) * m_fade;
            if (m > best_m) {
                best_m = m;
                best_n = (perp * n_loc.x + dir * n_loc.y) * m;
                best_h = sqrt(max(0.0, 1.0 - dot(n_loc, n_loc) * 0.7)) * m;
            }
        }
    }
    return vec4(best_n, best_m, best_h);
}

// Fast zero-trig tactile micro-grain for vinyl/leather & bead-blasted aluminum panel finish (~4 ALU ops!)
float cabin_hash3(vec3 p) {
    vec3 q = fract(p * vec3(0.1031, 0.1030, 0.0973));
    q += dot(q, q.yxz + 33.33);
    return fract((q.x + q.y) * q.z);
}
float cabin_grain(vec3 p) {
    vec3 i = floor(p);
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float n0 = mix(cabin_hash3(i), cabin_hash3(i + vec3(1.0, 0.0, 0.0)), f.x);
    float n1 = mix(cabin_hash3(i + vec3(0.0, 1.0, 0.0)), cabin_hash3(i + vec3(1.0, 1.0, 0.0)), f.x);
    return mix(n0, n1, f.y);
}

// Procedural glass spiderweb shatter + crazing field anchored at impact focal point.
float glass_crack_mask(vec2 uv, float crack, bool front_glass) {
    if (crack < 0.02) return 0.0;
    vec2 orig = front_glass ? vec2(0.42, 0.44) : vec2(0.48, 0.46);
    vec2 d = (uv - orig) * (front_glass ? vec2(1.15, 1.35) : vec2(1.0, 1.25));
    float rad = length(d);
    float reach = mix(0.08, 0.85, sqrt(crack));
    if (rad > reach) return 0.0;
    float env = 1.0 - smoothstep(reach * 0.45, reach, rad);
    float ang = atan(d.y, d.x);
    float n1 = cabin_grain(vec3(d * 19.0, 3.1));
    float n2 = cabin_grain(vec3(d * 48.0, 8.7));
    float rays = abs(fract((ang / 6.2831853) * 13.0 + (n1 - 0.5) * 0.48 + rad * 1.4) - 0.5) * 2.0;
    float ray_line = 1.0 - smoothstep(0.0, mix(0.045, 0.12, crack) / max(0.08, rad * 3.2), rays);
    float ring_pos = rad * (14.0 + crack * 8.0) + (n2 - 0.5) * 1.35;
    float rings = abs(fract(ring_pos) - 0.5) * 2.0;
    float ring_line = (1.0 - smoothstep(0.0, 0.085, rings)) * smoothstep(0.04, 0.14, rad);
    float core = (1.0 - smoothstep(0.02, 0.095, rad)) * smoothstep(0.20, 0.90, crack);
    return clamp(max(max(ray_line, ring_line * 0.85), core) * env, 0.0, 1.0);
}

void main() {
    vec4 tex = (u_is_interior > 0.5 && u_aircraft_detail > 0) ? texture(u_albedo_tex, v_uv, -0.25) : texture(u_albedo_tex, v_uv);
    vec3 base = tex.rgb * v_col.rgb * u_base_tint.rgb;
    if (u_livery > 0.5) {
        float side = smoothstep(0.16, 0.27, abs(v_obj_pos.x)) * (1.0 - smoothstep(0.48, 0.58, abs(v_obj_pos.x))) *
                     smoothstep(0.58, 0.82, abs(normalize(v_nrm).x));
        float fuselage = smoothstep(-3.35, -3.00, v_obj_pos.z) * (1.0 - smoothstep(2.80, 3.20, v_obj_pos.z));
        float blue_band = smoothstep(-0.44, -0.34, v_obj_pos.y) * (1.0 - smoothstep(-0.13, -0.03, v_obj_pos.y));
        float red_pin = smoothstep(-0.50, -0.47, v_obj_pos.y) * (1.0 - smoothstep(-0.43, -0.40, v_obj_pos.y));
        float paint = side * fuselage;
        base = mix(base, vec3(0.018, 0.075, 0.17), paint * blue_band * 0.98);
        base = mix(base, vec3(0.82, 0.12, 0.045), paint * red_pin * 0.95);
    }
    float alpha = (u_baked_reflection > 0.5 ? 1.0 : tex.a) * v_col.a * u_base_tint.a * u_alpha_mul;
    // Avoid discard so Hardware Early-Z stays active on opaque cockpit/fuselage passes;
    // all translucent submeshes render in Pass 1 with GL_BLEND + glDepthMask(GL_FALSE).
    if (alpha < 0.015) { frag_color = vec4(0.0); return; }

    // Optical instrument cover glass (GaugeGlass: alpha ~0.09) vs cockpit propeller blur veil (alpha ~0.88).
    if (u_is_interior > 0.5 && u_cabin_glass == 0 && u_unlit == 0 && u_base_tint.a < 0.94) {
        if (u_base_tint.a < 0.25) {
            // Physically simulated dielectric instrument glass (zero texture fetch fast path for Renoir 60 FPS):
            // Fresnel sky-horizon reflection, anti-reflective MgF2 sheen, and directional sun specular catchlight.
            vec3 N = normalize(v_nrm);
            vec3 V = normalize(-v_world_pos);
            if (dot(N, V) < 0.0) N = -N;
            vec3 L = normalize(u_sun_dir);
            float ndv = max(0.0, dot(N, V));
            float omv = 1.0 - ndv;
            float omv2 = omv * omv;
            float daylight = smoothstep(-0.10, 0.16, L.y);
            vec3 R = reflect(-V, N);
            float sky_t = clamp(R.y * 0.5 + 0.5, 0.0, 1.0);
            vec3 env_refl = mix(u_haze_warm * 0.52, u_haze_cool * 1.25, smoothstep(0.30, 0.85, sky_t));
            float fresnel = 0.032 + 0.32 * (omv2 * omv2);
            vec3 H = normalize(V + L);
            float sun_glint = pow(max(0.0, dot(N, H)), 128.0) * daylight * smoothstep(0.0, 0.25, dot(N, L));
            vec3 ar_tint = vec3(0.46, 0.62, 0.78) * (0.04 + 0.06 * daylight);
            vec3 glass_c = env_refl * (0.34 + 0.56 * daylight) + ar_tint + vec3(1.15, 1.12, 1.05) * (sun_glint * 0.90);
            float out_a = clamp(fresnel * (0.28 + 0.44 * daylight) + sun_glint * 0.28, 0.016, 0.30);
            frag_color = vec4(u_native_output != 0 ? sqrt(max(glass_c, vec3(0.0))) : glass_c, out_a);
            return;
        }
        float daylight = smoothstep(-0.10, 0.14, u_sun_dir.y);
        vec3 prop_c = base * mix(vec3(0.12, 0.14, 0.18), vec3(0.90, 0.92, 0.95), daylight);
        frag_color = vec4(native_display_color(prop_c), alpha);
        return;
    }

    if (u_unlit != 0) {
        // Cycles baked diffuse and occlusion enhanced with crisp object-space micro-grain
        // (leather/anodized panel finish) + live sun-angle response as the aircraft banks & turns!
        vec3 sdir = normalize(u_sun_dir);
        float daylight = smoothstep(-0.14, 0.10, sdir.y);
        vec3 N = normalize(v_nrm);
        vec3 V = normalize(-v_world_pos);
        if (dot(N, V) < 0.0) N = -N;
        vec3 cabin_scale = mix(vec3(0.12, 0.13, 0.16), vec3(1.0), daylight);
        vec3 baked_color = base * mix(vec3(1.0), cabin_scale, u_is_interior);
        if (u_is_interior > 0.5) {
            // Subtle high-frequency vinyl/leather micro-pebble grain (zero coarse 5mm blotches on close trim).
            float g_micro = cabin_grain(v_obj_pos * 1350.0) * 0.65 + cabin_grain(v_obj_pos * 480.0) * 0.35;
            float micro = 0.985 + 0.032 * g_micro;
            float sun_resp = mix(1.0, 0.86 + 0.28 * max(0.0, dot(N, sdir)), daylight);
            vec3 sun_tint = mix(vec3(1.0), mix(vec3(1.07, 0.93, 0.80), vec3(1.0, 0.99, 0.97), clamp(sdir.y / 0.35, 0.0, 1.0)), daylight * 0.50);
            baked_color *= micro * sun_resp * sun_tint;
            // Subtle crisp specular highlight on glareshield & anodized bezels
            vec3 H = normalize(V + sdir);
            float ndh = max(0.0, dot(N, H));
            float s2 = ndh * ndh; float s4 = s2 * s2; float s8 = s4 * s4; float s16 = s8 * s8;
            float rim_spec = (s16 * s16) * daylight * clamp(tex.a, 0.08, 0.65);
            baked_color += vec3(0.95, 0.93, 0.88) * rim_spec * 0.18;
            // Gentle warm panel floodlight at dusk/night + storm lightning cabin bounce
            float night_t = clamp((0.08 - sdir.y) / 0.14, 0.0, 1.0);
            baked_color += base * vec3(0.45, 0.22, 0.08) * (night_t * 0.65);
            baked_color += base * vec3(0.55, 0.68, 0.96) * (u_plane_wx.y * 0.48);
        }
        if (u_baked_reflection > 0.5 && (u_is_interior < 0.5 || u_aircraft_detail != 0)) {
            float ndv = max(0.0, dot(N, V));
            float reflect_mask = clamp(tex.a, 0.025, 0.88);
            if (reflect_mask > (u_aircraft_detail != 0 ? 0.20 : 0.32)) {
                vec3 R = reflect(-V, N);
                float roughness = mix(mix(0.88, 0.96, u_is_interior), mix(0.26, 0.68, u_is_interior), reflect_mask);
                vec3 environment;
                if (u_is_interior > 0.5) {
                    float sky_t = clamp(R.y * 0.5 + 0.5, 0.0, 1.0);
                    vec3 sky_env = mix(u_haze_warm * 0.45, u_haze_cool * 1.15, smoothstep(0.30, 0.85, sky_t));
                    environment = mix(vec3(0.08, 0.09, 0.11), sky_env, daylight) * (0.35 + 0.55 * (1.0 - roughness));
                } else {
                    vec2 ruv = vec2(atan(R.z, R.x) * (0.5 / 3.14159265) + 0.5,
                                    asin(clamp(R.y, -1.0, 1.0)) * (1.0 / 3.14159265) + 0.5);
                    environment = min(textureLod(u_refl_env, ruv, roughness * 5.2).rgb, vec3(4.0));
                }
                float omv = 1.0 - ndv;
                float omv2 = omv * omv;
                float fresnel = 0.04 + mix(0.50, 0.22, u_is_interior) * (omv2 * omv2 * omv);
                float reflection_gain = mix(0.20 + fresnel * 1.2, 0.035 + fresnel * 0.30, u_is_interior);
                baked_color += environment * reflect_mask * reflection_gain;
            }
        }
        frag_color = vec4(native_display_color(baked_color), alpha);
        return;
    }
    if (u_cabin_glass != 0) {
        vec3 N = normalize(v_nrm);
        vec3 V = normalize(-v_world_pos);
        if (dot(N, V) < 0.0) N = -N;
        float rain_mm = max(0.0, u_glass_weather.x);
        float wet = smoothstep(0.03, 1.8, rain_mm);
        float heavy_rain = smoothstep(3.5, 22.0, rain_mm);
        float frost_raw = clamp(u_glass_fx.x, 0.0, 1.0);
        float mist_raw  = clamp(u_glass_fx.y, 0.0, 1.0);
        float grime_raw = clamp(u_glass_fx.z, 0.0, 1.0);
        float snow_fac  = clamp(u_glass_fx.w, 0.0, 1.0);
        float flash     = clamp(u_plane_wx.y, 0.0, 1.0);
        float crack_raw = clamp(u_dmg_extra.y, 0.0, 1.0);

        // Dry, clean, ice-free, uncracked optical acrylic glass fast path (zero texture fetches / zero AgX-LUT
        // across 1080p windshield pixels, keeping >=60 FPS on Renoir while rendering Fresnel sky + sun + dash bounce).
        if (wet < 0.005 && frost_raw < 0.005 && mist_raw < 0.005 && grime_raw < 0.025 && crack_raw < 0.02) {
            float ndv = max(0.0, dot(N, V));
            float omv = 1.0 - ndv;
            float omv2 = omv * omv;
            float fresnel = 0.032 + 0.30 * (omv2 * omv2);
            vec3 L = normalize(u_sun_dir);
            float daylight = smoothstep(-0.12, 0.16, L.y);
            vec3 R = reflect(-V, N);
            float sky_t = clamp(R.y * 0.5 + 0.5, 0.0, 1.0);
            vec3 sky_env = mix(u_haze_warm * 0.45, u_haze_cool * 1.15, smoothstep(0.35, 0.85, sky_t));
            float sun_fwd = pow(max(0.0, dot(-V, L)), 12.0) * daylight;
            float dash_bounce = (u_cabin_glass == 2) ? ((1.0 - smoothstep(0.04, 0.48, v_uv.y)) * 0.028 * daylight) : 0.0;
            vec3 dry_col = sky_env * (0.28 + 0.72 * daylight)
                         + vec3(1.05, 0.96, 0.84) * (sun_fwd * 0.26)
                         + vec3(0.18, 0.20, 0.24) * (dash_bounce * 7.5);
            float glass_alpha = clamp(fresnel * (0.14 + 0.16 * daylight) + sun_fwd * 0.04 + dash_bounce, 0.0, 0.22);
            if (glass_alpha < 0.012) { frag_color = vec4(0.0); return; }
            frag_color = vec4(u_native_output != 0 ? sqrt(max(dry_col, vec3(0.0))) : dry_col, glass_alpha);
            return;
        }

        float sim_time = u_glass_weather.y;
        bool front_glass = (u_cabin_glass == 2);

        // 1. Procedural optical water beads & attitude/speed-coupled teardrops (zero grid/cell rectangles!).
        vec2 drop_nrm = vec2(0.0);
        float drop_mask = 0.0;
        float drop_relief = 0.0;
        bool baked_rain = u_aircraft_detail == 0 && (u_rain_luts & (front_glass ? 1 : 2)) != 0;
        if (wet > 0.005 && baked_rain) {
            // The relief volume stores normals in RG, dome height in B and
            // coverage in A. Advect the larger drops with the live pane flow.
            float rate_slice = rain_mm < 2.5 ? smoothstep(0.35, 2.5, rain_mm) :
                               rain_mm < 12.0 ? 1.0 + smoothstep(2.5, 12.0, rain_mm) :
                                                2.0 + smoothstep(12.0, 45.0, rain_mm);
            vec2 flow = front_glass ? u_glass_flow_front.xy : u_glass_flow_side.xy;
            vec2 uv = vec2(v_uv.x, v_uv.y) * (front_glass ? vec2(1.25, 0.95) : vec2(0.95, 0.76)) - flow;
            vec3 rain_uv = vec3(uv, (rate_slice + 0.5) * 0.25);
            vec4 rain = front_glass ? textureLod(u_windscreen_rain_front, rain_uv, 0.0) :
                                      textureLod(u_windscreen_rain_side, rain_uv, 0.0);
            drop_mask = smoothstep(0.12, 0.60, rain.a) * wet;
            drop_nrm = (rain.rg * 2.0 - 1.0) * vec2(1.0, -1.0) * drop_mask;
            drop_relief = rain.b * wet;
        } else if (wet > 0.005) {
            float rain_dens = clamp(0.24 + 0.48 * wet + 0.26 * heavy_rain, 0.0, 0.94);
            vec4 flow_in = front_glass ? u_glass_flow_front : u_glass_flow_side;
            vec2 flow_vel = flow_in.zw;
            float vel_mag = length(flow_vel);
            float speed01 = smoothstep(0.10, 0.85, vel_mag);
            vec2 flow_dir = (vel_mag > 1e-4) ? (flow_vel / vel_mag) : vec2(0.0, 1.0);
            if (front_glass) {
                // Aerodynamic outward radial push around curved windshield sides when airflow exceeds gravity
                float up_flow = clamp(-flow_vel.y, 0.0, 1.5);
                flow_dir = normalize(flow_dir + vec2((v_uv.x - 0.5) * 0.42 * up_flow, 0.0));
            }
            float stretch = mix(1.04, 1.42, speed01);
            vec2 base_uv = front_glass ? vec2(v_uv.x * 1.45, v_uv.y) : vec2(v_uv.x * 1.35, v_uv.y);
            vec2 base_shift = front_glass ? vec2(flow_in.x * 1.45, flow_in.y) : vec2(flow_in.x * 1.35, flow_in.y);

            float imp_dens = rain_dens * mix(0.86, 0.62, speed01);
            vec4 st0 = rain_static_beads(base_uv, front_glass ? 21.0 : 23.0, sim_time, imp_dens, flow_dir, stretch);
            vec4 st1 = rain_static_beads(base_uv + vec2(0.37, 0.61), front_glass ? 36.0 : 38.0, sim_time * 1.24 + 3.1, imp_dens * 0.90, flow_dir, stretch);
            vec4 rv0 = rain_rivulet_layer(base_uv, base_shift, flow_dir, front_glass ? 12.5 : 14.0, rain_dens * mix(0.42, 0.72, speed01), sim_time, speed01);
            vec4 rv1 = rain_rivulet_layer(base_uv + vec2(0.41, 0.23), base_shift * 0.72, flow_dir, front_glass ? 19.5 : 21.5, rain_dens * mix(0.30, 0.58, speed01), sim_time * 1.18 + 2.3, speed01);

            float m_st = max(st0.z, st1.z * 0.85);
            vec2 n_st = mix(st1.xy * 0.84, st0.xy, st0.z);
            float m_rv = max(rv0.z, rv1.z * 0.86);
            vec2 n_rv = mix(rv1.xy * 0.85, rv0.xy, rv0.z);

            drop_mask = clamp(max(m_st, m_rv), 0.0, 1.0) * wet;
            vec2 n_uv = mix(n_st, n_rv, clamp(m_rv * 1.20, 0.0, 1.0));
            drop_nrm = vec2(n_uv.x, -n_uv.y);
            drop_relief = clamp(max(max(st0.w, st1.w * 0.75), max(rv0.w, rv1.w * 0.80)), 0.0, 1.0) * wet;
        }

        // 2. Crystalline Frost / Rime Ice creeping inward from A-pillars & top frame, cleared by lower-center defroster!
        vec2 edge_d = abs(v_uv - vec2(0.5));
        float frame_edge = smoothstep(0.12, 0.49, max(edge_d.x * 1.04, edge_d.y * 1.04));
        // Pilot-side warm air defroster plume clears the lower-center horizon sightline (v_uv.y ~ 0.62)
        float defrost_zone = 1.0 - smoothstep(0.18, 0.42, length((v_uv - vec2(0.46, 0.62)) * vec2(1.25, 1.55)));
        float defrost_melt = clamp(u_plane_wx.z, 0.0, 1.0) * defrost_zone * (front_glass ? 0.96 : 0.25);
        float frost_mask = 0.0;
        float crystal_sparkle = 0.0;
        if (frost_raw > 0.005) {
            float c1 = cabin_grain(vec3(v_uv * 16.0, 1.3));
            float c2 = cabin_grain(vec3(v_uv * 54.0, 4.7));
            float c3 = cabin_grain(vec3(v_uv * 160.0, 9.1));
            float dendrite = 1.0 - abs(c2 * 2.0 - 1.0);
            float crystal = c1 * 0.40 + dendrite * 0.40 + c3 * 0.20;
            float reach = mix(0.26, 1.28, sqrt(frost_raw)) * (0.28 + 1.08 * frame_edge) - defrost_melt * 0.92;
            frost_mask = smoothstep(0.38, 0.78, reach + (crystal - 0.48) * 0.62) * smoothstep(0.02, 0.45, frost_raw);
            crystal_sparkle = smoothstep(0.70, 0.94, c3 * 0.6 + dendrite * 0.4) * frost_mask;
        }

        // 3. Cold-morning cabin condensation mist + persistent bug/rain grime
        float mist_mask = mist_raw * (0.35 + 0.65 * frame_edge) * (1.0 - defrost_melt * 0.85) * 0.30;
        float grime_mask = 0.0;
        if (grime_raw > 0.025) {
            float g_speck = cabin_grain(vec3(v_uv * 72.0, 11.4)) * cabin_grain(vec3(v_uv * 22.0, 2.8));
            grime_mask = (smoothstep(0.48, 0.84, g_speck) * 0.20 + 0.03 * frame_edge) * grime_raw;
        }
        float crack_mask = glass_crack_mask(v_uv, crack_raw, front_glass);

        float fresnel = pow(1.0 - max(0.0, dot(N, V)), 4.5);
        float drop_alpha = smoothstep(0.02, 0.35, drop_mask);
        // Smooth continuous optical water sheen without cell-clipped wipe tiles
        float film_alpha = front_glass && heavy_rain > 0.01 ?
            heavy_rain * 0.065 * (0.75 + 0.25 * cabin_grain(v_obj_pos * 12.0)) : 0.0;
        float non_drop_alpha = fresnel * 0.055 + frost_mask * 0.80 + mist_mask + grime_mask + crack_mask * 0.72;
        float glass_alpha = clamp(max(max(drop_alpha, film_alpha), non_drop_alpha), 0.0, 0.96);
        if (glass_alpha < 0.012) { frag_color = vec4(0.0); return; }

        float daylight = smoothstep(-0.10, 0.16, u_sun_dir.y);
        vec3 glass_color;
        if (u_native_output != 0 && (drop_alpha > 0.02 || film_alpha > 0.02)) {
            float slope_mag = clamp(length(drop_nrm), 0.0, 1.0);
            float dome_z = sqrt(max(0.08, 1.0 - slope_mag * slope_mag * 0.86));
            vec3 bead_n = normalize(vec3(drop_nrm * 0.92, dome_z));
            float water_fresnel = (0.025 + 0.975 * pow(1.0 - dome_z, 4.0)) * drop_alpha;
            float spec = pow(max(0.0, dot(bead_n, normalize(vec3(0.24, 0.62, 0.75)))), 36.0) * drop_alpha;
            float sky_arc = pow(max(0.0, dot(bead_n, normalize(vec3(-0.15, 0.85, 0.50)))), 5.0) * smoothstep(0.35, 0.92, slope_mag) * drop_alpha;
            float meniscus_dark = smoothstep(0.64, 0.96, slope_mag) * 0.34 * drop_alpha;
            vec3 sky_refl = mix(vec3(0.22, 0.28, 0.36), mix(u_haze_cool, vec3(0.92, 0.96, 1.0), 0.45), daylight);
            vec3 spec_col = mix(vec3(0.55, 0.65, 0.78), vec3(1.45, 1.48, 1.52), daylight);
            vec3 water_hdr;
            float out_drop_a;
            {
                // True optical HDR convex-lens refraction across both front windshield & side cabin glazing!
                vec2 screen_uv = clamp((v_clip_pos.xy / max(1e-5, v_clip_pos.w)) * 0.5 + 0.5, vec2(0.002), vec2(0.998));
                float edge_guard = smoothstep(0.0, 0.035, min(min(screen_uv.x, 1.0 - screen_uv.x), min(screen_uv.y, 1.0 - screen_uv.y)));
                vec2 refr_scale = front_glass ? vec2(0.030, 0.046) : vec2(0.022, 0.034);
                vec2 refr_uv = clamp(screen_uv - drop_nrm * refr_scale * (0.55 + 0.65 * drop_relief) * edge_guard,
                                     vec2(0.002), vec2(0.998));
                vec3 refr_hdr = textureLod(u_backdrop_tex, refr_uv, 0.0).rgb;
                if (u_aircraft_detail != 0 && film_alpha > 0.02 && drop_alpha < 0.85) {
                    vec2 blur_r = vec2(0.0020, 0.0034) * edge_guard;
                    vec3 film_hdr = 0.25 * (
                        textureLod(u_backdrop_tex, clamp(screen_uv + vec2(blur_r.x, blur_r.y), vec2(0.002), vec2(0.998)), 0.0).rgb +
                        textureLod(u_backdrop_tex, clamp(screen_uv + vec2(-blur_r.x, blur_r.y), vec2(0.002), vec2(0.998)), 0.0).rgb +
                        textureLod(u_backdrop_tex, clamp(screen_uv + vec2(blur_r.x, -blur_r.y), vec2(0.002), vec2(0.998)), 0.0).rgb +
                        textureLod(u_backdrop_tex, clamp(screen_uv + vec2(-blur_r.x, -blur_r.y), vec2(0.002), vec2(0.998)), 0.0).rgb);
                    refr_hdr = mix(film_hdr, refr_hdr, drop_alpha);
                }
                water_hdr = refr_hdr * (1.0 - meniscus_dark)
                          + sky_refl * (water_fresnel * 0.52 + sky_arc * 0.22)
                          + spec_col * (spec * 0.85)
                          + vec3(0.68, 0.84, 1.20) * (flash * (0.16 * drop_alpha + spec * 0.90));
                out_drop_a = max(drop_alpha, film_alpha);
            }
            water_hdr = mix(water_hdr, vec3(0.84, 0.90, 0.97) * (0.30 + 0.72 * daylight), snow_fac * 0.28 * drop_alpha);

            vec3 frost_col = mix(vec3(0.80, 0.89, 0.98), vec3(1.12, 1.16, 1.20), crystal_sparkle) * (0.25 + 0.88 * daylight)
                           + vec3(0.55, 0.70, 0.95) * (flash * 0.30);
            vec3 mist_col  = mix(vec3(0.64, 0.72, 0.80), u_haze_cool * 0.95, 0.45) * (0.20 + 0.78 * daylight);
            vec3 grime_col = vec3(0.32, 0.30, 0.24) * (0.20 + 0.75 * daylight);
            glass_color = mix(water_hdr, frost_col, frost_mask * 0.85);
            glass_color = mix(glass_color, mist_col, mist_mask);
            glass_color = mix(glass_color, grime_col, grime_mask);
            vec3 crack_col = mix(vec3(0.68, 0.76, 0.84), vec3(1.18, 1.22, 1.26), fresnel) * (0.28 + 0.82 * daylight);
            glass_color = mix(glass_color, crack_col, crack_mask * 0.88);
            frag_color = vec4(native_display_color(glass_color), clamp(max(out_drop_a, non_drop_alpha), 0.0, 0.96));
            return;
        }

        // Frost / mist / grime / cracks non-native fallback
        vec3 base_glass = mix(vec3(0.45, 0.56, 0.68), u_haze_cool, 0.65);
        vec3 frost_col = mix(vec3(0.80, 0.89, 0.98), vec3(1.12, 1.16, 1.20), crystal_sparkle) * (0.25 + 0.88 * daylight)
                       + vec3(0.55, 0.70, 0.95) * (flash * 0.30);
        vec3 mist_col  = mix(vec3(0.64, 0.72, 0.80), u_haze_cool * 0.95, 0.45) * (0.20 + 0.78 * daylight);
        vec3 grime_col = vec3(0.32, 0.30, 0.24) * (0.20 + 0.75 * daylight);
        vec3 crack_col = mix(vec3(0.68, 0.76, 0.84), vec3(1.15, 1.20, 1.25), fresnel) * (0.28 + 0.82 * daylight);
        float w_sum = max(0.001, fresnel * 0.055 + frost_mask * 0.80 + mist_mask + grime_mask + crack_mask * 0.72);
        glass_color = (base_glass * (fresnel * 0.055) + frost_col * (frost_mask * 0.80) +
                       mist_col * mist_mask + grime_col * grime_mask + crack_col * (crack_mask * 0.72)) / w_sum;
        if (drop_alpha > 0.02) {
            float rim = clamp(length(drop_nrm) * 1.8, 0.0, 1.0);
            glass_color = mix(glass_color, mix(vec3(0.12), u_haze_cool, rim), drop_alpha * 0.55);
            non_drop_alpha = max(non_drop_alpha, drop_alpha * (0.04 + rim * 0.20));
        }
        frag_color = vec4(native_display_color(glass_color), clamp(non_drop_alpha, 0.0, 0.90));
        return;
    }

    vec3 nrm = normalize(v_nrm);
    if (!gl_FrontFacing) nrm = -nrm;
    if (u_aircraft_detail != 0 && u_pbr_factors.z > 0.5) {
        vec3 mn = texture(u_normal_tex, v_uv).rgb * 2.0 - 1.0;
        nrm = perturb_normal(nrm, v_world_pos, v_uv, mn, mix(0.78, 0.90, u_is_interior));
    }

    float ao = 1.0;
    float rough = clamp(u_pbr_factors.y, 0.05, 0.96);
    float metal = clamp(u_pbr_factors.x, 0.0, 1.0);
    if (u_pbr_factors.w > 0.5) {
        vec3 orm = texture(u_mr_tex, v_uv).rgb;
        ao = clamp(orm.r, 0.22, 1.0);
        rough = clamp(orm.g * u_pbr_factors.y * 1.15, 0.05, 0.96);
        metal = clamp(orm.b * max(0.65, u_pbr_factors.x * 2.0), 0.0, 1.0);
    }
    // Resolution-independent 4K-equivalent object-space OpenSky aviation PBR livery, panel seams & rivets!
    if (u_livery > 0.5 && u_is_interior < 0.5) {
        vec3 p = v_ac_pos; // X=right, Y=up, Z=aft (nose ~ -3.7, tail ~ +3.7, wings +/-5.5)
        float ax = abs(p.x);
        float z = p.z;
        float y = p.y;
        float ao_base = clamp(dot(base, vec3(0.3333)), 0.55, 1.0);

        // Crisp Matterhorn Pearl White clearcoat base
        vec3 paint = vec3(0.92, 0.93, 0.95) * ao_base;
        float liv_rough = 0.17;
        float liv_metal = 0.07;

        vec3 navy = vec3(0.032, 0.078, 0.175) * ao_base;
        vec3 crimson = vec3(0.56, 0.045, 0.055) * ao_base;
        vec3 gold = vec3(0.68, 0.50, 0.16) * ao_base;

        // 1. Fuselage & Vertical Tail swept cheatline (Midnight Navy + Crimson Accent + Metallic Champagne Gold)
        float on_body = smoothstep(1.02, 0.76, ax) * smoothstep(-3.60, -3.25, z) * smoothstep(3.75, 3.35, z);
        float curve_y = -0.30 + 0.07 * sin(clamp((z + 2.8) * 0.65, 0.0, 3.6))
                      + smoothstep(1.35, 3.25, z) * 0.62;
        float dy = y - curve_y;
        float fw_y = max(0.005, fwidth(dy) * 1.25);

        float band_navy = smoothstep(-0.22 - fw_y, -0.22 + fw_y, dy) * smoothstep(0.02 + fw_y, 0.02 - fw_y, dy) * on_body
                        * smoothstep(-3.42, -3.34, z);
        float band_crim = smoothstep(0.035 - fw_y, 0.035 + fw_y, dy) * smoothstep(0.115 + fw_y, 0.115 - fw_y, dy) * on_body
                        * smoothstep(-3.16, -3.08, z);
        float band_gold = smoothstep(0.014 - fw_y, 0.014 + fw_y, dy) * smoothstep(0.034 + fw_y, 0.034 - fw_y, dy) * on_body
                        * smoothstep(-3.36, -3.28, z);

        // Tail fin sweeping crimson/navy flash
        float on_fin = smoothstep(0.28, 0.14, ax) * smoothstep(1.85, 2.25, z) * smoothstep(0.12, 0.35, y);
        float fin_line = (y - 0.18) - (z - 2.15) * 0.72;
        float fw_f = max(0.006, fwidth(fin_line) * 1.25);
        float fin_navy = smoothstep(-0.42 - fw_f, -0.42 + fw_f, fin_line) * smoothstep(-0.08 + fw_f, -0.08 - fw_f, fin_line) * on_fin;
        float fin_crim = smoothstep(-0.06 - fw_f, -0.06 + fw_f, fin_line) * smoothstep( 0.16 + fw_f,  0.16 - fw_f, fin_line) * on_fin;

        // Wingtip & Horizontal Stabilizer Navy + Gold aerodynamic tip bands
        float on_wing = smoothstep(0.75, 0.95, ax) * smoothstep(0.10, 0.40, y) * smoothstep(1.20, 0.80, z);
        float wt_navy = smoothstep(4.78, 4.82, ax) * on_wing;
        float wt_gold = smoothstep(4.68, 4.70, ax) * smoothstep(4.78, 4.76, ax) * on_wing;
        float stab_tip = smoothstep(1.25, 1.32, ax) * smoothstep(2.30, 2.55, z) * smoothstep(0.25, 0.05, abs(y - 0.15));

        // Wheel-pant sleek lower navy fairing stripe
        float on_pant = smoothstep(-0.72, -0.82, y);
        float pant_navy = smoothstep(-1.02, -1.06, y) * on_pant;
        float pant_gold = smoothstep(-0.98, -1.00, y) * smoothstep(-1.04, -1.02, y) * on_pant;

        // Matte anti-glare charcoal top cowling in front of windshield
        float glare_pad = smoothstep(-2.05, -2.18, z) * smoothstep(-3.60, -3.42, z)
                        * smoothstep(0.08, 0.16, y) * smoothstep(0.42, 0.34, ax) * smoothstep(0.35, 0.65, nrm.y);

        float m_navy = clamp(max(max(band_navy, fin_navy), max(max(wt_navy, stab_tip), pant_navy)), 0.0, 1.0);
        float m_crim = clamp(max(band_crim, fin_crim), 0.0, 1.0);
        float m_gold = clamp(max(max(band_gold, wt_gold), pant_gold), 0.0, 1.0);

        paint = mix(paint, navy, m_navy);
        paint = mix(paint, crimson, m_crim);
        paint = mix(paint, gold, m_gold);
        liv_rough = mix(liv_rough, 0.15, max(m_navy, m_crim));
        liv_metal = mix(liv_metal, 0.72, m_gold);
        liv_rough = mix(liv_rough, 0.22, m_gold);

        paint = mix(paint, vec3(0.045, 0.048, 0.054) * ao_base, glare_pad);
        liv_rough = mix(liv_rough, 0.78, glare_pad);

        // Crisp anti-aliased riveted aluminum sheet-metal panel seams (zero pixelation at walkaround distance!)
        float pz_grid = abs(fract(z / 0.64) - 0.5) * 0.64;
        float px_grid = abs(fract(ax / 0.58) - 0.5) * 0.58;
        float fw_z = max(0.0025, fwidth(z) * 1.15);
        float fw_x = max(0.0025, fwidth(ax) * 1.15);
        float seam_z = 1.0 - smoothstep(0.0025, 0.0025 + fw_z, pz_grid);
        float seam_x = (1.0 - smoothstep(0.0025, 0.0025 + fw_x, px_grid)) * smoothstep(0.75, 0.95, ax);
        float hinge_z = (1.0 - smoothstep(0.004, 0.004 + fw_z, abs(z - (-0.82)))) * on_wing;
        float cowl_z = (1.0 - smoothstep(0.0035, 0.0035 + fw_z, abs(z - (-2.24)))) * on_body;
        float seam = clamp(max(max(seam_z * 0.45, seam_x * 0.45), max(hinge_z * 0.85, cowl_z * 0.75)), 0.0, 0.85);
        // Subtle rivet dimples running alongside panel seams
        float riv_z = smoothstep(0.018, 0.008, abs(pz_grid - 0.022)) *
                      (1.0 - smoothstep(0.003, 0.006, abs(fract(y * 16.0) - 0.5) / 16.0)) *
                      smoothstep(0.012, 0.004, fw_z);
        paint *= (1.0 - seam * 0.34 - riv_z * 0.14);
        liv_rough = clamp(liv_rough + seam * 0.25, 0.08, 0.94);

        base = paint;
        rough = liv_rough;
        metal = liv_metal;
    }

    // Exterior / visible high-wing & cowling rime ice + wet rain sheen!
    float exposed_part = (u_is_interior < 0.5 || abs(v_obj_pos.x) > 0.65 || v_obj_pos.z < -0.95) ? 1.0 : 0.0;
    if (exposed_part > 0.5) {
        if (u_plane_wx.w > 0.02) {
            rough = mix(rough, max(0.08, rough * 0.42), u_plane_wx.w * 0.72);
        }
        if (u_plane_wx.x > 0.01) {
            // Heading-independent rime accretion on upper surfaces, nose cowling & forward wing leading edges
            float fwd_edge = smoothstep(0.45, -1.85, v_obj_pos.z);
            float leading = clamp(max(0.0, nrm.y) * 0.60 + fwd_edge * 0.55, 0.0, 1.0);
            float rime_n = cabin_grain(v_obj_pos * 18.0) * 0.6 + cabin_grain(v_obj_pos * 54.0) * 0.4;
            float rime = smoothstep(0.16, 0.78, u_plane_wx.x * (0.55 + 0.95 * leading) + (rime_n - 0.5) * 0.32) * smoothstep(0.02, 0.55, u_plane_wx.x);
            base = mix(base, vec3(0.88, 0.93, 0.98), rime * 0.88);
            rough = mix(rough, 0.82, rime * 0.88);
            metal *= (1.0 - rime * 0.88);
        }
        // Procedural exterior structural scratches, torn paint to bare aluminum, and engine scorch/oil soot
        float z_nose = smoothstep(-1.45, -3.45, v_ac_pos.z) * u_dmg_zones.x;
        float z_wl   = smoothstep(-0.70, -4.20, v_ac_pos.x) * u_dmg_zones.y;
        float z_wr   = smoothstep( 0.70,  4.20, v_ac_pos.x) * u_dmg_zones.z;
        float z_tail = smoothstep( 1.75,  3.75, v_ac_pos.z) * u_dmg_zones.w;
        float z_bel  = smoothstep(-0.20, -0.95, v_ac_pos.y) * u_dmg_extra.x * 0.65;
        float zone_d = clamp(z_nose + z_wl + z_wr + z_tail + z_bel, 0.0, 1.0);
        if (zone_d > 0.02) {
            float streak = cabin_grain(vec3(v_ac_pos.xy * 18.0, v_ac_pos.z * 2.2)) * 0.65 +
                           cabin_grain(vec3(v_ac_pos.xy * 44.0, v_ac_pos.z * 5.0)) * 0.35;
            float gouge = smoothstep(0.50 - zone_d * 0.38, 0.72 - zone_d * 0.25, streak);
            float primer_rim = smoothstep(0.42 - zone_d * 0.38, 0.52 - zone_d * 0.34, streak) * (1.0 - gouge);
            vec3 bare_alu = vec3(0.54, 0.56, 0.60) * (0.85 + 0.30 * streak);
            base = mix(base, vec3(0.11, 0.13, 0.10), primer_rim * zone_d * 0.90);
            base = mix(base, bare_alu, gouge * zone_d);
            metal = mix(metal, 0.90, gouge * zone_d);
            rough = mix(rough, 0.28, gouge * zone_d);
        }
        float scorch_amt = clamp(u_dmg_extra.z, 0.0, 1.0);
        if (scorch_amt > 0.02) {
            float cowl_aft = smoothstep(-3.6, -2.2, v_ac_pos.z) * (1.0 - smoothstep(-0.6, 1.6, v_ac_pos.z));
            float exhaust = smoothstep(0.45, -0.55, v_ac_pos.y) * (1.0 - smoothstep(0.35, 0.90, abs(v_ac_pos.x)));
            float soot_n = cabin_grain(vec3(v_ac_pos.xy * 12.0, v_ac_pos.z * 1.5));
            float soot = clamp(cowl_aft * exhaust * scorch_amt * (0.55 + 0.75 * soot_n), 0.0, 0.95);
            base = mix(base, vec3(0.028, 0.024, 0.022), soot);
            rough = mix(rough, 0.94, soot);
            metal *= (1.0 - soot);
        }
    }

    vec3 sdir = normalize(u_sun_dir);
    vec3 V = normalize(-v_world_pos);
    float day_t = clamp((sdir.y + 0.035) / 0.14, 0.0, 1.0);
    float night_t = clamp((0.08 - sdir.y) / 0.14, 0.0, 1.0);
    vec3 ldir = (sdir.y >= -0.025) ? sdir : normalize(vec3(-sdir.x, 0.55, -sdir.z));

    float sun_scale = mix(2.40, 1.25, u_is_interior);
    vec3 sun_c = (sdir.y >= -0.025)
        ? mix(vec3(1.0, 0.65, 0.34), vec3(1.0, 0.96, 0.90), clamp(sdir.y / 0.45, 0.0, 1.0)) * mix(0.45, sun_scale, day_t)
        : vec3(0.30, 0.44, 0.72) * 0.34;
    vec3 sky_amb = mix(vec3(0.08, 0.12, 0.22), mix(vec3(0.42, 0.50, 0.62), u_haze_cool * 0.95, 0.55), day_t);
    vec3 gnd_amb = mix(vec3(0.03, 0.03, 0.04), vec3(0.24, 0.21, 0.17), day_t);
    // Directional cabin skylight + subtle twilight amber panel floodlight so leather & bezels stay sculpted
    vec3 amb_c = mix(gnd_amb, sky_amb, nrm.y * 0.5 + 0.5) * mix(0.48, 1.00, u_is_interior);
    if (u_is_interior > 0.5) {
        // Low-strength neutral cockpit fill keeps the molded cabin, seat fabric, and
        // lower panel readable without flattening the gauge shadows.
        amb_c += vec3(0.075, 0.082, 0.090);
        float win_fill = max(0.0, nrm.y * 0.55 + nrm.z * 0.45 + 0.35);
        amb_c += vec3(0.16, 0.18, 0.20) * win_fill * day_t;
        amb_c += vec3(0.30, 0.15, 0.065) * max(0.25, dot(nrm, V)) * night_t;
        amb_c += vec3(0.55, 0.70, 1.00) * (u_plane_wx.y * 0.65);
    } else if (u_plane_wx.y > 0.01) {
        amb_c += vec3(0.50, 0.66, 0.95) * (u_plane_wx.y * 0.55);
    }

    // Cook-Torrance GGX PBR + Glossy Aviation Clearcoat & Canopy Sky Reflection!
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
    vec3 F0 = mix(vec3(0.04), base, metal);
    vec3 F = F0 + (1.0 - F0) * pow(1.0 - max(0.0, dot(H, V)), 5.0);
    vec3 spec = clamp((D * G * F) / (4.0 * ndv * max(0.001, ndl) + 1e-4), vec3(0.0), vec3(6.0));
    vec3 kD = (vec3(1.0) - F) * (1.0 - metal);

    vec3 refl = reflect(-V, nrm);
    vec2 ruv = vec2(atan(refl.z, refl.x) * (0.5 / 3.14159265) + 0.5, asin(clamp(refl.y, -1.0, 1.0)) * (1.0 / 3.14159265) + 0.5);
    vec3 ibl_env = textureLod(u_refl_env, ruv, rough * 5.6).rgb;
    vec3 F_ibl = F0 + (max(vec3(1.0 - rough), F0) - F0) * pow(1.0 - ndv, 4.0);
    float ibl_w = mix(0.95, 0.34, u_is_interior);

    // Extra glossy Fresnel reflection + procedural windshield crack on exterior cabin glass (never on interior CockpitPropVeil!)
    float is_glass = (u_is_interior < 0.5 && u_base_tint.a < 0.94 && u_base_tint.a > 0.25) ? 1.0 : 0.0;
    float glass_fres = is_glass * (0.14 + 0.72 * pow(1.0 - ndv, 3.5));
    float ext_crack = (is_glass > 0.5 && u_dmg_extra.y > 0.02) ? glass_crack_mask(v_uv, u_dmg_extra.y, true) : 0.0;

    vec3 lit = kD * base * (amb_c * ao + sun_c * (ndl * ao)) +
               sun_c * (spec * ndl * 0.80) +
               ibl_env * (F_ibl * ibl_w + vec3(glass_fres * 0.95)) +
               base * u_emissive_col;
    lit = mix(lit, vec3(0.85, 0.90, 0.95) * (0.35 + 0.65 * day_t), ext_crack * 0.85);
    frag_color = vec4(native_display_color(lit), clamp(alpha + glass_fres * 0.35 + ext_crack * 0.75, 0.0, 1.0));
}
