#include "airfield_vegetation.hpp"
#include "../third_party/stb_image.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace nsim {
namespace {

inline uint32_t mix_bits(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    return x ^ (x >> 16);
}

inline float frac01(uint32_t x) {
    return static_cast<float>(x & 0xFFFFu) * (1.0f / 65536.0f);
}

} // namespace

AirfieldVegetation::~AirfieldVegetation() {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        stop_ = true;
    }
    generation_.fetch_add(1, std::memory_order_acq_rel);
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    if (ssbo_) glDeleteBuffers(1, &ssbo_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
    if (leaf_albedo_) glDeleteTextures(1, &leaf_albedo_);
    if (leaf_normal_) glDeleteTextures(1, &leaf_normal_);
    if (bark_albedo_) glDeleteTextures(1, &bark_albedo_);
    if (prog_) glDeleteProgram(prog_);
}

GLuint AirfieldVegetation::load_tex2d(const std::string &path, bool srgb) {
    int w = 0, h = 0, ch = 0;
    unsigned char *pix = stbi_load(path.c_str(), &w, &h, &ch, 4);
    if (!pix || w <= 0 || h <= 0) {
        std::fprintf(stderr, "[VEG] Failed to load texture: %s\n", path.c_str());
        if (pix) stbi_image_free(pix);
        return 0;
    }
    int mips = 1;
    for (int m = std::max(w, h); m > 1; m >>= 1) ++mips;
    GLuint tex = 0;
    glCreateTextures(GL_TEXTURE_2D, 1, &tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTextureStorage2D(tex, mips, srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8, w, h);
    glTextureSubImage2D(tex, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pix);
    stbi_image_free(pix);
    glGenerateTextureMipmap(tex);
    glTextureParameteri(tex, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTextureParameteri(tex, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(tex, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTextureParameteri(tex, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTextureParameterf(tex, GL_TEXTURE_MAX_ANISOTROPY, 4.0f);
    return tex;
}

bool AirfieldVegetation::init(const std::string &assets, const std::string &shaders) {
    prog_ = compile_program_files(shaders + "/airfield_trees.vert", shaders + "/airfield_trees.frag");
    if (!prog_) return false;

    u_vp_       = glGetUniformLocation(prog_, "u_vp");
    u_cam_      = glGetUniformLocation(prog_, "u_camera");
    u_agl_      = glGetUniformLocation(prog_, "u_cam_agl");
    u_time_     = glGetUniformLocation(prog_, "u_time");
    u_wind_     = glGetUniformLocation(prog_, "u_wind_en");
    u_sun_      = glGetUniformLocation(prog_, "u_sun");
    u_cool_     = glGetUniformLocation(prog_, "u_haze_cool");
    u_warm_     = glGetUniformLocation(prog_, "u_haze_warm");
    u_haze_     = glGetUniformLocation(prog_, "u_haze_density");
    u_season_   = glGetUniformLocation(prog_, "u_season");
    u_snowline_ = glGetUniformLocation(prog_, "u_snowline_m");

    leaf_albedo_ = load_tex2d(assets + "/pbr/foliage_cluster_albedo.png", true);
    leaf_normal_ = load_tex2d(assets + "/pbr/foliage_cluster_normal.png", false);
    bark_albedo_ = load_tex2d(assets + "/pbr/bark_src_albedo.jpg", true);
    if (!leaf_albedo_ || !leaf_normal_ || !bark_albedo_) return false;

    glCreateVertexArrays(1, &vao_);
    glCreateBuffers(1, &ssbo_);
    glNamedBufferStorage(ssbo_, static_cast<GLsizeiptr>(sizeof(TreeVolGpu) * max_instances),
                         nullptr, GL_DYNAMIC_STORAGE_BIT);
    stop_ = false;
    worker_ = std::thread(&AirfieldVegetation::worker_loop, this);
    return true;
}

void AirfieldVegetation::invalidate() {
    generation_.fetch_add(1, std::memory_order_acq_rel);
    std::lock_guard<std::mutex> lk(mtx_);
    job_queued_ = false;
    ready_upload_ = false;
    ready_data_.clear();
    prepared_ = false;
    req_pending_coords_ = false;
    count_ = 0;
}

void AirfieldVegetation::worker_loop() {
    while (true) {
        const PackReader *pack = nullptr;
        std::shared_ptr<const PackReader::WorldFrame> frame;
        double lat = 0.0, lon = 0.0;
        uint64_t gen = 0;
        {
            std::unique_lock<std::mutex> lk(mtx_);
            cv_.wait(lk, [&]() { return stop_ || job_queued_; });
            if (stop_) break;
            job_queued_ = false;
            job_running_ = true;
            pack = worker_pack_;
            frame = pack ? pack->world_frame() : nullptr;
            lat = job_lat_;
            lon = job_lon_;
            gen = job_gen_;
        }
        std::vector<TreeVolGpu> upload;
        if (pack && frame && gen == generation_.load(std::memory_order_acquire)) {
            prepare_at(*pack, *frame, lat, lon, nullptr, &upload, gen);
        }
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (pack && frame && gen == generation_.load(std::memory_order_acquire)) {
                ready_centre_ = {
                    static_cast<float>((lon - frame->anchor_lon) * frame->metres_lon),
                    static_cast<float>((lat - frame->anchor_lat) * frame->metres_lat)
                };
                ready_data_ = std::move(upload);
                ready_upload_ = true;
            }
            job_running_ = false;
        }
        cv_.notify_all();
    }
}

void AirfieldVegetation::prepare(const PackReader &pack, const AirportInfo &airport) {
    generation_.fetch_add(1, std::memory_order_acq_rel);
    {
        std::unique_lock<std::mutex> lk(mtx_);
        job_queued_ = false;
        ready_upload_ = false;
        cv_.wait(lk, [&]() { return !job_running_; });
    }
    const auto frame = pack.world_frame();
    if (frame) prepare_at(pack, *frame, airport.lat, airport.lon, airport.id.c_str(), nullptr, 0);
}

void AirfieldVegetation::update_around(const PackReader &pack, double lat_deg, double lon_deg, float agl) {
    if (!prog_ || agl > 350.0f) return;
    double ap_dist = 1e9;
    const AirportInfo *nearest = pack.find_nearest_airport(lat_deg, lon_deg, &ap_dist);
    // Airfield vegetation builds only near airfields (<1.8 km) or very low bush altitude (<115 m AGL)
    // so flying low over cities like Barcelona never wastes CPU scanning urban street graphs.
    if ((!nearest || ap_dist > 1800.0) && agl > 115.0f) return;

    Vec2 cur{static_cast<float>((lon_deg - pack.anchor_lon()) * pack.anchor_m_lon()),
             static_cast<float>((lat_deg - pack.anchor_lat()) * pack.anchor_m_lat())};
    if (prepared_ && std::hypot(cur.x - centre_.x, cur.y - centre_.y) < 340.0f) return;

    std::lock_guard<std::mutex> lk(mtx_);
    if (req_pending_coords_ && std::hypot(cur.x - req_centre_.x, cur.y - req_centre_.y) < 340.0f) return;
    worker_pack_ = &pack;
    job_lat_ = lat_deg;
    job_lon_ = lon_deg;
    job_gen_ = generation_.load(std::memory_order_acquire);
    job_queued_ = true;
    req_centre_ = cur;
    req_pending_coords_ = true;
    cv_.notify_one();
}

void AirfieldVegetation::prepare_at(const PackReader &pack, const PackReader::WorldFrame &frame,
                                    double center_lat, double center_lon, const char *label,
                                    std::vector<TreeVolGpu> *out_upload, uint64_t expected_gen) {
    struct Candidate {
        TreeVolGpu gpu;
        float dist_sq;
        int species;
    };
    std::vector<Candidate> cands;
    cands.reserve(4096);

    const Vec2 local_centre{static_cast<float>((center_lon - frame.anchor_lon) * frame.metres_lon),
                            static_cast<float>((center_lat - frame.anchor_lat) * frame.metres_lat)};
    if (!out_upload) {
        centre_ = local_centre;
        req_centre_ = local_centre;
        prepared_ = true;
        req_pending_coords_ = false;
    }

    // 1. Interlocking woodland groves & hedgerow trees from IGN PNOA-LiDAR canopy heights.
    constexpr float spacing = 10.0f;
    constexpr int radius_m = 700;
    const int steps = static_cast<int>(radius_m / spacing);
    const double cell_base_e = std::floor(local_centre.x / spacing) * spacing;
    const double cell_base_n = std::floor(local_centre.y / spacing) * spacing;

    for (int gy = -steps; gy <= steps; ++gy) {
        if (expected_gen != 0 && expected_gen != generation_.load(std::memory_order_acquire)) return;
        for (int gx = -steps; gx <= steps; ++gx) {
            const double grid_e = cell_base_e + gx * spacing;
            const double grid_n = cell_base_n + gy * spacing;
            const double rel_e0 = grid_e - local_centre.x;
            const double rel_n0 = grid_n - local_centre.y;
            if (rel_e0 * rel_e0 + rel_n0 * rel_n0 > double(radius_m) * radius_m) continue;

            // World-locked deterministic hash so trees never pop/shift when recentering!
            const int32_t ix = static_cast<int32_t>(std::llround(grid_e / spacing));
            const int32_t iy = static_cast<int32_t>(std::llround(grid_n / spacing));
            const uint32_t seed = mix_bits(static_cast<uint32_t>(ix) * 0x9e3779b9u ^
                                           static_cast<uint32_t>(iy) * 0x85ebca6bu);
            const double east = grid_e + (frac01(seed) - 0.5) * spacing * 0.82;
            const double north = grid_n + (frac01(seed >> 11) - 0.5) * spacing * 0.82;
            const double lat = frame.anchor_lat + north / frame.metres_lat;
            const double lon = frame.anchor_lon + east / frame.metres_lon;

            const float lidar = pack.sample_canopy_height(lat, lon);
            const Vec3 surf = pack.sample_surface(lat, lon);
            const float eff_h = (lidar >= 2.4f) ? lidar : (surf.x > 0.45f ? (4.8f + surf.x * 6.0f) : 0.0f);
            if (eff_h < 2.4f || surf.z > 0.22f ||
                pack.is_excluded_for_vegetation(frame, lat, lon) ||
                pack.is_near_building(frame, lat, lon, 2.2) ||
                pack.is_near_road(frame, lat, lon, 2.4)) continue;

            const float ground = static_cast<float>(pack.sample_elevation(lat, lon));
            // Embed root slightly deeper on steep mountain cliffs so trees never float above coarser LOD triangles.
            const float g_e = static_cast<float>(pack.sample_elevation(lat, lon + 5.0 / frame.metres_lon));
            const float g_n = static_cast<float>(pack.sample_elevation(lat + 5.0 / frame.metres_lat, lon));
            const float slope_drop = std::min(1.6f, std::hypot(g_e - ground, g_n - ground) * 0.45f);
            const uint32_t s2 = mix_bits(seed ^ 0x517cc1b7u);
            const float height = std::clamp(eff_h * (0.86f + frac01(s2) * 0.28f), 4.5f, 13.5f);

            // Species distribution tailored to Northern Spain / Pyrenees valleys:
            // 0 = Holm Oak, 1 = Scots Pine, 2 = Valley Poplar, 3 = Maquis Shrub
            int species = 0;
            if (ground > 820.0f && ((s2 >> 18) & 3u) != 0) species = 1;       // Pyrenean Scots Pine
            else if (eff_h >= 8.2f && ((s2 >> 20) & 3u) == 0) species = 2;    // Tall river/hedgerow Poplar
            else if (((s2 >> 22) & 7u) == 0) species = 1;                     // Scattered foothill Pine

            const Vec3 pnoa = pack.sample_imagery_rgb(lat, lon);
            const float sp_seed = static_cast<float>(species) + std::clamp(frac01(s2 >> 8), 0.001f, 0.995f);
            const float de = static_cast<float>(east - local_centre.x);
            const float dn = static_cast<float>(north - local_centre.y);
            cands.push_back({
                {static_cast<float>(east), ground - 0.22f - slope_drop, static_cast<float>(north), height,
                 pnoa.x, pnoa.y, pnoa.z, sp_seed},
                de * de + dn * dn, species
            });
        }
    }

    // 2. Low maquis bushes & woodland-margin shrubs around airstrip perimeters.
    constexpr float sh_spacing = 7.5f;
    constexpr int sh_radius = 400;
    const int sh_steps = static_cast<int>(sh_radius / sh_spacing);
    const double sb_e = std::floor(local_centre.x / sh_spacing) * sh_spacing;
    const double sb_n = std::floor(local_centre.y / sh_spacing) * sh_spacing;

    for (int gy = -sh_steps; gy <= sh_steps; ++gy) {
        if (expected_gen != 0 && expected_gen != generation_.load(std::memory_order_acquire)) return;
        for (int gx = -sh_steps; gx <= sh_steps; ++gx) {
            const double grid_e = sb_e + gx * sh_spacing;
            const double grid_n = sb_n + gy * sh_spacing;
            const double rel_e0 = grid_e - local_centre.x;
            const double rel_n0 = grid_n - local_centre.y;
            if (rel_e0 * rel_e0 + rel_n0 * rel_n0 > double(sh_radius) * sh_radius) continue;

            const int32_t ix = static_cast<int32_t>(std::llround(grid_e / sh_spacing));
            const int32_t iy = static_cast<int32_t>(std::llround(grid_n / sh_spacing));
            const uint32_t seed = mix_bits(static_cast<uint32_t>(ix) * 0x27d4eb2du ^
                                           static_cast<uint32_t>(iy) * 0x165667b1u);
            const double east = grid_e + (frac01(seed) - 0.5) * sh_spacing * 0.80;
            const double north = grid_n + (frac01(seed >> 11) - 0.5) * sh_spacing * 0.80;
            const double lat = frame.anchor_lat + north / frame.metres_lat;
            const double lon = frame.anchor_lon + east / frame.metres_lon;

            const float lidar = pack.sample_canopy_height(lat, lon);
            const Vec3 surf = pack.sample_surface(lat, lon);
            const bool is_shrub = (lidar >= 1.0f && lidar < 2.4f) ||
                                  (lidar < 2.4f && surf.y >= 0.22f && surf.y <= 0.46f && ((seed >> 24) & 3u) == 0);
            if (!is_shrub || surf.z > 0.18f ||
                pack.is_excluded_for_vegetation(frame, lat, lon) ||
                pack.is_near_building(frame, lat, lon, 2.2) ||
                pack.is_near_road(frame, lat, lon, 2.4)) continue;

            const float ground = static_cast<float>(pack.sample_elevation(lat, lon));
            const uint32_t s2 = mix_bits(seed ^ 0x7f4a7c15u);
            const float height = std::clamp(lidar >= 1.0f ? lidar * 1.15f : (1.35f + frac01(s2) * 1.35f), 1.3f, 2.9f);
            const Vec3 pnoa = pack.sample_imagery_rgb(lat, lon);
            const float sp_seed = 3.0f + std::clamp(frac01(s2 >> 8), 0.001f, 0.995f);
            const float de = static_cast<float>(east - local_centre.x);
            const float dn = static_cast<float>(north - local_centre.y);
            cands.push_back({
                {static_cast<float>(east), ground - 0.12f, static_cast<float>(north), height,
                 pnoa.x, pnoa.y, pnoa.z, sp_seed},
                de * de + dn * dn, 3
            });
        }
    }

    // Sort front-to-back from airfield center so Reverse-Z conservative Hi-Z culls background volumes early!
    std::sort(cands.begin(), cands.end(), [](const Candidate &a, const Candidate &b) {
        return a.dist_sq < b.dist_sq;
    });
    if (static_cast<int>(cands.size()) > max_instances) cands.resize(max_instances);

    std::vector<TreeVolGpu> upload;
    upload.reserve(cands.size());
    int sp_counts[4] = {};
    for (const auto &c : cands) {
        upload.push_back(c.gpu);
        sp_counts[c.species & 3]++;
    }
    if (out_upload) {
        *out_upload = std::move(upload);
        return;
    }
    count_ = static_cast<int>(upload.size());
    if (!upload.empty()) {
        glNamedBufferSubData(ssbo_, 0, static_cast<GLsizeiptr>(upload.size() * sizeof(TreeVolGpu)), upload.data());
    }
    if (label) {
        std::printf("[VEG] %s: %d volumetric impostors (oak=%d pine=%d poplar=%d shrub=%d, 0 3D meshes)\n",
                    label, count_, sp_counts[0], sp_counts[1], sp_counts[2], sp_counts[3]);
    }
}

void AirfieldVegetation::draw(const Mat4 &vp, Vec3 camera_enu, const AtmosphereState &atmo,
                              float agl, float time_sec, Vec2 wind_en, int season, float snowline_m,
                              int veg_detail, float density_scale) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (ready_upload_) {
            centre_ = ready_centre_;
            prepared_ = true;
            req_pending_coords_ = false;
            ready_upload_ = false;
            count_ = static_cast<int>(ready_data_.size());
            if (!ready_data_.empty()) {
                glNamedBufferSubData(ssbo_, 0, static_cast<GLsizeiptr>(ready_data_.size() * sizeof(TreeVolGpu)), ready_data_.data());
            }
            ready_data_.clear();
        }
    }
    const float max_agl = (veg_detail >= 3) ? 520.0f : (veg_detail <= 0 ? 220.0f : (veg_detail == 1 ? 420.0f : 365.0f));
    if (!prog_ || count_ <= 0 || agl > max_agl) return;
    if (std::hypot(camera_enu.x - centre_.x, camera_enu.z - centre_.y) > 920.0f) return;
    int draw_count = count_;
    if (veg_detail <= 0) draw_count = std::max(1, count_ / 4);
    else if (veg_detail == 1) draw_count = std::max(1, (count_ * 4) / 5);
    draw_count = std::max(1, static_cast<int>(draw_count * std::clamp(density_scale, 0.35f, 1.0f)));

    glDisable(GL_CULL_FACE);
    glUseProgram(prog_);
    glUniformMatrix4fv(u_vp_, 1, GL_FALSE, vp.m);
    glUniform3f(u_cam_, camera_enu.x, camera_enu.y, camera_enu.z);
    glUniform1f(u_agl_, agl);
    glUniform1f(u_time_, time_sec);
    glUniform2f(u_wind_, wind_en.x, wind_en.y);
    glUniform3f(u_sun_, atmo.sun_dir.x, atmo.sun_dir.y, atmo.sun_dir.z);
    glUniform3f(u_cool_, atmo.haze_cool.x, atmo.haze_cool.y, atmo.haze_cool.z);
    glUniform3f(u_warm_, atmo.haze_warm.x, atmo.haze_warm.y, atmo.haze_warm.z);
    glUniform1f(u_haze_, atmo.haze_density);
    glUniform1i(u_season_, std::clamp(season, 0, 3));
    glUniform1f(u_snowline_, snowline_m);

    glBindTextureUnit(0, leaf_albedo_);
    glBindTextureUnit(1, leaf_normal_);
    glBindTextureUnit(2, bark_albedo_);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, ssbo_);

    // 1 single instanced draw call: 4 verts (2 tris) per volumetric tree!
    glBindVertexArray(vao_);
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, draw_count);
    glBindVertexArray(0);
    glEnable(GL_CULL_FACE);
}

} // namespace nsim
