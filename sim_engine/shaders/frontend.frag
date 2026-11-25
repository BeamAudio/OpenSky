#version 460 core
in vec2 v_uv;
layout(location = 0) out vec4 frag_color;
layout(binding = 0) uniform sampler2D u_land;
layout(binding = 1) uniform sampler2D u_relief;
layout(binding = 2) uniform sampler2D u_region_map;
layout(std430, binding = 0) readonly buffer InstalledBounds { vec4 bounds[]; };
uniform vec3 u_globe; // centre latitude, longitude, and radius / viewport height
uniform float u_aspect;
uniform int u_region_count;
uniform int u_region_map_ready;
uniform vec4 u_relief_bounds; // west, south, longitude span, latitude span

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
        for (int i = 0; i < u_region_count; ++i) {
            vec4 b = bounds[i];
            bool inside_lon = b.x <= b.z ? degrees.x >= b.x && degrees.x <= b.z
                                         : degrees.x >= b.x || degrees.x <= b.z;
            installed = installed || (inside_lon && degrees.y >= b.y && degrees.y <= b.w);
        }
        if (installed) {
            color = mix(vec3(.072, .29, .34), vec3(.20, .49, .38), land);
            vec2 dem = (degrees - u_relief_bounds.xy) / u_relief_bounds.zw;
            if (all(greaterThanEqual(dem, vec2(0))) && all(lessThanEqual(dem, vec2(1)))) {
                float height = texture(u_relief, dem).r;
                float offset_height = texture(u_relief, dem + vec2(-.003,.003)).r;
                float relief = clamp((height - offset_height) / 300.0, -.30, .28);
                float highlands = smoothstep(380.0, 2500.0, height);
                vec3 terrain = mix(vec3(.15,.41,.30), vec3(.47,.53,.41), highlands);
                terrain = mix(terrain, vec3(.65,.72,.70), smoothstep(2350.0,3150.0,height));
                color = (terrain + relief) * mix(.75, 1.0, land);
            }
        }
        if (u_region_map_ready != 0) {
            vec2 detailed = texture(u_region_map, v_uv).rg;
            if (detailed.g > .5) {
                vec2 offset = texture(u_region_map, v_uv + vec2(-.0025,.004)).rg;
                float offset_height = offset.g > .5 ? offset.r : detailed.r;
                float relief = clamp((detailed.r - offset_height) / 300.0, -.30, .28);
                float highlands = smoothstep(380.0, 2500.0, detailed.r);
                vec3 terrain = mix(vec3(.15,.41,.30), vec3(.47,.53,.41), highlands);
                terrain = mix(terrain, vec3(.65,.72,.70), smoothstep(2350.0,3150.0,detailed.r));
                color = (terrain + relief) * mix(.75, 1.0, land);
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
