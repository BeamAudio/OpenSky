#version 460 core
in vec2 v_uv;
layout(location = 0) out vec4 frag_color;
layout(binding = 0) uniform sampler2D u_land;
layout(binding = 1) uniform sampler2DArray u_relief;
layout(std430, binding = 0) readonly buffer InstalledBounds { vec4 bounds[]; };
uniform vec3 u_globe; // centre latitude, longitude, and radius / viewport height
uniform float u_aspect;
uniform int u_region_count;
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
        for (int i = 0; i < u_region_count; ++i) {
            vec4 b = bounds[2 * i];
            if (any(lessThan(degrees, b.xy)) || any(greaterThan(degrees, b.zw))) continue;
            installed = true;
            int layer = int(bounds[2 * i + 1].x);
            if (relief_layer < 0 && layer >= 0) {
                relief_layer = layer;
                relief_uv = (degrees - b.xy) / (b.zw - b.xy);
            }
        }
        if (installed) {
            color = mix(vec3(.072, .29, .34), vec3(.20, .49, .38), land);
            if (relief_layer >= 0) {
                vec4 relief = texture(u_relief, vec3(relief_uv.x, 1.0 - relief_uv.y, relief_layer));
                color = mix(color, relief.rgb, relief.a * land);
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
