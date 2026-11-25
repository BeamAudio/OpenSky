#version 460 core
uniform vec3 u_sun_dir;
uniform vec3 u_haze_cool;
uniform vec3 u_haze_warm;
uniform float u_haze_density;
uniform float u_time;

in vec3 v_world_pos;
in vec3 v_nrm;
in float v_part;
in float v_livery;
in vec4 v_lights;
in vec2 v_local_xz;
in float v_dist;

layout(location = 0) out vec4 frag_color;

vec3 primary_palette(float id) {
    int k = int(mod(floor(id), 8.0));
    if (k == 0) return vec3(0.88, 0.89, 0.90); // Crisp white GA / airliner
    if (k == 1) return vec3(0.85, 0.86, 0.88); // Pearl white
    if (k == 2) return vec3(0.16, 0.22, 0.30); // Deep naval hull / executive
    if (k == 3) return vec3(0.38, 0.14, 0.12); // Rust-red cargo hull
    if (k == 4) return vec3(0.18, 0.34, 0.56); // Mediterranean ferry blue
    if (k == 5) return vec3(0.78, 0.72, 0.56); // Warm cream GA
    if (k == 6) return vec3(0.22, 0.28, 0.24); // Dark green cargo hull
    return vec3(0.84, 0.84, 0.82);             // White yacht/ferry
}

vec3 accent_palette(float id) {
    int k = int(mod(floor(id), 8.0));
    if (k == 0) return vec3(0.78, 0.14, 0.10); // Iberia/Spanish red tail
    if (k == 1) return vec3(0.92, 0.74, 0.08); // Vueling/Catalan yellow
    if (k == 2) return vec3(0.10, 0.32, 0.64); // Sky blue stripe
    if (k == 3) return vec3(0.76, 0.32, 0.12); // Orange/red shipping containers
    if (k == 4) return vec3(0.90, 0.91, 0.92); // White ferry superstructure
    if (k == 5) return vec3(0.14, 0.44, 0.26); // Forest green GA stripe
    if (k == 6) return vec3(0.16, 0.42, 0.62); // Blue/teal containers
    return vec3(0.18, 0.28, 0.46);             // Navy accent
}

void main() {
    vec3 nrm = normalize(v_nrm);
    vec3 sdir = normalize(u_sun_dir);
    vec3 view_dir = normalize(v_world_pos);
    float day_t = clamp((sdir.y + 0.035) / 0.14, 0.0, 1.0);
    float night_t = clamp((0.10 - sdir.y) / 0.16, 0.0, 1.0);

    vec3 base = primary_palette(v_livery);
    float rough = 0.28;
    float metal = 0.22;
    vec3 emiss = vec3(0.0);

    if (v_part > 0.5 && v_part < 1.5) {
        base = accent_palette(v_livery);
        rough = 0.34;
    } else if (v_part > 1.5 && v_part < 2.5) {
        base = vec3(0.04, 0.07, 0.11);
        rough = 0.08;
        metal = 0.12;
    } else if (v_part > 2.5 && v_part < 3.5) {
        // Port red nav + flashing red anti-collision beacon
        float pulse = step(0.84, fract(u_time * 0.85 + v_lights.x));
        base = vec3(0.70, 0.06, 0.05);
        emiss = vec3(3.8, 0.25, 0.14) * (0.45 + 1.8 * night_t + 3.2 * pulse * v_lights.y);
    } else if (v_part > 3.5 && v_part < 4.5) {
        // Starboard green nav light
        base = vec3(0.08, 0.72, 0.22);
        emiss = vec3(0.28, 3.2, 0.75) * (0.45 + 1.8 * night_t);
    } else if (v_part > 4.5 && v_part < 5.5) {
        // Wingtip white strobe / ship masthead light
        float s1 = step(0.90, fract(u_time * 1.15 + v_lights.x));
        float s2 = step(0.90, fract(u_time * 1.15 + v_lights.x + 0.12));
        float flash = max(s1, s2) * v_lights.y;
        base = vec3(0.95, 0.96, 1.0);
        emiss = vec3(4.5, 4.6, 5.0) * (0.25 + 1.0 * night_t + 4.5 * flash);
    } else if (v_part > 5.5 && v_part < 6.5) {
        // Landing/taxi light or ship deck illumination
        base = vec3(0.96, 0.92, 0.80);
        emiss = vec3(3.2, 2.8, 2.0) * (v_lights.z * (0.65 + 2.2 * night_t));
    } else if (v_part > 6.5) {
        // Procedural scrolling frothy Mediterranean wake behind moving ships (zero extra pass!)
        float wave = 0.65 + 0.35 * sin(v_local_xz.y * 4.5 - u_time * 5.2 + abs(v_local_xz.x) * 3.0);
        float edge = clamp(1.0 - abs(v_local_xz.x) * 0.75, 0.0, 1.0);
        float tail = clamp(1.0 - max(0.0, v_local_xz.y - 0.45) * 0.52, 0.0, 1.0);
        float foam = edge * tail * wave * v_lights.w;
        vec3 sea_col = mix(vec3(0.03, 0.08, 0.13), u_haze_cool * 0.45, 0.30) * max(0.15, day_t);
        vec3 foam_col = mix(vec3(0.62, 0.72, 0.78), vec3(0.92, 0.95, 0.98), day_t);
        base = mix(sea_col, foam_col, foam * 0.85);
        rough = 0.75;
        metal = 0.0;
    }

    vec3 ldir = (sdir.y >= -0.025) ? sdir : vec3(0.0, 1.0, 0.0);
    vec3 sun_c = (sdir.y >= -0.025)
        ? mix(vec3(1.0, 0.64, 0.32), vec3(1.0, 0.96, 0.88), clamp(sdir.y / 0.45, 0.0, 1.0)) * mix(0.55, 2.35, day_t)
        : vec3(0.24, 0.34, 0.56) * 0.32;
    vec3 amb_c = mix(vec3(0.06, 0.10, 0.18), u_haze_cool * 0.88, day_t) * 0.56;
    float ndl = max(0.0, dot(nrm, ldir));
    vec3 H = normalize(-view_dir + ldir);
    float ndh = max(0.0, dot(nrm, H));
    float s2 = ndh * ndh; float s4 = s2 * s2; float s8 = s4 * s4;
    float spec = (s8 * s8) * (1.0 - rough) * 0.9;
    vec3 lit = base * (amb_c + sun_c * ndl) + sun_c * (spec * ndl * (0.25 + metal));

    float mist = clamp(1.0 - exp(-v_dist * max(u_haze_density, 0.000018) * 1.05), 0.0, 0.86);
    vec3 haze_c = mix(u_haze_cool * 0.86, u_haze_warm * 1.08, clamp(pow(max(0.0, dot(view_dir, sdir)), 4.0) * 0.45 * day_t, 0.0, 0.65));
    // Aircraft beacons & strobes punch cleanly through aerial haze at long VFR range!
    frag_color = vec4(mix(lit, haze_c, mist * 0.88) + emiss * (1.0 - mist * 0.28), 1.0);
}
