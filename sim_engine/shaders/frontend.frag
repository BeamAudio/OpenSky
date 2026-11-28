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
uniform vec4 u_wx_geo; // lon_min, lat_min, inv_lon_span, inv_lat_span of the published model
uniform bool u_wx_wind;
uniform bool u_wx_cloud_rain;
uniform float u_wx_time;

vec2 hash22(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * vec3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.xx + p3.yz) * p3.zy);
}

vec2 grid_uv(vec2 uv, ivec2 size) {
    return (uv * vec2(size - 1) + 0.5) / vec2(size);
}

float model_edge_km(vec2 uv, vec2 km_per_deg) {
    vec2 edge = min(uv, 1.0 - uv) * (km_per_deg / u_wx_geo.zw);
    return min(edge.x, edge.y);
}

bool sample_wind(vec2 deg, vec2 km_per_deg, out vec2 wind) {
    const float feather_km = 8.0;
    bool has_model = all(greaterThan(u_wx_geo.zw, vec2(0.0)));
    vec2 model_uv = (deg - u_wx_geo.xy) * u_wx_geo.zw;
    float model_edge = has_model ? model_edge_km(model_uv, km_per_deg) : -1.0;
    if (model_edge >= feather_km) {
        wind = textureLod(u_wx_field, vec3(grid_uv(model_uv, textureSize(u_wx_field, 0).xy), 0.0), 0.0).xy;
        return true;
    }

    vec2 preview = vec2(0.0);
    float total = 0.0;
    bool preview_covers = false;
    for (int i = 0; i < u_region_count; ++i) {
        vec4 b = bounds[2 * i];
        int wlayer = int(bounds[2 * i + 1].y);
        if (wlayer < 0) continue;
        vec2 edge = min(deg - b.xy, b.zw - deg) * km_per_deg;
        float inside = min(edge.x, edge.y);
        float distance = length(min(edge, vec2(0.0)));
        if (distance >= feather_km) continue;
        // Extend the edge vectors into the feather area so even touching packs share a wind field.
        preview_covers = preview_covers || inside >= 0.0;
        float weight = 1.0 - smoothstep(0.0, feather_km, distance);
        vec2 uv = clamp((deg - b.xy) / (b.zw - b.xy), 0.0, 1.0);
        preview += weight * textureLod(u_region_wx, vec3(grid_uv(uv, textureSize(u_region_wx, 0).xy), float(wlayer)), 0.0).xy;
        total += weight;
    }
    wind = preview / max(total, 0.001);
    if (model_edge >= 0.0) {
        vec2 model = textureLod(u_wx_field, vec3(grid_uv(model_uv, textureSize(u_wx_field, 0).xy), 0.0), 0.0).xy;
        float preview_weight = (1.0 - smoothstep(0.0, feather_km, model_edge)) * min(total, 1.0);
        wind = mix(model, wind, preview_weight);
        return true;
    }
    return preview_covers;
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
float flow_streaks(vec2 mpos, float cos_lat, vec2 km_per_deg, float cell_size, float time_s) {
    vec2 base_id = floor(mpos / cell_size);
    float best = 0.0;
    for (int oy = -1; oy <= 1; ++oy) {
        for (int ox = -1; ox <= 1; ++ox) {
            vec2 cid = base_id + vec2(float(ox), float(oy));
            vec2 h = hash22(cid);
            vec2 p0 = (cid + 0.14 + 0.72 * h) * cell_size;
            vec2 cw;
            if (!sample_wind(vec2(p0.x / cos_lat, p0.y), km_per_deg, cw)) continue;
            float spd = length(cw);
            if (spd < 0.25) continue;
            vec2 d0 = cw / spd;
            float rate = clamp(0.24 + spd * 0.044, 0.28, 1.45);
            float phase = fract(h.x * 1.73 + h.y * 2.41 + time_s * rate);
            float life = sin(phase * 3.14159265);
            // Advect head forward along curved field, then trace 3-segment curved tail backward.
            // Keep all three segments within reach of the nine seeds searched above.
            vec2 head = p0 + d0 * ((phase - 0.35) * cell_size * 0.40);
            float seg_len = cell_size * min(0.15 + spd * 0.009, 0.26);
            vec2 cur = head;
            vec2 dir = d0;
            float best_s = 0.0;
            for (int s = 0; s < 3; ++s) {
                vec2 sw;
                if (sample_wind(vec2(cur.x / cos_lat, cur.y), km_per_deg, sw) && length(sw) > 0.25)
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
            best = max(best, val);
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
            // Use one coordinate scale for the whole view so seeds don't slide sideways with latitude.
            float cos_lat = max(0.25, cos(u_globe.x));
            vec2 km_per_deg = 111.1 * vec2(cos_lat, 1.0);
            if (u_wx_cloud_rain) {
                vec2 uv = (degrees - u_wx_geo.xy) * u_wx_geo.zw;
                float edge = model_edge_km(uv, km_per_deg);
                if (edge > 0.0) {
                    float coverage = smoothstep(0.0, 4.0, edge);
                    vec2 tc = grid_uv(uv, textureSize(u_wx_field, 0).xy);
                    float cloud = textureLod(u_wx_field, vec3(tc, 1.0), 0.0).a;
                    float rain = textureLod(u_wx_field, vec3(tc, 4.0), 0.0).r;
                    color = mix(color, vec3(0.84, 0.90, 0.96), smoothstep(0.22, 0.94, cloud) * 0.28 * coverage);
                    vec3 rad = mix(vec3(0.14, 0.78, 0.42), vec3(0.95, 0.76, 0.15), smoothstep(0.4, 6.0, rain));
                    rad = mix(rad, vec3(0.92, 0.18, 0.28), smoothstep(8.0, 22.0, rain));
                    color = mix(color, rad, smoothstep(0.25, 3.5, rain) * 0.58 * coverage);
                }
            }
            if (u_wx_wind) {
                vec2 wind;
                if (sample_wind(degrees, km_per_deg, wind)) {
                    // Dual-octave zoom-adaptive flowmap streamlines stay crisp from globe to valley zoom.
                    vec2 mpos = vec2(degrees.x * cos_lat, degrees.y);
                    float target_cell = clamp(2.2 / max(0.45, u_globe.z), 0.018, 0.48);
                    float oct_f = log2(target_cell);
                    float oct0 = floor(oct_f);
                    float blend = fract(oct_f);
                    float cell0 = exp2(oct0);
                    float f0 = flow_streaks(mpos, cos_lat, km_per_deg, cell0, u_wx_time);
                    float f1 = flow_streaks(mpos, cos_lat, km_per_deg, cell0 * 2.0, u_wx_time * 0.72);
                    float flow = mix(f0, f1, blend);
                    vec3 streak_col = mix(wind_palette(length(wind)), vec3(0.96, 0.99, 1.0), 0.40);
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
