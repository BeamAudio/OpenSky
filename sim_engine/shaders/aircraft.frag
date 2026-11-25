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
uniform vec4 u_glass_fx;      // x=frost_ice01, y=cabin_mist01, z=grime01, w=snow_factor01
uniform vec4 u_plane_wx;      // x=airframe_ice01, y=lightning_flash01, z=defrost_clear01, w=wet_sheen01
uniform int u_native_output;
uniform int u_aircraft_detail;
uniform float u_output_exposure;

in vec3 v_world_pos;
in vec3 v_nrm;
in vec4 v_col;
in vec2 v_uv;
in vec3 v_obj_pos;

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

vec4 sample_rain_lut(vec2 uv, float weather_z, bool front_glass) {
    vec3 coord = vec3(uv, weather_z);
    return front_glass ? texture(u_windscreen_rain_front, coord)
                       : texture(u_windscreen_rain_side, coord);
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

void main() {
    // Mild negative mip bias keeps cockpit dials crisp without thrashing 4K texture cache on iGPUs.
    vec4 tex = (u_is_interior > 0.5) ? texture(u_albedo_tex, v_uv, -0.25) : texture(u_albedo_tex, v_uv);
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

    // Fast subtle propeller blur veil in cockpit (skips heavy GGX + IBL over center of windshield!)
    if (u_is_interior > 0.5 && u_cabin_glass == 0 && u_unlit == 0 && u_base_tint.a < 0.94) {
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
            // Single-octave tactile cabin finish cuts 1080p native-cockpit ALU cost on iGPUs.
            float g1 = cabin_grain(v_obj_pos * 220.0);
            float micro = 0.94 + 0.11 * g1;
            float sun_resp = mix(1.0, 0.84 + 0.32 * max(0.0, dot(N, sdir)), daylight);
            vec3 sun_tint = mix(vec3(1.0), mix(vec3(1.08, 0.92, 0.78), vec3(1.0, 0.99, 0.97), clamp(sdir.y / 0.35, 0.0, 1.0)), daylight * 0.55);
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
                vec2 ruv = vec2(atan(R.z, R.x) * (0.5 / 3.14159265) + 0.5,
                                asin(clamp(R.y, -1.0, 1.0)) * (1.0 / 3.14159265) + 0.5);
                float interior = clamp(u_is_interior, 0.0, 1.0);
                float roughness = mix(mix(0.88, 0.96, interior), mix(0.26, 0.68, interior), reflect_mask);
                vec3 environment = textureLod(u_refl_env, ruv, roughness * 5.2).rgb;
                environment = min(environment, mix(vec3(4.0), vec3(1.15), interior));
                float fresnel = 0.04 + mix(0.50, 0.22, interior) * pow(1.0 - ndv, 5.0);
                float reflection_gain = mix(0.20 + fresnel * 1.2, 0.035 + fresnel * 0.30, interior);
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

        // Dry, clean, ice-free fast path: skip full AgX/LUT for faint <=5.5% glass sky reflection.
        if (wet < 0.005 && frost_raw < 0.005 && mist_raw < 0.005 && grime_raw < 0.025) {
            float omv = 1.0 - max(0.0, dot(N, V));
            float omv2 = omv * omv;
            float fresnel = omv2 * omv2;
            float glass_alpha = fresnel * 0.055;
            if (glass_alpha < 0.010) { frag_color = vec4(0.0); return; }
            vec3 dry_col = mix(vec3(0.45, 0.56, 0.68), u_haze_cool, 0.65);
            frag_color = vec4(u_native_output != 0 ? sqrt(max(dry_col, vec3(0.0))) : dry_col, glass_alpha);
            return;
        }

        float airspeed = clamp(u_glass_weather.z, 0.0, 160.0);
        float sim_time = u_glass_weather.y;
        bool front_glass = (u_cabin_glass == 2);

        // 1. Physically-simulated 4-layer fluid rain LUTs (real surface tension, organic beads & streaks,
        //    ZERO grid popping or tadpole shapes!) shaded with TRUE optical HDR backdrop refraction!
        vec2 drop_nrm = vec2(0.0);
        float drop_mask = 0.0;
        float drop_relief = 0.0;
        if (wet > 0.005) {
            float rain_index = clamp(log2(1.0 + rain_mm * 0.75) / log2(1.0 + 42.0 * 0.75) * 3.0, 0.0, 3.0);
            float rain_layer = (rain_index + 0.5) * 0.25;
            // Two overlaid tiling scales of the fluid LUT eliminate visible repetition & give fine + coarse beads
            vec2 uv_a = v_uv * (front_glass ? vec2(2.15, 1.55) : vec2(1.65, 1.55));
            vec2 uv_b = v_uv * (front_glass ? vec2(3.40, 2.35) : vec2(2.60, 2.35)) + vec2(0.41, 0.27);
            float spd_c = min(airspeed, 110.0);
            if (front_glass) {
                // Air pushes droplets UP the curved windshield at cruise; gravity creeps them slowly down when parked
                float v_up = sim_time * spd_c * 0.0042;
                float v_dn = sim_time * 0.012;
                uv_a.y = uv_a.y / (1.0 + spd_c * 0.0045) - mod(v_dn, 1.0) + mod(v_up, 1.0);
                uv_b.y = uv_b.y / (1.0 + spd_c * 0.0030) - mod(v_dn * 0.7, 1.0) + mod(v_up * 0.68, 1.0);
            } else {
                // Side windows: airflow sweeps droplets AFT (towards tail = +u_x in UV, so subtract from sample UV.x)
                float v_aft = sim_time * spd_c * 0.0055;
                float v_dn = sim_time * 0.014;
                uv_a.x = uv_a.x / (1.0 + spd_c * 0.0040) - mod(v_aft, 1.0);
                uv_a.y -= mod(v_dn, 1.0);
                uv_b.x = uv_b.x / (1.0 + spd_c * 0.0028) - mod(v_aft * 0.65, 1.0);
                uv_b.y -= mod(v_dn * 0.75, 1.0);
            }
            vec4 da = sample_rain_lut(uv_a, rain_layer, front_glass);
            vec4 db = sample_rain_lut(uv_b, max(0.125, rain_layer - 0.25), front_glass);
            float ma = da.a * wet;
            float mb = db.a * wet * (0.55 + 0.35 * heavy_rain);
            vec2 na = (da.rg * 2.0 - 1.0) * ma;
            vec2 nb = (db.rg * 2.0 - 1.0) * mb;
            drop_mask = clamp(max(ma, mb * 0.85), 0.0, 1.0);
            drop_relief = clamp(max(da.b * ma, db.b * mb), 0.0, 1.0);
            drop_nrm = clamp(na + nb * (1.0 - ma * 0.65), vec2(-1.0), vec2(1.0));
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

        float fresnel = pow(1.0 - max(0.0, dot(N, V)), 4.5);
        float drop_alpha = smoothstep(0.04, 0.42, drop_mask);
        float non_drop_alpha = fresnel * 0.055 + frost_mask * 0.80 + mist_mask + grime_mask;
        float glass_alpha = clamp(max(drop_alpha, non_drop_alpha), 0.0, 0.96);
        if (glass_alpha < 0.012) { frag_color = vec4(0.0); return; }

        float daylight = smoothstep(-0.10, 0.16, u_sun_dir.y);
        vec3 glass_color;
        if (u_native_output != 0 && drop_alpha > 0.02) {
            float slope_mag = clamp(length(drop_nrm), 0.0, 1.0);
            float crest = pow(max(0.0, dot(normalize(drop_nrm + vec2(1e-4)), vec2(0.28, 0.96))), 5.0) * smoothstep(0.22, 0.82, slope_mag);
            float edge_dark = smoothstep(0.58, 0.96, slope_mag) * 0.26;
            vec3 water_hdr;
            float out_drop_a;
            if (front_glass) {
                // Front windshield: TRUE optical refraction of live linear HDR world+sky behind the glass!
                vec2 screen_uv = gl_FragCoord.xy / vec2(textureSize(u_backdrop_tex, 0));
                vec2 refr_uv = clamp(screen_uv - drop_nrm * vec2(0.022, 0.032) * (0.45 + 0.75 * drop_relief), vec2(0.002), vec2(0.998));
                vec3 refr_hdr = textureLod(u_backdrop_tex, refr_uv, 0.0).rgb;
                water_hdr = refr_hdr * (1.0 - edge_dark)
                          + mix(vec3(0.45, 0.55, 0.68), vec3(1.10, 1.14, 1.18), daylight) * (crest * 0.48)
                          + vec3(0.68, 0.84, 1.20) * (flash * (0.20 + crest * 0.85));
                out_drop_a = drop_alpha;
            } else {
                // Side windows sit in front of the cockpit wing underside (not in u_backdrop_tex):
                // use translucent optical rim + crisp sky reflection so drops blend naturally over BOTH wing & sky!
                float dip = clamp(dot(drop_nrm, vec2(0.28, 0.92)), -1.0, 1.0);
                vec3 rim_dark = vec3(0.05, 0.07, 0.10) * (0.35 + 0.65 * daylight);
                vec3 rim_lit  = mix(u_haze_cool, vec3(0.96, 0.98, 1.0), 0.55) * (0.35 + 0.75 * daylight);
                water_hdr = mix(rim_dark, rim_lit, dip * 0.5 + 0.5)
                          + vec3(0.95, 0.98, 1.0) * (crest * 0.55)
                          + vec3(0.68, 0.84, 1.20) * (flash * (0.18 + crest * 0.70));
                out_drop_a = drop_alpha * clamp(0.14 + slope_mag * 0.24 + crest * 0.34, 0.0, 0.52);
            }
            water_hdr = mix(water_hdr, vec3(0.84, 0.90, 0.97) * (0.30 + 0.72 * daylight), snow_fac * 0.30);

            vec3 frost_col = mix(vec3(0.80, 0.89, 0.98), vec3(1.12, 1.16, 1.20), crystal_sparkle) * (0.25 + 0.88 * daylight)
                           + vec3(0.55, 0.70, 0.95) * (flash * 0.30);
            vec3 mist_col  = mix(vec3(0.64, 0.72, 0.80), u_haze_cool * 0.95, 0.45) * (0.20 + 0.78 * daylight);
            vec3 grime_col = vec3(0.32, 0.30, 0.24) * (0.20 + 0.75 * daylight);
            glass_color = mix(water_hdr, frost_col, frost_mask * 0.85);
            glass_color = mix(glass_color, mist_col, mist_mask);
            glass_color = mix(glass_color, grime_col, grime_mask);
            frag_color = vec4(native_display_color(glass_color), clamp(max(out_drop_a, non_drop_alpha), 0.0, 0.96));
            return;
        }

        // Frost / mist / grime / non-native fallback
        vec3 base_glass = mix(vec3(0.45, 0.56, 0.68), u_haze_cool, 0.65);
        vec3 frost_col = mix(vec3(0.80, 0.89, 0.98), vec3(1.12, 1.16, 1.20), crystal_sparkle) * (0.25 + 0.88 * daylight)
                       + vec3(0.55, 0.70, 0.95) * (flash * 0.30);
        vec3 mist_col  = mix(vec3(0.64, 0.72, 0.80), u_haze_cool * 0.95, 0.45) * (0.20 + 0.78 * daylight);
        vec3 grime_col = vec3(0.32, 0.30, 0.24) * (0.20 + 0.75 * daylight);
        float w_sum = max(0.001, fresnel * 0.055 + frost_mask * 0.80 + mist_mask + grime_mask);
        glass_color = (base_glass * (fresnel * 0.055) + frost_col * (frost_mask * 0.80) +
                       mist_col * mist_mask + grime_col * grime_mask) / w_sum;
        frag_color = vec4(native_display_color(glass_color), clamp(non_drop_alpha, 0.0, 0.86));
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

    // Extra glossy Fresnel reflection only on exterior cabin glass (never on interior CockpitPropVeil!)
    float is_glass = (u_is_interior < 0.5 && u_base_tint.a < 0.94 && u_base_tint.a > 0.25) ? 1.0 : 0.0;
    float glass_fres = is_glass * (0.14 + 0.72 * pow(1.0 - ndv, 3.5));

    vec3 lit = kD * base * (amb_c * ao + sun_c * (ndl * ao)) +
               sun_c * (spec * ndl * 0.80) +
               ibl_env * (F_ibl * ibl_w + vec3(glass_fres * 0.95)) +
               base * u_emissive_col;
    frag_color = vec4(native_display_color(lit), clamp(alpha + glass_fres * 0.35, 0.0, 1.0));
}
