#version 460 core

layout(binding = 0) uniform sampler2D u_asphalt_tex;
layout(binding = 1) uniform sampler2D u_gravel_tex;
layout(binding = 2) uniform sampler2D u_asphalt_nrm;
layout(binding = 3) uniform sampler2D u_gravel_nrm;

uniform vec3 u_sun_dir;
uniform vec3 u_haze_cool;
uniform vec3 u_haze_warm;
uniform float u_haze_density;

in vec3 v_world_pos;
in vec3 v_nrm;
in vec2 v_uv;
in float v_paved;
in float v_is_runway;
in float v_dist;

layout(location = 0) out vec4 frag_color;

void main() {
    vec3 sdir = normalize(u_sun_dir);
    vec3 view_dir = normalize(v_world_pos);
    float day_t = clamp((sdir.y + 0.035) / 0.14, 0.0, 1.0);
    float night_t = clamp((0.08 - sdir.y) / 0.14, 0.0, 1.0);

    int mat_id = int(floor(v_paved + 0.5));      // 0=dirt, 1=gravel, 2=asphalt, 3=concrete
    int feat_id = int(floor(v_is_runway + 0.5)); // 0=apron, 1=runway, 2=taxiway, 3=access_road
    float is_paved = (mat_id >= 2) ? 1.0 : 0.0;
    float is_runway = (feat_id == 1) ? 1.0 : 0.0;
    float is_taxiway = (feat_id == 2) ? 1.0 : 0.0;
    float is_road = (feat_id == 3) ? 1.0 : 0.0;
    float is_strip = (feat_id >= 1) ? 1.0 : 0.0;

    vec2 strip_uv = mix(vec2(v_uv.x * 6.5, v_uv.y * 0.28), vec2(v_uv.x * 3.8, v_uv.y * 0.11), is_paved);
    vec2 apron_uv = v_uv * mix(0.26, 0.10, is_paved);
    vec2 tuv = mix(apron_uv, strip_uv, is_strip);

    vec3 asph_raw = texture(u_asphalt_tex, tuv).rgb;
    vec3 grav_t = texture(u_gravel_tex, tuv).rgb;
    vec3 grav_macro = texture(u_gravel_tex, tuv * 0.23).rgb;
    vec3 anrm = texture(u_asphalt_nrm, tuv).rgb * 2.0 - 1.0;
    vec3 gnrm = texture(u_gravel_nrm, tuv).rgb * 2.0 - 1.0;

    float close_rw = 1.0 - smoothstep(90.0, 380.0, v_dist);
    if (close_rw > 0.01) {
        vec3 g_hi = texture(u_gravel_tex, tuv * 4.2).rgb;
        vec3 a_hi = texture(u_asphalt_tex, tuv * 4.2).rgb;
        vec3 gn_hi = texture(u_gravel_nrm, tuv * 4.2).rgb * 2.0 - 1.0;
        vec3 an_hi = texture(u_asphalt_nrm, tuv * 4.2).rgb * 2.0 - 1.0;
        grav_t *= mix(1.0, clamp(dot(g_hi, vec3(0.3, 0.59, 0.11)) / 0.20, 0.62, 1.45), close_rw * 0.75);
        asph_raw *= mix(1.0, clamp(dot(a_hi, vec3(0.3, 0.59, 0.11)) / 0.16, 0.68, 1.38), close_rw * 0.72);
        gnrm = normalize(mix(gnrm, gn_hi, close_rw * 0.60));
        anrm = normalize(mix(anrm, an_hi, close_rw * 0.60));
    }

    vec3 base;
    vec3 mnrm;
    float rough;
    if (mat_id == 3) {
        // Concrete slabs (aprons, heavy ramps, concrete runways) with subtle 9m expansion joints
        vec2 slab_p = (feat_id == 0) ? v_uv * 0.11 : vec2(v_uv.x * 2.2, v_uv.y * 0.11);
        vec2 jf = abs(fract(slab_p) - 0.5);
        float joint = 1.0 - smoothstep(0.475, 0.498, max(jf.x, jf.y));
        base = mix(vec3(0.30, 0.295, 0.28), asph_raw * 1.65 + vec3(0.14), 0.45) * (0.76 + 0.24 * joint);
        mnrm = anrm * 0.75;
        rough = 0.74;
    } else if (mat_id == 2) {
        // Weathered charcoal-grey European asphalt
        base = asph_raw * (is_road > 0.5 ? 0.56 : 0.64) * vec3(1.03, 1.00, 0.93);
        mnrm = anrm;
        rough = 0.70;
    } else if (mat_id == 1) {
        // Compacted pale grey-ochre crushed gravel
        base = mix(grav_t, grav_macro, 0.38) * vec3(0.95, 0.93, 0.86);
        mnrm = gnrm;
        rough = 0.86;
    } else {
        // Warm Aragonese dirt/earth strip
        base = mix(grav_t, grav_macro, 0.40) * vec3(0.92, 0.78, 0.58);
        mnrm = gnrm;
        rough = 0.88;
    }

    float edge_dist = abs(v_uv.x - 0.5) * 2.0;
    // Tire rubber touchdown tracks on paved runways / grassy verge & ruts on unpaved strips
    float wheel_track = (1.0 - smoothstep(0.04, 0.28, abs(edge_dist - 0.28))) * is_runway;
    float center_grass = (1.0 - is_paved) * (1.0 - smoothstep(0.0, 0.26, edge_dist)) *
                         (0.65 + 0.35 * sin(v_uv.y * 0.17 + gnrm.y * 3.0)) * is_runway;
    float verge_grass  = (1.0 - is_paved) * smoothstep(0.58, 0.96, edge_dist + gnrm.x * 0.14) * is_strip;
    base = mix(base, vec3(0.18, 0.23, 0.11) * (0.85 + 0.35 * grav_t.g), clamp(center_grass * 0.45 + verge_grass * 0.60, 0.0, 0.76));
    base *= mix(1.0, mix(1.18, 0.76, is_paved), wheel_track * mix(0.50, 0.66, is_paved));
    rough = mix(rough, 0.42, wheel_track * is_paved);

    // ICAO markings: white runway centerline/side stripes, yellow taxiway centerline, subtle road edge lines
    float center_line = (1.0 - smoothstep(0.015, 0.035, edge_dist)) * step(0.45, fract(v_uv.y / 45.0)) * is_paved * is_runway;
    float side_line = (1.0 - smoothstep(0.01, 0.04, abs(edge_dist - 0.90))) * is_paved * is_runway;
    float white_mark = clamp(center_line + side_line * 0.85, 0.0, 1.0);
    float road_edge = (1.0 - smoothstep(0.02, 0.06, abs(edge_dist - 0.86))) * is_paved * is_road * 0.55;
    base = mix(base, vec3(0.80, 0.79, 0.76), clamp(white_mark * 0.88 + road_edge, 0.0, 0.90));

    float twy_yellow = (1.0 - smoothstep(0.025, 0.055, edge_dist)) * is_paved * is_taxiway;
    base = mix(base, vec3(0.76, 0.56, 0.08), twy_yellow * 0.86);

    // Soft feathered shoulders on unpaved strips, crisp edge feathering on paved strips/roads
    float shoulder_alpha = mix(1.0 - smoothstep(0.62, 1.0, edge_dist + (gnrm.x * 0.14 + (grav_macro.r - 0.5) * 0.22)),
                               1.0 - smoothstep(0.93, 1.0, edge_dist), is_paved);
    shoulder_alpha = mix(1.0, shoulder_alpha, is_strip);
    if (shoulder_alpha < 0.05) discard;

    // ICAO Edge Lights glowing cleanly during twilight & Short Night!
    float light_dot = (1.0 - smoothstep(0.01, 0.035, abs(edge_dist - 0.95))) * is_runway *
                      (1.0 - smoothstep(0.02, 0.08, abs(fract(v_uv.y / 60.0) - 0.5)));
    vec3 emiss = vec3(1.85, 1.45, 0.85) * (light_dot * (0.25 + 1.8 * night_t));

    // Cook-Torrance GGX PBR Asphalt/Concrete/Gravel Specular!
    vec3 nrm = normalize(normalize(v_nrm) + vec3(mnrm.x * 0.32, 0.0, mnrm.y * 0.32));
    vec3 ldir = (sdir.y >= -0.025) ? sdir : normalize(vec3(-sdir.x, 0.55, -sdir.z));
    vec3 sun_c = (sdir.y >= -0.025) ? mix(vec3(1.0, 0.62, 0.30), vec3(1.0, 0.95, 0.86), clamp(sdir.y / 0.45, 0.0, 1.0)) * mix(0.55, 2.38, day_t) : vec3(0.32, 0.45, 0.72) * 0.36;
    vec3 amb_c = mix(vec3(0.08, 0.12, 0.24), u_haze_cool * 0.92, day_t) * 0.50;
    vec3 V = -view_dir;
    vec3 H = normalize(V + ldir);
    float ndl = max(0.0, dot(nrm, ldir));
    float ndv = max(0.001, dot(nrm, V));
    float ndh = max(0.0, dot(nrm, H));
    float a2 = pow(max(0.05, rough * rough), 2.0);
    float dn = ndh * ndh * (a2 - 1.0) + 1.0;
    float spec = clamp((a2 / (3.14159 * dn * dn + 1e-4)) * 0.25, 0.0, 3.0);
    float F = 0.04 + 0.96 * pow(1.0 - ndv, 4.5);
    vec3 lit = base * (amb_c + sun_c * ndl) + sun_c * (spec * F * ndl) + u_haze_cool * (F * (1.0 - rough) * day_t * 0.12);

    float cos_th = max(0.0, dot(view_dir, sdir));
    float sun_vis = clamp((sdir.y + 0.08) / 0.24, 0.0, 1.0);
    float hg_w = (1.0 - 0.3844) / (4.0 * 3.14159 * pow(max(0.04, 1.3844 - 1.24 * cos_th), 1.5));
    vec3 insc = mix(u_haze_cool * 0.80, u_haze_warm * 1.30, clamp(hg_w * 0.55 * sun_vis, 0.0, 0.88));
    float mist = clamp(1.0 - exp(-v_dist * max(u_haze_density, 0.000018) * 1.35), 0.0, 0.82);
    frag_color = vec4(mix(lit, insc, mist * 0.86) + emiss * (1.0 - mist * 0.5), shoulder_alpha);
}
