#include "cdlod_terrain.hpp"
#include "../third_party/stb_image.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace nsim {

namespace {

inline float hash2_tile(int ix, int iy, int period, uint32_t seed) {
    int x = ((ix % period) + period) % period;
    int y = ((iy % period) + period) % period;
    uint32_t h = static_cast<uint32_t>(x) * 374761393u ^ static_cast<uint32_t>(y) * 668265263u ^ seed;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= (h >> 16);
    return static_cast<float>(h & 0xFFFFu) * (1.0f / 65535.0f);
}

inline uint32_t hash3_tile(int x, int y, int z, int period, uint32_t seed) {
    auto wrap = [period](int v) { return ((v % period) + period) % period; };
    uint32_t h = static_cast<uint32_t>(wrap(x)) * 0x8da6b343u ^
                 static_cast<uint32_t>(wrap(y)) * 0xd8163841u ^
                 static_cast<uint32_t>(wrap(z)) * 0xcb1ab31fu ^ seed;
    h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; h ^= h >> 16;
    return h;
}

inline float worley3_tile(float x, float y, float z, int cells, uint32_t seed) {
    const int ix = static_cast<int>(std::floor(x));
    const int iy = static_cast<int>(std::floor(y));
    const int iz = static_cast<int>(std::floor(z));
    float min_d2 = 4.0f;
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                const int cx = ix + dx, cy = iy + dy, cz = iz + dz;
                const uint32_t h0 = hash3_tile(cx, cy, cz, cells, seed);
                const uint32_t h1 = hash3_tile(cx, cy, cz, cells, seed ^ 0x9e3779b9u);
                const float px = static_cast<float>(cx) + (h0 & 0xFFFFu) * (1.0f / 65536.0f) - x;
                const float py = static_cast<float>(cy) + (h0 >> 16) * (1.0f / 65536.0f) - y;
                const float pz = static_cast<float>(cz) + (h1 & 0xFFFFu) * (1.0f / 65536.0f) - z;
                min_d2 = std::min(min_d2, px * px + py * py + pz * pz);
            }
        }
    }
    return 1.0f - std::clamp(std::sqrt(min_d2) * 0.77f, 0.0f, 1.0f);
}

inline float vnoise_tile(float u, float v, int period, uint32_t seed) {
    float x = u * period, y = v * period;
    int ix = static_cast<int>(std::floor(x)), iy = static_cast<int>(std::floor(y));
    float fx = x - ix, fy = y - iy;
    float ux = fx * fx * (3.0f - 2.0f * fx), uy = fy * fy * (3.0f - 2.0f * fy);
    float a = hash2_tile(ix, iy, period, seed) * (1.0f - ux) + hash2_tile(ix + 1, iy, period, seed) * ux;
    float b = hash2_tile(ix, iy + 1, period, seed) * (1.0f - ux) + hash2_tile(ix + 1, iy + 1, period, seed) * ux;
    return a * (1.0f - uy) + b * uy;
}

inline float fbm_tile(float u, float v, int p, int oct, uint32_t seed) {
    float sum = 0.0f, amp = 0.5f, norm = 0.0f;
    for (int o = 0; o < oct; o++) {
        sum += amp * vnoise_tile(u, v, p, seed + static_cast<uint32_t>(o) * 1013u);
        norm += amp; amp *= 0.5f; p *= 2;
    }
    return sum / norm;
}

inline float worley_tile(float u, float v, int cells, uint32_t seed) {
    float x = u * cells, y = v * cells;
    int ix = static_cast<int>(std::floor(x)), iy = static_cast<int>(std::floor(y));
    float min_d2 = 9.0f;
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            int cx = ix + dx, cy = iy + dy;
            float px = cx + hash2_tile(cx, cy, cells, seed + 17u) - x;
            float py = cy + hash2_tile(cx, cy, cells, seed + 97u) - y;
            min_d2 = std::min(min_d2, px * px + py * py);
        }
    }
    return 1.0f - std::clamp(std::sqrt(min_d2), 0.0f, 1.0f);
}

} // namespace

CdlodTerrain::~CdlodTerrain() {
    shutdown();
}

GLuint CdlodTerrain::load_tex2d(const std::string &path, bool srgb, bool mipmaps, bool repeat) {
    int w = 0, h = 0, ch = 0;
    unsigned char *pix = stbi_load(path.c_str(), &w, &h, &ch, 4);
    if (!pix || w <= 0 || h <= 0) {
        std::fprintf(stderr, "[TERRAIN] Failed to load texture: %s\n", path.c_str());
        if (pix) stbi_image_free(pix);
        return 0;
    }
    int levels = 1;
    if (mipmaps) {
        int m = std::max(w, h);
        while (m > 1) { m >>= 1; levels++; }
    }
    GLuint tex = 0;
    glCreateTextures(GL_TEXTURE_2D, 1, &tex);
    glTextureStorage2D(tex, levels, srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8, w, h);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTextureSubImage2D(tex, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pix);
    if (mipmaps) glGenerateTextureMipmap(tex);
    glTextureParameteri(tex, GL_TEXTURE_MIN_FILTER, mipmaps ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    glTextureParameteri(tex, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(tex, GL_TEXTURE_WRAP_S, repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    glTextureParameteri(tex, GL_TEXTURE_WRAP_T, repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    if (mipmaps) glTextureParameterf(tex, GL_TEXTURE_MAX_ANISOTROPY, 8.0f);
    stbi_image_free(pix);
    return tex;
}

bool CdlodTerrain::load_sat_array_16(const std::string &atlas_path) {
    int w = 0, h = 0, ch = 0;
    unsigned char *pix = stbi_load(atlas_path.c_str(), &w, &h, &ch, 4);
    if (!pix || w < 256 || h < 256) {
        std::fprintf(stderr, "[TERRAIN] Failed to load 16-pattern atlas: %s\n", atlas_path.c_str());
        if (pix) stbi_image_free(pix);
        return false;
    }
    int sw = w / 4, sh = h / 4;
    int levels = 1;
    for (int m = std::max(sw, sh); m > 1; m >>= 1) levels++;

    glCreateTextures(GL_TEXTURE_2D_ARRAY, 1, &sat_array_tex_);
    glTextureStorage3D(sat_array_tex_, levels, GL_RGBA8, sw, sh, 16);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    std::vector<uint8_t> tile(static_cast<size_t>(sw) * sh * 4);
    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 4; col++) {
            int layer = row * 4 + col;
            for (int y = 0; y < sh; y++) {
                const uint8_t *src = pix + (static_cast<size_t>(row * sh + y) * w + col * sw) * 4;
                std::memcpy(tile.data() + static_cast<size_t>(y) * sw * 4, src, static_cast<size_t>(sw) * 4);
            }
            glTextureSubImage3D(sat_array_tex_, 0, 0, 0, layer, sw, sh, 1, GL_RGBA, GL_UNSIGNED_BYTE, tile.data());
        }
    }
    // Native hardware mipmaps + seamless GL_REPEAT across all 16 regional patterns (ZERO manual fract() seams!)
    glGenerateTextureMipmap(sat_array_tex_);
    glTextureParameteri(sat_array_tex_, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTextureParameteri(sat_array_tex_, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(sat_array_tex_, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTextureParameteri(sat_array_tex_, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTextureParameterf(sat_array_tex_, GL_TEXTURE_MAX_ANISOTROPY, 8.0f);
    stbi_image_free(pix);
    return true;
}

bool CdlodTerrain::load_cloud_vol_3d(const std::string &atlas_path) {
    int w = 0, h = 0, ch = 0;
    unsigned char *pix = stbi_load(atlas_path.c_str(), &w, &h, &ch, 4);
    if (!pix || w < 64 || h < 64) {
        std::fprintf(stderr, "[TERRAIN] Failed to load 3D cloud volume: %s\n", atlas_path.c_str());
        if (pix) stbi_image_free(pix);
        return false;
    }
    // Unpack 4x4 16-slice 2D atlas directly into native hardware GL_TEXTURE_3D (128x128x16) for 1-tap trilinear wrap!
    int sw = w / 4, sh = h / 4;
    std::vector<uint8_t> vol(static_cast<size_t>(sw) * sh * 16 * 4);
    for (int z = 0; z < 16; z++) {
        int row = z / 4, col = z % 4;
        for (int y = 0; y < sh; y++) {
            const uint8_t *src = pix + (static_cast<size_t>(row * sh + y) * w + col * sw) * 4;
            uint8_t *dst = vol.data() + (static_cast<size_t>(z * sh + y) * sw) * 4;
            std::memcpy(dst, src, static_cast<size_t>(sw) * 4);
        }
    }
    glCreateTextures(GL_TEXTURE_3D, 1, &cloud_vol_tex_);
    glTextureStorage3D(cloud_vol_tex_, 1, GL_RGBA8, sw, sh, 16);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTextureSubImage3D(cloud_vol_tex_, 0, 0, 0, 0, sw, sh, 16, GL_RGBA, GL_UNSIGNED_BYTE, vol.data());
    glTextureParameteri(cloud_vol_tex_, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(cloud_vol_tex_, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(cloud_vol_tex_, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTextureParameteri(cloud_vol_tex_, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTextureParameteri(cloud_vol_tex_, GL_TEXTURE_WRAP_R, GL_REPEAT);
    stbi_image_free(pix);
    return true;
}

void CdlodTerrain::build_procedural_cloud_and_micro_tex() {
    // 1. 256x256 seamless Perlin-Worley macro cloud coverage map (shared by volumetric clouds & terrain cloud shadows!)
    constexpr int CN = 256;
    std::vector<uint8_t> crgba(CN * CN * 4);
    for (int y = 0; y < CN; y++) {
        float v = static_cast<float>(y) / CN;
        for (int x = 0; x < CN; x++) {
            float u = static_cast<float>(x) / CN;
            float perlin = fbm_tile(u, v, 4, 4, 808u);
            float w1 = worley_tile(u, v, 6, 909u);
            float w2 = worley_tile(u, v, 14, 1010u);
            float w3 = worley_tile(u, v, 28, 1111u);
            float base_s = std::clamp(perlin * 0.58f + w1 * 0.30f + w2 * 0.12f, 0.0f, 1.0f);
            float ero = std::clamp(w2 * 0.62f + w3 * 0.38f, 0.0f, 1.0f);
            float mac = fbm_tile(u, v, 2, 3, 1212u);
            size_t di = (static_cast<size_t>(y) * CN + x) * 4;
            crgba[di + 0] = static_cast<uint8_t>(base_s * 255.0f);
            crgba[di + 1] = static_cast<uint8_t>(ero * 255.0f);
            crgba[di + 2] = static_cast<uint8_t>(mac * 255.0f);
            crgba[di + 3] = 255;
        }
    }
    glCreateTextures(GL_TEXTURE_2D, 1, &cloud_noise_tex_);
    glTextureStorage2D(cloud_noise_tex_, 6, GL_RGBA8, CN, CN);
    glTextureSubImage2D(cloud_noise_tex_, 0, 0, 0, CN, CN, GL_RGBA, GL_UNSIGNED_BYTE, crgba.data());
    glGenerateTextureMipmap(cloud_noise_tex_);
    glTextureParameteri(cloud_noise_tex_, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTextureParameteri(cloud_noise_tex_, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(cloud_noise_tex_, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTextureParameteri(cloud_noise_tex_, GL_TEXTURE_WRAP_T, GL_REPEAT);

    // Prebaked tileable 3D Worley erosion detail. The shader maps the 64-texel
    // tile to 64 m, with two texels per smallest cellular feature near the aircraft. Mips filter away
    // detail as it falls below a pixel, so the weather pass adds no CPU work.
    constexpr int DN = 64;
    std::vector<uint8_t> cloud_detail(static_cast<size_t>(DN) * DN * DN * 4);
    for (int z = 0; z < DN; ++z) {
        for (int y = 0; y < DN; ++y) {
            for (int x = 0; x < DN; ++x) {
                const size_t i = ((static_cast<size_t>(z) * DN + y) * DN + x) * 4;
                const float fx = (x + 0.5f) / DN, fy = (y + 0.5f) / DN, fz = (z + 0.5f) / DN;
                cloud_detail[i + 0] = static_cast<uint8_t>(worley3_tile(fx * 32.0f, fy * 32.0f, fz * 32.0f, 32, 0x3812a5u) * 255.0f + 0.5f);
                cloud_detail[i + 1] = static_cast<uint8_t>(worley3_tile(fx * 16.0f, fy * 16.0f, fz * 16.0f, 16, 0x95c317u) * 255.0f + 0.5f);
                cloud_detail[i + 2] = static_cast<uint8_t>(worley3_tile(fx * 8.0f, fy * 8.0f, fz * 8.0f, 8, 0x6a09e6u) * 255.0f + 0.5f);
                cloud_detail[i + 3] = static_cast<uint8_t>(worley3_tile(fx * 4.0f, fy * 4.0f, fz * 4.0f, 4, 0xbb67aeu) * 255.0f + 0.5f);
            }
        }
    }
    glCreateTextures(GL_TEXTURE_3D, 1, &cloud_detail_tex_);
    glTextureStorage3D(cloud_detail_tex_, 7, GL_RGBA8, DN, DN, DN);
    glTextureSubImage3D(cloud_detail_tex_, 0, 0, 0, 0, DN, DN, DN, GL_RGBA, GL_UNSIGNED_BYTE, cloud_detail.data());
    glGenerateTextureMipmap(cloud_detail_tex_);
    glTextureParameteri(cloud_detail_tex_, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTextureParameteri(cloud_detail_tex_, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(cloud_detail_tex_, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTextureParameteri(cloud_detail_tex_, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTextureParameteri(cloud_detail_tex_, GL_TEXTURE_WRAP_R, GL_REPEAT);

    // 2. 512x512 RGBA8 high-detail micro-detail map: R=grass blades, G=sand/pebbles, B=clustered Iberian bushes, A=road grit
    constexpr int MN = 512;
    std::vector<uint8_t> mrgba(MN * MN * 4);
    for (int y = 0; y < MN; y++) {
        float v = static_cast<float>(y) / MN;
        for (int x = 0; x < MN; x++) {
            float u = static_cast<float>(x) / MN;
            float grass = std::clamp(fbm_tile(u, v, 14, 3, 6101u) * 0.45f + worley_tile(u, v, 32, 6102u) * 0.35f + hash2_tile(x, y, MN, 6105u) * 0.20f, 0.0f, 1.0f);
            float sand  = std::clamp(worley_tile(u, v, 48, 6201u) * 0.45f + vnoise_tile(u, v, 128, 6203u) * 0.35f + hash2_tile(x, y, MN, 6204u) * 0.20f, 0.0f, 1.0f);
            float bush_raw = worley_tile(u, v, 18, 6302u) * (0.68f + 0.32f * fbm_tile(u, v, 8, 3, 6301u)) + (vnoise_tile(u, v, 96, 6303u) - 0.5f) * 0.22f;
            float bush  = std::clamp((bush_raw - 0.28f) * 1.65f, 0.0f, 1.0f);
            // Store leaf transmission alongside crown relief in the same atlas.
            float leaf = vnoise_tile(u, v, 128, 6401u);
            float transmission = bush * (0.50f + 0.50f * leaf);
            size_t di = (static_cast<size_t>(y) * MN + x) * 4;
            mrgba[di + 0] = static_cast<uint8_t>(grass * 255.0f);
            mrgba[di + 1] = static_cast<uint8_t>(sand  * 255.0f);
            mrgba[di + 2] = static_cast<uint8_t>(bush  * 255.0f);
            mrgba[di + 3] = static_cast<uint8_t>(transmission * 255.0f);
        }
    }
    glCreateTextures(GL_TEXTURE_2D, 1, &micro_detail_tex_);
    glTextureStorage2D(micro_detail_tex_, 7, GL_RGBA8, MN, MN);
    glTextureSubImage2D(micro_detail_tex_, 0, 0, 0, MN, MN, GL_RGBA, GL_UNSIGNED_BYTE, mrgba.data());
    glGenerateTextureMipmap(micro_detail_tex_);
    glTextureParameteri(micro_detail_tex_, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTextureParameteri(micro_detail_tex_, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(micro_detail_tex_, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTextureParameteri(micro_detail_tex_, GL_TEXTURE_WRAP_T, GL_REPEAT);
}

bool CdlodTerrain::init(const std::string &asset_root, const std::string &shader_root) {
    prog_ = compile_program_files(shader_root + "/cdlod_terrain.vert", shader_root + "/cdlod_terrain.frag");
    if (!prog_) return false;
    u_.vp              = glGetUniformLocation(prog_, "u_vp");
    u_.cam_alt         = glGetUniformLocation(prog_, "u_cam_alt_m");
    u_.cam_agl         = glGetUniformLocation(prog_, "u_cam_agl_m");
    u_.sun_dir         = glGetUniformLocation(prog_, "u_sun_dir");
    u_.haze_cool       = glGetUniformLocation(prog_, "u_haze_cool");
    u_.haze_warm       = glGetUniformLocation(prog_, "u_haze_warm");
    u_.haze_density    = glGetUniformLocation(prog_, "u_haze_density");
    u_.cloud_cov       = glGetUniformLocation(prog_, "u_cloud_cov");
    u_.time            = glGetUniformLocation(prog_, "u_time");
    u_.detail_strength = glGetUniformLocation(prog_, "u_detail_strength");
    u_.wind_en         = glGetUniformLocation(prog_, "u_wind_en");
    u_.quality         = glGetUniformLocation(prog_, "u_quality");
    u_.season          = glGetUniformLocation(prog_, "u_season");
    u_.snowline_m      = glGetUniformLocation(prog_, "u_snowline_m");
    u_.grass_zone      = glGetUniformLocation(prog_, "u_grass_zone");
    u_.grass_zone_bounds = glGetUniformLocation(prog_, "u_grass_zone_bounds");
    u_.wx_env          = glGetUniformLocation(prog_, "u_wx_env");

    if (!load_sat_array_16(asset_root + "/pbr/sat_detail_atlas.jpg")) return false;
    if (!load_cloud_vol_3d(asset_root + "/pbr/cloud_vol_3d.png")) return false;
    rock_albedo_tex_   = load_tex2d(asset_root + "/pbr/rock_albedo.jpg", true, true, true);
    rock_normal_tex_   = load_tex2d(asset_root + "/pbr/rock_normal.jpg", false, true, true);
    grass_albedo_tex_  = load_tex2d(asset_root + "/pbr/grass_albedo.jpg", true, true, true);
    grass_normal_tex_  = load_tex2d(asset_root + "/pbr/grass_normal.jpg", false, true, true);
    gravel_albedo_tex_ = load_tex2d(asset_root + "/pbr/gravel_albedo.jpg", true, true, true);
    gravel_normal_tex_ = load_tex2d(asset_root + "/pbr/gravel_normal.jpg", false, true, true);
    lut_tex_           = load_tex2d(asset_root + "/pbr/lut_spain_photo.png", false, false, false);
    build_procedural_cloud_and_micro_tex();

    // Build single 33x33 unit UV grid mesh (instanced across all visible CDLOD quadtree leaves!)
    std::vector<Vec2> verts;
    verts.reserve(PATCH_N * PATCH_N);
    for (int y = 0; y < PATCH_N; y++) {
        for (int x = 0; x < PATCH_N; x++) {
            verts.push_back({static_cast<float>(x) / (PATCH_N - 1), static_cast<float>(y) / (PATCH_N - 1)});
        }
    }
    std::vector<uint16_t> idx;
    idx.reserve((PATCH_N - 1) * (PATCH_N - 1) * 6);
    for (int y = 0; y < PATCH_N - 1; y++) {
        for (int x = 0; x < PATCH_N - 1; x++) {
            uint16_t a = static_cast<uint16_t>(y * PATCH_N + x);
            uint16_t b = a + 1;
            uint16_t c = static_cast<uint16_t>(a + PATCH_N);
            uint16_t d = c + 1;
            // Counter-clockwise when viewed from +Y up (X=East, Z=-North => +y in UV goes -Z North)
            idx.push_back(a); idx.push_back(b); idx.push_back(c);
            idx.push_back(b); idx.push_back(d); idx.push_back(c);
        }
    }
    index_count_ = static_cast<int>(idx.size());

    glCreateVertexArrays(1, &vao_);
    glCreateBuffers(1, &vbo_);
    glCreateBuffers(1, &ebo_);
    glCreateBuffers(1, &inst_ssbo_);
    glNamedBufferStorage(vbo_, static_cast<GLsizeiptr>(verts.size() * sizeof(Vec2)), verts.data(), 0);
    glNamedBufferStorage(ebo_, static_cast<GLsizeiptr>(idx.size() * sizeof(uint16_t)), idx.data(), 0);
    glNamedBufferData(inst_ssbo_, static_cast<GLsizeiptr>(MAX_VISIBLE_NODES * sizeof(NodeInstanceGpu)), nullptr, GL_STREAM_DRAW);

    glVertexArrayVertexBuffer(vao_, 0, vbo_, 0, sizeof(Vec2));
    glVertexArrayElementBuffer(vao_, ebo_);
    glEnableVertexArrayAttrib(vao_, 0);
    glVertexArrayAttribFormat(vao_, 0, 2, GL_FLOAT, GL_FALSE, 0);
    glVertexArrayAttribBinding(vao_, 0, 0);

    // Calibrated CDLOD distance bands (each level doubles cleanly -> smooth morphing from 320m quads to 163km horizon!)
    for (int l = 0; l <= AsyncTerrainStreamer::MAX_LOD; l++) {
        double quad_size = AsyncTerrainStreamer::WORLD_SPAN_M / static_cast<double>(1u << l);
        lod_ranges_[l] = static_cast<float>(quad_size * 2.55);
    }
    lod_ranges_[0] = 500000.0f;
    visible_.reserve(MAX_VISIBLE_NODES);
    return true;
}

void CdlodTerrain::shutdown() {
    if (prog_) { glDeleteProgram(prog_); prog_ = 0; }
    if (vao_) { glDeleteVertexArrays(1, &vao_); vao_ = 0; }
    if (vbo_) { glDeleteBuffers(1, &vbo_); vbo_ = 0; }
    if (ebo_) { glDeleteBuffers(1, &ebo_); ebo_ = 0; }
    if (inst_ssbo_) { glDeleteBuffers(1, &inst_ssbo_); inst_ssbo_ = 0; }
    auto del_tex = [](GLuint &t) { if (t) { glDeleteTextures(1, &t); t = 0; } };
    del_tex(sat_array_tex_);
    del_tex(rock_albedo_tex_);
    del_tex(rock_normal_tex_);
    del_tex(grass_albedo_tex_);
    del_tex(grass_normal_tex_);
    del_tex(gravel_albedo_tex_);
    del_tex(gravel_normal_tex_);
    del_tex(cloud_noise_tex_);
    del_tex(cloud_vol_tex_);
    del_tex(cloud_detail_tex_);
    del_tex(micro_detail_tex_);
    del_tex(lut_tex_);
}

void CdlodTerrain::select_node(
    AsyncTerrainStreamer &streamer,
    QuadKey k,
    int parent_slice,
    float parent_uv_x, float parent_uv_y,
    const Frustum &frustum,
    double cam_e, double cam_n, double cam_alt, double cam_agl,
    double pred_e, double pred_n,
    int max_desired_lod,
    float now_sec,
    bool is_prefetch_only
) {
    double ce = 0.0, cn = 0.0, hs = 0.0;
    AsyncTerrainStreamer::quad_bounds_en(k, ce, cn, hs);
    double size = hs * 2.0;

    // Camera-relative world AABB (+X = East, +Y = Up, -Z = North) with generous elevation range [-100 .. 3600 m]
    float min_x = static_cast<float>(ce - hs - cam_e);
    float max_x = static_cast<float>(ce + hs - cam_e);
    float min_z = static_cast<float>(-(cn + hs - cam_n));
    float max_z = static_cast<float>(-(cn - hs - cam_n));
    // Account for Earth curvature drop at distance so far peaks stay inside frustum AABB
    double d_e = std::max(0.0, std::abs(ce - cam_e) - hs);
    double d_n = std::max(0.0, std::abs(cn - cam_n) - hs);
    double horiz_d = std::hypot(d_e, d_n);
    float curv_drop = static_cast<float>((horiz_d * horiz_d) / (2.0 * 6378137.0));
    float min_y = static_cast<float>(-150.0 - cam_alt) - curv_drop - 80.0f;
    float max_y = static_cast<float>(3600.0 - cam_alt) + 80.0f;

    bool in_frustum = frustum.intersects_aabb({min_x, min_y, min_z}, {max_x, max_y, max_z});

    // Velocity-predictive distance (streams tiles ahead of flight path BEFORE camera enters their LOD band!)
    double pe = std::max(0.0, std::abs(ce - pred_e) - hs);
    double pn = std::max(0.0, std::abs(cn - pred_n) - hs);
    double pred_d = std::hypot(pe, pn);

    const double vx = pred_e - cam_e;
    const double vn = pred_n - cam_n;
    const double v_len = std::hypot(vx, vn);
    const double tx_len = std::hypot(ce - cam_e, cn - cam_n);
    const bool behind_cam = (v_len > 15.0 && tx_len > 1.0) &&
                            (((ce - cam_e) * vx + (cn - cam_n) * vn) < -0.15 * v_len * tx_len);

    if (!in_frustum && !is_prefetch_only) {
        // Outside visible frustum: prefetch only coarse L0..L5 horizon tiles along/ahead of flight track,
        // reserving high-LOD L6..L9 cache budget strictly for in-sight terrain.
        if (k.lod > 5 || (behind_cam && horiz_d > size) || pred_d > lod_ranges_[k.lod]) return;
        is_prefetch_only = true;
    }

    const bool visible_here = in_frustum && !is_prefetch_only;
    float importance = static_cast<float>(size / std::max(40.0, horiz_d + std::max(0.0, cam_agl) * 0.35));
    if (horiz_d < 2500.0) importance += static_cast<float>(5000.0 / (1.0 + horiz_d * 0.004));

    float fade = 1.0f;
    int my_slice = streamer.find_resident(k.pack(), now_sec, &fade, visible_here, importance);
    if (my_slice < 0) {
        float prio = 0.0f;
        if (k.lod <= 2) {
            prio = 20000.0f / static_cast<float>(k.lod + 1);
        } else if (visible_here) {
            prio = 1800.0f +
                   static_cast<float>(8000.0 / (1.0 + horiz_d * 0.002 + std::max(0.0, pred_d - horiz_d) * 0.0005)) +
                   static_cast<float>(10 - k.lod) * 35.0f;
            if (cam_agl < 250.0 && k.lod >= 8) prio += 1200.0f;
        } else {
            prio = 140.0f + static_cast<float>(size / std::max(100.0, pred_d * 1.2 + horiz_d * 0.5));
        }
        streamer.request_page(k.pack(), prio);
        stats_.prefetch_requests++;
        return; // Parent will gracefully render this quadrant with ZERO cracks or blank holes!
    }

    // Distance metric for splitting (uses actual camera AGL, not MSL, so high Pyrenees valleys get sharp L8/L9!)
    double dist_cam = std::hypot(horiz_d, std::max(0.0, cam_agl) * 0.45);
    const int max_prefetch_lod = visible_here ? max_desired_lod : std::min(max_desired_lod, 5);
    bool want_split_render = visible_here && (k.lod < max_desired_lod) && (dist_cam < lod_ranges_[k.lod + 1]);
    bool want_split_prefetch = (k.lod < max_prefetch_lod) && (pred_d < lod_ranges_[k.lod + 1] * 1.10);

    if (want_split_render || want_split_prefetch) {
        // Check if all 4 children are resident before splitting visually (atomic quad split = ZERO T-junction gaps!)
        int child_slices[4];
        int ready_count = 0;
        for (int ci = 0; ci < 4; ci++) {
            uint16_t cx = static_cast<uint16_t>(k.qx * 2 + (ci & 1));
            uint16_t cy = static_cast<uint16_t>(k.qy * 2 + (ci >> 1));
            QuadKey ck{static_cast<uint8_t>(k.lod + 1), cx, cy};
            child_slices[ci] = streamer.find_resident(ck.pack(), now_sec, nullptr, want_split_render, importance * 1.05f);
            if (child_slices[ci] >= 0) ++ready_count;
        }
        bool all_children_ready = (ready_count == 4);
        if (!all_children_ready) {
            float sibling_boost = (want_split_render && ready_count > 0) ? 1600.0f : 0.0f;
            for (int ci = 0; ci < 4; ci++) {
                if (child_slices[ci] >= 0) continue;
                uint16_t cx = static_cast<uint16_t>(k.qx * 2 + (ci & 1));
                uint16_t cy = static_cast<uint16_t>(k.qy * 2 + (ci >> 1));
                QuadKey ck{static_cast<uint8_t>(k.lod + 1), cx, cy};
                double cce = 0.0, ccn = 0.0, chs = 0.0;
                AsyncTerrainStreamer::quad_bounds_en(ck, cce, ccn, chs);
                double cd_pred = std::hypot(std::max(0.0, std::abs(cce - pred_e) - chs), std::max(0.0, std::abs(ccn - pred_n) - chs));
                double cd_cam = std::hypot(std::max(0.0, std::abs(cce - cam_e) - chs), std::max(0.0, std::abs(ccn - cam_n) - chs));
                float prio = 0.0f;
                if (ck.lod <= 2) {
                    prio = 20000.0f / static_cast<float>(ck.lod + 1);
                } else if (want_split_render) {
                    prio = 1800.0f + sibling_boost +
                           static_cast<float>(8000.0 / (1.0 + cd_cam * 0.002 + std::max(0.0, cd_pred - cd_cam) * 0.0005)) +
                           static_cast<float>(10 - ck.lod) * 35.0f;
                    if (cam_agl < 250.0 && ck.lod >= 8) prio += 1200.0f;
                } else {
                    prio = 140.0f + static_cast<float>((chs * 2.0) / std::max(80.0, cd_pred * 1.2 + cd_cam * 0.5));
                }
                streamer.request_page(ck.pack(), prio);
                stats_.prefetch_requests++;
            }
        }

        if (want_split_render && all_children_ready) {
            for (int ci = 0; ci < 4; ci++) {
                uint16_t cx = static_cast<uint16_t>(k.qx * 2 + (ci & 1));
                uint16_t cy = static_cast<uint16_t>(k.qy * 2 + (ci >> 1));
                QuadKey ck{static_cast<uint8_t>(k.lod + 1), cx, cy};
                select_node(
                    streamer, ck, my_slice,
                    (ci & 1) ? 0.5f : 0.0f, (ci >> 1) ? 0.5f : 0.0f,
                    frustum, cam_e, cam_n, cam_alt, cam_agl, pred_e, pred_n,
                    max_desired_lod, now_sec, false
                );
            }
            return;
        }
        if (is_prefetch_only && all_children_ready) {
            for (int ci = 0; ci < 4; ci++) {
                uint16_t cx = static_cast<uint16_t>(k.qx * 2 + (ci & 1));
                uint16_t cy = static_cast<uint16_t>(k.qy * 2 + (ci >> 1));
                QuadKey ck{static_cast<uint8_t>(k.lod + 1), cx, cy};
                select_node(
                    streamer, ck, my_slice,
                    (ci & 1) ? 0.5f : 0.0f, (ci >> 1) ? 0.5f : 0.0f,
                    frustum, cam_e, cam_n, cam_alt, cam_agl, pred_e, pred_n,
                    max_desired_lod, now_sec, true
                );
            }
            return;
        }
    }

    if (is_prefetch_only || !in_frustum || static_cast<int>(visible_.size()) >= MAX_VISIBLE_NODES) return;

    NodeInstanceGpu inst{};
    inst.min_e_rel = static_cast<float>(ce - hs - cam_e);
    inst.min_n_rel = static_cast<float>(cn - hs - cam_n);
    inst.size_m = static_cast<float>(size);
    // Wide smooth geomorph zone (0.56..0.98) -> buttery continuous vertex & texture transitions!
    inst.morph_end = lod_ranges_[k.lod] * 0.98f;
    inst.morph_start = lod_ranges_[k.lod] * 0.56f;
    inst.page_slice = static_cast<float>(my_slice);
    inst.parent_slice = static_cast<float>(parent_slice >= 0 ? parent_slice : my_slice);
    inst.fade_in = (parent_slice >= 0) ? fade : 1.0f;
    inst.parent_uv_off_x = parent_uv_x;
    inst.parent_uv_off_y = parent_uv_y;
    inst.world_min_e = static_cast<float>(ce - hs);
    inst.world_min_n = static_cast<float>(cn - hs);
    visible_.push_back(inst);
    if (k.lod > stats_.max_active_lod) stats_.max_active_lod = k.lod;
    if (k.lod < 12) stats_.visible_by_lod[k.lod]++;
}

void CdlodTerrain::update_and_render(
    AsyncTerrainStreamer &streamer,
    const Mat4 &vp,
    const Frustum &frustum,
    double cam_east_m, double cam_north_m, double cam_alt_m, double cam_agl_m,
    double vel_east_mps, double vel_north_mps,
    const Vec3 &sun_dir,
    const Vec3 &haze_cool, const Vec3 &haze_warm,
    float haze_density, float cloud_coverage, float time_sec,
    const Vec2 &wind_en_mps, GLuint refl_env_tex, int quality,
    int season, float snowline_m, int lod_distance_percent,
    const TerrainGrassZone &grass_zone,
    const Vec4 &wx_env
) {
    visible_.clear();
    stats_ = {};

    // Velocity-predictive lookahead scales smoothly with airspeed (100 kt -> ~4s, 350 kt -> ~14s ahead!)
    double spd = std::hypot(vel_east_mps, vel_north_mps);
    double lookahead_s = std::clamp(2.5 + spd * 0.065, 2.5, 16.0);
    double pred_e = cam_east_m + vel_east_mps * lookahead_s;
    double pred_n = cam_north_m + vel_north_mps * lookahead_s;

    // AGL & speed adaptive max LOD (keep L9 active up to 1150m AGL for sharp pattern & top-down clarity!)
    int max_lod = (quality == 0 && cam_agl_m > 420.0) ? 8 : AsyncTerrainStreamer::MAX_LOD;
    if (cam_agl_m > 2200.0) max_lod = std::min(max_lod, 7);
    else if (cam_agl_m > 1150.0) max_lod = std::min(max_lod, 8);

    // Balanced LOD distance scale keeps sharp L8/L9 near camera while hitting 60 FPS on Renoir iGPUs
    float q_mult = (quality == 0) ? 2.25f : (quality == 1 ? 2.65f : (quality == 2 ? 2.90f : 3.45f));
    q_mult *= std::clamp(lod_distance_percent, 75, 500) / 100.0f;
    for (int l = 1; l <= AsyncTerrainStreamer::MAX_LOD; l++) {
        double quad_size = AsyncTerrainStreamer::WORLD_SPAN_M / static_cast<double>(1u << l);
        lod_ranges_[l] = static_cast<float>(quad_size * q_mult);
    }

    select_node(
        streamer, QuadKey{0, 0, 0}, -1, 0.0f, 0.0f,
        frustum, cam_east_m, cam_north_m, cam_alt_m, cam_agl_m, pred_e, pred_n,
        max_lod, time_sec, false
    );
    stats_.visible_nodes = static_cast<int>(visible_.size());
    if (visible_.empty()) return;

    glNamedBufferSubData(inst_ssbo_, 0, static_cast<GLsizeiptr>(visible_.size() * sizeof(NodeInstanceGpu)), visible_.data());

    glUseProgram(prog_);
    glUniformMatrix4fv(u_.vp, 1, GL_FALSE, vp.m);
    glUniform1f(u_.cam_alt, static_cast<float>(cam_alt_m));
    glUniform1f(u_.cam_agl, static_cast<float>(cam_agl_m));
    glUniform3f(u_.sun_dir, sun_dir.x, sun_dir.y, sun_dir.z);
    glUniform3f(u_.haze_cool, haze_cool.x, haze_cool.y, haze_cool.z);
    glUniform3f(u_.haze_warm, haze_warm.x, haze_warm.y, haze_warm.z);
    glUniform1f(u_.haze_density, haze_density);
    glUniform1f(u_.cloud_cov, cloud_coverage);
    glUniform1f(u_.time, time_sec);
    glUniform1f(u_.detail_strength, quality == 0 ? 0.94f : 1.0f);
    glUniform2f(u_.wind_en, wind_en_mps.x, wind_en_mps.y);
    glUniform1i(u_.quality, quality);
    glUniform1i(u_.season, std::clamp(season, 0, 3));
    glUniform1f(u_.snowline_m, std::clamp(snowline_m, 1000.0f, 5000.0f));
    glUniform4f(u_.grass_zone, grass_zone.center_en.x, grass_zone.center_en.y,
                grass_zone.axis_en.x, grass_zone.axis_en.y);
    glUniform4f(u_.grass_zone_bounds, grass_zone.half_length_m, grass_zone.outer_half_width_m,
                grass_zone.runway_half_width_m, grass_zone.grass_runway ? 1.0f : 0.0f);
    glUniform4f(u_.wx_env, wx_env.x, wx_env.y, wx_env.z, wx_env.w);

    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, inst_ssbo_);
    glBindTextureUnit(0, streamer.height_array_tex());
    glBindTextureUnit(1, streamer.ortho_array_tex());
    glBindTextureUnit(2, streamer.norm_array_tex());
    glBindTextureUnit(3, streamer.surf_array_tex());
    glBindTextureUnit(4, sat_array_tex_);
    glBindTextureUnit(5, rock_albedo_tex_);
    glBindTextureUnit(6, rock_normal_tex_);
    glBindTextureUnit(7, cloud_noise_tex_);
    glBindTextureUnit(8, micro_detail_tex_);
    glBindTextureUnit(9, grass_albedo_tex_);
    glBindTextureUnit(10, grass_normal_tex_);
    glBindTextureUnit(11, gravel_normal_tex_);
    glBindTextureUnit(12, gravel_albedo_tex_);
    glBindTextureUnit(13, refl_env_tex ? refl_env_tex : rock_albedo_tex_);

    glBindVertexArray(vao_);
    glDrawElementsInstanced(GL_TRIANGLES, index_count_, GL_UNSIGNED_SHORT, nullptr, static_cast<GLsizei>(visible_.size()));
    glBindVertexArray(0);
}

} // namespace nsim
