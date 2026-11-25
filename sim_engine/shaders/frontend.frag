#version 460 core
in vec2 v_uv;
layout(location = 0) out vec4 frag_color;
layout(binding = 0) uniform sampler2D u_land;
layout(binding = 1) uniform sampler2DArray u_relief;
layout(binding = 2) uniform sampler2DArray u_wx_field;
layout(std430, binding = 0) readonly buffer InstalledBounds { vec4 bounds[]; };
uniform vec3 u_globe; // centre latitude, longitude, and radius / viewport height
uniform float u_aspect;
uniform int u_region_count;
uniform vec4 u_wx_geo;     // lon_min, lat_min, inv_lon_span, inv_lat_span
uniform float u_wx_overlay; // >0.5 draws live weather model cloud/rain/wind radar over map
uniform float u_wx_time;
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
            if (u_wx_overlay > 0.5 && u_wx_geo.z > 0.0 && u_wx_geo.w > 0.0) {
                vec2 wuv = (degrees - u_wx_geo.xy) * u_wx_geo.zw;
                if (all(greaterThanEqual(wuv, vec2(0.0))) && all(lessThanEqual(wuv, vec2(1.0)))) {
                    vec4 mf = textureLod(u_wx_field, vec3(wuv, 1.0), 0.0);
                    float rain = textureLod(u_wx_field, vec3(wuv, 4.0), 0.0).g;
                    float cloud = clamp(mf.w, 0.0, 1.0);
                    float wspd = length(mf.xy);
                    // Animated streamlines flowing along the mesoscale wind vector field
                    vec2 wdir = (wspd > 0.3) ? (mf.xy / wspd) : vec2(1.0, 0.0);
                    vec2 wperp = vec2(-wdir.y, wdir.x);
                    float along = dot(degrees, wdir) * 26.0 - u_wx_time * clamp(wspd * 0.14, 0.2, 2.2);
                    float across = dot(degrees, wperp) * 42.0;
                    float stream = smoothstep(0.65, 0.98, 0.5 + 0.5 * sin(across) * cos(along));
                    color += vec3(0.32, 0.58, 0.78) * (stream * smoothstep(2.0, 16.0, wspd) * 0.22);
                    // Translucent solved cloud veil + hillside updraft highlight
                    color = mix(color, vec3(0.84, 0.90, 0.96), smoothstep(0.18, 0.92, cloud) * 0.36);
                    // Meteorological precipitation radar palette (cyan/green -> amber -> crimson)
                    if (rain > 0.25) {
                        vec3 rad = mix(vec3(0.14, 0.78, 0.42), vec3(0.95, 0.76, 0.15), smoothstep(0.4, 6.0, rain));
                        rad = mix(rad, vec3(0.92, 0.18, 0.28), smoothstep(8.0, 22.0, rain));
                        color = mix(color, rad, smoothstep(0.25, 3.5, rain) * 0.65);
                    }
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
