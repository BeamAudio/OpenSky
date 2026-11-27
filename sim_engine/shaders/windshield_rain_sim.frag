#version 460 core

in vec2 v_uv;
layout(location = 0) out vec4 out_sim; // R = water thickness h, GB = flow vel (u, v), A = surface wetness memory

layout(binding = 0) uniform sampler2D u_prev_sim;

uniform vec4 u_flow_params;   // xy = aero+gravity flow vel (u/s, v/s), z = dt (s), w = rain_mm_hr
uniform vec4 u_wiper_params;  // x = prev_pos [0..1], y = cur_pos [0..1], z = park_age_s, w = sim_time_s
uniform vec4 u_env_params;    // x = airspeed_mps, y = defrost_clear [0..1], z = crosswind_mps (rightward +), w = propwash01 [0..1]

float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

vec3 hash32(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * vec3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yxz + 33.33);
    return fract((p3.xxy + p3.yzz) * p3.zyx);
}

// Compute signed arc sweep for one bottom-pivoted windshield wiper in Windshield UV coords.
void eval_wiper_hit(vec2 uv, vec2 piv, float prev_pos, float cur_pos, float park_age,
                    out float out_swept, out float out_bow, out vec2 out_push_dir) {
    out_swept = 0.0;
    out_bow = 0.0;
    out_push_dir = vec2(0.0);

    const float kAx = 2.25;
    vec2 d = vec2((uv.x - piv.x) * kAx, piv.y - uv.y);
    float r = length(d);
    if (r < 0.125 || r > 0.645 || d.x < -0.02 || d.y < -0.02) return;
    float r_mask = smoothstep(0.125, 0.155, r) * (1.0 - smoothstep(0.60, 0.645, r));

    float ang = atan(max(0.0, d.y), max(0.0, d.x));
    const float ang0 = -0.02, ang1 = 1.38;
    float phi = (ang - ang0) / (ang1 - ang0);
    if (phi < -0.03 || phi > 1.03) return;
    float stroke = abs(cur_pos - prev_pos);
    float wiper_on = (park_age < 0.12) ? 1.0 : 0.0;
    if (wiper_on < 0.5 && stroke < 1e-4) return;

    float lo = min(prev_pos, cur_pos) - 0.012;
    float hi = max(prev_pos, cur_pos) + 0.012;
    float in_sweep = smoothstep(lo - 0.014, lo + 0.008, phi) * (1.0 - smoothstep(hi - 0.008, hi + 0.014, phi));
    out_swept = in_sweep * r_mask * wiper_on;

    // Leading edge ahead of cur_pos pushes accumulated water sideways & radially off the blade!
    float dir_s = (cur_pos >= prev_pos) ? 1.0 : -1.0;
    float lead = phi - cur_pos;
    float bow_band = exp(-((lead - dir_s * 0.022) * (lead - dir_s * 0.022)) / 0.00055);
    out_bow = bow_band * smoothstep(0.001, 0.015, stroke) * r_mask * wiper_on;

    // Blade tangent vector pointing in sweep motion direction (in UV coords, v flips vs d.y)
    vec2 tang_d = vec2(-d.y, d.x) / max(0.1, r);
    out_push_dir = normalize(vec2((tang_d.x * dir_s) / kAx, -tang_d.y * dir_s) + vec2(0.0, -0.25));
}

void main() {
    float dt = clamp(u_flow_params.z, 0.002, 0.040);
    float rain_mm = max(0.0, u_flow_params.w);
    float time_s = u_wiper_params.w;
    vec2 px = 1.0 / vec2(textureSize(u_prev_sim, 0));

    vec4 center = texture(u_prev_sim, v_uv);
    vec4 sL = texture(u_prev_sim, clamp(v_uv - vec2(px.x, 0.0), vec2(0.002), vec2(0.998)));
    vec4 sR = texture(u_prev_sim, clamp(v_uv + vec2(px.x, 0.0), vec2(0.002), vec2(0.998)));
    vec4 sD = texture(u_prev_sim, clamp(v_uv - vec2(0.0, px.y), vec2(0.002), vec2(0.998)));
    vec4 sU = texture(u_prev_sim, clamp(v_uv + vec2(0.0, px.y), vec2(0.002), vec2(0.998)));

    // 1. Real shallow-water momentum & height-slope + surface-tension channeling pressure
    vec2 grad_h = vec2(sR.r - sL.r, sU.r - sD.r) * 0.5;
    vec2 grad_wet = vec2(sR.a - sL.a, sU.a - sD.a) * 0.5;

    // Base aerodynamic slipstream + cowl divergence + propeller swirl + crosswind + gravity (UV/sec)
    float airspeed_mps = max(0.0, u_env_params.x);
    float airspeed01   = clamp(airspeed_mps / 48.0, 0.0, 1.8);
    float cross_mps    = u_env_params.z;
    float prop01       = clamp(u_env_params.w, 0.0, 1.0);

    float cowl_push_u  = (v_uv.x - 0.5) * (0.34 * clamp(-u_flow_params.y, 0.0, 1.6) + 0.08 * airspeed01);
    float prop_zone    = prop01 * exp(-((v_uv.x - 0.44) * (v_uv.x - 0.44)) / 0.085);
    vec2  prop_swirl   = vec2(0.065 * (0.55 - v_uv.y), -0.14) * prop_zone;
    float top_accel    = 1.0 + 0.30 * (1.0 - v_uv.y) * clamp(airspeed01, 0.0, 1.2);
    vec2  env_flow     = vec2(u_flow_params.x + cowl_push_u, u_flow_params.y * top_accel) + prop_swirl;

    // Contact-angle hysteresis: very thin layers cling to acrylic, thick pools accelerate dynamically into rivulets!
    float mobility = smoothstep(0.05, 0.30, center.r);
    // Lateral instability breaks uniform sheets into branching serpentine channels (zero periodic patterns!)
    float channel_pull = (grad_wet.x * 0.22 + grad_wet.y * 0.12);
    vec2 tgt_vel = env_flow * (0.10 + 1.35 * mobility)
                 - grad_h * (0.045 + 0.09 * center.r)
                 + vec2(channel_pull, grad_wet.y * 0.09) * mobility;

    vec2 vel = mix(center.gb, tgt_vel, clamp(dt * (4.8 + 4.5 * airspeed01), 0.0, 1.0));
    float spd = length(vel);
    if (spd > 1.05) vel *= (1.05 / spd);

    // 2. Upwind semi-Lagrangian fluid transport + subtle capillary smoothing
    vec2 back_uv = v_uv - vel * dt;
    vec4 adv = texture(u_prev_sim, clamp(back_uv, vec2(0.002), vec2(0.998)));
    // Slight MacCormack error compensation / diffusion without smearing sharp channels
    float lap_h = (sL.r + sR.r + sD.r + sU.r) * 0.25 - center.r;
    float h = mix(adv.r, center.r, 0.14 * (1.0 - mobility)) + lap_h * (0.12 * mobility + 0.025);

    // Conserve lateral pooling by mildly boosting converging velocity regions (-div v)
    float div_v = ((sR.g - sL.g) + (sU.b - sD.b)) * 0.5;
    h = max(0.0, h - div_v * (h * dt * 2.0));

    // Aerodynamic boundary-layer shedding at high speed shears thick pooled water off into crisp rivulets
    float shear_shed = smoothstep(18.0, 62.0, airspeed_mps) * 0.48;
    h = max(0.0, h - h * shear_shed * mobility * dt);

    // 3. True dynamic stochastic raindrop impacts scaled by airspeed scoop flux & crosswind bias
    if (rain_mm > 0.35) {
        float rain_rate = clamp(rain_mm / 16.0, 0.05, 1.6);
        float impact_flux = clamp(0.85 + (airspeed_mps / 18.0) * 0.72 + abs(cross_mps) * 0.035, 0.70, 3.6);
        float cross_norm = clamp(cross_mps / 15.0, -1.0, 1.0);
        float windward = clamp(1.0 - cross_norm * (v_uv.x - 0.5) * 0.55, 0.65, 1.45);
        float t_tick = floor(time_s * 30.0);
        // Two irregular multi-scale impact sweeps per step so every swept window fills organically!
        for (int g = 0; g < 2; ++g) {
            float sc = (g == 0) ? 24.0 : 42.0;
            vec2 j_off = vec2(hash12(vec2(t_tick, float(g) * 7.1 + 1.3)),
                              hash12(vec2(float(g) * 3.7, t_tick + 5.9)));
            vec2 gp = (v_uv + j_off) * vec2(sc * 1.45, sc);
            vec2 cell = floor(gp);
            vec2 f = fract(gp) - 0.5;
            vec3 rnd = hash32(cell + vec2(t_tick * (0.71 + float(g) * 0.37), float(g) * 19.3));
            float drop_prob = clamp(rain_rate * (g == 0 ? 0.18 : 0.26) * impact_flux * windward * (dt * 60.0), 0.0, 0.90);
            if (rnd.x < drop_prob) {
                vec2 center_d = (rnd.yz - 0.5) * 0.55;
                float r_drop = mix(0.24, 0.45, rnd.z) * mix(1.0, 0.78, clamp(airspeed01 * 0.7, 0.0, 1.0));
                float dist_d = length(f - center_d);
                if (dist_d < r_drop) {
                    float dome = 1.0 - (dist_d / r_drop);
                    h += dome * dome * (g == 0 ? 0.30 : 0.20) * clamp(rain_rate * 0.90, 0.30, 1.40);
                    vel += normalize(f - center_d + 1e-4) * (0.055 * dome);
                }
            }
        }
        // Wind- and airspeed-driven spray film feed hitting forward-facing acrylic
        h += rain_rate * 0.018 * impact_flux * windward * dt;
    }

    // 4. Physical Wiper Blade Sweep + Mass-Conserving Bow-Wave Pileup!
    float sw_l, bow_l, sw_r, bow_r;
    vec2 push_l, push_r;
    eval_wiper_hit(v_uv, vec2(0.18, 1.02), u_wiper_params.x, u_wiper_params.y, u_wiper_params.z, sw_l, bow_l, push_l);
    eval_wiper_hit(v_uv, vec2(0.54, 1.02), u_wiper_params.x, u_wiper_params.y, u_wiper_params.z, sw_r, bow_r, push_r);
    float swept = clamp(max(sw_l, sw_r), 0.0, 1.0);
    float bow = clamp(max(bow_l, bow_r), 0.0, 1.0);
    vec2 bow_dir = (bow_l >= bow_r) ? push_l : push_r;

    if (bow > 0.01) {
        // Pushed liquid scooped onto the blade edge pools & surges forward ahead of the rubber squeegee!
        h += bow * 0.22 * (0.25 + clamp(center.r, 0.0, 1.0));
        vel = mix(vel, bow_dir * 0.30, bow * 0.85);
    }
    if (swept > 0.01) {
        h *= (1.0 - 0.94 * swept);
    }

    // 5. Warm cockpit defroster & windshield boundary drainage
    float defrost_zone = 1.0 - smoothstep(0.18, 0.44, length((v_uv - vec2(0.46, 0.62)) * vec2(1.25, 1.55)));
    float defrost_evap = u_env_params.y * defrost_zone * 0.14;
    float dry_evap = (rain_mm < 0.35) ? 0.16 : 0.015;
    h = max(0.0, h - (dry_evap + defrost_evap) * dt);

    // Clean border drain near rubber seals so water leaves edge naturally
    float border = smoothstep(0.0, 0.025, min(min(v_uv.x, 1.0 - v_uv.x), min(v_uv.y, 1.0 - v_uv.y)));
    h = clamp(h * border, 0.0, 1.45);

    // Wet surface memory lags behind h slightly so running drops naturally follow established tracks
    float wet_mem = clamp(max(mix(adv.a, h, dt * 1.4), smoothstep(0.02, 0.30, h)) * (1.0 - 0.80 * swept), 0.0, 1.0);
    out_sim = vec4(h, vel.x, vel.y, wet_mem);
}
