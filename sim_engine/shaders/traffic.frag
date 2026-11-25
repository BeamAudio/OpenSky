#version 460 core
uniform vec3 u_sun_dir;
uniform vec3 u_haze_cool;
uniform vec3 u_haze_warm;
uniform float u_haze_density;
uniform float u_cam_alt_m;

in vec3 v_world_pos;
in vec3 v_nrm;
in float v_part;
in float v_col_id;
in float v_elev_m;
in float v_dist;

layout(location = 0) out vec4 frag_color;

vec3 car_palette(float id) {
    int k = int(mod(floor(id), 8.0));
    if (k == 0) return vec3(0.84, 0.85, 0.86); // White Spanish sedan/van
    if (k == 1) return vec3(0.56, 0.59, 0.63); // Silver metallic
    if (k == 2) return vec3(0.11, 0.13, 0.16); // Anthracite black
    if (k == 3) return vec3(0.62, 0.11, 0.10); // Crimson red
    if (k == 4) return vec3(0.11, 0.22, 0.44); // Deep Mediterranean blue
    if (k == 5) return vec3(0.76, 0.78, 0.80); // Light grey van
    if (k == 6) return vec3(0.24, 0.27, 0.29); // Graphite
    return vec3(0.68, 0.54, 0.16);             // Warm ochre/gold
}

void main() {
    vec3 nrm = normalize(v_nrm);
    vec3 sdir = normalize(u_sun_dir);
    vec3 view_dir = normalize(v_world_pos);
    float day_t = clamp((sdir.y + 0.035) / 0.14, 0.0, 1.0);
    float night_t = clamp((0.10 - sdir.y) / 0.16, 0.0, 1.0);

    vec3 base = car_palette(v_col_id);
    float rough = 0.22;
    float metal = 0.35;
    vec3 emiss = vec3(0.0);

    if (v_part > 0.5 && v_part < 1.5) {
        // Tinted automotive glass
        base = vec3(0.04, 0.06, 0.09);
        rough = 0.06;
        metal = 0.10;
    } else if (v_part > 1.5 && v_part < 2.5) {
        // Warm white xenon/halogen headlights (always crisp, bright at dusk/night!)
        base = vec3(0.95, 0.92, 0.82);
        emiss = vec3(2.4, 2.15, 1.65) * (0.45 + 2.2 * night_t);
    } else if (v_part > 2.5 && v_part < 3.5) {
        // Red LED taillights
        base = vec3(0.65, 0.06, 0.05);
        emiss = vec3(2.4, 0.20, 0.12) * (0.45 + 2.2 * night_t);
    } else if (v_part > 3.5) {
        // Rubber wheels & bumper trim
        base = vec3(0.05, 0.055, 0.06);
        rough = 0.85;
        metal = 0.0;
    }

    vec3 ldir = (sdir.y >= -0.025) ? sdir : vec3(0.0, 1.0, 0.0);
    vec3 sun_c = (sdir.y >= -0.025)
        ? mix(vec3(1.0, 0.64, 0.32), vec3(1.0, 0.96, 0.88), clamp(sdir.y / 0.45, 0.0, 1.0)) * mix(0.55, 2.35, day_t)
        : vec3(0.32, 0.45, 0.72) * 0.36;
    vec3 amb_c = mix(vec3(0.08, 0.12, 0.22), u_haze_cool * 0.90, day_t) * 0.52;
    vec3 H = normalize(-view_dir + ldir);
    float ndl = max(0.0, dot(nrm, ldir));
    float ndh = max(0.0, dot(nrm, H));
    float ndv = max(0.001, dot(nrm, -view_dir));
    float a2 = pow(max(0.05, rough * rough), 2.0);
    float d = ndh * ndh * (a2 - 1.0) + 1.0;
    float spec = clamp((a2 / (3.14159 * d * d + 1e-4)) * 0.35, 0.0, 4.5);
    vec3 f0 = mix(vec3(0.04), base, metal);
    vec3 F = f0 + (1.0 - f0) * pow(1.0 - ndv, 4.0);
    vec3 lit = base * (1.0 - metal * 0.65) * (amb_c + sun_c * ndl) + sun_c * (F * spec * ndl) + u_haze_cool * (F * (1.0 - rough) * day_t * 0.65);

    float mist = clamp(1.0 - exp(-v_dist * max(u_haze_density, 0.000018) * 1.15), 0.0, 0.78);
    frag_color = vec4(mix(lit, u_haze_cool * 0.82, mist * 0.84) + emiss * (1.0 - mist * 0.4), 1.0);
}
