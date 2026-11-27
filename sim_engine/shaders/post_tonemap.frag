#version 460 core

in vec2 v_uv;
layout(location = 0) out vec4 frag_color;
layout(location = 1) out vec4 out_history; // Linear HDR RGB + reverse-Z depth in A

layout(binding = 0) uniform sampler2D u_scene_tex;
layout(binding = 1) uniform sampler2D u_cloud_tex;  // Quarter-res temporally reprojected volumetric clouds
layout(binding = 2) uniform sampler2D u_lut_tex;    // 256x4 monotonic 1D RGB Spain filmic curve
layout(binding = 3) uniform sampler2D u_scene_depth;// Reverse-Z scene depth for crepuscular sun-shaft scattering!
layout(binding = 4) uniform sampler2D u_history_tex;
layout(binding = 5) uniform sampler2D u_cloud_depth;

uniform float u_exposure;
uniform vec4 u_sun_uv_vis; // xy = projected sun screen UV, z = front-hemisphere visibility [0..1], w = sun elev vis [0..1]
uniform vec3 u_haze_warm;
uniform mat4 u_inv_vp;
uniform mat4 u_prev_vp;
uniform vec3 u_camera_delta; // Current aircraft origin minus previous origin in ENU render axes
uniform int u_history_valid;
uniform int u_aa_mode; // 0 = spatial FXAA, 1 = balanced TAA, 2 = crisp TAA

// Blender / Godot AgX Filmic Tonemapper (clean golden highlight rolloff, zero blown-out neon whites!)
vec3 agx_tonemap(vec3 val) {
    val = max(val, vec3(0.0));
    const mat3 agx_mat = mat3(
        vec3(0.84247906, 0.04232824, 0.04237565),
        vec3(0.07843360, 0.87846864, 0.07843360),
        vec3(0.07922375, 0.07916613, 0.87914297)
    );
    const mat3 agx_mat_inv = mat3(
        vec3( 1.19687900, -0.05289685, -0.05297163),
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

vec4 reconstruct_cloud(float scene_depth) {
    vec4 cloud = texture(u_cloud_tex, v_uv);
    if (cloud.a < 0.004 || scene_depth <= 0.000001) return cloud;

    // Reject sky samples from the cloud grid where they would bleed over a
    // nearer mountain. Preserve clouds on the same side of the depth edge.
    vec4 depth = textureGather(u_cloud_depth, v_uv);
    float bias = max(0.000001, scene_depth * 0.1);
    vec4 weights = smoothstep(vec4(scene_depth - bias * 3.0), vec4(scene_depth - bias), depth);
    if (min(min(weights.x, weights.y), min(weights.z, weights.w)) >= 0.999) return cloud;
    if (max(max(weights.x, weights.y), max(weights.z, weights.w)) <= 0.001) return vec4(0.0);

    ivec2 limit = textureSize(u_cloud_tex, 0) - 1;
    vec2 grid = v_uv * vec2(limit + 1) - 0.5;
    ivec2 p = ivec2(floor(grid));
    vec2 f = fract(grid);
    weights *= vec4((1.0 - f.x) * f.y, f.x * f.y, f.x * (1.0 - f.y), (1.0 - f.x) * (1.0 - f.y));
    vec4 filtered = texelFetch(u_cloud_tex, clamp(p + ivec2(0, 1), ivec2(0), limit), 0) * weights.x
                  + texelFetch(u_cloud_tex, clamp(p + ivec2(1, 1), ivec2(0), limit), 0) * weights.y
                  + texelFetch(u_cloud_tex, clamp(p + ivec2(1, 0), ivec2(0), limit), 0) * weights.z
                  + texelFetch(u_cloud_tex, clamp(p, ivec2(0), limit), 0) * weights.w;
    return filtered / max(0.20, dot(weights, vec4(1.0)));
}

void main() {
    // The resolve runs on the same grid as the scene. Fetch exact neighbours
    // for the anti-aliasing clamp and use interpolation for diagonal FXAA taps.
    ivec2 coord = ivec2(gl_FragCoord.xy);
    ivec2 size = textureSize(u_scene_tex, 0);
    vec2 pixel = 1.0 / vec2(size);
    vec3 scene = texelFetch(u_scene_tex, coord, 0).rgb;
    vec3 north = texelFetch(u_scene_tex, ivec2(coord.x, min(coord.y + 1, size.y - 1)), 0).rgb;
    vec3 south = texelFetch(u_scene_tex, ivec2(coord.x, max(coord.y - 1, 0)), 0).rgb;
    vec3 east = texelFetch(u_scene_tex, ivec2(min(coord.x + 1, size.x - 1), coord.y), 0).rgb;
    vec3 west = texelFetch(u_scene_tex, ivec2(max(coord.x - 1, 0), coord.y), 0).rgb;
    const vec3 weights = vec3(0.2126, 0.7152, 0.0722);
    float centre_luma = dot(scene, weights);
    vec4 neighbours = vec4(dot(north, weights), dot(south, weights), dot(east, weights), dot(west, weights));
    float edge = max(max(neighbours.x, neighbours.y), max(neighbours.z, neighbours.w)) -
                 min(min(neighbours.x, neighbours.y), min(neighbours.z, neighbours.w));
    float grad_x = neighbours.z - neighbours.w;
    float grad_y = neighbours.x - neighbours.y;
    float strength = smoothstep(0.035, 0.16, edge / max(0.08, centre_luma));
    vec3 nb_lo = min(scene, min(min(north, south), min(east, west)));
    vec3 nb_hi = max(scene, max(max(north, south), max(east, west)));
    if (u_aa_mode == 0 && strength > 0.10) {
        // Filter along the measured edge angle. The diagonal taps are only
        // needed on visible edges and reuse this composite pass.
        vec2 gradient = vec2(grad_x, grad_y);
        float axis = max(abs(grad_x), abs(grad_y));
        if (axis > 0.0001) {
            vec2 span = vec2(-grad_y, grad_x) * (pixel * 0.58 / axis);
            vec3 edge_pair = 0.5 * (texture(u_scene_tex, v_uv + span).rgb +
                                    texture(u_scene_tex, v_uv - span).rgb);
            scene = mix(scene, edge_pair, strength * 0.44);
        }
    }
    if (u_aa_mode != 0) {
        // Bounded reconstruction sharpening; crisp mode offsets the selected
        // low internal pixel budget without a second full-screen texture pass.
        // Reuse the FXAA neighborhood taps, so ortho detail is restored without
        // another texture pass or amplifying values beyond the local sample range.
        vec3 neighborhood = (north + south + east + west) * 0.25;
        float sharpen_amount = u_aa_mode == 2 ? 0.48 : 0.26;
        vec3 sharpened = scene + (scene - neighborhood) * (sharpen_amount * (1.0 - strength));
        scene = clamp(sharpened, nb_lo - vec3(0.012), nb_hi + vec3(0.012));
    }
    float d_here = texelFetch(u_scene_depth, coord, 0).r;
    vec4 cld = reconstruct_cloud(d_here);

    // clouds_qres.frag already stops rays conservatively at static terrain depth;
    // fade the distant cloud/rain veil off foreground objects (<~40m, e.g. chase/parked aircraft)
    // using live Reverse-Z scene depth (near=0.18m => d=0.0045 at 40m).
    float fg_clear = 1.0 - smoothstep(0.0022, 0.0065, d_here);
    vec3 hdr = scene * (1.0 - cld.a * fg_clear) + cld.rgb * fg_clear;

    // A broad atmospheric glow uses the already sampled cloud and depth data.
    if (u_sun_uv_vis.z > 0.01 && u_sun_uv_vis.w > 0.02) {
        float radius = length(u_sun_uv_vis.xy - v_uv);
        float radial = max(0.0, 1.0 - radius);
        float sky = 1.0 - smoothstep(0.000015, 0.0002, d_here);
        hdr += u_haze_warm * (radial * radial * sky * (1.0 - cld.a * 0.78) *
                              u_sun_uv_vis.z * u_sun_uv_vis.w * 0.08);
    }

    // Subpixel-jitter temporal AA with depth-tested reprojection. History is
    // neighborhood-clamped to avoid trails on moving aircraft, vehicles, and clouds.
    if (u_aa_mode != 0 && u_history_valid != 0) {
        vec2 ndc = v_uv * 2.0 - 1.0;
        vec4 prev_clip;
        float expected_depth = 0.0;
        if (d_here > 0.000001) {
            vec4 world = u_inv_vp * vec4(ndc, d_here, 1.0);
            world.xyz /= max(1e-6, world.w);
            world.xyz += u_camera_delta;
            prev_clip = u_prev_vp * vec4(world.xyz, 1.0);
            expected_depth = prev_clip.z / max(1e-6, prev_clip.w);
        } else {
            vec4 far_wp = u_inv_vp * vec4(ndc, 0.0001, 1.0);
            vec3 ray = normalize(far_wp.xyz / max(1e-6, far_wp.w));
            prev_clip = u_prev_vp * vec4(ray, 0.0);
        }
        vec2 prev_uv = prev_clip.xy / max(1e-6, prev_clip.w) * 0.5 + 0.5;
        if (prev_clip.w > 0.0 && prev_uv.x > 0.002 && prev_uv.x < 0.998 &&
            prev_uv.y > 0.002 && prev_uv.y < 0.998) {
            vec4 hist = texture(u_history_tex, prev_uv);
            float depth_gate = 1.0;
            if (d_here > 0.000001) {
                float depth_tol = max(0.000002, expected_depth * 0.045);
                depth_gate = 1.0 - smoothstep(depth_tol, depth_tol * 3.0,
                                               abs(hist.a - expected_depth));
            } else {
                depth_gate = 1.0 - smoothstep(0.00001, 0.00008, hist.a);
            }
            vec3 lo = min(hdr, min(scene, nb_lo));
            vec3 hi = max(hdr, max(scene, nb_hi));
            vec3 pad = max(vec3(0.025), abs(hdr) * 0.045);
            vec3 history_color = clamp(hist.rgb, lo - pad, hi + pad);
            float delta_luma = abs(dot(history_color - hdr, vec3(0.2126, 0.7152, 0.0722))) /
                               max(0.10, dot(hdr, vec3(0.2126, 0.7152, 0.0722)));
            float stability = 1.0 - smoothstep(0.20, 0.85, delta_luma) * 0.46;
            float history_weight = u_aa_mode == 2 ? 0.34 : 0.48;
            hdr = mix(hdr, history_color, history_weight * depth_gate * stability);
        }
    }
    out_history = vec4(hdr, d_here);

    // Troy Sobotka 6th-order AgX polynomial ALREADY outputs display-encoded sRGB!
    vec3 srgb = agx_tonemap(hdr * u_exposure);
    // Gentle tonal curve + natural Mediterranean vibrance that preserves authentic PNOA satellite colors!
    srgb = pow(max(srgb, vec3(0.0)), vec3(1.03));
    srgb = mix(srgb, smoothstep(vec3(0.008), vec3(0.992), srgb), 0.14);
    float luma = dot(srgb, vec3(0.2126, 0.7152, 0.0722));
    float max_c = max(srgb.r, max(srgb.g, srgb.b));
    float min_c = min(srgb.r, min(srgb.g, srgb.b));
    float cur_sat = (max_c - min_c) / max(0.001, max_c);
    float vib_boost = 1.12 + 0.08 * (1.0 - cur_sat);
    srgb = clamp(mix(vec3(luma), srgb, vib_boost), 0.0, 1.0);
    vec3 graded = vec3(
        textureLod(u_lut_tex, vec2(srgb.r, 0.5), 0.0).r,
        textureLod(u_lut_tex, vec2(srgb.g, 0.5), 0.0).g,
        textureLod(u_lut_tex, vec2(srgb.b, 0.5), 0.0).b
    );
    frag_color = vec4(graded, 1.0);
}
