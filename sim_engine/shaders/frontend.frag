#version 460 core
in vec2 v_uv;
layout(location = 0) out vec4 frag_color;
layout(binding = 0) uniform sampler2D u_land;
layout(binding = 1) uniform sampler2DArray u_relief;
layout(binding = 2) uniform sampler2DArray u_wx_field;
layout(binding = 3) uniform sampler2DArray u_region_wx;
layout(std430, binding = 0) readonly buffer InstalledBounds { vec4 bounds[]; };
uniform vec3 u_globe; // centre latitude, longitude, and radius / viewport height
uniform float u_aspect;
uniform int u_region_count;
uniform vec4 u_wx_geo;        // lon_min, lat_min, inv_lon_span, inv_lat_span (active live model)
uniform float u_wx_overlay;   // >0.5 draws animated wind flowmap, clouds, and precipitation
uniform float u_wx_base_cloud;
uniform float u_wx_time;

vec2 hash22(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * vec3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.xx + p3.yz) * p3.zy);
}

bool sample_flow_wx(vec2 deg, out vec2 wind, out float cloud, out float rain) {
    wind = vec2(0.0);
    cloud = 0.0;
    rain = 0.0;
    if (u_wx_geo.z > 0.0 && u_wx_geo.w > 0.0) {
        vec2 wuv = (deg - u_wx_geo.xy) * u_wx_geo.zw;
        if (all(greaterThanEqual(wuv, vec2(0.0))) && all(lessThanEqual(wuv, vec2(1.0)))) {
            // Surface/valley layer (L0) exposes full topographic flow deflection, ridges, and channeling.
            vec4 mf0 = textureLod(u_wx_field, vec3(wuv, 0.0), 0.0);
            vec2 off = vec2(0.012, 0.016);
            float cw_avg = 0.25 * (
                textureLod(u_wx_field, vec3(wuv + vec2(off.x, 0.0), 1.0), 0.0).w +
                textureLod(u_wx_field, vec3(wuv - vec2(off.x, 0.0), 1.0), 0.0).w +
                textureLod(u_wx_field, vec3(wuv + vec2(0.0, off.y), 1.0), 0.0).w +
                textureLod(u_wx_field, vec3(wuv - vec2(0.0, off.y), 1.0), 0.0).w);
            wind = mf0.xy;
            cloud = clamp(u_wx_base_cloud + cw_avg, 0.0, 1.0);
            rain = textureLod(u_wx_field, vec3(wuv, 4.0), 0.0).g;
            return true;
        }
    }
    for (int i = 0; i < u_region_count; ++i) {
        vec4 b = bounds[2 * i];
        int wlayer = int(bounds[2 * i + 1].y);
        if (wlayer < 0 || any(lessThan(deg, b.xy)) || any(greaterThan(deg, b.zw))) continue;
        vec2 uv = (deg - b.xy) / max(vec2(1e-5), b.zw - b.xy);
        vec4 rf = textureLod(u_region_wx, vec3(uv, float(wlayer)), 0.0);
        wind = rf.xy;
        rain = rf.z;
        cloud = clamp(rf.w, 0.0, 1.0);
        return true;
    }
    return false;
}

vec3 wind_palette(float spd) {
    vec3 c0 = vec3(0.38, 0.78, 0.98); // light (<8 kt)
    vec3 c1 = vec3(0.32, 0.94, 0.86); // moderate (~18 kt)
    vec3 c2 = vec3(0.95, 0.88, 0.34); // fresh/strong (~30 kt)
    vec3 c3 = vec3(1.00, 0.46, 0.28); // gale (40+ kt)
    vec3 col = mix(c0, c1, smoothstep(3.0, 9.0, spd));
    col = mix(col, c2, smoothstep(9.0, 15.5, spd));
    return mix(col, c3, smoothstep(15.5, 22.0, spd));
}

// One zoom-octave of animated multi-segment curved streamlines wrapping around ridges and valleys.
float flow_streaks(vec2 mpos, float cos_lat, float cell_size, float time_s, out float best_spd) {
    vec2 base_id = floor(mpos / cell_size);
    float best = 0.0;
    best_spd = 0.0;
    for (int oy = -1; oy <= 1; ++oy) {
        for (int ox = -1; ox <= 1; ++ox) {
            vec2 cid = base_id + vec2(float(ox), float(oy));
            vec2 h = hash22(cid);
            vec2 p0 = (cid + 0.14 + 0.72 * h) * cell_size;
            vec2 cw; float cc, cr;
            if (!sample_flow_wx(vec2(p0.x / cos_lat, p0.y), cw, cc, cr)) continue;
            float spd = length(cw);
            if (spd < 0.25) continue;
            vec2 d0 = cw / spd;
            float rate = clamp(0.24 + spd * 0.044, 0.28, 1.45);
            float phase = fract(h.x * 1.73 + h.y * 2.41 + time_s * rate);
            float life = sin(phase * 3.14159265);
            // Advect head forward along curved field, then trace 3-segment curved tail backward.
            vec2 head = p0 + d0 * ((phase - 0.35) * cell_size * 1.25);
            float seg_len = cell_size * clamp(0.24 + spd * 0.020, 0.24, 0.48);
            vec2 cur = head;
            vec2 dir = d0;
            float best_s = 0.0;
            for (int s = 0; s < 3; ++s) {
                vec2 sw; float sc, sr;
                if (sample_flow_wx(vec2(cur.x / cos_lat, cur.y), sw, sc, sr) && length(sw) > 0.25)
                    dir = normalize(sw);
                vec2 prev = cur - dir * seg_len;
                vec2 ba = prev - cur;
                float denom = max(1e-7, dot(ba, ba));
                float u = clamp(dot(mpos - cur, ba) / denom, 0.0, 1.0);
                float dist = length((mpos - cur) - ba * u) / cell_size;
                float t_tail = (float(s) + u) / 3.0;
                float half_w = mix(0.048, 0.011, t_tail);
                float seg_mask = 1.0 - smoothstep(half_w * 0.42, half_w + 0.022, dist);
                float fade = mix(1.0, 0.12, pow(t_tail, 0.72));
                best_s = max(best_s, seg_mask * fade);
                cur = prev;
            }
            float val = best_s * (0.28 + 0.72 * life);
            if (val > best) {
                best = val;
                best_spd = spd;
            }
        }
    }
    return best;
}

void main() {
    vec2 p = (v_uv - .5) * vec2(u_aspect, 1.0) / u_globe.z;
    float r2 = dot(p, p);
    vec3 space = mix(vec3(.023, .042, .061), vec3(.068, .106, .149), v_uv.y);
    vec3 color = space;
    if (r2 <= 1.0) {
        float z = sqrt(max(0.0, 1.0 - r2));
        float sl = sin(u_globe.x), cl = cos(u_globe.x);
        float lat = asin(clamp(p.y * cl + z * sl, -1.0, 1.0));
        float lon = u_globe.y + atan(p.x, z * cl - p.y * sl);
        vec2 geo = vec2(fract(lon / 6.2831853 + .5), .5 - lat / 3.14159265);
        float land = texture(u_land, geo).r;
        float coast = 1.0 - smoothstep(.02,.28,abs(land - .5));
        color = mix(vec3(.116, .137, .159), vec3(.37, .41, .45), land) + .052 * coast;
        vec2 degrees = vec2(geo.x * 360.0 - 180.0, lat * 57.29578);
        bool installed = false;
        int relief_layer = -1;
        vec2 relief_uv = vec2(0);
        vec2 relief_scale = vec2(1);
        vec2 geo_dx = dFdx(degrees), geo_dy = dFdy(degrees);
        for (int i = 0; i < u_region_count; ++i) {
            vec4 b = bounds[2 * i];
            if (any(lessThan(degrees, b.xy)) || any(greaterThan(degrees, b.zw))) continue;
            installed = true;
            int layer = int(bounds[2 * i + 1].x);
            if (relief_layer < 0 && layer >= 0) {
                relief_layer = layer;
                relief_scale = 1.0 / (b.zw - b.xy);
                relief_uv = (degrees - b.xy) * relief_scale;
            }
        }
        if (installed) {
            color = mix(vec3(.072, .29, .34), vec3(.20, .49, .38), land);
            if (relief_layer >= 0) {
                vec2 texture_scale = relief_scale * vec2(1.0, -1.0);
                vec4 relief = textureGrad(u_relief, vec3(relief_uv.x, 1.0 - relief_uv.y, relief_layer),
                                          geo_dx * texture_scale, geo_dy * texture_scale);
                color = mix(color, relief.rgb, relief.a * land);
            }
            if (u_wx_overlay > 0.5) {
                vec2 wind; float cloud, rain;
                if (sample_flow_wx(degrees, wind, cloud, rain)) {
                    float wspd = length(wind);
                    vec3 flow_hue = wind_palette(wspd);
                    // Subtle speed-coded wind wash keeps relief readable while showing jet corridors.
                    color = mix(color, flow_hue, smoothstep(1.5, 22.0, wspd) * 0.16);
                    // Translucent cloud veil and precipitation radar underneath streamlines.
                    color = mix(color, vec3(0.84, 0.90, 0.96), smoothstep(0.22, 0.94, cloud) * 0.28);
                    if (rain > 0.25) {
                        vec3 rad = mix(vec3(0.14, 0.78, 0.42), vec3(0.95, 0.76, 0.15), smoothstep(0.4, 6.0, rain));
                        rad = mix(rad, vec3(0.92, 0.18, 0.28), smoothstep(8.0, 22.0, rain));
                        color = mix(color, rad, smoothstep(0.25, 3.5, rain) * 0.58);
                    }
                    // Dual-octave zoom-adaptive flowmap streamlines stay crisp from globe to valley zoom.
                    float cos_lat = max(0.25, cos(lat));
                    vec2 mpos = vec2(degrees.x * cos_lat, degrees.y);
                    float target_cell = clamp(2.2 / max(0.45, u_globe.z), 0.018, 0.48);
                    float oct_f = log2(target_cell);
                    float oct0 = floor(oct_f);
                    float blend = fract(oct_f);
                    float cell0 = exp2(oct0);
                    float s0 = 0.0, s1 = 0.0;
                    float f0 = flow_streaks(mpos, cos_lat, cell0, u_wx_time, s0);
                    float f1 = flow_streaks(mpos, cos_lat, cell0 * 2.0, u_wx_time * 0.72, s1);
                    float flow = mix(f0, f1, blend);
                    float flow_spd = mix(s0, s1, blend);
                    vec3 streak_col = mix(wind_palette(flow_spd), vec3(0.96, 0.99, 1.0), 0.40);
                    color = mix(color, streak_col, clamp(flow * 0.94, 0.0, 0.96));
                }
            }
        }
        vec2 grid = degrees / (u_globe.z < 4.0 ? 15.0 : 1.0);
        vec2 cell = abs(fract(grid - .5) - .5) / max(fwidth(grid), vec2(.001));
        color += (1.0 - smoothstep(.15, 1.0, min(cell.x, cell.y))) * .047;
        color *= .74 + .26 * max(0, dot(vec3(p, z), normalize(vec3(-.3,.5,1))));
        color = mix(space, color, smoothstep(0, max(fwidth(r2), .00001), 1.0-r2));
    }
    frag_color = vec4(color, 1.0);
}
