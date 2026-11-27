#include "pack_reader.hpp"
#include "app_paths.hpp"

#define STB_IMAGE_IMPLEMENTATION
#include "../third_party/stb_image.h"
#include "../third_party/miniz_tinfl.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <set>
#include <thread>

namespace nsim {

namespace {

inline uint64_t double_bits(double v) {
    uint64_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    return bits;
}

inline double bits_double(uint64_t bits) {
    double v = 0.0;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}

inline float smoothstep_f(float e0, float e1, float x) {
    float t = std::clamp((x - e0) / std::max(1e-6f, e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

inline uint32_t hash2i(int x, int y, uint32_t seed) {
    uint32_t h = static_cast<uint32_t>(x) * 374761393u ^ static_cast<uint32_t>(y) * 668265263u ^ seed;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

inline float vnoise2(float x, float y, uint32_t seed) {
    int ix = static_cast<int>(std::floor(x));
    int iy = static_cast<int>(std::floor(y));
    float fx = x - ix, fy = y - iy;
    float ux = fx * fx * (3.0f - 2.0f * fx);
    float uy = fy * fy * (3.0f - 2.0f * fy);
    auto h01 = [&](int a, int b) { return static_cast<float>(hash2i(a, b, seed) & 0xFFFFu) * (1.0f / 65535.0f); };
    float a = h01(ix, iy) * (1.0f - ux) + h01(ix + 1, iy) * ux;
    float b = h01(ix, iy + 1) * (1.0f - ux) + h01(ix + 1, iy + 1) * ux;
    return a * (1.0f - uy) + b * uy;
}

#pragma pack(push, 1)
struct RawScnHeader {
    char magic[4];
    uint32_t version;
    uint32_t bldg_count;
    uint32_t road_count;
    uint32_t road_pt_count;
    float bbox[4];
};
struct RawBldgV2 {
    int32_t lat_e6;
    int32_t lon_e6;
    float width_m;
    float depth_m;
    float height_m;
    float yaw_rad;
    float ground_m;
    uint8_t archetype;
    uint8_t color_var;
    uint8_t roof_r;
    uint8_t roof_g;
    uint8_t roof_b;
    uint8_t canyon_ao;
    uint16_t pad;
};
struct RawRoad {
    uint32_t offset;
    uint16_t count;
    uint8_t kind;
    uint8_t pad;
};
#pragma pack(pop)

// Minimal JSON field extractors for the flat airports array inside our own .nspack manifest
bool extract_json_str(const std::string &obj, const char *key, std::string &out) {
    std::string pat = std::string("\"") + key + "\"";
    size_t pos = obj.find(pat);
    if (pos == std::string::npos) return false;
    pos = obj.find(':', pos + pat.size());
    if (pos == std::string::npos) return false;
    size_t q0 = obj.find('"', pos + 1);
    if (q0 == std::string::npos) return false;
    size_t q1 = obj.find('"', q0 + 1);
    if (q1 == std::string::npos) return false;
    out = obj.substr(q0 + 1, q1 - q0 - 1);
    return true;
}

bool extract_json_num(const std::string &obj, const char *key, double &out) {
    std::string pat = std::string("\"") + key + "\"";
    size_t pos = obj.find(pat);
    if (pos == std::string::npos) return false;
    pos = obj.find(':', pos + pat.size());
    if (pos == std::string::npos) return false;
    size_t s = obj.find_first_of("-0123456789.", pos + 1);
    if (s == std::string::npos) return false;
    out = std::strtod(obj.c_str() + s, nullptr);
    return true;
}

// Queue ranking only needs regional distances. Compute the local Mercator
// scale once so scanning the bounded queue under its mutex stays cheap.
struct OnlineTileFocus {
    double x, y, span_m;

    OnlineTileFocus(double lat, double lon)
        : x((lon + 180.0) / 360.0),
          y((1.0 - std::asinh(std::tan(lat * DEG2RAD_D)) / PI_D) * 0.5),
          span_m(40075016.69 * std::cos(lat * DEG2RAD_D)) {}

    double distance_m(uint64_t key) const {
        const int zoom = static_cast<int>((key >> 48) & 0xFFu);
        const double n = static_cast<double>(1u << zoom);
        const double dx = (((key >> 24) & 0xFFFFFFu) + 0.5) / n - x;
        const double dy = ((key & 0xFFFFFFu) + 0.5) / n - y;
        return std::sqrt(dx * dx + dy * dy) * span_m;
    }
};

inline double online_tile_priority_score(uint64_t okey, double dist_m) {
    const int z = static_cast<int>((okey >> 48) & 0xFFu);
    // Finish nearby detail while retaining priority for midrange carrier imagery.
    if (z >= 19 && dist_m <= 500.0) return std::max(30.0, dist_m * 0.20);
    const double mult = (z <= 15 && dist_m < 24000.0) ? 0.20 : (1.0 + std::max(0, z - 16) * 0.12);
    return std::max(120.0, dist_m) * mult;
}

} // namespace

PackReader::~PackReader() {
    {
        std::lock_guard<std::mutex> lk(online_mtx_);
        online_stop_.store(true, std::memory_order_release);
    }
    online_cv_.notify_all();
    for (auto &t : online_workers_) {
        if (t.joinable()) t.join();
    }
    clear_regions();
}

void PackReader::clear_regions() {
    for (LoadedRegion &r : regions_) {
        close_file(r.pack_fd);
        close_file(r.sh_fd);
    }
    regions_.clear();
    index_.clear();
    airports_.clear();
    buildings_.clear();
    bldg_grid_.clear();
    roads_.clear();
    road_pts_.clear();
    road_grid_.clear();
    elev_zooms_desc_.clear();
    img_zooms_desc_.clear();
    surf_zooms_desc_.clear();
    vegetation_zooms_desc_.clear();
    world_frame_ = std::make_shared<const WorldFrame>();
    world_frame_atomic_.store(world_frame_, std::memory_order_release);
}

void PackReader::set_online_provider(OnlineOrthoProvider p) {
    online_provider_.store(p, std::memory_order_release);
    if (p != OnlineOrthoProvider::OFF) {
        std::error_code ec;
        std::filesystem::create_directories(opensky_cache_dir() / "online_tiles", ec);
        std::lock_guard<std::mutex> lk(online_mtx_);
        if (online_workers_.empty()) {
            online_stop_.store(false, std::memory_order_release);
            for (int i = 0; i < 6; i++) {
                online_workers_.emplace_back(&PackReader::online_dl_worker, this);
            }
        }
    }
}

const char *PackReader::online_provider_name() const {
    return online_provider_.load(std::memory_order_relaxed) == OnlineOrthoProvider::PNOA
        ? "PNOA Z20-14"
        : "OFFLINE";
}

void PackReader::set_online_camera(double lat_deg, double lon_deg) {
    online_cam_lat_bits_.store(double_bits(lat_deg), std::memory_order_relaxed);
    online_cam_lon_bits_.store(double_bits(lon_deg), std::memory_order_relaxed);
}

int PackReader::online_page_zoom(double lat, double lon, double half_span_m, int ortho_res) const {
    if (online_provider() != OnlineOrthoProvider::PNOA) return 0;
    const double cam_lat = bits_double(online_cam_lat_bits_.load(std::memory_order_relaxed));
    const double cam_lon = bits_double(online_cam_lon_bits_.load(std::memory_order_relaxed));
    const double distance = nspain::haversine_m(cam_lat, cam_lon, lat, lon);
    const double texel_m = 2.0 * half_span_m / ortho_res;
    // A 320 m leaf fits a Z20 source grid. Include the nearest leaf even
    // when the camera is exactly on the junction of four leaf pages.
    if (texel_m <= 0.625 && distance <= 230.0) return 20;
    int zoom = online_zoom_for_distance_m(distance);
    const double metres_per_pixel = 156543.03 * std::max(0.2, std::cos(lat * DEG2RAD_D));
    while (zoom > 13 && metres_per_pixel / static_cast<double>(1u << zoom) < texel_m * 0.35) --zoom;
    return zoom;
}

bool PackReader::download_online_tile_to_disk(
    OnlineOrthoProvider prov, int z, int x, int y, const std::string &cache_path
) const {
    if (prov != OnlineOrthoProvider::PNOA) return false;
    char ubuf[384];
    std::snprintf(ubuf, sizeof(ubuf),
        "https://www.ign.es/wmts/pnoa-ma?SERVICE=WMTS&REQUEST=GetTile&VERSION=1.0.0"
        "&LAYER=OI.OrthoimageCoverage&STYLE=default&FORMAT=image/jpeg"
        "&TILEMATRIXSET=GoogleMapsCompatible&TILEMATRIX=%d&TILEROW=%d&TILECOL=%d",
        z, y, x);
    const std::string url = ubuf;

    std::string tmp_path = cache_path + ".tmp." + std::to_string(
        std::hash<std::thread::id>{}(std::this_thread::get_id()) & 0xFFFFu);
    const bool dl_ok = http_download_to_file(
        url, "OpenSky-VFR/0.3 (+https://github.com/BeamAudio/OpenSky)", 2000, 4000, tmp_path);
    int w = 0, h = 0, channels = 0;
    if (dl_ok && file_larger_than(tmp_path, 512) &&
        stbi_info(tmp_path.c_str(), &w, &h, &channels) && w == 256 && h == 256 &&
        atomic_replace_file(tmp_path, cache_path)) {
        online_fetched_count_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    std::error_code rm_ec;
    std::filesystem::remove(tmp_path, rm_ec);
    return false;
}

void PackReader::online_dl_worker() const {
    while (!online_stop_.load(std::memory_order_acquire)) {
        uint64_t okey = 0;
        {
            std::unique_lock<std::mutex> lk(online_mtx_);
            online_cv_.wait(lk, [&]() {
                return online_stop_.load(std::memory_order_acquire) || !online_dl_queue_.empty();
            });
            if (online_stop_.load(std::memory_order_acquire)) break;
            if (online_dl_queue_.empty()) continue;
            const double cam_lat = bits_double(online_cam_lat_bits_.load(std::memory_order_relaxed));
            const double cam_lon = bits_double(online_cam_lon_bits_.load(std::memory_order_relaxed));
            const bool have_cam = (std::abs(cam_lat) > 0.1 || std::abs(cam_lon) > 0.1);
            size_t best_idx = online_dl_queue_.size() - 1;
            if (have_cam) {
                const OnlineTileFocus focus(cam_lat, cam_lon);
                std::erase_if(online_dl_queue_, [&](uint64_t key) {
                    if (focus.distance_m(key) <= 55000.0) return false;
                    online_dl_seen_.erase(key);
                    return true;
                });
                double best_score = 1e18;
                for (size_t qi = 0; qi < online_dl_queue_.size(); ++qi) {
                    const uint64_t kq = online_dl_queue_[qi];
                    const double sc = online_tile_priority_score(kq, focus.distance_m(kq));
                    if (sc < best_score) {
                        best_score = sc;
                        best_idx = qi;
                    }
                }
            }
            if (online_dl_queue_.empty()) continue;
            okey = online_dl_queue_[best_idx];
            online_dl_queue_[best_idx] = online_dl_queue_.back();
            online_dl_queue_.pop_back();
        }
        int prov_id = static_cast<int>((okey >> 56) & 0xFFu) - 10;
        int z       = static_cast<int>((okey >> 48) & 0xFFu);
        int x       = static_cast<int>((okey >> 24) & 0xFFFFFFu);
        int y       = static_cast<int>(okey & 0xFFFFFFu);
        if (prov_id != static_cast<int>(OnlineOrthoProvider::PNOA)) continue;
        auto prov = OnlineOrthoProvider::PNOA;
        const std::string cpath = (opensky_cache_dir() / "online_tiles" /
            ("pnoa_z" + std::to_string(z) + "_" + std::to_string(x) + "_" + std::to_string(y) + ".jpg")).string();
        bool ok = file_larger_than(cpath, 512);
        if (!ok) {
            ok = download_online_tile_to_disk(prov, z, x, y, cpath);
        }
        {
            std::lock_guard<std::mutex> lk(online_mtx_);
            online_dl_seen_.erase(okey);
            if (ok) {
                online_failed_until_.erase(okey);
                online_revision_.fetch_add(1, std::memory_order_release);
            } else {
                const double now_s = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
                if (online_failed_until_.size() > 8192) online_failed_until_.clear();
                online_failed_until_[okey] = now_s + 30.0;
            }
        }
    }
}

std::shared_ptr<DecodedRgbTile> PackReader::get_online_rgb_tile(
    OnlineOrthoProvider prov, int z, int x, int y,
    bool queue_download
) const {
    if (prov != OnlineOrthoProvider::PNOA) return nullptr;
    uint8_t layer_id = static_cast<uint8_t>(10 + static_cast<int>(prov));
    uint64_t okey = nspain::make_tile_key(layer_id, static_cast<uint8_t>(z), static_cast<uint32_t>(x), static_cast<uint32_t>(y));
    if (auto hit = online_cache_.get(okey)) {
        online_used_count_.fetch_add(1, std::memory_order_relaxed);
        return hit;
    }

    const std::string cpath = (opensky_cache_dir() / "online_tiles" /
        ("pnoa_z" + std::to_string(z) + "_" + std::to_string(x) + "_" + std::to_string(y) + ".jpg")).string();
    if (!file_larger_than(cpath, 512)) {
        if (!queue_download) return nullptr;
        bool should_notify = false;
        {
            std::lock_guard<std::mutex> lk(online_mtx_);
            const double now_s = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
            auto fit = online_failed_until_.find(okey);
            if (fit != online_failed_until_.end() && now_s < fit->second) return nullptr;
            if (online_dl_seen_.size() > 4096) online_dl_seen_.clear();
            if (online_dl_seen_.find(okey) == online_dl_seen_.end()) {
                if (online_dl_queue_.size() >= 256) {
                    const double cam_lat = bits_double(online_cam_lat_bits_.load(std::memory_order_relaxed));
                    const double cam_lon = bits_double(online_cam_lon_bits_.load(std::memory_order_relaxed));
                    const OnlineTileFocus focus(cam_lat, cam_lon);
                    size_t worst_idx = 0;
                    double worst_score = -1.0;
                    for (size_t qi = 0; qi < online_dl_queue_.size(); ++qi) {
                        const uint64_t queued = online_dl_queue_[qi];
                        const double score = online_tile_priority_score(queued, focus.distance_m(queued));
                        if (score > worst_score) {
                            worst_score = score;
                            worst_idx = qi;
                        }
                    }
                    if (online_tile_priority_score(okey, focus.distance_m(okey)) >= worst_score) return nullptr;
                    online_dl_seen_.erase(online_dl_queue_[worst_idx]);
                    online_dl_queue_[worst_idx] = online_dl_queue_.back();
                    online_dl_queue_.pop_back();
                }
                online_dl_seen_.insert(okey);
                online_dl_queue_.push_back(okey);
                should_notify = true;
            }
        }
        if (should_notify) online_cv_.notify_one();
        return nullptr;
    }

    std::vector<uint8_t> raw;
    if (!read_entire_file_checked(cpath, 512, raw)) return nullptr;

    int w = 0, h = 0, ch = 0;
    unsigned char *pix = stbi_load_from_memory(raw.data(), static_cast<int>(raw.size()), &w, &h, &ch, 3);
    if (!pix || w <= 0 || h <= 0) {
        if (pix) stbi_image_free(pix);
        return nullptr;
    }
    auto tile = std::make_shared<DecodedRgbTile>();
    tile->width = static_cast<uint16_t>(w);
    tile->height = static_cast<uint16_t>(h);
    tile->rgb.assign(pix, pix + static_cast<size_t>(w) * h * 3);
    stbi_image_free(pix);
    online_cache_.put(okey, tile);
    online_used_count_.fetch_add(1, std::memory_order_relaxed);
    return tile;
}

void PackReader::prefetch_online_box(double center_lat, double center_lon, double half_span_m, int zoom) const {
    OnlineOrthoProvider prov = online_provider_.load(std::memory_order_relaxed);
    if (prov == OnlineOrthoProvider::OFF) return;
    std::error_code ec;
    std::filesystem::create_directories(opensky_cache_dir() / "online_tiles", ec);
    double m_lat = 111132.0, m_lon = 82600.0;
    nspain::meters_per_degree(center_lat, m_lat, m_lon);
    double dlat = half_span_m / m_lat;
    double dlon = half_span_m / m_lon;
    nspain::MercatorTilePos p0 = nspain::latlon_to_tile(center_lat + dlat, center_lon - dlon, zoom);
    nspain::MercatorTilePos p1 = nspain::latlon_to_tile(center_lat - dlat, center_lon + dlon, zoom);
    int tx0 = std::min(p0.tx, p1.tx), tx1 = std::max(p0.tx, p1.tx);
    int ty0 = std::min(p0.ty, p1.ty), ty1 = std::max(p0.ty, p1.ty);
    struct TileCoord { int tx, ty; };
    std::vector<TileCoord> coords;
    for (int ty = ty0; ty <= ty1; ty++) {
        for (int tx = tx0; tx <= tx1; tx++) {
            coords.push_back({tx, ty});
        }
    }
    int n = static_cast<int>(coords.size());
    int nw = std::min(n, 6);
    auto warm = [&](int tx, int ty) {
        const std::string cpath = (opensky_cache_dir() / "online_tiles" /
            ("pnoa_z" + std::to_string(zoom) + "_" + std::to_string(tx) + "_" + std::to_string(ty) + ".jpg")).string();
        if (!file_larger_than(cpath, 512)) download_online_tile_to_disk(prov, zoom, tx, ty, cpath);
        get_online_rgb_tile(prov, zoom, tx, ty);
    };
    if (nw <= 1) {
        for (const auto &c : coords) warm(c.tx, c.ty);
    } else {
        std::atomic<int> next_idx{0};
        std::vector<std::thread> pool;
        for (int w = 0; w < nw; w++) {
            pool.emplace_back([&]() {
                while (true) {
                    int idx = next_idx.fetch_add(1, std::memory_order_relaxed);
                    if (idx >= n) break;
                    warm(coords[idx].tx, coords[idx].ty);
                }
            });
        }
        for (auto &t : pool) t.join();
    }
}

int PackReader::city_shadow_tile_count() const {
    int tot = 0;
    for (const auto &r : regions_) tot += static_cast<int>(r.sh_idx.size());
    return tot;
}

bool PackReader::load_city_shadows(const std::string &nsshd_path, uint32_t region_idx) {
    if (region_idx >= regions_.size()) return false;
    LoadedRegion &reg = regions_[region_idx];
    close_file(reg.sh_fd);
    reg.sh_idx.clear();
    FileHandle fd = open_read_only(nsshd_path);
    if (fd == kInvalidFileHandle) return false;
#pragma pack(push, 1)
    struct RawShHdr { char magic[4]; uint32_t version; uint32_t res; float chunk_m; uint32_t count; };
    struct RawShIdx { int32_t cx, cy; uint32_t offset, comp_size; };
#pragma pack(pop)
    RawShHdr hdr{};
    if (!read_at_offset(fd, &hdr, sizeof(hdr), 0) ||
        std::memcmp(hdr.magic, "NSSH", 4) != 0 || hdr.version != 2 || hdr.res < 16 || hdr.res > 512) {
        close_file(fd);
        return false;
    }
    std::vector<RawShIdx> idx(hdr.count);
    const size_t idx_bytes = idx.size() * sizeof(RawShIdx);
    if (idx_bytes > 0 && !read_at_offset(fd, idx.data(), idx_bytes, sizeof(hdr))) {
        close_file(fd);
        return false;
    }
    reg.sh_res = static_cast<int>(hdr.res);
    reg.sh_chunk_m = std::max(50.0f, hdr.chunk_m);
    reg.sh_idx.reserve(hdr.count * 2 + 16);
    for (const RawShIdx &e : idx) {
        reg.sh_idx[pack_cell(e.cx, e.cy)] = {e.offset, e.comp_size};
    }
    reg.sh_fd = fd;
    return true;
}

const PackReader::LoadedRegion *PackReader::find_sh_region_at_ll(double lat_deg, double lon_deg) const {
    const LoadedRegion *best = nullptr;
    double best_area = 1e18;
    for (const auto &r : regions_) {
        if (r.sh_fd == kInvalidFileHandle || r.sh_idx.empty()) continue;
        if (lon_deg >= r.hdr.lon_min && lon_deg <= r.hdr.lon_max &&
            lat_deg >= r.hdr.lat_min && lat_deg <= r.hdr.lat_max) {
            double area = (r.hdr.lon_max - r.hdr.lon_min) * (r.hdr.lat_max - r.hdr.lat_min);
            if (area < best_area) { best_area = area; best = &r; }
        }
    }
    return best;
}

std::shared_ptr<std::vector<uint32_t>> PackReader::get_city_sh_tile(const LoadedRegion &reg, int32_t cx, int32_t cy) const {
    if (reg.sh_fd == kInvalidFileHandle || reg.sh_idx.empty()) return nullptr;
    int64_t k = pack_cell(cx, cy);
    uint64_t uk = (static_cast<uint64_t>(reg.idx) << 44u) ^ (static_cast<uint64_t>(k) & 0x00000FFFFFFFFFFFULL);
    if (auto hit = city_sh_cache_.get(uk)) return hit;
    auto it = reg.sh_idx.find(k);
    if (it == reg.sh_idx.end()) return nullptr;
    const CityShEntry &e = it->second;
    std::vector<uint8_t> comp(e.comp_size);
    if (!read_at_offset(reg.sh_fd, comp.data(), e.comp_size, e.offset)) {
        return nullptr;
    }
    size_t n_px = static_cast<size_t>(reg.sh_res) * static_cast<size_t>(reg.sh_res);
    size_t raw_sz = n_px * sizeof(uint32_t);
    auto out = std::make_shared<std::vector<uint32_t>>(n_px);
    size_t dec = tinfl_decompress_mem_to_mem(out->data(), raw_sz, comp.data(), comp.size(), TINFL_FLAG_PARSE_ZLIB_HEADER);
    if (dec != raw_sz) return nullptr;
    city_sh_cache_.put(uk, out);
    return out;
}

bool PackReader::append_pack_internal(const std::string &nspack_path) {
    FileHandle fd = open_read_only(nspack_path);
    if (fd == kInvalidFileHandle) {
        std::fprintf(stderr, "[PACK] Failed to open %s\n", nspack_path.c_str());
        return false;
    }
    nspain::PackHeader hdr{};
    if (!read_at_offset(fd, &hdr, sizeof(hdr), 0) ||
        std::memcmp(hdr.magic, nspain::PACK_MAGIC, 4) != 0 || hdr.version != nspain::PACK_VERSION) {
        std::fprintf(stderr, "[PACK] Invalid .nspack header in %s\n", nspack_path.c_str());
        close_file(fd);
        return false;
    }

    uint64_t file_size = 0;
    if (!get_file_size(fd, file_size)) {
        close_file(fd);
        return false;
    }
    const auto within_file = [file_size](uint64_t offset, uint64_t size) {
        return offset >= sizeof(nspain::PackHeader) && offset <= file_size &&
               size <= file_size - offset;
    };
    const auto index_size = static_cast<uint64_t>(hdr.index_count) * sizeof(nspain::PackTileEntry);
    constexpr uint64_t max_manifest_size = 64 * 1024 * 1024;
    constexpr uint64_t max_index_size = 256 * 1024 * 1024;
    if (index_size > max_index_size || !within_file(hdr.index_offset, index_size) ||
        hdr.manifest_size > max_manifest_size ||
        (hdr.manifest_size != 0 && !within_file(hdr.manifest_offset, hdr.manifest_size)) ||
        !(hdr.lat_min >= -90 && hdr.lat_min < hdr.lat_max && hdr.lat_max <= 90 &&
          hdr.lon_min >= -180 && hdr.lon_min < hdr.lon_max && hdr.lon_max <= 180)) {
        std::fprintf(stderr, "[PACK] Invalid bounds, index or manifest in %s\n", nspack_path.c_str());
        close_file(fd);
        return false;
    }

    std::vector<nspain::PackTileEntry> raw_idx(hdr.index_count);
    const size_t idx_bytes = raw_idx.size() * sizeof(nspain::PackTileEntry);
    if (idx_bytes > 0 && !read_at_offset(fd, raw_idx.data(), idx_bytes, hdr.index_offset)) {
        close_file(fd);
        return false;
    }
    for (const auto &tile : raw_idx) {
        const auto zoom = (tile.key >> 48) & 0xff;
        const auto x = (tile.key >> 24) & 0xffffff;
        const auto y = tile.key & 0xffffff;
        constexpr uint32_t max_tile_bytes = 16 * 1024 * 1024;
        constexpr uint16_t max_tile_side = 2048;
        if (tile.size == 0 || tile.size > max_tile_bytes || !within_file(tile.offset, tile.size) ||
            tile.width == 0 || tile.width > max_tile_side || tile.height == 0 || tile.height > max_tile_side ||
            zoom > 23 || x >= (1u << zoom) || y >= (1u << zoom)) {
            std::fprintf(stderr, "[PACK] Invalid tile record in %s\n", nspack_path.c_str());
            close_file(fd);
            return false;
        }
    }
    std::string manifest(hdr.manifest_size, '\0');
    if (hdr.manifest_size > 0 &&
        !read_at_offset(fd, manifest.data(), hdr.manifest_size, hdr.manifest_offset)) {
        close_file(fd);
        return false;
    }

    uint32_t ridx = static_cast<uint32_t>(regions_.size());
    regions_.emplace_back();
    LoadedRegion &reg = regions_.back();
    reg.idx = ridx;
    reg.pack_path = nspack_path;
    reg.hdr = hdr;
    reg.pack_fd = fd;
    reg.anchor_lon = 0.5 * (hdr.lon_min + hdr.lon_max);
    reg.anchor_lat = 0.5 * (hdr.lat_min + hdr.lat_max);
    nspain::meters_per_degree(reg.anchor_lat, reg.m_lat, reg.m_lon);

    std::filesystem::path pp(nspack_path);
    std::string stem = pp.stem().string();
    if (!extract_json_str(manifest, "region_id", reg.region_id)) reg.region_id = stem;
    std::string sb = reg.region_id;
    const std::string suf = "_pyrenees";
    if (sb.size() > suf.size() && sb.compare(sb.size() - suf.size(), suf.size(), suf) == 0) {
        sb.erase(sb.size() - suf.size());
    }
    reg.stem_base = sb;

    auto find_companion = [&](const std::string &tail) -> std::string {
        std::filesystem::path dir = pp.parent_path();
        for (const std::string &cand_base : {sb, reg.region_id, stem}) {
            std::filesystem::path c = dir / (cand_base + tail);
            if (std::filesystem::exists(c)) return c.string();
        }
        return {};
    };
    reg.city_path = find_companion("_cities.nscity");
    reg.scenery_path = find_companion("_scenery.nsscn");
    reg.weather_path = find_companion("_weather.nswx");

    index_.reserve(index_.size() + raw_idx.size());
    for (const auto &e : raw_idx) {
        index_.push_back({e.key, ridx, e});
    }
    parse_airports_from_manifest(manifest, reg.region_id);

    std::string sh_path = find_companion("_city_sh.nsshd");
    if (!sh_path.empty()) load_city_shadows(sh_path, ridx);
    if (!reg.scenery_path.empty()) load_scenery(reg.scenery_path, true);
    return true;
}

void PackReader::finalize_packs(double anchor_lat, double anchor_lon) {
    if (regions_.empty()) return;
    header_ = regions_[0].hdr;
    for (const auto &r : regions_) {
        header_.lon_min = std::min(header_.lon_min, r.hdr.lon_min);
        header_.lat_min = std::min(header_.lat_min, r.hdr.lat_min);
        header_.lon_max = std::max(header_.lon_max, r.hdr.lon_max);
        header_.lat_max = std::max(header_.lat_max, r.hdr.lat_max);
    }
    // For equal layer and XYZ keys the first loaded pack wins (user packs precede shipped packs).
    // All tile readers resolve through find_entry(), including cached sampling.
    std::sort(index_.begin(), index_.end(), [](const IndexedTile &a, const IndexedTile &b) {
        if (a.key != b.key) return a.key < b.key;
        return a.pack_idx < b.pack_idx;
    });

    std::set<int, std::greater<int>> ez, iz, sz, vz;
    for (const auto &e : index_) {
        uint8_t layer = static_cast<uint8_t>((e.key >> 56) & 0xFFu);
        int z = static_cast<int>((e.key >> 48) & 0xFFu);
        if (layer == nspain::LAYER_ELEVATION) ez.insert(z);
        else if (layer == nspain::LAYER_IMAGERY) iz.insert(z);
        else if (layer == nspain::LAYER_SURFACE) sz.insert(z);
        else if (layer == nspain::LAYER_VEGETATION) vz.insert(z);
    }
    elev_zooms_desc_.assign(ez.begin(), ez.end());
    img_zooms_desc_.assign(iz.begin(), iz.end());
    surf_zooms_desc_.assign(sz.begin(), sz.end());
    vegetation_zooms_desc_.assign(vz.begin(), vz.end());

    // Airport buildings parsed during append_pack_internal ran before index_/elev_zooms_desc_ were ready;
    // resolve their true terrain elevation now so sea-level/highland terminals sit flush on the ground.
    for (BuildingRec &b : buildings_) {
        if (b.airport_feature) {
            b.ground_m = static_cast<float>(sample_elevation(b.lat, b.lon));
        }
    }

    reanchor_to(anchor_lat, anchor_lon);
}

bool PackReader::load_pack(const std::string &nspack_path) {
    clear_regions();
    if (!append_pack_internal(nspack_path)) return false;
    finalize_packs(regions_[0].anchor_lat, regions_[0].anchor_lon);
    return true;
}

bool PackReader::load_packs_dir(const std::string &packs_dir, const std::string &only_region_id, const std::string &prefer_region_id) {
    return load_packs_dirs({packs_dir}, only_region_id, prefer_region_id);
}

bool PackReader::load_packs_dirs(const std::vector<std::string> &packs_dirs, const std::string &only_region_id, const std::string &prefer_region_id) {
    clear_regions();
    std::error_code ec;
    std::vector<std::filesystem::path> files;
    std::unordered_set<std::string> seen_pack_stems;
    for (const auto &packs_dir : packs_dirs) {
        ec.clear();
        if (!std::filesystem::is_directory(packs_dir, ec)) continue;
        std::vector<std::filesystem::path> dir_files;
        for (const auto &ent : std::filesystem::directory_iterator(packs_dir, ec)) {
            if (ent.is_regular_file() && ent.path().extension() == ".nspack") {
                if (!only_region_id.empty()) {
                    std::string stem = ent.path().stem().string();
                    if (stem != only_region_id && stem.find(only_region_id) == std::string::npos) continue;
                }
                if (seen_pack_stems.insert(ent.path().stem().string()).second)
                    dir_files.push_back(ent.path());
            }
        }
        std::sort(dir_files.begin(), dir_files.end());
        files.insert(files.end(), dir_files.begin(), dir_files.end());
    }
    for (const auto &p : files) {
        append_pack_internal(p.string());
    }
    if (regions_.empty()) {
        std::fprintf(stderr, "[PACK] No installed .nspack found in the configured packs directories%s\n",
                     only_region_id.empty() ? "" : (" for region " + only_region_id).c_str());
        return false;
    }
    const LoadedRegion *start = nullptr;
    if (!prefer_region_id.empty()) start = find_region_by_id(prefer_region_id);
    if (!start) start = find_region_by_id("es_aragon_pyrenees");
    if (!start) start = &regions_.front();
    finalize_packs(start->anchor_lat, start->anchor_lon);
    std::printf("[ECOSYSTEM] Loaded %zu region pack(s), %zu tiles, %zu airports | active=%s\n",
                regions_.size(), index_.size(), airports_.size(), start->region_id.c_str());
    return true;
}

const PackReader::LoadedRegion *PackReader::find_region_at(double lat_deg, double lon_deg) const {
    const LoadedRegion *best = nullptr;
    double best_area = 1e18;
    for (const auto &r : regions_) {
        if (lon_deg >= r.hdr.lon_min && lon_deg <= r.hdr.lon_max &&
            lat_deg >= r.hdr.lat_min && lat_deg <= r.hdr.lat_max) {
            double area = (r.hdr.lon_max - r.hdr.lon_min) * (r.hdr.lat_max - r.hdr.lat_min);
            if (area < best_area) { best_area = area; best = &r; }
        }
    }
    return best;
}

const PackReader::LoadedRegion *PackReader::find_region_by_id(const std::string &region_id) const {
    for (const auto &r : regions_) {
        if (r.region_id == region_id) return &r;
    }
    return nullptr;
}

std::string PackReader::active_weather_path(double lat_deg, double lon_deg) const {
    if (const LoadedRegion *r = find_region_at(lat_deg, lon_deg)) {
        return r->weather_path;
    }
    // Never apply weather from an unrelated installed region outside its bounds.
    return {};
}

std::shared_ptr<const PackReader::WorldFrame> PackReader::prepare_world_frame(
    double lat_deg, double lon_deg, bool snap_to_region
) const {
    // Only snap to a region's default center when within 45 km of it; larger regions
    // (e.g. 250 km Catalonia Coast) exceed the +-81.92 km CDLOD quadtree if snapped far from target.
    if (snap_to_region) {
        if (const LoadedRegion *r = find_region_at(lat_deg, lon_deg)) {
            if (nspain::haversine_m(lat_deg, lon_deg, r->anchor_lat, r->anchor_lon) <= 45000.0) {
                lat_deg = r->anchor_lat;
                lon_deg = r->anchor_lon;
            }
        }
    }
    const auto cur = world_frame();
    if (cur && !cur->dem.empty() &&
        nspain::haversine_m(lat_deg, lon_deg, cur->anchor_lat, cur->anchor_lon) < 500.0) {
        return nullptr;
    }
    auto next = std::make_shared<WorldFrame>();
    next->anchor_lat = lat_deg;
    next->anchor_lon = lon_deg;
    nspain::meters_per_degree(lat_deg, next->metres_lat, next->metres_lon);
    bake_corridor_dem(*next);
    return next;
}

void PackReader::commit_world_frame(std::shared_ptr<const WorldFrame> frame) {
    if (!frame) return;
    world_frame_ = std::move(frame);
    world_frame_atomic_.store(world_frame_, std::memory_order_release);
    set_online_camera(world_frame_->anchor_lat, world_frame_->anchor_lon);
}

bool PackReader::reanchor_to(double lat_deg, double lon_deg) {
    auto next = prepare_world_frame(lat_deg, lon_deg, true);
    if (!next) return false;
    commit_world_frame(std::move(next));
    return true;
}

void PackReader::parse_airports_from_manifest(const std::string &json, const std::string &region_id) {
    size_t ap_pos = json.find("\"airports\"");
    if (ap_pos == std::string::npos) return;
    size_t arr_start = json.find('[', ap_pos);
    if (arr_start == std::string::npos) return;

    size_t i = arr_start + 1;
    auto json_objects = [](const std::string &src, const char *key) {
        std::vector<std::string> result;
        const std::string pat = std::string("\"") + key + "\"";
        size_t pos = src.find(pat);
        if (pos == std::string::npos) return result;
        const size_t begin = src.find('[', pos + pat.size());
        if (begin == std::string::npos) return result;
        size_t cursor = begin + 1;
        while (cursor < src.size()) {
            cursor = src.find_first_not_of(" \t\r\n,", cursor);
            if (cursor == std::string::npos || src[cursor] == ']') break;
            if (src[cursor] != '{') { ++cursor; continue; }
            const size_t object_begin = cursor;
            int depth = 0;
            bool quoted = false, escaped = false;
            for (; cursor < src.size(); ++cursor) {
                const char ch = src[cursor];
                if (quoted) {
                    if (escaped) escaped = false;
                    else if (ch == '\\') escaped = true;
                    else if (ch == '"') quoted = false;
                } else if (ch == '"') quoted = true;
                else if (ch == '{') ++depth;
                else if (ch == '}' && --depth == 0) {
                    ++cursor;
                    result.emplace_back(src.substr(object_begin, cursor - object_begin));
                    break;
                }
            }
            if (cursor >= src.size()) break;
        }
        return result;
    };
    auto json_points = [](const std::string &src) {
        std::vector<std::pair<double, double>> result;
        const size_t key = src.find("\"pts\"");
        if (key == std::string::npos) return result;
        const size_t begin = src.find('[', key);
        if (begin == std::string::npos) return result;
        size_t end = begin;
        int depth = 0;
        bool quoted = false, escaped = false;
        for (; end < src.size(); ++end) {
            const char ch = src[end];
            if (quoted) {
                if (escaped) escaped = false;
                else if (ch == '\\') escaped = true;
                else if (ch == '"') quoted = false;
            } else if (ch == '"') quoted = true;
            else if (ch == '[') ++depth;
            else if (ch == ']' && --depth == 0) break;
        }
        if (end >= src.size()) return result;
        for (size_t p = begin + 1; p < end;) {
            if (src[p] != '[') { ++p; continue; }
            char *after_lat = nullptr;
            const double lat = std::strtod(src.c_str() + p + 1, &after_lat);
            if (after_lat == src.c_str() + p + 1) { ++p; continue; }
            size_t comma = static_cast<size_t>(after_lat - src.c_str());
            comma = src.find(',', comma);
            if (comma == std::string::npos || comma >= end) { ++p; continue; }
            char *after_lon = nullptr;
            const double lon = std::strtod(src.c_str() + comma + 1, &after_lon);
            if (after_lon == src.c_str() + comma + 1) { ++p; continue; }
            result.emplace_back(lat, lon);
            p = static_cast<size_t>(after_lon - src.c_str());
        }
        if (result.size() > 3 && std::abs(result.front().first - result.back().first) < 1e-7 &&
            std::abs(result.front().second - result.back().second) < 1e-7) result.pop_back();
        return result;
    };
    while (i < json.size()) {
        size_t o0 = json.find('{', i);
        size_t arr_end = json.find(']', i);
        if (o0 == std::string::npos || (arr_end != std::string::npos && arr_end < o0)) break;
        int depth = 0;
        size_t o1 = o0;
        for (; o1 < json.size(); o1++) {
            if (json[o1] == '{') depth++;
            else if (json[o1] == '}') {
                depth--;
                if (depth == 0) break;
            }
        }
        if (o1 >= json.size()) break;
        std::string obj = json.substr(o0, o1 - o0 + 1);
        i = o1 + 1;

        AirportInfo ap{};
        if (!extract_json_str(obj, "id", ap.id)) continue;
        ap.region_id = region_id;
        extract_json_str(obj, "name", ap.name);
        extract_json_str(obj, "kind", ap.kind);
        extract_json_str(obj, "surface", ap.surface);
        extract_json_str(obj, "freq_type", ap.freq_type);
        extract_json_num(obj, "lat", ap.lat);
        extract_json_num(obj, "lon", ap.lon);
        extract_json_num(obj, "elev_m", ap.elev_m);
        extract_json_num(obj, "freq_mhz", ap.freq_mhz);
        ap.rwy_lat_a = ap.lat; ap.rwy_lon_a = ap.lon; ap.rwy_elev_a = ap.elev_m;
        ap.rwy_lat_b = ap.lat + 0.004; ap.rwy_lon_b = ap.lon; ap.rwy_elev_b = ap.elev_m;
        // Keep every OSM/OurAirports runway. Large fields commonly have two or
        // more strips; treating the first as the entire airport hid their real
        // layout and broke runway clearance, turf placement and rendering.
        for (const std::string &rwo : json_objects(obj, "runways")) {
            AirportRunway rw{};
            rw.lat_a = ap.lat; rw.lon_a = ap.lon; rw.elev_a_m = ap.elev_m;
            rw.lat_b = ap.lat + 0.004; rw.lon_b = ap.lon; rw.elev_b_m = ap.elev_m;
            extract_json_num(rwo, "lat_a", rw.lat_a);
            extract_json_num(rwo, "lon_a", rw.lon_a);
            extract_json_num(rwo, "lat_b", rw.lat_b);
            extract_json_num(rwo, "lon_b", rw.lon_b);
            extract_json_num(rwo, "elev_a_m", rw.elev_a_m);
            extract_json_num(rwo, "elev_b_m", rw.elev_b_m);
            extract_json_num(rwo, "elev_a", rw.elev_a_m);
            extract_json_num(rwo, "elev_b", rw.elev_b_m);
            extract_json_num(rwo, "width_m", rw.width_m);
            extract_json_str(rwo, "surface", rw.surface);
            if (rw.width_m < 10.0) rw.width_m = 22.0;
            if (nspain::haversine_m(rw.lat_a, rw.lon_a, rw.lat_b, rw.lon_b) >= 100.0)
                ap.runways.push_back(std::move(rw));
        }
        if (!ap.runways.empty()) {
            const AirportRunway &rw = ap.runways.front();
            ap.rwy_lat_a = rw.lat_a; ap.rwy_lon_a = rw.lon_a; ap.rwy_elev_a = rw.elev_a_m;
            ap.rwy_lat_b = rw.lat_b; ap.rwy_lon_b = rw.lon_b; ap.rwy_elev_b = rw.elev_b_m;
            ap.rwy_width_m = rw.width_m; ap.surface = rw.surface;
        }

        // Keep the mapped taxiways and apron polygons for every airport.
        // These are shared region data, not home-airport scenery.
        for (const std::string &feature : json_objects(obj, "taxiways")) {
            AirportGroundFeature ground{};
            ground.kind = AirportGroundFeature::Kind::Taxiway;
            extract_json_num(feature, "width_m", ground.width_m);
            extract_json_str(feature, "surface", ground.surface);
            ground.points_lat_lon = json_points(feature);
            if (ground.points_lat_lon.size() >= 2) ap.ground_features.push_back(std::move(ground));
        }
        for (const std::string &feature : json_objects(obj, "aprons")) {
            AirportGroundFeature ground{};
            ground.kind = AirportGroundFeature::Kind::Apron;
            extract_json_str(feature, "surface", ground.surface);
            ground.points_lat_lon = json_points(feature);
            if (ground.points_lat_lon.size() >= 4) ap.ground_features.push_back(std::move(ground));
        }
        for (const std::string &feature : json_objects(obj, "access_roads")) {
            AirportGroundFeature ground{};
            ground.kind = AirportGroundFeature::Kind::AccessRoad;
            ground.width_m = 5.0;
            extract_json_num(feature, "width_m", ground.width_m);
            extract_json_str(feature, "surface", ground.surface);
            ground.points_lat_lon = json_points(feature);
            if (ground.points_lat_lon.size() >= 2) ap.ground_features.push_back(std::move(ground));
        }
        for (const std::string &feature : json_objects(obj, "fences")) {
            AirportFence fn{};
            extract_json_str(feature, "kind", fn.kind);
            fn.points_lat_lon = json_points(feature);
            if (fn.points_lat_lon.size() >= 2) ap.fences.push_back(std::move(fn));
        }
        {
            const size_t wk = obj.find("\"windsocks\"");
            if (wk != std::string::npos) {
                const size_t wb = obj.find('[', wk);
                if (wb != std::string::npos) {
                    size_t we = wb;
                    int wd = 0;
                    for (; we < obj.size(); ++we) {
                        if (obj[we] == '[') ++wd;
                        else if (obj[we] == ']' && --wd == 0) break;
                    }
                    for (size_t p = wb + 1; p < we;) {
                        if (obj[p] != '[') { ++p; continue; }
                        char *al = nullptr;
                        double wlat = std::strtod(obj.c_str() + p + 1, &al);
                        if (al == obj.c_str() + p + 1) { ++p; continue; }
                        size_t cm = obj.find(',', static_cast<size_t>(al - obj.c_str()));
                        if (cm == std::string::npos || cm >= we) { ++p; continue; }
                        char *ao = nullptr;
                        double wlon = std::strtod(obj.c_str() + cm + 1, &ao);
                        if (ao == obj.c_str() + cm + 1) { ++p; continue; }
                        ap.windsocks.emplace_back(wlat, wlon);
                        p = static_cast<size_t>(ao - obj.c_str());
                    }
                }
            }
        }

        // Preserve the actual OSM/ENAIRE airport building footprints. The
        // duplicate Castejon OurAirports record carries the mapped hangars,
        // so import these before airport de-duplication merges runway records.
        for (const std::string &feature : json_objects(obj, "buildings")) {
            const auto pts = json_points(feature);
            if (pts.size() < 4) continue;
            // Skip legacy orthophoto_vision heuristics (prone to mistaking concrete aprons/marinas
            // for hangars on runways); real OSM footprints come from manifest OSM + map-wide .nscity.
            std::string bldg_src;
            if (extract_json_str(feature, "source", bldg_src) && bldg_src == "orthophoto_vision") continue;
            std::string kind;
            extract_json_str(feature, "kind", kind);
            double height = (kind == "control_tower") ? 18.0 : ((kind == "terminal") ? 9.0 : 7.0);
            double roof_r = 158.0, roof_g = 150.0, roof_b = 130.0;
            extract_json_num(feature, "height_m", height);
            extract_json_num(feature, "roof_r", roof_r);
            extract_json_num(feature, "roof_g", roof_g);
            extract_json_num(feature, "roof_b", roof_b);

            double mean_e = 0.0, mean_n = 0.0;
            double local_m_lat = 111132.0, local_m_lon = 82600.0;
            nspain::meters_per_degree(ap.lat, local_m_lat, local_m_lon);
            for (const auto &[lat, lon] : pts) {
                mean_e += lon * local_m_lon;
                mean_n += lat * local_m_lat;
            }
            mean_e /= pts.size(); mean_n /= pts.size();
            double cxx = 0.0, cyy = 0.0, cxy = 0.0;
            for (const auto &[lat, lon] : pts) {
                const double de = lon * local_m_lon - mean_e;
                const double dn = lat * local_m_lat - mean_n;
                cxx += de * de; cyy += dn * dn; cxy += de * dn;
            }
            const double yaw = 0.5 * std::atan2(2.0 * cxy, cxx - cyy);
            const double ux = std::cos(yaw), uy = std::sin(yaw);
            double min_u = 1e30, max_u = -1e30, min_v = 1e30, max_v = -1e30;
            for (const auto &[lat, lon] : pts) {
                const double de = lon * local_m_lon - mean_e;
                const double dn = lat * local_m_lat - mean_n;
                const double u = de * ux + dn * uy;
                const double v = -de * uy + dn * ux;
                min_u = std::min(min_u, u); max_u = std::max(max_u, u);
                min_v = std::min(min_v, v); max_v = std::max(max_v, v);
            }
            const float width = static_cast<float>(max_u - min_u);
            const float depth = static_cast<float>(max_v - min_v);
            if (width < 3.0f || depth < 3.0f || width > 220.0f || depth > 220.0f) continue;

            BuildingRec b{};
            b.lat = mean_n / local_m_lat;
            b.lon = mean_e / local_m_lon;
            b.width_m = width;
            b.depth_m = depth;
            b.height_m = std::clamp(static_cast<float>(height), 4.0f, 36.0f);
            b.yaw_rad = static_cast<float>(yaw);
            b.ground_m = static_cast<float>(sample_elevation(b.lat, b.lon));
            b.archetype = kind == "control_tower" ? 3 : (kind == "hangar" ? 4 : (kind == "terminal" ? 2 : 5));
            const uint32_t color_hash = hash2i(static_cast<int>(std::abs(b.lat * 100000.0)),
                                              static_cast<int>(std::abs(b.lon * 100000.0)), 0x61b2a34du);
            b.color_var = static_cast<uint8_t>(color_hash >> 24);
            b.roof_r = static_cast<uint8_t>(std::clamp(std::lround(roof_r), 0l, 255l));
            b.roof_g = static_cast<uint8_t>(std::clamp(std::lround(roof_g), 0l, 255l));
            b.roof_b = static_cast<uint8_t>(std::clamp(std::lround(roof_b), 0l, 255l));
            b.airport_feature = true;
            const bool duplicate_feature = std::any_of(buildings_.begin(), buildings_.end(), [&](const BuildingRec &old) {
                return old.airport_feature && nspain::haversine_m(old.lat, old.lon, b.lat, b.lon) < 3.0;
            });
            if (!duplicate_feature) {
                const uint32_t idx = static_cast<uint32_t>(buildings_.size());
                buildings_.push_back(b);
                const int32_t cx = static_cast<int32_t>(std::floor(b.lon / BLDG_CELL_DEG));
                const int32_t cy = static_cast<int32_t>(std::floor(b.lat / BLDG_CELL_DEG));
                bldg_grid_[pack_cell(cx, cy)].push_back(idx);
            }
        }
        // Merge essentially coincident airport records. Nearby distinct fields
        // must not be collapsed into a single airport.
        auto dup = std::find_if(airports_.begin(), airports_.end(), [&](const AirportInfo &prev) {
            return nspain::haversine_m(prev.lat, prev.lon, ap.lat, ap.lon) < 30.0;
        });
        if (dup == airports_.end()) {
            airports_.push_back(std::move(ap));
        } else {
            for (const AirportRunway &rw : ap.runways) {
                const bool exists = std::any_of(dup->runways.begin(), dup->runways.end(), [&](const AirportRunway &old) {
                    const double same = nspain::haversine_m(old.lat_a, old.lon_a, rw.lat_a, rw.lon_a) +
                                        nspain::haversine_m(old.lat_b, old.lon_b, rw.lat_b, rw.lon_b);
                    const double reversed = nspain::haversine_m(old.lat_a, old.lon_a, rw.lat_b, rw.lon_b) +
                                            nspain::haversine_m(old.lat_b, old.lon_b, rw.lat_a, rw.lon_a);
                    if (std::min(same, reversed) < 80.0) return true;
                    // Records may describe different lengths of the same strip.
                    // Compare direction and lateral separation in metres, then
                    // require their projected spans to overlap substantially.
                    double ml, mn;
                    nspain::meters_per_degree(ap.lat, mn, ml);
                    const double ax = (old.lon_b - old.lon_a) * ml;
                    const double ay = (old.lat_b - old.lat_a) * mn;
                    const double bx = (rw.lon_b - rw.lon_a) * ml;
                    const double by = (rw.lat_b - rw.lat_a) * mn;
                    const double al = std::hypot(ax, ay), bl = std::hypot(bx, by);
                    if (al < 100.0 || bl < 100.0 || std::abs(ax * bx + ay * by) / (al * bl) < 0.985) return false;
                    const double dx = ((rw.lon_a + rw.lon_b) - (old.lon_a + old.lon_b)) * 0.5 * ml;
                    const double dy = ((rw.lat_a + rw.lat_b) - (old.lat_a + old.lat_b)) * 0.5 * mn;
                    const double lateral = std::abs(dx * ay - dy * ax) / al;
                    const double along = std::abs(dx * ax + dy * ay) / al;
                    return lateral < 35.0 && along < (al + bl) * 0.35;
                });
                if (!exists) dup->runways.push_back(rw);
                const double old_len = nspain::haversine_m(dup->rwy_lat_a, dup->rwy_lon_a, dup->rwy_lat_b, dup->rwy_lon_b);
                const double new_len = nspain::haversine_m(rw.lat_a, rw.lon_a, rw.lat_b, rw.lon_b);
                auto surf_rank = [](const std::string &s) {
                    if (s == "grass" || s == "asphalt" || s == "concrete" || s == "gravel") return 2;
                    if (s == "dirt") return 1;
                    return 0;
                };
                if (!exists && new_len > old_len) {
                    dup->rwy_lat_a = rw.lat_a; dup->rwy_lon_a = rw.lon_a; dup->rwy_elev_a = rw.elev_a_m;
                    dup->rwy_lat_b = rw.lat_b; dup->rwy_lon_b = rw.lon_b; dup->rwy_elev_b = rw.elev_b_m;
                    dup->rwy_width_m = rw.width_m;
                    if (surf_rank(rw.surface) >= surf_rank(dup->surface)) dup->surface = rw.surface;
                } else if (exists && surf_rank(rw.surface) > surf_rank(dup->surface)) {
                    dup->surface = rw.surface;
                }
            }
            dup->ground_features.insert(dup->ground_features.end(),
                                        ap.ground_features.begin(), ap.ground_features.end());
            dup->fences.insert(dup->fences.end(), ap.fences.begin(), ap.fences.end());
            dup->windsocks.insert(dup->windsocks.end(), ap.windsocks.begin(), ap.windsocks.end());
        }
    }
}

bool PackReader::load_scenery(const std::string &nsscn_path, bool append) {
    std::ifstream ifs(nsscn_path, std::ios::binary);
    if (!ifs.is_open()) return false;
    RawScnHeader hdr{};
    ifs.read(reinterpret_cast<char *>(&hdr), sizeof(hdr));
    if (std::memcmp(hdr.magic, "NSCN", 4) != 0 || hdr.version < 2) return false;

    if (!append) {
        buildings_.clear();
        bldg_grid_.clear();
        roads_.clear();
        road_pts_.clear();
        road_grid_.clear();
    }
    uint32_t base_b = static_cast<uint32_t>(buildings_.size());
    std::vector<RawBldgV2> raw(hdr.bldg_count);
    ifs.read(reinterpret_cast<char *>(raw.data()), static_cast<std::streamsize>(raw.size() * sizeof(RawBldgV2)));
    buildings_.resize(base_b + hdr.bldg_count);
    bldg_grid_.reserve(bldg_grid_.size() + 4096);
    for (uint32_t i = 0; i < hdr.bldg_count; i++) {
        const RawBldgV2 &r = raw[i];
        uint32_t dst_i = base_b + i;
        BuildingRec &b = buildings_[dst_i];
        b.lat = static_cast<double>(r.lat_e6) * 1e-6;
        b.lon = static_cast<double>(r.lon_e6) * 1e-6;
        b.width_m = r.width_m;
        b.depth_m = r.depth_m;
        b.height_m = r.height_m;
        b.yaw_rad = r.yaw_rad;
        b.ground_m = r.ground_m;
        b.archetype = std::min<uint8_t>(r.archetype, 5);
        b.color_var = r.color_var;
        b.roof_r = r.roof_r;
        b.roof_g = r.roof_g;
        b.roof_b = r.roof_b;
        b.canyon_ao = r.canyon_ao;

        int32_t cx = static_cast<int32_t>(std::floor(b.lon / BLDG_CELL_DEG));
        int32_t cy = static_cast<int32_t>(std::floor(b.lat / BLDG_CELL_DEG));
        bldg_grid_[pack_cell(cx, cy)].push_back(dst_i);
    }

    if (hdr.road_count > 0 && hdr.road_pt_count > 0) {
        uint32_t base_r = static_cast<uint32_t>(roads_.size());
        uint32_t base_p = static_cast<uint32_t>(road_pts_.size());
        std::vector<RawRoad> raw_r(hdr.road_count);
        ifs.read(reinterpret_cast<char *>(raw_r.data()), static_cast<std::streamsize>(raw_r.size() * sizeof(RawRoad)));
        roads_.resize(base_r + hdr.road_count);
        for (uint32_t i = 0; i < hdr.road_count; i++) {
            roads_[base_r + i] = {raw_r[i].offset + base_p, raw_r[i].count, raw_r[i].kind};
        }
        road_pts_.resize(base_p + hdr.road_pt_count);
        ifs.read(reinterpret_cast<char *>(road_pts_.data() + base_p), static_cast<std::streamsize>(hdr.road_pt_count * sizeof(RoadPt)));
        road_grid_.reserve(road_grid_.size() + 4096);
        for (uint32_t i = 0; i < hdr.road_count; i++) {
            uint32_t ridx = base_r + i;
            const RoadRec &rd = roads_[ridx];
            if (rd.count < 2 || rd.offset + rd.count > road_pts_.size()) continue;
            uint32_t samples[3] = {rd.offset, rd.offset + rd.count / 2, rd.offset + rd.count - 1};
            int64_t prev_k = INT64_MAX;
            for (uint32_t si : samples) {
                int32_t cx = static_cast<int32_t>(std::floor((road_pts_[si].lon_e6 * 1e-6) / BLDG_CELL_DEG));
                int32_t cy = static_cast<int32_t>(std::floor((road_pts_[si].lat_e6 * 1e-6) / BLDG_CELL_DEG));
                int64_t k = pack_cell(cx, cy);
                if (k != prev_k) {
                    road_grid_[k].push_back(ridx);
                    prev_k = k;
                }
            }
        }
    }
    return true;
}

void PackReader::query_roads_in_box(double min_lat, double max_lat, double min_lon, double max_lon, std::vector<uint32_t> &out_indices) const {
    out_indices.clear();
    if (road_grid_.empty()) return;
    int32_t cx0 = static_cast<int32_t>(std::floor(min_lon / BLDG_CELL_DEG)) - 1;
    int32_t cx1 = static_cast<int32_t>(std::floor(max_lon / BLDG_CELL_DEG)) + 1;
    int32_t cy0 = static_cast<int32_t>(std::floor(min_lat / BLDG_CELL_DEG)) - 1;
    int32_t cy1 = static_cast<int32_t>(std::floor(max_lat / BLDG_CELL_DEG)) + 1;
    for (int32_t cy = cy0; cy <= cy1; cy++) {
        for (int32_t cx = cx0; cx <= cx1; cx++) {
            auto it = road_grid_.find(pack_cell(cx, cy));
            if (it == road_grid_.end()) continue;
            out_indices.insert(out_indices.end(), it->second.begin(), it->second.end());
        }
    }
    std::sort(out_indices.begin(), out_indices.end());
    out_indices.erase(std::unique(out_indices.begin(), out_indices.end()), out_indices.end());
}

const AirportInfo *PackReader::find_airport(const std::string &id) const {
    for (const auto &a : airports_) {
        if (a.id == id) return &a;
    }
    return nullptr;
}

const AirportInfo *PackReader::find_nearest_airport(double lat_deg, double lon_deg, double *out_dist_m) const {
    const AirportInfo *best = nullptr;
    double best_d = 1e18;
    for (const auto &a : airports_) {
        double d = nspain::haversine_m(lat_deg, lon_deg, a.lat, a.lon);
        if (d < best_d) {
            best_d = d;
            best = &a;
        }
    }
    if (out_dist_m) *out_dist_m = best_d;
    return best;
}

std::vector<ParkingStand> PackReader::compute_parking_stands(const AirportInfo &ap, int max_stands) const {
    std::vector<ParkingStand> out;
    if (max_stands <= 0) return out;
    double m_lat = 111132.0, m_lon = 82600.0;
    nspain::meters_per_degree(ap.lat, m_lat, m_lon);

    struct RwySeg { double ea, na, eb, nb, len, half_w; };
    std::vector<RwySeg> rwys;
    auto push_rwy = [&](double la, double loa, double lb, double lob, double w) {
        double ea = (loa - ap.lon) * m_lon, na = (la - ap.lat) * m_lat;
        double eb = (lob - ap.lon) * m_lon, nb = (lb - ap.lat) * m_lat;
        double len = std::hypot(eb - ea, nb - na);
        if (len >= 80.0) rwys.push_back({ea, na, eb, nb, len, std::max(8.0, w * 0.5)});
    };
    if (ap.runways.empty()) push_rwy(ap.rwy_lat_a, ap.rwy_lon_a, ap.rwy_lat_b, ap.rwy_lon_b, ap.rwy_width_m);
    else for (const auto &rw : ap.runways) push_rwy(rw.lat_a, rw.lon_a, rw.lat_b, rw.lon_b, rw.width_m);

    auto clearance_to_runways = [&](double e, double n, double &min_rwy_d, double &near_e, double &near_n) -> bool {
        min_rwy_d = 1e9;
        near_e = 0.0; near_n = 0.0;
        for (const auto &r : rwys) {
            double de = r.eb - r.ea, dn = r.nb - r.na;
            double t = ((e - r.ea) * de + (n - r.na) * dn) / (r.len * r.len);
            double tc = std::clamp(t, 0.0, 1.0);
            double pe = r.ea + de * tc, pn = r.na + dn * tc;
            double d = std::hypot(e - pe, n - pn);
            if (d < min_rwy_d) { min_rwy_d = d; near_e = pe; near_n = pn; }
            if (t >= -22.0 / r.len && t <= 1.0 + 22.0 / r.len && d < r.half_w + 13.0) return false;
        }
        return true;
    };

    auto local_slope = [&](double lat, double lon) -> double {
        double dlat = 5.0 / m_lat, dlon = 5.0 / m_lon;
        double he = sample_elevation(lat, lon + dlon) - sample_elevation(lat, lon - dlon);
        double hn = sample_elevation(lat + dlat, lon) - sample_elevation(lat - dlat, lon);
        return std::hypot(he, hn) / 10.0;
    };

    auto nearest_taxiway_dir = [&](double e, double n, double fallback_e, double fallback_n) -> double {
        double best_d = 1e9, tx_e = fallback_e, tx_n = fallback_n;
        for (const auto &gf : ap.ground_features) {
            if (gf.kind != AirportGroundFeature::Kind::Taxiway) continue;
            for (const auto &[tlat, tlon] : gf.points_lat_lon) {
                double te = (tlon - ap.lon) * m_lon, tn = (tlat - ap.lat) * m_lat;
                double d = std::hypot(te - e, tn - n);
                if (d > 6.0 && d < best_d && d < 220.0) { best_d = d; tx_e = te; tx_n = tn; }
            }
        }
        double hdg = std::atan2(tx_e - e, tx_n - n) * (180.0 / PI_D);
        if (hdg < 0.0) hdg += 360.0;
        return hdg;
    };

    struct Cand { ParkingStand st; double score; };
    std::vector<Cand> cands;

    auto pt_in_poly = [](double x, double y, const std::vector<std::pair<double, double>> &poly) -> bool {
        bool inside = false;
        size_t n = poly.size();
        for (size_t i = 0, j = n - 1; i < n; j = i++) {
            double xi = poly[i].first, yi = poly[i].second;
            double xj = poly[j].first, yj = poly[j].second;
            if (((yi > y) != (yj > y)) && (x < (xj - xi) * (y - yi) / ((yj - yi) + 1e-12) + xi)) inside = !inside;
        }
        return inside;
    };

    // 1. Sample interior candidates inside real OSM paved aprons first.
    for (const auto &gf : ap.ground_features) {
        if (gf.kind != AirportGroundFeature::Kind::Apron || gf.points_lat_lon.size() < 4) continue;
        std::string s = gf.surface;
        for (char &ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        const bool is_grass = (s.find("grass") != std::string::npos || s.find("turf") != std::string::npos);

        std::vector<std::pair<double, double>> poly;
        poly.reserve(gf.points_lat_lon.size());
        double min_e = 1e9, max_e = -1e9, min_n = 1e9, max_n = -1e9, ce = 0.0, cn = 0.0;
        for (const auto &[plat, plon] : gf.points_lat_lon) {
            double pe = (plon - ap.lon) * m_lon, pn = (plat - ap.lat) * m_lat;
            poly.emplace_back(pe, pn);
            min_e = std::min(min_e, pe); max_e = std::max(max_e, pe);
            min_n = std::min(min_n, pn); max_n = std::max(max_n, pn);
            ce += pe; cn += pn;
        }
        ce /= poly.size(); cn /= poly.size();
        if (max_e - min_e < 10.0 || max_n - min_n < 10.0) continue;

        std::vector<std::pair<double, double>> probes;
        probes.emplace_back(ce, cn);
        const int gx = std::clamp(static_cast<int>((max_e - min_e) / 28.0), 2, 5);
        const int gy = std::clamp(static_cast<int>((max_n - min_n) / 28.0), 2, 5);
        for (int iy = 0; iy < gy; ++iy) {
            for (int ix = 0; ix < gx; ++ix) {
                double pe = min_e + (max_e - min_e) * ((ix + 0.5) / gx);
                double pn = min_n + (max_n - min_n) * ((iy + 0.5) / gy);
                probes.emplace_back(pe, pn);
            }
        }
        for (const auto &[pe, pn] : probes) {
            if (!pt_in_poly(pe, pn, poly)) continue;
            // Require 6 m interior margin so wingtips stay cleanly on pavement.
            if (!pt_in_poly(pe + 6.0, pn, poly) || !pt_in_poly(pe - 6.0, pn, poly) ||
                !pt_in_poly(pe, pn + 6.0, poly) || !pt_in_poly(pe, pn - 6.0, poly)) continue;
            double rwy_d = 0.0, ne = 0.0, nn = 0.0;
            if (!clearance_to_runways(pe, pn, rwy_d, ne, nn)) continue;
            double plat = ap.lat + pn / m_lat, plon = ap.lon + pe / m_lon;
            if (is_near_building(plat, plon, 11.0)) continue;
            double slope = local_slope(plat, plon);
            if (slope > 0.075) continue;
            double ideal_d = std::abs(rwy_d - 120.0);
            double score = ideal_d + slope * 1800.0 + (is_grass ? 220.0 : 0.0);
            double hdg = nearest_taxiway_dir(pe, pn, ne, nn);
            cands.push_back({{plat, plon, sample_elevation(plat, plon), hdg, !is_grass}, score});
        }
    }

    std::sort(cands.begin(), cands.end(), [](const Cand &a, const Cand &b) { return a.score < b.score; });
    for (const auto &c : cands) {
        if (static_cast<int>(out.size()) >= max_stands) break;
        bool too_close = false;
        for (const auto &ex : out) {
            if (nspain::haversine_m(ex.lat, ex.lon, c.st.lat, c.st.lon) < 32.0) { too_close = true; break; }
        }
        if (!too_close) out.push_back(c.st);
    }

    // 2. Fallback: evaluate both sides of primary runway for flattest, building-free shoulder stands.
    if (static_cast<int>(out.size()) < max_stands && !rwys.empty()) {
        const RwySeg &r = rwys.front();
        double ue = (r.eb - r.ea) / r.len, un = (r.nb - r.na) / r.len;
        double rhdg = std::atan2(ue, un) * (180.0 / PI_D);
        if (rhdg < 0.0) rhdg += 360.0;
        std::vector<Cand> fb;
        const double alongs[3] = {std::min(42.0, r.len * 0.14), std::min(74.0, r.len * 0.24), std::min(108.0, r.len * 0.34)};
        for (int side : {1, -1}) {
            double lat_off = (r.half_w + 13.5) * side;
            for (double al : alongs) {
                double pe = r.ea + ue * al + un * lat_off;
                double pn = r.na + un * al - ue * lat_off;
                double plat = ap.lat + pn / m_lat, plon = ap.lon + pe / m_lon;
                double slope = local_slope(plat, plon);
                bool bldg = is_near_building(plat, plon, 10.5);
                double score = slope * 2500.0 + (bldg ? 1500.0 : 0.0) + al * 0.4;
                fb.push_back({{plat, plon, sample_elevation(plat, plon), rhdg, false}, score});
            }
        }
        std::sort(fb.begin(), fb.end(), [](const Cand &a, const Cand &b) { return a.score < b.score; });
        for (const auto &c : fb) {
            if (static_cast<int>(out.size()) >= max_stands) break;
            bool too_close = false;
            for (const auto &ex : out) {
                if (nspain::haversine_m(ex.lat, ex.lon, c.st.lat, c.st.lon) < 16.0) { too_close = true; break; }
            }
            if (!too_close) out.push_back(c.st);
        }
    }
    if (out.empty()) {
        out.push_back({ap.lat, ap.lon, sample_elevation(ap.lat, ap.lon), 0.0, false});
    }
    return out;
}

bool PackReader::is_excluded_for_vegetation(double lat_deg, double lon_deg) const {
    const auto frame = world_frame();
    return frame && is_excluded_for_vegetation(*frame, lat_deg, lon_deg);
}

bool PackReader::is_excluded_for_vegetation(const WorldFrame &frame, double lat_deg, double lon_deg) const {
    // Oriented runway corridor clearance (keeps runway & approach clear while allowing trees right beside strip borders!)
    for (const auto &a : airports_) {
        auto excluded_by = [&](double lat_a, double lon_a, double lat_b, double lon_b, double width) {
            double ax = (lon_a - lon_deg) * frame.metres_lon;
            double ay = (lat_a - lat_deg) * frame.metres_lat;
            double bx = (lon_b - lon_deg) * frame.metres_lon;
            double by = (lat_b - lat_deg) * frame.metres_lat;
            double dx = bx - ax, dy = by - ay;
            double len2 = dx * dx + dy * dy;
            if (len2 < 100.0) {
                double dn = (lat_deg - a.lat) * frame.metres_lat;
                double de = (lon_deg - a.lon) * frame.metres_lon;
                return dn * dn + de * de < 95.0 * 95.0;
            }
            double len = std::sqrt(len2);
            double t = (-ax * dx - ay * dy) / len2;
            double along_ext = 95.0 / len;
            if (t < -along_ext || t > 1.0 + along_ext) return false;
            double tc = std::clamp(t, 0.0, 1.0);
            double lat_dist = std::hypot(ax + tc * dx, ay + tc * dy);
            return lat_dist < std::max(34.0, width * 1.45);
        };
        if (a.runways.empty()) {
            if (excluded_by(a.rwy_lat_a, a.rwy_lon_a, a.rwy_lat_b, a.rwy_lon_b, a.rwy_width_m)) return true;
        } else {
            for (const AirportRunway &rw : a.runways)
                if (excluded_by(rw.lat_a, rw.lon_a, rw.lat_b, rw.lon_b, rw.width_m)) return true;
        }
        if (std::abs(lat_deg - a.lat) < 0.025 && std::abs(lon_deg - a.lon) < 0.035) {
            for (const AirportGroundFeature &gf : a.ground_features) {
                if (gf.points_lat_lon.size() < 2) continue;
                const double pad = (gf.kind == AirportGroundFeature::Kind::AccessRoad)
                    ? 22.0
                    : std::max(26.0, gf.width_m * 0.75 + 14.0);
                bool inside = false;
                const size_t n = gf.points_lat_lon.size();
                for (size_t i = 0; i < n; ++i) {
                    const size_t j = (i + 1) % n;
                    if (gf.kind != AirportGroundFeature::Kind::Apron && i + 1 == n) break;
                    const double ax = (gf.points_lat_lon[i].second - lon_deg) * frame.metres_lon;
                    const double ay = (gf.points_lat_lon[i].first - lat_deg) * frame.metres_lat;
                    const double bx = (gf.points_lat_lon[j].second - lon_deg) * frame.metres_lon;
                    const double by = (gf.points_lat_lon[j].first - lat_deg) * frame.metres_lat;
                    const double dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy;
                    if (l2 > 1e-4) {
                        const double tc = std::clamp((-ax * dx - ay * dy) / l2, 0.0, 1.0);
                        if (std::hypot(ax + tc * dx, ay + tc * dy) < pad) return true;
                    }
                    if (gf.kind == AirportGroundFeature::Kind::Apron &&
                        ((ay > 0.0) != (by > 0.0)) &&
                        (0.0 < (bx - ax) * (-ay) / (by - ay) + ax)) {
                        inside = !inside;
                    }
                }
                if (inside) return true;
            }
        }
    }
    return false;
}

bool PackReader::is_near_building(double lat_deg, double lon_deg, double pad_m) const {
    const auto frame = world_frame();
    return frame && is_near_building(*frame, lat_deg, lon_deg, pad_m);
}

bool PackReader::is_near_building(const WorldFrame &frame, double lat_deg, double lon_deg, double pad_m) const {
    if (bldg_grid_.empty()) return false;
    int32_t cx0 = static_cast<int32_t>(std::floor(lon_deg / BLDG_CELL_DEG));
    int32_t cy0 = static_cast<int32_t>(std::floor(lat_deg / BLDG_CELL_DEG));
    for (int32_t dy = -1; dy <= 1; dy++) {
        for (int32_t dx = -1; dx <= 1; dx++) {
            auto it = bldg_grid_.find(pack_cell(cx0 + dx, cy0 + dy));
            if (it == bldg_grid_.end()) continue;
            for (uint32_t idx : it->second) {
                const BuildingRec &b = buildings_[idx];
                double de = (lon_deg - b.lon) * frame.metres_lon;
                double dn = (lat_deg - b.lat) * frame.metres_lat;
                double r = 0.52 * std::max(b.width_m, b.depth_m) + pad_m;
                if (de * de + dn * dn < r * r) return true;
            }
        }
    }
    return false;
}

bool PackReader::is_near_road(double lat_deg, double lon_deg, double pad_m) const {
    const auto frame = world_frame();
    return frame && is_near_road(*frame, lat_deg, lon_deg, pad_m);
}

bool PackReader::is_near_road(const WorldFrame &frame, double lat_deg, double lon_deg, double pad_m) const {
    if (road_grid_.empty()) return false;
    int32_t cx0 = static_cast<int32_t>(std::floor(lon_deg / BLDG_CELL_DEG));
    int32_t cy0 = static_cast<int32_t>(std::floor(lat_deg / BLDG_CELL_DEG));
    double pad2 = pad_m * pad_m;
    for (int32_t dy = -1; dy <= 1; dy++) {
        for (int32_t dx = -1; dx <= 1; dx++) {
            auto it = road_grid_.find(pack_cell(cx0 + dx, cy0 + dy));
            if (it == road_grid_.end()) continue;
            for (uint32_t ridx : it->second) {
                const RoadRec &rr = roads_[ridx];
                if (rr.count < 2 || rr.offset + rr.count > road_pts_.size()) continue;
                double extra = (rr.kind == 0) ? 7.0 : ((rr.kind == 1) ? 4.5 : 2.0);
                double thr2 = (pad_m + extra) * (pad_m + extra);
                for (uint16_t pi = 0; pi + 1 < rr.count; pi++) {
                    const RoadPt &p0 = road_pts_[rr.offset + pi];
                    const RoadPt &p1 = road_pts_[rr.offset + pi + 1];
                    double ax = (p0.lon_e6 * 1e-6 - lon_deg) * frame.metres_lon;
                    double ay = (p0.lat_e6 * 1e-6 - lat_deg) * frame.metres_lat;
                    double bx = (p1.lon_e6 * 1e-6 - lon_deg) * frame.metres_lon;
                    double by = (p1.lat_e6 * 1e-6 - lat_deg) * frame.metres_lat;
                    double sx = bx - ax, sy = by - ay;
                    double l2 = sx * sx + sy * sy;
                    double t = (l2 > 1e-4) ? std::clamp((-ax * sx - ay * sy) / l2, 0.0, 1.0) : 0.0;
                    double px = ax + t * sx, py = ay + t * sy;
                    if (px * px + py * py < thr2) return true;
                }
            }
        }
    }
    (void)pad2;
    return false;
}

void PackReader::fill_cell_occupancy(double min_e, double min_n, double span_m, int res, uint8_t *out_mask,
                                    bool include_verges) const {
    fill_cell_occupancy(*world_frame_, min_e, min_n, span_m, res, out_mask, include_verges);
}

void PackReader::fill_cell_occupancy(const WorldFrame &frame, double min_e, double min_n,
                                     double span_m, int res, uint8_t *out_mask, bool include_verges) const {
    if (!out_mask || res <= 0 || span_m <= 1.0) return;
    std::memset(out_mask, 0, static_cast<size_t>(res) * static_cast<size_t>(res));
    double max_e = min_e + span_m, max_n = min_n + span_m;
    double cell_m = span_m / static_cast<double>(res);
    double inv_cell = 1.0 / cell_m;
    double pad_lat = 40.0 / frame.metres_lat;
    double pad_lon = 40.0 / frame.metres_lon;
    double lat0 = frame.anchor_lat + min_n / frame.metres_lat - pad_lat;
    double lat1 = frame.anchor_lat + max_n / frame.metres_lat + pad_lat;
    double lon0 = frame.anchor_lon + min_e / frame.metres_lon - pad_lon;
    double lon1 = frame.anchor_lon + max_e / frame.metres_lon + pad_lon;

    int32_t cx0 = static_cast<int32_t>(std::floor(lon0 / BLDG_CELL_DEG));
    int32_t cx1 = static_cast<int32_t>(std::floor(lon1 / BLDG_CELL_DEG));
    int32_t cy0 = static_cast<int32_t>(std::floor(lat0 / BLDG_CELL_DEG));
    int32_t cy1 = static_cast<int32_t>(std::floor(lat1 / BLDG_CELL_DEG));

    auto stamp_disk = [&](double e, double n, double rad_m, uint8_t bit) {
        int gx0 = std::max(0, static_cast<int>(std::floor((e - rad_m - min_e) * inv_cell)));
        int gx1 = std::min(res - 1, static_cast<int>(std::floor((e + rad_m - min_e) * inv_cell)));
        int gy0 = std::max(0, static_cast<int>(std::floor((n - rad_m - min_n) * inv_cell)));
        int gy1 = std::min(res - 1, static_cast<int>(std::floor((n + rad_m - min_n) * inv_cell)));
        for (int gy = gy0; gy <= gy1; gy++) {
            uint8_t *row = out_mask + gy * res;
            for (int gx = gx0; gx <= gx1; gx++) row[gx] |= bit;
        }
    };

    // 1. Stamp buildings overlapping this cell (bit 0)
    if (!bldg_grid_.empty()) {
        for (int32_t cy = cy0; cy <= cy1; cy++) {
            for (int32_t cx = cx0; cx <= cx1; cx++) {
                auto it = bldg_grid_.find(pack_cell(cx, cy));
                if (it == bldg_grid_.end()) continue;
                for (uint32_t idx : it->second) {
                    const BuildingRec &b = buildings_[idx];
                    double be = (b.lon - frame.anchor_lon) * frame.metres_lon;
                    double bn = (b.lat - frame.anchor_lat) * frame.metres_lat;
                    // Keep strict roof interior when !include_verges so urban park, courtyard & street trees keep LiDAR volume.
                    double r = include_verges
                        ? (0.54 * std::max(b.width_m, b.depth_m) + 5.5)
                        : (0.36 * std::min(b.width_m, b.depth_m));
                    if (be + r < min_e || be - r > max_e || bn + r < min_n || bn - r > max_n) continue;
                    stamp_disk(be, bn, r, 0x01u);
                }
            }
        }
    }

    // 2. Rasterize road polylines overlapping this cell (bit 1) using fast segment stepping
    if (!road_grid_.empty()) {
        for (int32_t cy = cy0 - 1; cy <= cy1 + 1; cy++) {
            for (int32_t cx = cx0 - 1; cx <= cx1 + 1; cx++) {
                auto it = road_grid_.find(pack_cell(cx, cy));
                if (it == road_grid_.end()) continue;
                for (uint32_t ridx : it->second) {
                    const RoadRec &rr = roads_[ridx];
                    if (rr.count < 2 || rr.offset + rr.count > road_pts_.size()) continue;
                    double rad = include_verges ? (rr.kind == 0 ? 14.5 : rr.kind == 1 ? 11.5 : 8.5)
                                                : (rr.kind == 0 ? 4.2 : rr.kind == 1 ? 2.6 : 1.2);
                    for (uint16_t pi = 0; pi + 1 < rr.count; pi++) {
                        const RoadPt &p0 = road_pts_[rr.offset + pi];
                        const RoadPt &p1 = road_pts_[rr.offset + pi + 1];
                        double e0 = (p0.lon_e6 * 1e-6 - frame.anchor_lon) * frame.metres_lon;
                        double n0 = (p0.lat_e6 * 1e-6 - frame.anchor_lat) * frame.metres_lat;
                        double e1 = (p1.lon_e6 * 1e-6 - frame.anchor_lon) * frame.metres_lon;
                        double n1 = (p1.lat_e6 * 1e-6 - frame.anchor_lat) * frame.metres_lat;
                        if (std::max(e0, e1) + rad < min_e || std::min(e0, e1) - rad > max_e ||
                            std::max(n0, n1) + rad < min_n || std::min(n0, n1) - rad > max_n) continue;
                        double len = std::hypot(e1 - e0, n1 - n0);
                        int steps = std::clamp(static_cast<int>(std::ceil(len / (cell_m * 0.85))), 1, 64);
                        for (int st = 0; st <= steps; st++) {
                            double t = static_cast<double>(st) / steps;
                            stamp_disk(e0 + (e1 - e0) * t, n0 + (n1 - n0) * t, rad, 0x02u);
                        }
                    }
                }
            }
        }
    }

    // 3. Clear airport runway corridors (bit 2) so LiDAR vegetation never pops up on runways
    for (const auto &a : airports_) {
        auto stamp_rwy = [&](double lat_a, double lon_a, double lat_b, double lon_b, double width) {
            double e0 = (lon_a - frame.anchor_lon) * frame.metres_lon;
            double n0 = (lat_a - frame.anchor_lat) * frame.metres_lat;
            double e1 = (lon_b - frame.anchor_lon) * frame.metres_lon;
            double n1 = (lat_b - frame.anchor_lat) * frame.metres_lat;
            double rad = std::max(22.0, width * 0.85);
            if (std::max(e0, e1) + rad < min_e || std::min(e0, e1) - rad > max_e ||
                std::max(n0, n1) + rad < min_n || std::min(n0, n1) - rad > max_n) return;
            double len = std::hypot(e1 - e0, n1 - n0);
            int steps = std::clamp(static_cast<int>(std::ceil(len / std::max(4.0, cell_m * 0.75))), 2, 160);
            for (int st = 0; st <= steps; st++) {
                double t = static_cast<double>(st) / steps;
                stamp_disk(e0 + (e1 - e0) * t, n0 + (n1 - n0) * t, rad, 0x04u);
            }
        };
        if (a.runways.empty()) {
            stamp_rwy(a.rwy_lat_a, a.rwy_lon_a, a.rwy_lat_b, a.rwy_lon_b, a.rwy_width_m);
        } else {
            for (const auto &rw : a.runways) stamp_rwy(rw.lat_a, rw.lon_a, rw.lat_b, rw.lon_b, rw.width_m);
        }
    }
}

bool PackReader::query_runway(double lat_deg, double lon_deg, double &out_elev_m, double &out_mu, double &out_rough, std::string &out_id) const {
    for (const auto &a : airports_) {
        auto query = [&](double lat_a, double lon_a, double elev_a, double lat_b, double lon_b,
                         double elev_b, double width, const std::string &surface) {
            double ax = (lon_a - lon_deg) * world_frame_->metres_lon;
            double ay = (lat_a - lat_deg) * world_frame_->metres_lat;
            double bx = (lon_b - lon_deg) * world_frame_->metres_lon;
            double by = (lat_b - lat_deg) * world_frame_->metres_lat;
            double dx = bx - ax, dy = by - ay;
            double len2 = dx * dx + dy * dy;
            if (len2 < 100.0) return false;
            double t = (-ax * dx - ay * dy) / len2;
            if (t < -0.02 || t > 1.02) return false;
            double px = ax + t * dx, py = ay + t * dy;
            if (std::hypot(px, py) > width * 0.60) return false;
            const double tc = std::clamp(t, 0.0, 1.0);
            out_elev_m = elev_a * (1.0 - tc) + elev_b * tc;
            const bool paved = surface.starts_with("ASP") || surface.starts_with("CON") ||
                               surface.starts_with("asp") || surface.starts_with("con") ||
                               surface.starts_with("bit") || surface.starts_with("pav");
            const bool grass = surface.starts_with("GRS") || surface.starts_with("grs") ||
                               surface.starts_with("grass") || surface.starts_with("turf");
            const bool gravel = surface.starts_with("GVL") || surface.starts_with("grav");
            if (!paved) out_elev_m = sample_elevation(lat_deg, lon_deg) - .35;
            if (paved)       { out_mu = 0.82; out_rough = 0.025; }
            else if (gravel) { out_mu = 0.66; out_rough = 0.16; }
            else if (grass)  { out_mu = 0.62; out_rough = 0.18; }
            else             { out_mu = 0.58; out_rough = 0.24; }
            out_id = a.id;
            return true;
        };
        if (a.runways.empty()) {
            if (query(a.rwy_lat_a, a.rwy_lon_a, a.rwy_elev_a, a.rwy_lat_b, a.rwy_lon_b,
                      a.rwy_elev_b, a.rwy_width_m, a.surface)) return true;
        } else {
            for (const AirportRunway &rw : a.runways)
                if (query(rw.lat_a, rw.lon_a, rw.elev_a_m, rw.lat_b, rw.lon_b,
                          rw.elev_b_m, rw.width_m, rw.surface)) return true;
        }
    }
    return false;
}

const PackReader::IndexedTile *PackReader::find_entry(uint64_t key) const {
    auto it = std::lower_bound(
        index_.begin(), index_.end(), key,
        [](const IndexedTile &e, uint64_t k) { return e.key < k; }
    );
    if (it != index_.end() && it->key == key) return &(*it);
    return nullptr;
}

std::shared_ptr<DecodedElevTile> PackReader::get_elev_tile(int z, int x, int y) const {
    uint64_t key = nspain::make_tile_key(nspain::LAYER_ELEVATION, static_cast<uint8_t>(z), static_cast<uint32_t>(x), static_cast<uint32_t>(y));
    if (auto hit = elev_cache_.get(key)) return hit;
    const IndexedTile *it = find_entry(key);
    if (!it || it->pack_idx >= regions_.size()) return nullptr;
    const nspain::PackTileEntry *e = &it->entry;
    FileHandle fd = regions_[it->pack_idx].pack_fd;
    if (fd == kInvalidFileHandle) return nullptr;

    thread_local std::vector<uint8_t> raw;
    raw.resize(e->size);
    if (!read_at_offset(fd, raw.data(), e->size, e->offset)) return nullptr;

    const size_t count = static_cast<size_t>(e->width) * e->height;
    const size_t expected = sizeof(nspain::HeightTileHeader) + count * sizeof(uint16_t);
    thread_local std::vector<uint8_t> uncomp;
    const uint8_t *ptr = nullptr;
    if (e->encoding == nspain::ENC_ZLIB_U16_HEIGHT) {
        uncomp.resize(expected);
        size_t res = tinfl_decompress_mem_to_mem(
            uncomp.data(), uncomp.size(), raw.data(), raw.size(), TINFL_FLAG_PARSE_ZLIB_HEADER
        );
        if (res == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED || res < expected) return nullptr;
        ptr = uncomp.data();
    } else {
        if (raw.size() < expected) return nullptr;
        ptr = raw.data();
    }

    nspain::HeightTileHeader hh{};
    std::memcpy(&hh, ptr, sizeof(hh));
    const uint16_t *q = reinterpret_cast<const uint16_t *>(ptr + sizeof(hh));
    auto tile = std::make_shared<DecodedElevTile>();
    tile->width = e->width;
    tile->height = e->height;
    tile->heights.resize(count);
    for (size_t i = 0; i < count; i++) {
        tile->heights[i] = hh.base_m + static_cast<float>(q[i]) * hh.step_m;
    }
    elev_cache_.put(key, tile);
    return tile;
}

std::shared_ptr<DecodedRgbTile> PackReader::get_rgb_tile(uint8_t layer, int z, int x, int y) const {
    uint64_t key = nspain::make_tile_key(layer, static_cast<uint8_t>(z), static_cast<uint32_t>(x), static_cast<uint32_t>(y));
    auto &cache = (layer == nspain::LAYER_IMAGERY) ? img_cache_ : surf_cache_;
    if (auto hit = cache.get(key)) return hit;
    const IndexedTile *it = find_entry(key);
    if (!it || it->pack_idx >= regions_.size()) return nullptr;
    const nspain::PackTileEntry *e = &it->entry;
    FileHandle fd = regions_[it->pack_idx].pack_fd;
    if (fd == kInvalidFileHandle) return nullptr;

    thread_local std::vector<uint8_t> raw;
    raw.resize(e->size);
    if (!read_at_offset(fd, raw.data(), e->size, e->offset)) return nullptr;

    int w = 0, h = 0, ch = 0;
    unsigned char *pix = stbi_load_from_memory(raw.data(), static_cast<int>(raw.size()), &w, &h, &ch, 3);
    if (!pix || w <= 0 || h <= 0) {
        if (pix) stbi_image_free(pix);
        return nullptr;
    }
    auto tile = std::make_shared<DecodedRgbTile>();
    tile->width = static_cast<uint16_t>(w);
    tile->height = static_cast<uint16_t>(h);
    tile->rgb.assign(pix, pix + static_cast<size_t>(w) * h * 3);
    stbi_image_free(pix);
    cache.put(key, tile);
    return tile;
}

double PackReader::sample_elevation(double lat_deg, double lon_deg) const {
    double clat = std::clamp(lat_deg, header_.lat_min, header_.lat_max);
    double clon = std::clamp(lon_deg, header_.lon_min, header_.lon_max);
    for (int z : elev_zooms_desc_) {
        nspain::MercatorTilePos tp = nspain::latlon_to_tile(clat, clon, z);
        auto tile = get_elev_tile(z, tp.tx, tp.ty);
        if (!tile) continue;
        double px = tp.fx * static_cast<double>(tile->width - 1);
        double py = tp.fy * static_cast<double>(tile->height - 1);
        int x0 = std::clamp(static_cast<int>(std::floor(px)), 0, tile->width - 1);
        int y0 = std::clamp(static_cast<int>(std::floor(py)), 0, tile->height - 1);
        int x1 = std::min<int>(tile->width - 1, x0 + 1);
        int y1 = std::min<int>(tile->height - 1, y0 + 1);
        double dx = px - x0, dy = py - y0;
        float a = tile->heights[y0 * tile->width + x0] * static_cast<float>(1.0 - dx) + tile->heights[y0 * tile->width + x1] * static_cast<float>(dx);
        float b = tile->heights[y1 * tile->width + x0] * static_cast<float>(1.0 - dx) + tile->heights[y1 * tile->width + x1] * static_cast<float>(dx);
        return std::max(0.0, static_cast<double>(a * static_cast<float>(1.0 - dy) + b * static_cast<float>(dy)));
    }
    return 400.0;
}

void PackReader::fill_elevation_grid(const WorldFrame &frame, double center_east_m, double center_north_m,
                                    double half_span_m, int width, int height,
                                    std::vector<float> &out_height) const {
    if (width < 2 || height < 2 || half_span_m <= 0.0) {
        out_height.clear();
        return;
    }
    out_height.resize(static_cast<size_t>(width) * height);
    std::array<uint64_t, 24> tile_keys;
    std::array<std::shared_ptr<DecodedElevTile>, 24> tiles{};
    tile_keys.fill(UINT64_MAX);
    const double step_e = (half_span_m * 2.0) / (width - 1.0);
    const double step_n = (half_span_m * 2.0) / (height - 1.0);
    const double inv_m_lon = 1.0 / frame.metres_lon;
    const double inv_m_lat = 1.0 / frame.metres_lat;
    for (int y = 0; y < height; ++y) {
        const double north = center_north_m - half_span_m + y * step_n;
        const double lat = std::clamp(frame.anchor_lat + north * inv_m_lat, header_.lat_min, header_.lat_max);
        const double merc_y = (1.0 - std::asinh(std::tan(lat * DEG2RAD_D)) / PI_D) * 0.5;
        for (int x = 0; x < width; ++x) {
            const double east = center_east_m - half_span_m + x * step_e;
            const double lon = std::clamp(frame.anchor_lon + east * inv_m_lon, header_.lon_min, header_.lon_max);
            const double merc_x = (lon + 180.0) / 360.0;
            float value = 400.0f;
            for (int z : elev_zooms_desc_) {
                const double scale = static_cast<double>(1u << z);
                const int32_t limit = static_cast<int32_t>(1u << z) - 1;
                const double txf = merc_x * scale, tyf = merc_y * scale;
                const int32_t tx = std::clamp(static_cast<int32_t>(std::floor(txf)), 0, limit);
                const int32_t ty = std::clamp(static_cast<int32_t>(std::floor(tyf)), 0, limit);
                const uint64_t key = nspain::make_tile_key(nspain::LAYER_ELEVATION, static_cast<uint8_t>(z), tx, ty);
                const size_t zi = static_cast<size_t>(std::clamp(z, 0, 23));
                if (tile_keys[zi] != key) {
                    tile_keys[zi] = key;
                    tiles[zi] = get_elev_tile(z, tx, ty);
                }
                const auto *tile = tiles[zi].get();
                if (!tile) continue;
                const double px = std::clamp(txf - tx, 0.0, 1.0) * (tile->width - 1);
                const double py = std::clamp(tyf - ty, 0.0, 1.0) * (tile->height - 1);
                const int x0 = std::clamp(static_cast<int>(px), 0, tile->width - 1);
                const int y0 = std::clamp(static_cast<int>(py), 0, tile->height - 1);
                const int x1 = std::min(tile->width - 1, x0 + 1);
                const int y1 = std::min(tile->height - 1, y0 + 1);
                const float fx = static_cast<float>(px - x0), fy = static_cast<float>(py - y0);
                const float a = std::lerp(tile->heights[y0 * tile->width + x0],
                                          tile->heights[y0 * tile->width + x1], fx);
                const float b = std::lerp(tile->heights[y1 * tile->width + x0],
                                          tile->heights[y1 * tile->width + x1], fx);
                value = std::max(0.0f, std::lerp(a, b, fy));
                break;
            }
            out_height[static_cast<size_t>(y) * width + x] = value;
        }
    }
}

float PackReader::sample_canopy_height(double lat, double lon) const {
    for (int z : vegetation_zooms_desc_) {
        const auto p = nspain::latlon_to_tile(lat, lon, z);
        const auto tile = get_rgb_tile(nspain::LAYER_VEGETATION, z, p.tx, p.ty);
        if (!tile) continue;
        const int x = std::clamp(int(p.fx * tile->width), 0, int(tile->width) - 1);
        const int y = std::clamp(int(p.fy * tile->height), 0, int(tile->height) - 1);
        const size_t offset = (size_t(y) * tile->width + x) * 3;
        if (tile->rgb[offset + 1] > 127) return tile->rgb[offset] * (25.0f / 255.0f);
    }
    return 0;
}

Vec3 PackReader::sample_surface(double lat_deg, double lon_deg) const {
    double clat = std::clamp(lat_deg, header_.lat_min, header_.lat_max);
    double clon = std::clamp(lon_deg, header_.lon_min, header_.lon_max);
    for (int z : surf_zooms_desc_) {
        nspain::MercatorTilePos tp = nspain::latlon_to_tile(clat, clon, z);
        auto tile = get_rgb_tile(nspain::LAYER_SURFACE, z, tp.tx, tp.ty);
        if (!tile) continue;
        int px = std::clamp(static_cast<int>(tp.fx * (tile->width - 1)), 0, tile->width - 1);
        int py = std::clamp(static_cast<int>(tp.fy * (tile->height - 1)), 0, tile->height - 1);
        size_t si = (static_cast<size_t>(py) * tile->width + px) * 3;
        return {tile->rgb[si] / 255.0f, tile->rgb[si + 1] / 255.0f, tile->rgb[si + 2] / 255.0f};
    }
    return {0.0f, 0.0f, 0.0f};
}

Vec3 PackReader::sample_imagery_rgb(double lat_deg, double lon_deg) const {
    double clat = std::clamp(lat_deg, header_.lat_min, header_.lat_max);
    double clon = std::clamp(lon_deg, header_.lon_min, header_.lon_max);
    for (int z : img_zooms_desc_) {
        nspain::MercatorTilePos tp = nspain::latlon_to_tile(clat, clon, z);
        auto tile = get_rgb_tile(nspain::LAYER_IMAGERY, z, tp.tx, tp.ty);
        if (!tile) continue;
        int px = std::clamp(static_cast<int>(tp.fx * (tile->width - 1)), 0, tile->width - 1);
        int py = std::clamp(static_cast<int>(tp.fy * (tile->height - 1)), 0, tile->height - 1);
        size_t si = (static_cast<size_t>(py) * tile->width + px) * 3;
        // Convert sRGB ortho sample to linear RGB to match terrain shader space.
        auto to_lin = [](uint8_t v) {
            float s = v * (1.0f / 255.0f);
            return s * s;
        };
        return {to_lin(tile->rgb[si]), to_lin(tile->rgb[si + 1]), to_lin(tile->rgb[si + 2])};
    }
    return {0.16f, 0.22f, 0.11f};
}

void PackReader::bake_corridor_dem(WorldFrame &frame) const {
    const double cd_dlat = 82000.0 / frame.metres_lat;
    const double cd_dlon = 82000.0 / frame.metres_lon;
    double b_lon0 = std::clamp(frame.anchor_lon - cd_dlon, header_.lon_min, header_.lon_max);
    double b_lon1 = std::clamp(frame.anchor_lon + cd_dlon, header_.lon_min, header_.lon_max);
    double b_lat0 = std::clamp(frame.anchor_lat - cd_dlat, header_.lat_min, header_.lat_max);
    double b_lat1 = std::clamp(frame.anchor_lat + cd_dlat, header_.lat_min, header_.lat_max);
    if (const LoadedRegion *ar = find_region_at(frame.anchor_lat, frame.anchor_lon)) {
        b_lon0 = std::min(b_lon0, ar->hdr.lon_min);
        b_lat0 = std::min(b_lat0, ar->hdr.lat_min);
        b_lon1 = std::max(b_lon1, ar->hdr.lon_max);
        b_lat1 = std::max(b_lat1, ar->hdr.lat_max);
    }
    frame.min_e = (b_lon0 - frame.anchor_lon) * frame.metres_lon;
    frame.min_n = (b_lat0 - frame.anchor_lat) * frame.metres_lat;
    double max_e = (b_lon1 - frame.anchor_lon) * frame.metres_lon;
    double max_n = (b_lat1 - frame.anchor_lat) * frame.metres_lat;
    frame.span_e = std::max(1000.0, max_e - frame.min_e);
    frame.span_n = std::max(1000.0, max_n - frame.min_n);
    frame.dem.resize(SH_W * SH_H);
    frame.lighting.assign(static_cast<size_t>(SH_W) * SH_H, 0x0FFFFFFFu);
    for (int y = 0; y < SH_H; y++) {
        double lat = b_lat0 + (y + 0.5) / SH_H * (b_lat1 - b_lat0);
        for (int x = 0; x < SH_W; x++) {
            double lon = b_lon0 + (x + 0.5) / SH_W * (b_lon1 - b_lon0);
            frame.dem[y * SH_W + x] = static_cast<float>(sample_elevation(lat, lon));
        }
    }

    // Pre-bake whole-corridor 50km 10-Time-of-Day mountain horizon shadows + 8-dir Valley Sky AO in PARALLEL across CPU cores!
    const float cell_e = static_cast<float>(frame.span_e / SH_W);
    const float cell_n = static_cast<float>(frame.span_n / SH_H);
    const float cell_m = 0.5f * (cell_e + cell_n);
    const size_t npix = static_cast<size_t>(SH_W) * SH_H;
    std::vector<uint8_t> phase_q[10];
    for (int ch = 0; ch < 10; ch++) phase_q[ch].resize(npix, 3u);
    std::vector<uint8_t> ao_buf(npix, 255u);

    auto run_phase = [&](int ch) {
        std::vector<uint8_t> raw_sh(npix, 255u);
        float az = kTodAzDeg[ch] * DEG2RAD_F;
        float el = kTodElDeg[ch] * DEG2RAD_F;
        float de_u = std::sin(az);
        float dn_u = std::cos(az);
        float tan_el = std::tan(el);
        for (int y = 0; y < SH_H; y++) {
            for (int x = 0; x < SH_W; x++) {
                float h0 = frame.dem[y * SH_W + x];
                float max_occ = 0.0f;
                for (int s = 1; s <= 44; s++) {
                    float dist_px = (s <= 14) ? static_cast<float>(s) : (14.0f + (s - 14) * 2.4f);
                    int tx = static_cast<int>(std::lround(x + de_u * dist_px));
                    int ty = static_cast<int>(std::lround(y + dn_u * dist_px));
                    if (tx < 0 || tx >= SH_W || ty < 0 || ty >= SH_H) break;
                    float dist_m = dist_px * cell_m;
                    float dh = frame.dem[ty * SH_W + tx] - (h0 + dist_m * tan_el + 10.0f);
                    if (dh > 0.0f) {
                        max_occ = std::max(max_occ, std::min(1.0f, dh / std::max(34.0f, dist_m * 0.042f)));
                        if (max_occ >= 0.99f) break;
                    }
                }
                raw_sh[y * SH_W + x] = static_cast<uint8_t>(std::clamp((1.0f - max_occ) * 255.0f + 0.5f, 0.0f, 255.0f));
            }
        }
        uint8_t *dst = phase_q[ch].data();
        for (int y = 0; y < SH_H; y++) {
            int ym = std::max(0, y - 1), yp = std::min(SH_H - 1, y + 1);
            for (int x = 0; x < SH_W; x++) {
                int xm = std::max(0, x - 1), xp = std::min(SH_W - 1, x + 1);
                int s = raw_sh[y * SH_W + x] * 4 +
                        (raw_sh[y * SH_W + xm] + raw_sh[y * SH_W + xp] + raw_sh[ym * SH_W + x] + raw_sh[yp * SH_W + x]) * 2 +
                        (raw_sh[ym * SH_W + xm] + raw_sh[ym * SH_W + xp] + raw_sh[yp * SH_W + xm] + raw_sh[yp * SH_W + xp]);
                float vis = static_cast<float>((s + 8) >> 4) * (1.0f / 255.0f);
                dst[y * SH_W + x] = static_cast<uint8_t>(std::clamp(std::lround(vis * 3.0f), 0L, 3L));
            }
        }
    };

    auto run_ao = [&]() {
        static constexpr float kAoDistPx[4] = {1.5f, 3.5f, 7.5f, 15.0f};
        for (int y = 0; y < SH_H; y++) {
            for (int x = 0; x < SH_W; x++) {
                float h0 = frame.dem[y * SH_W + x];
                float ao_sum = 0.0f;
                for (int d = 0; d < 8; d++) {
                    float a = (2.0f * PI_F * d) / 8.0f;
                    float cx = std::cos(a), sy = std::sin(a);
                    float max_sin = 0.0f;
                    for (float r : kAoDistPx) {
                        int tx = std::clamp(static_cast<int>(std::lround(x + cx * r)), 0, SH_W - 1);
                        int ty = std::clamp(static_cast<int>(std::lround(y + sy * r)), 0, SH_H - 1);
                        float dh = std::max(0.0f, frame.dem[ty * SH_W + tx] - h0);
                        float dm = r * cell_m;
                        max_sin = std::max(max_sin, dh / std::sqrt(dh * dh + dm * dm));
                    }
                    ao_sum += (1.0f - std::pow(max_sin, 1.15f));
                }
                ao_buf[y * SH_W + x] = static_cast<uint8_t>(std::clamp(ao_sum * 0.125f, 0.32f, 1.0f) * 255.0f + 0.5f);
            }
        }
    };

    unsigned int hw = std::max(1u, std::thread::hardware_concurrency());
    unsigned int n_thr = std::min(hw, 11u);
    if (n_thr > 1u) {
        std::vector<std::thread> threads;
        threads.reserve(n_thr);
        for (unsigned int tid = 0; tid < n_thr; tid++) {
            threads.emplace_back([&, tid, n_thr]() {
                for (unsigned int job = tid; job < 11u; job += n_thr) {
                    if (job < 10u) run_phase(static_cast<int>(job));
                    else run_ao();
                }
            });
        }
        for (auto &t : threads) t.join();
    } else {
        for (int ch = 0; ch < 10; ch++) run_phase(ch);
        run_ao();
    }

    std::vector<uint32_t> acc(npix, 0u);
    for (size_t i = 0; i < npix; i++) {
        uint32_t v = static_cast<uint32_t>(ao_buf[i]) << 20u;
        for (int ch = 0; ch < 10; ch++) v |= (static_cast<uint32_t>(phase_q[ch][i]) << (ch * 2));
        acc[i] = v;
    }
    frame.lighting.swap(acc);
}

float PackReader::sample_global_shadow(const WorldFrame &frame, double east_m, double north_m, float phase10) {
    if (frame.lighting.empty()) return 1.0f;
    double u = (east_m - frame.min_e) / frame.span_e;
    double v = (north_m - frame.min_n) / frame.span_n;
    if (u <= 0.002 || u >= 0.998 || v <= 0.002 || v >= 0.998) return 1.0f;
    double fx = std::clamp(u * SH_W - 0.5, 0.0, static_cast<double>(SH_W - 1));
    double fy = std::clamp(v * SH_H - 0.5, 0.0, static_cast<double>(SH_H - 1));
    int x0 = std::min(SH_W - 2, static_cast<int>(fx));
    int y0 = std::min(SH_H - 2, static_cast<int>(fy));
    float dx = static_cast<float>(fx - x0), dy = static_cast<float>(fy - y0);
    float s00 = unpack_tod10_vis(frame.lighting[y0 * SH_W + x0], phase10);
    float s10 = unpack_tod10_vis(frame.lighting[y0 * SH_W + x0 + 1], phase10);
    float s01 = unpack_tod10_vis(frame.lighting[(y0 + 1) * SH_W + x0], phase10);
    float s11 = unpack_tod10_vis(frame.lighting[(y0 + 1) * SH_W + x0 + 1], phase10);
    float a = s00 * (1.0f - dx) + s10 * dx;
    float b = s01 * (1.0f - dx) + s11 * dx;
    return a * (1.0f - dy) + b * dy;
}

uint32_t PackReader::sample_baked_gi_u32_en(double east_m, double north_m) const {
    float vis[10] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
    float sky_ao = 1.0f;
    float night_l = 0.0f;

    auto sample_grid_bilerp = [&](const uint32_t *grid, int w, int h, double fx, double fy, bool take_min) {
        int x0 = std::clamp(static_cast<int>(fx), 0, w - 1);
        int y0 = std::clamp(static_cast<int>(fy), 0, h - 1);
        int x1 = std::min(w - 1, x0 + 1), y1 = std::min(h - 1, y0 + 1);
        float dx = static_cast<float>(fx - x0), dy = static_cast<float>(fy - y0);
        float w00 = (1.0f - dx) * (1.0f - dy), w10 = dx * (1.0f - dy), w01 = (1.0f - dx) * dy, w11 = dx * dy;
        uint32_t p00 = grid[static_cast<size_t>(y0) * w + x0];
        uint32_t p10 = grid[static_cast<size_t>(y0) * w + x1];
        uint32_t p01 = grid[static_cast<size_t>(y1) * w + x0];
        uint32_t p11 = grid[static_cast<size_t>(y1) * w + x1];
        for (int k = 0; k < 10; k++) {
            uint32_t sh = static_cast<uint32_t>(k * 2);
            float v = (static_cast<float>((p00 >> sh) & 3u) * w00 +
                       static_cast<float>((p10 >> sh) & 3u) * w10 +
                       static_cast<float>((p01 >> sh) & 3u) * w01 +
                       static_cast<float>((p11 >> sh) & 3u) * w11) * (1.0f / 3.0f);
            vis[k] = take_min ? std::min(vis[k], v) : v;
        }
        float ao = (unpack_tod10_ao(p00) * w00 + unpack_tod10_ao(p10) * w10 +
                    unpack_tod10_ao(p01) * w01 + unpack_tod10_ao(p11) * w11);
        sky_ao = take_min ? (sky_ao * ao) : ao;
        if (take_min) {
            float nl = (unpack_tod10_night(p00) * w00 + unpack_tod10_night(p10) * w10 +
                        unpack_tod10_night(p01) * w01 + unpack_tod10_night(p11) * w11);
            night_l = std::max(night_l, nl);
        }
    };

    // 1. Whole-corridor 50km 10-TOD mountain horizon shadows + valley Sky AO
    if (!world_frame_->lighting.empty()) {
        double fx = std::clamp((east_m  - world_frame_->min_e) / world_frame_->span_e, 0.0, 0.9999) * (SH_W - 1);
        double fy = std::clamp((north_m - world_frame_->min_n) / world_frame_->span_n, 0.0, 0.9999) * (SH_H - 1);
        sample_grid_bilerp(world_frame_->lighting.data(), SH_W, SH_H, fx, fy, false);
    }
    // 2. Populated-zone 3.9m/px 10-TOD building shadows + street-canyon Sky AO + RT Night Local Lights!
    double pt_lat = world_frame_->anchor_lat + north_m / world_frame_->metres_lat;
    double pt_lon = world_frame_->anchor_lon + east_m / world_frame_->metres_lon;
    if (const LoadedRegion *sr = find_sh_region_at_ll(pt_lat, pt_lon)) {
        double re = (pt_lon - sr->anchor_lon) * sr->m_lon;
        double rn = (pt_lat - sr->anchor_lat) * sr->m_lat;
        int32_t scx = static_cast<int32_t>(std::floor(re / sr->sh_chunk_m));
        int32_t scy = static_cast<int32_t>(std::floor(rn / sr->sh_chunk_m));
        if (auto tile = get_city_sh_tile(*sr, scx, scy)) {
            double fx = std::clamp((re - scx * sr->sh_chunk_m) / sr->sh_chunk_m, 0.0, 0.9999) * (sr->sh_res - 1);
            double fy = std::clamp((rn - scy * sr->sh_chunk_m) / sr->sh_chunk_m, 0.0, 0.9999) * (sr->sh_res - 1);
            sample_grid_bilerp(tile->data(), sr->sh_res, sr->sh_res, fx, fy, true);
        }
    }
    return pack_tod10_u32(vis, sky_ao, night_l);
}

void PackReader::fill_corridor_gi_rg8(float phase10, std::vector<uint8_t> &out_rg) const {
    out_rg.resize(static_cast<size_t>(SH_W) * SH_H * 2u);
    if (world_frame_->lighting.empty()) {
        std::fill(out_rg.begin(), out_rg.end(), 255u);
        return;
    }
    for (size_t i = 0; i < world_frame_->lighting.size(); i++) {
        uint32_t b = world_frame_->lighting[i];
        out_rg[i * 2 + 0] = static_cast<uint8_t>(std::clamp(unpack_tod10_vis(b, phase10) * 255.0f + 0.5f, 0.0f, 255.0f));
        out_rg[i * 2 + 1] = static_cast<uint8_t>((b >> 20u) & 0xFFu);
    }
}

void PackReader::rasterize_building_mask(
    double center_lat, double center_lon, double half_span_m,
    int res, std::vector<uint8_t> &out_alpha
) const {
    out_alpha.assign(static_cast<size_t>(res) * res, 0);
    if (buildings_.empty() || res < 4) return;

    double m_lat = 111132.0, m_lon = 82600.0;
    nspain::meters_per_degree(center_lat, m_lat, m_lon);
    double d_lat = half_span_m / m_lat;
    double d_lon = half_span_m / m_lon;
    double min_lat = center_lat - d_lat, max_lat = center_lat + d_lat;
    double min_lon = center_lon - d_lon, max_lon = center_lon + d_lon;

    int32_t cx0 = static_cast<int32_t>(std::floor(min_lon / BLDG_CELL_DEG));
    int32_t cx1 = static_cast<int32_t>(std::floor(max_lon / BLDG_CELL_DEG));
    int32_t cy0 = static_cast<int32_t>(std::floor(min_lat / BLDG_CELL_DEG));
    int32_t cy1 = static_cast<int32_t>(std::floor(max_lat / BLDG_CELL_DEG));
    double inv_span = 1.0 / std::max(1.0, 2.0 * half_span_m);
    bool pad = ((2.0 * half_span_m) / res) > 36.0;

    for (int32_t cy = cy0; cy <= cy1; cy++) {
        for (int32_t cx = cx0; cx <= cx1; cx++) {
            auto it = bldg_grid_.find(pack_cell(cx, cy));
            if (it == bldg_grid_.end()) continue;
            for (uint32_t idx : it->second) {
                const BuildingRec &b = buildings_[idx];
                if (b.lat < min_lat || b.lat > max_lat || b.lon < min_lon || b.lon > max_lon) continue;
                double u01 = ((b.lon - center_lon) * m_lon + half_span_m) * inv_span;
                double v01 = ((b.lat - center_lat) * m_lat + half_span_m) * inv_span;
                int px = std::clamp(static_cast<int>(u01 * (res - 1) + 0.5), 0, res - 1);
                int py = std::clamp(static_cast<int>(v01 * (res - 1) + 0.5), 0, res - 1);
                uint8_t val = static_cast<uint8_t>(std::clamp(b.height_m / 18.0f, 0.30f, 1.0f) * 255.0f);
                size_t k = static_cast<size_t>(py) * res + px;
                if (val > out_alpha[k]) out_alpha[k] = val;
                if (pad) {
                    uint8_t halo = static_cast<uint8_t>(val * 0.58f);
                    for (int dy = -1; dy <= 1; dy++) {
                        int ny = py + dy;
                        if (ny < 0 || ny >= res) continue;
                        for (int dx = -1; dx <= 1; dx++) {
                            int nx = px + dx;
                            if (nx < 0 || nx >= res) continue;
                            size_t nk = static_cast<size_t>(ny) * res + nx;
                            if (halo > out_alpha[nk]) out_alpha[nk] = halo;
                        }
                    }
                }
            }
        }
    }
}

void PackReader::fill_page_data(
    const WorldFrame &frame,
    double center_east_m, double center_north_m, double half_span_m,
    int height_n, int ortho_res, int aux_res,
    const Vec3 &sun_dir,
    std::vector<float> &out_height,
    std::vector<uint8_t> &out_ortho_rgba,
    std::vector<uint8_t> &out_norm_ao_rgba,
    std::vector<uint8_t> &out_surf_rgba,
    PageImagery *out_imagery
) const {
    if (out_imagery) *out_imagery = PageImagery{online_revision()};
    const double center_lat = frame.anchor_lat + center_north_m / frame.metres_lat;
    const double center_lon = frame.anchor_lon + center_east_m / frame.metres_lon;
    const double span_m = half_span_m * 2.0;

    // 1. Height grid (with 1-cell border gutter so normals & vertex morphing are 100% crack-free across CDLOD quads!)
    const int inner_n = height_n - 2;
    const double h_step = span_m / static_cast<double>(std::max(1, inner_n - 1));
    out_height.resize(static_cast<size_t>(height_n) * height_n);
    {
        uint64_t z_key[24];
        std::shared_ptr<DecodedElevTile> z_tile[24];
        for (int k = 0; k < 24; k++) z_key[k] = UINT64_MAX;
        for (int j = 0; j < height_n; j++) {
            double north_m = center_north_m - half_span_m + static_cast<double>(j - 1) * h_step;
            double clat = std::clamp(frame.anchor_lat + north_m / frame.metres_lat, header_.lat_min, header_.lat_max);
            double merc_y01 = (1.0 - std::asinh(std::tan(clat * DEG2RAD_D)) / PI_D) * 0.5;
            for (int i = 0; i < height_n; i++) {
                double east_m = center_east_m - half_span_m + static_cast<double>(i - 1) * h_step;
                double clon = std::clamp(frame.anchor_lon + east_m / frame.metres_lon, header_.lon_min, header_.lon_max);
                double merc_x01 = (clon + 180.0) / 360.0;
                float val = 400.0f;
                for (int z : elev_zooms_desc_) {
                    double nt = static_cast<double>(1u << z);
                    int32_t lim = static_cast<int32_t>(1u << z) - 1;
                    double xt = merc_x01 * nt, yt = merc_y01 * nt;
                    int32_t tx = std::clamp(static_cast<int32_t>(std::floor(xt)), 0, lim);
                    int32_t ty = std::clamp(static_cast<int32_t>(std::floor(yt)), 0, lim);
                    uint64_t k = nspain::make_tile_key(nspain::LAYER_ELEVATION, static_cast<uint8_t>(z), tx, ty);
                    int zs = std::clamp(z, 0, 23);
                    if (k != z_key[zs]) { z_key[zs] = k; z_tile[zs] = get_elev_tile(z, tx, ty); }
                    const DecodedElevTile *t = z_tile[zs].get();
                    if (!t) continue;
                    double px = std::clamp(xt - tx, 0.0, 1.0) * (t->width - 1);
                    double py = std::clamp(yt - ty, 0.0, 1.0) * (t->height - 1);
                    int x0 = std::clamp(static_cast<int>(std::floor(px)), 0, t->width - 1);
                    int y0 = std::clamp(static_cast<int>(std::floor(py)), 0, t->height - 1);
                    int x1 = std::min<int>(t->width - 1, x0 + 1);
                    int y1 = std::min<int>(t->height - 1, y0 + 1);
                    double dx = px - x0, dy = py - y0;
                    float a = t->heights[y0 * t->width + x0] * static_cast<float>(1.0 - dx) + t->heights[y0 * t->width + x1] * static_cast<float>(dx);
                    float b = t->heights[y1 * t->width + x0] * static_cast<float>(1.0 - dx) + t->heights[y1 * t->width + x1] * static_cast<float>(dx);
                    val = a * static_cast<float>(1.0 - dy) + b * static_cast<float>(dy);
                    break;
                }
                // Clamp submarine Terrarium bathymetry (<0 m) to sea level so coastal airfields never drop into ocean trenches.
                out_height[j * height_n + i] = std::max(0.0f, val);
            }
        }
    }

    // 2. Ultra-Fast Tile-Batched IGN PNOA Ortho & Surface Resampler (hoisted 1D Mercator + zero per-pixel locks!)
    auto sample_rgb_grid = [&](uint8_t layer, const std::vector<int> &zooms, int res, std::vector<uint8_t> &dst, bool jitter_coarse) {
        dst.assign(static_cast<size_t>(res) * res * 3, layer == nspain::LAYER_VEGETATION ? 0 : 96);
        double texel_m = span_m / static_cast<double>(res);
        int cap_z = zooms.empty() ? 11 : zooms.front();
        if (texel_m > 2.2) {
            for (int z : zooms) {
                double m_px = (156543.03 * std::cos(center_lat * DEG2RAD_D)) / static_cast<double>(1u << z);
                if (m_px <= texel_m * 1.45) cap_z = z;
            }
        }
        int active_z[16];
        int n_az = 0;
        OnlineOrthoProvider oprov = (layer == nspain::LAYER_IMAGERY)
            ? online_provider_.load(std::memory_order_relaxed)
            : OnlineOrthoProvider::OFF;
        int tier_z = 0;
        if (oprov == OnlineOrthoProvider::PNOA) {
            tier_z = online_page_zoom(center_lat, center_lon, half_span_m, res);
            if (out_imagery) out_imagery->target_zoom = tier_z;
            // Include already-cached online zooms (tier_z down to 13) so a higher tier step never drops back to baked Z12.
            for (int z = tier_z; z >= 13 && n_az < 16; z--) active_z[n_az++] = z;
        }
        for (int z : zooms) {
            if (z > cap_z || n_az >= 16) continue;
            bool dup = false;
            for (int i = 0; i < n_az; i++) if (active_z[i] == z) { dup = true; break; }
            if (!dup) active_z[n_az++] = z;
        }
        std::sort(active_z, active_z + n_az, std::greater<int>{});

        const double step = span_m / static_cast<double>(std::max(1, res - 1));
        thread_local std::vector<double> col_east, col_x01;
        col_east.resize(res);
        col_x01.resize(res);
        const double inv_m_lon = 1.0 / frame.metres_lon;
        const double inv_m_lat = 1.0 / frame.metres_lat;
        for (int i = 0; i < res; i++) {
            double em = center_east_m - half_span_m + i * step;
            col_east[i] = em;
            double clon = std::clamp(frame.anchor_lon + em * inv_m_lon, header_.lon_min, header_.lon_max);
            col_x01[i] = (clon + 180.0) * (1.0 / 360.0);
        }

        // Pre-resolve the small 2D grid of source tiles for each active zoom ONCE (turns 262,144 lookups into <= 9!)
        struct ZoomTileGrid {
            int z = 0;
            double nt = 1.0;
            int32_t lim = 0, tx0 = 0, ty0 = 0, nx = 0, ny = 0;
            std::vector<std::shared_ptr<DecodedRgbTile>> tiles;
        };
        double clat_s = std::clamp(frame.anchor_lat + (center_north_m - half_span_m) * inv_m_lat, header_.lat_min, header_.lat_max);
        double clat_n = std::clamp(frame.anchor_lat + (center_north_m + half_span_m) * inv_m_lat, header_.lat_min, header_.lat_max);
        double my_top = (1.0 - std::asinh(std::tan(clat_n * DEG2RAD_D)) / PI_D) * 0.5;
        double my_bot = (1.0 - std::asinh(std::tan(clat_s * DEG2RAD_D)) / PI_D) * 0.5;
        double mx_min = col_x01[0], mx_max = col_x01[res - 1];

        ZoomTileGrid grids[16];
        int n_grids = 0;
        for (int az = 0; az < n_az; az++) {
            int z = active_z[az];
            double nt = static_cast<double>(1u << z);
            int32_t lim = static_cast<int32_t>(1u << z) - 1;
            int32_t tx0 = std::clamp(static_cast<int32_t>(std::floor(mx_min * nt)), 0, lim);
            int32_t tx1 = std::clamp(static_cast<int32_t>(std::floor(mx_max * nt)), 0, lim);
            int32_t ty0 = std::clamp(static_cast<int32_t>(std::floor(my_top * nt)), 0, lim);
            int32_t ty1 = std::clamp(static_cast<int32_t>(std::floor(my_bot * nt)), 0, lim);
            int32_t nx = tx1 - tx0 + 1;
            int32_t ny = ty1 - ty0 + 1;
            if (nx <= 0 || ny <= 0 || nx * ny > 256) continue;
            ZoomTileGrid &g = grids[n_grids];
            g.z = z; g.nt = nt; g.lim = lim; g.tx0 = tx0; g.ty0 = ty0; g.nx = nx; g.ny = ny;
            g.tiles.resize(static_cast<size_t>(nx) * ny);
            bool any = false;
            for (int32_t ty = ty0; ty <= ty1; ty++) {
                for (int32_t tx = tx0; tx <= tx1; tx++) {
                    // Prefer current PNOA imagery at its distance tier. If it is
                    // not cached yet, queue its download and keep the installed
                    // pack tile as a seamless offline fallback for this page.
                    std::shared_ptr<DecodedRgbTile> tile;
                    if (oprov == OnlineOrthoProvider::PNOA && tier_z > 0 && z <= tier_z && z >= 13) {
                        const int carrier_z = (tier_z >= 16) ? 15 : ((tier_z == 15) ? 14 : tier_z);
                        const bool queue_dl = z == tier_z || z == carrier_z;
                        tile = get_online_rgb_tile(oprov, z, tx, ty, queue_dl);
                        if (out_imagery && z == tier_z) {
                            if (tile) ++out_imagery->target_tiles_ready;
                            else ++out_imagery->target_tiles_missing;
                        }
                    }
                    if (!tile) tile = get_rgb_tile(layer, z, tx, ty);
                    if (tile) any = true;
                    g.tiles[static_cast<size_t>(ty - ty0) * nx + (tx - tx0)] = std::move(tile);
                }
            }
            if (any) n_grids++;
        }

        (void)jitter_coarse;
        for (int j = 0; j < res; j++) {
            const double north_m = center_north_m - half_span_m + j * step;
            const double clat = std::clamp(frame.anchor_lat + north_m * inv_m_lat, header_.lat_min, header_.lat_max);
            const double merc_y01 = (1.0 - std::asinh(std::tan(clat * DEG2RAD_D)) / PI_D) * 0.5;
            uint8_t *row_out = dst.data() + static_cast<size_t>(j) * res * 3;

            for (int i = 0; i < res; i++) {
                const double merc_x01 = col_x01[i];
                for (int gi = 0; gi < n_grids; gi++) {
                    const ZoomTileGrid &g = grids[gi];
                    const double xt = merc_x01 * g.nt;
                    const double yt = merc_y01 * g.nt;
                    const int32_t tx = std::clamp(static_cast<int32_t>(std::floor(xt)), g.tx0, g.tx0 + g.nx - 1);
                    const int32_t ty = std::clamp(static_cast<int32_t>(std::floor(yt)), g.ty0, g.ty0 + g.ny - 1);
                    const DecodedRgbTile *t = g.tiles[static_cast<size_t>(ty - g.ty0) * g.nx + (tx - g.tx0)].get();
                    if (!t) continue;
                    const int tw = t->width, th = t->height;
                    const double px = std::clamp(xt - tx, 0.0, 1.0) * (tw - 1);
                    const double py = std::clamp(yt - ty, 0.0, 1.0) * (th - 1);
                    const int x0 = std::min(tw - 1, static_cast<int>(px));
                    const int y0 = std::min(th - 1, static_cast<int>(py));
                    const int x1 = std::min(tw - 1, x0 + 1);
                    const int y1 = std::min(th - 1, y0 + 1);
                    const float dx = static_cast<float>(px - x0);
                    const float dy = static_cast<float>(py - y0);
                    const float w00 = (1.0f - dx) * (1.0f - dy);
                    const float w10 = dx * (1.0f - dy);
                    const float w01 = (1.0f - dx) * dy;
                    const float w11 = dx * dy;
                    const uint8_t *p00 = t->rgb.data() + (static_cast<size_t>(y0) * tw + x0) * 3;
                    const uint8_t *p10 = t->rgb.data() + (static_cast<size_t>(y0) * tw + x1) * 3;
                    const uint8_t *p01 = t->rgb.data() + (static_cast<size_t>(y1) * tw + x0) * 3;
                    const uint8_t *p11 = t->rgb.data() + (static_cast<size_t>(y1) * tw + x1) * 3;
                    if (layer == nspain::LAYER_VEGETATION &&
                        p00[1] * w00 + p10[1] * w10 + p01[1] * w01 + p11[1] * w11 < 127.0f)
                        continue;
                    uint8_t *o = row_out + i * 3;
                    o[0] = static_cast<uint8_t>(p00[0] * w00 + p10[0] * w10 + p01[0] * w01 + p11[0] * w11 + 0.5f);
                    o[1] = static_cast<uint8_t>(p00[1] * w00 + p10[1] * w10 + p01[1] * w01 + p11[1] * w11 + 0.5f);
                    o[2] = static_cast<uint8_t>(p00[2] * w00 + p10[2] * w10 + p01[2] * w01 + p11[2] * w11 + 0.5f);
                    break;
                }
            }
        }
    };

    thread_local std::vector<uint8_t> raw_rgb, raw_surf, raw_vegetation;
    sample_rgb_grid(nspain::LAYER_IMAGERY, img_zooms_desc_, ortho_res, raw_rgb, false);
    sample_rgb_grid(nspain::LAYER_SURFACE, surf_zooms_desc_, aux_res, raw_surf, false);
    sample_rgb_grid(nspain::LAYER_VEGETATION, vegetation_zooms_desc_, aux_res, raw_vegetation, false);

    // 3. Photography-Preserving Gentle Unsharp Clarity (preserves 100% genuine IGN PNOA imagery without cartoon shock blotches!)
    const size_t n_opix = static_cast<size_t>(ortho_res) * ortho_res;
    out_ortho_rgba.resize(n_opix * 4);
    const uint8_t *rr = raw_rgb.data();
    uint8_t *oo = out_ortho_rgba.data();
    for (int j = 0; j < ortho_res; j++) {
        const int jm = std::max(0, j - 1) * ortho_res;
        const int jc = j * ortho_res;
        const int jp = std::min(ortho_res - 1, j + 1) * ortho_res;
        for (int i = 0; i < ortho_res; i++) {
            const int im = std::max(0, i - 1);
            const int ip = std::min(ortho_res - 1, i + 1);
            const size_t sc = static_cast<size_t>(jc + i) * 3;
            const size_t sW = static_cast<size_t>(jc + im) * 3, sE = static_cast<size_t>(jc + ip) * 3;
            const size_t sS = static_cast<size_t>(jm + i) * 3,  sN = static_cast<size_t>(jp + i) * 3;
            const size_t di = static_cast<size_t>(jc + i) * 4;
            // Add a weak world-locked, zero-mean luminance grain while pages are
            // generated. The same pattern is reused across adjacent pages.
            float grain = 0.0f;
            if (span_m < 1500.0) {
                double e = center_east_m - half_span_m + span_m * i / std::max(1, ortho_res - 1);
                double n = center_north_m - half_span_m + span_m * j / std::max(1, ortho_res - 1);
                int gx = static_cast<int>(std::floor(e * 2.0));
                int gy = static_cast<int>(std::floor(n * 2.0));
                // Adjacent pairs sum to zero, avoiding a systematic color shift.
                uint32_t bits = hash2i(gx >> 1, gy, 8723u);
                float magnitude = static_cast<float>(bits & 255u) / 255.0f;
                grain = ((gx & 1) ? magnitude : -magnitude) * 0.035f;
            }
            for (int c = 0; c < 3; c++) {
                float vc = static_cast<float>(rr[sc + c]);
                float avg = 0.25f * (rr[sW + c] + rr[sE + c] + rr[sS + c] + rr[sN + c]);
                float sharp = std::clamp((vc + (vc - avg) * 0.14f) * (1.0f + grain), 0.0f, 255.0f);
                oo[di + c] = static_cast<uint8_t>(sharp + 0.5f);
            }
            oo[di + 3] = 255;
        }
    }

    // 4. High-res Sobel DEM Normal + Topographic Cavity AO (computed from out_height bilinearly -> ultra-fast!)
    auto h_at = [&](float u01, float v01) -> float {
        float fx = 1.0f + u01 * static_cast<float>(inner_n - 1);
        float fy = 1.0f + v01 * static_cast<float>(inner_n - 1);
        int x0 = std::clamp(static_cast<int>(std::floor(fx)), 0, height_n - 2);
        int y0 = std::clamp(static_cast<int>(std::floor(fy)), 0, height_n - 2);
        float dx = fx - x0, dy = fy - y0;
        float a = out_height[y0 * height_n + x0] * (1.0f - dx) + out_height[y0 * height_n + x0 + 1] * dx;
        float b = out_height[(y0 + 1) * height_n + x0] * (1.0f - dx) + out_height[(y0 + 1) * height_n + x0 + 1] * dx;
        return a * (1.0f - dy) + b * dy;
    };

    out_norm_ao_rgba.resize(static_cast<size_t>(aux_res) * aux_res * 4);
    out_surf_rgba.resize(static_cast<size_t>(aux_res) * aux_res * 4);

    // 5. PMRF OSM Building Footprints -> surface_tex.a
    std::vector<uint8_t> bldg_alpha;
    rasterize_building_mask(center_lat, center_lon, half_span_m, aux_res, bldg_alpha);
    std::vector<uint8_t> vegetation_exclusions;
    if (span_m <= 2500.0) {
        vegetation_exclusions.resize(static_cast<size_t>(aux_res) * aux_res);
        fill_cell_occupancy(frame, center_east_m - half_span_m, center_north_m - half_span_m,
                            span_m, aux_res, vegetation_exclusions.data(), false);
    }

    const float aux_step_m = static_cast<float>(span_m / std::max(1, aux_res - 1));
    const float du = 1.0f / static_cast<float>(std::max(1, aux_res - 1));
    const Vec3 nd = normalize(sun_dir);
    const float phase10 = sun_dir_to_phase10(nd);
    const LoadedRegion *sh_reg = find_sh_region_at_ll(center_lat, center_lon);
    const bool has_city_sh = (sh_reg != nullptr);
    int64_t last_csh_k = INT64_MAX;
    std::shared_ptr<std::vector<uint32_t>> cur_csh;

    for (int j = 0; j < aux_res; j++) {
        float v01 = static_cast<float>(j) * du;
        double north_m = center_north_m - half_span_m + v01 * span_m;
        for (int i = 0; i < aux_res; i++) {
            float u01 = static_cast<float>(i) * du;
            double east_m = center_east_m - half_span_m + u01 * span_m;

            float hc = h_at(u01, v01);
            // Sample into the 1-texel heightmap border gutter (u01/v01 outside [0..1]) so edge normals & cavity AO
            // are 100% bit-identical across adjacent CDLOD quadtree tiles -> ZERO ridge/tile seams!
            float hW = h_at(u01 - du, v01);
            float hE = h_at(u01 + du, v01);
            float hS = h_at(u01, v01 - du);
            float hN = h_at(u01, v01 + du);
            float gx = (hE - hW) / std::max(1.0f, 2.0f * aux_step_m);
            float gy = (hN - hS) / std::max(1.0f, 2.0f * aux_step_m);
            // World: +X=East, +Y=Up, -Z=North -> normal = normalize(-dh/de, 1, +dh/dn)
            Vec3 nrm = normalize(Vec3(-gx, 1.0f, gy));
            float avg_nb = 0.25f * (hW + hE + hS + hN);
            float curv = (hc - avg_nb) / std::max(1.5f, aux_step_m * 0.22f);
            float cav_ao = std::clamp(0.55f + curv * 0.42f, 0.05f, 1.0f);

            size_t di = (static_cast<size_t>(j) * aux_res + i) * 4;
            size_t si = (static_cast<size_t>(j) * aux_res + i) * 3;
            float f_raw = raw_vegetation[si + 0] * (1.0f / 255.0f);
            if (!vegetation_exclusions.empty() && vegetation_exclusions[static_cast<size_t>(j) * aux_res + i]) f_raw = 0.0f;
            float w_raw = raw_surf[si + 2] * (1.0f / 255.0f);
            float sl = std::hypot(gx, gy);
            w_raw *= (1.0f - smoothstep_f(0.16f, 0.34f, sl));

            // Pre-bake 50km planetary 10-TOD horizon shadow + local cliff shadow + UNIVERSAL populated-zone 10-TOD shadows!
            float occ = 1.0f - sample_global_shadow(frame, east_m, north_m, phase10);

            // Fold pre-baked 3.9m/px populated-zone 10-TOD building shadows, Sky AO & RT Night Local Lights into EVERY town/village!
            float rt_night = 0.0f;
            if (has_city_sh && w_raw < 0.35f) {
                double re = east_m, rn = north_m;
                if (sh_reg->anchor_lat != frame.anchor_lat || sh_reg->anchor_lon != frame.anchor_lon) {
                    double p_lat = frame.anchor_lat + north_m / frame.metres_lat;
                    double p_lon = frame.anchor_lon + east_m  / frame.metres_lon;
                    re = (p_lon - sh_reg->anchor_lon) * sh_reg->m_lon;
                    rn = (p_lat - sh_reg->anchor_lat) * sh_reg->m_lat;
                }
                int32_t scx = static_cast<int32_t>(std::floor(re / sh_reg->sh_chunk_m));
                int32_t scy = static_cast<int32_t>(std::floor(rn / sh_reg->sh_chunk_m));
                int64_t sk = pack_cell(scx, scy);
                if (sk != last_csh_k) {
                    last_csh_k = sk;
                    cur_csh = get_city_sh_tile(*sh_reg, scx, scy);
                }
                if (cur_csh) {
                    const int sres = sh_reg->sh_res;
                    const double schm = sh_reg->sh_chunk_m;
                    double fx = std::clamp((re - scx * schm) / schm, 0.0, 0.9999) * (sres - 1);
                    double fy = std::clamp((rn - scy * schm) / schm, 0.0, 0.9999) * (sres - 1);
                    int x0 = static_cast<int>(fx), y0 = static_cast<int>(fy);
                    int x1 = std::min(sres - 1, x0 + 1), y1 = std::min(sres - 1, y0 + 1);
                    float dx = static_cast<float>(fx - x0), dy = static_cast<float>(fy - y0);
                    float w00 = (1.0f - dx) * (1.0f - dy), w10 = dx * (1.0f - dy), w01 = (1.0f - dx) * dy, w11 = dx * dy;
                    const uint32_t *d = cur_csh->data();
                    uint32_t p00 = d[static_cast<size_t>(y0) * sres + x0];
                    uint32_t p10 = d[static_cast<size_t>(y0) * sres + x1];
                    uint32_t p01 = d[static_cast<size_t>(y1) * sres + x0];
                    uint32_t p11 = d[static_cast<size_t>(y1) * sres + x1];
                    float c_sun = unpack_tod10_vis(p00, phase10) * w00 + unpack_tod10_vis(p10, phase10) * w10 +
                                  unpack_tod10_vis(p01, phase10) * w01 + unpack_tod10_vis(p11, phase10) * w11;
                    float c_ao  = unpack_tod10_ao(p00) * w00 + unpack_tod10_ao(p10) * w10 +
                                  unpack_tod10_ao(p01) * w01 + unpack_tod10_ao(p11) * w11;
                    float c_nl  = unpack_tod10_night(p00) * w00 + unpack_tod10_night(p10) * w10 +
                                  unpack_tod10_night(p01) * w01 + unpack_tod10_night(p11) * w11;
                    float dry_w = 1.0f - smoothstep_f(0.10f, 0.35f, w_raw);
                    occ = 1.0f - (1.0f - occ) * std::lerp(1.0f, std::lerp(0.18f, 1.0f, c_sun), dry_w);
                    cav_ao = std::clamp(cav_ao * std::lerp(1.0f, std::lerp(0.45f, 1.0f, c_ao), dry_w), 0.05f, 1.0f);
                    rt_night = c_nl * dry_w;
                }
            }

            out_norm_ao_rgba[di + 0] = static_cast<uint8_t>(std::clamp((nrm.x * 0.5f + 0.5f) * 255.0f, 0.0f, 255.0f));
            out_norm_ao_rgba[di + 1] = static_cast<uint8_t>(std::clamp((nrm.z * 0.5f + 0.5f) * 255.0f, 0.0f, 255.0f));
            out_norm_ao_rgba[di + 2] = static_cast<uint8_t>(std::clamp(cav_ao * 255.0f, 0.0f, 255.0f));
            out_norm_ao_rgba[di + 3] = static_cast<uint8_t>(std::clamp(rt_night * 255.0f + 0.5f, 0.0f, 255.0f));

            out_surf_rgba[di + 0] = static_cast<uint8_t>(std::clamp(f_raw * 255.0f, 0.0f, 255.0f));
            out_surf_rgba[di + 1] = static_cast<uint8_t>(std::clamp((1.0f - occ) * 255.0f, 0.0f, 255.0f));
            out_surf_rgba[di + 2] = static_cast<uint8_t>(std::clamp(w_raw * 255.0f, 0.0f, 255.0f));
            out_surf_rgba[di + 3] = bldg_alpha[static_cast<size_t>(j) * aux_res + i];
        }
    }
}

PackReader::ObjectCollisionHit PackReader::query_object_collision(
    double lat_deg, double lon_deg, double alt_msl_m, double probe_r_m
) const {
    ObjectCollisionHit best{};
    const auto wf = world_frame();
    if (!wf) return best;
    const double r = std::max(0.25, probe_r_m);
    auto consider_obb = [&](double obj_lat, double obj_lon, float ground_m, float height_m,
                            float width_m, float depth_m, float yaw_rad, uint8_t kind) {
        const double top_m = static_cast<double>(ground_m) + height_m + r;
        const double bot_m = static_cast<double>(ground_m) - 1.5;
        if (alt_msl_m >= top_m || alt_msl_m <= bot_m) return;
        const double de = (lon_deg - obj_lon) * wf->metres_lon;
        const double dn = (lat_deg - obj_lat) * wf->metres_lat;
        const double max_ext = 0.5 * (static_cast<double>(width_m) + static_cast<double>(depth_m)) + r;
        if (std::abs(de) >= max_ext || std::abs(dn) >= max_ext) return;
        const double dx = de;
        const double dz = -dn;
        const double c = std::cos(yaw_rad), s = std::sin(yaw_rad);
        const double lx = dx * c - dz * s;
        const double lz = dx * s + dz * c;
        const double hx = width_m * 0.5 + r;
        const double hz = depth_m * 0.5 + r;
        if (std::abs(lx) >= hx || std::abs(lz) >= hz) return;

        const double pen_x = hx - std::abs(lx);
        const double pen_z = hz - std::abs(lz);
        const double pen_y = top_m - alt_msl_m;
        float ne = 0.0f, nup = 0.0f, nn = 0.0f;
        double pen = 0.0;
        if (pen_y < std::min(pen_x, pen_z) && alt_msl_m >= static_cast<double>(ground_m) + height_m * 0.65) {
            pen = pen_y;
            nup = 1.0f;
        } else if (pen_x <= pen_z) {
            pen = pen_x;
            const double sx = (lx >= 0.0) ? 1.0 : -1.0;
            ne = static_cast<float>(sx * c);
            nn = static_cast<float>(sx * s);
        } else {
            pen = pen_z;
            const double sz = (lz >= 0.0) ? 1.0 : -1.0;
            ne = static_cast<float>(sz * s);
            nn = static_cast<float>(-sz * c);
        }
        if (!best.hit || pen > best.penetration_m) {
            best.hit = true;
            best.kind = kind;
            best.normal_e = ne;
            best.normal_up = nup;
            best.normal_n = nn;
            best.penetration_m = static_cast<float>(pen);
        }
    };

    // 1. OSM Buildings, Hangars, Control Towers & Terminals in spatial hash
    if (!bldg_grid_.empty()) {
        const int32_t cx0 = static_cast<int32_t>(std::floor(lon_deg / BLDG_CELL_DEG));
        const int32_t cy0 = static_cast<int32_t>(std::floor(lat_deg / BLDG_CELL_DEG));
        for (int32_t dy = -1; dy <= 1; ++dy) {
            for (int32_t dx = -1; dx <= 1; ++dx) {
                auto it = bldg_grid_.find(pack_cell(cx0 + dx, cy0 + dy));
                if (it == bldg_grid_.end()) continue;
                for (uint32_t idx : it->second) {
                    const BuildingRec &b = buildings_[idx];
                    consider_obb(b.lat, b.lon, b.ground_m, b.height_m, b.width_m, b.depth_m, b.yaw_rad, 0);
                }
            }
        }
    }

    // 2. Iconic Landmarks (Basílica del Pilar, Puente de Piedra, Torre del Agua, Huesca, Loarre, Mediano)
    struct StaticLandmark { double lat, lon; float yaw_deg, w, d, y0, h; };
    static constexpr StaticLandmark kLandmarks[] = {
        {41.6569, -0.8787, -27.0f, 136.0f, 70.0f, -4.0f, 36.0f},  // Basilica del Pilar nave+domes
        {41.6569, -0.8787, -27.0f, 128.0f, 66.0f, -4.0f, 92.0f},  // Pilar 4 corner towers silhouette? Wait check center!
        {41.6578, -0.8755,  62.0f, 224.0f, 14.5f, -6.0f, 17.5f},  // Puente de Piedra
        {41.6693, -0.9073,  12.0f,  24.0f, 24.0f, -3.0f, 76.0f},  // Torre del Agua
        {42.1404, -0.4086, -12.0f,  56.0f, 40.0f, -4.0f, 51.0f},  // Catedral de Huesca + tower
        {42.3257, -0.6119,  15.0f,  62.0f, 44.0f, -6.0f, 28.0f},  // Castillo de Loarre
        {42.3175,  0.2037,   8.0f,   8.4f,  8.4f, -12.0f, 28.5f}, // Torre de Mediano
    };
    for (int li = 0; li < static_cast<int>(sizeof(kLandmarks) / sizeof(kLandmarks[0])); ++li) {
        if (li == 1) continue; // Pilar corner towers would wrap courtyard in 128x66x92 box; test 4 corners explicitly below.
        const auto &lm = kLandmarks[li];
        if (std::abs(lat_deg - lm.lat) > 0.004 || std::abs(lon_deg - lm.lon) > 0.005) continue;
        const float g = static_cast<float>(sample_elevation(lm.lat, lm.lon));
        consider_obb(lm.lat, lm.lon, g + lm.y0, lm.h - lm.y0, lm.w, lm.d, lm.yaw_deg * DEG2RAD_F, 1);
    }
    if (std::abs(lat_deg - 41.6569) < 0.003 && std::abs(lon_deg - (-0.8787)) < 0.004) {
        const float pg = static_cast<float>(sample_elevation(41.6569, -0.8787));
        const float yr = -27.0f * DEG2RAD_F, cy = std::cos(yr), sy = std::sin(yr);
        for (float lx : {-57.0f, 57.0f}) {
            for (float lz : {-27.0f, 27.0f}) {
                const double tlon = -0.8787 + (lx * cy + lz * sy) / wf->metres_lon;
                const double tlat = 41.6569 + (lx * sy - lz * cy) / wf->metres_lat;
                consider_obb(tlat, tlon, pg, 93.0f, 12.8f, 12.8f, yr, 1);
            }
        }
    }

    // 3. Airport perimeter stone walls, fences & hedges
    for (const auto &a : airports_) {
        if (std::abs(lat_deg - a.lat) > 0.03 || std::abs(lon_deg - a.lon) > 0.04) continue;
        for (const auto &fn : a.fences) {
            const float fh = (fn.kind == "wall") ? 2.1f : 1.7f;
            for (size_t pi = 1; pi < fn.points_lat_lon.size(); ++pi) {
                const auto [lat0, lon0] = fn.points_lat_lon[pi - 1];
                const auto [lat1, lon1] = fn.points_lat_lon[pi];
                const double mlat = 0.5 * (lat0 + lat1), mlon = 0.5 * (lon0 + lon1);
                if (std::abs(lat_deg - mlat) > 0.003 || std::abs(lon_deg - mlon) > 0.004) continue;
                const double de = (lon1 - lon0) * wf->metres_lon;
                const double dn = (lat1 - lat0) * wf->metres_lat;
                const double len = std::hypot(de, dn);
                if (len < 1.5 || len > 1500.0) continue;
                const float g = static_cast<float>(sample_elevation(mlat, mlon));
                const float yaw = static_cast<float>(std::atan2(-dn, de));
                consider_obb(mlat, mlon, g, fh, static_cast<float>(len), 0.7f, yaw, 1);
            }
        }
    }

    // 4. Discrete Forest & Airfield Trees matching AirfieldVegetation's 10m world-locked grid
    if (!best.hit) {
        auto mix_bits = [](uint32_t x) {
            x ^= x >> 16; x *= 0x7feb352du;
            x ^= x >> 15; x *= 0x846ca68bu;
            return x ^ (x >> 16);
        };
        auto frac01 = [](uint32_t x) { return static_cast<float>(x & 0xFFFFu) * (1.0f / 65536.0f); };
        const double pe = (lon_deg - wf->anchor_lon) * wf->metres_lon;
        const double pn = (lat_deg - wf->anchor_lat) * wf->metres_lat;
        const int32_t ix0 = static_cast<int32_t>(std::floor(pe / 10.0));
        const int32_t iy0 = static_cast<int32_t>(std::floor(pn / 10.0));
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                const int32_t ix = ix0 + dx;
                const int32_t iy = iy0 + dy;
                const uint32_t seed = mix_bits(static_cast<uint32_t>(ix) * 0x9e3779b9u ^
                                               static_cast<uint32_t>(iy) * 0x85ebca6bu);
                const double east = ix * 10.0 + (frac01(seed) - 0.5) * 8.2;
                const double north = iy * 10.0 + (frac01(seed >> 11) - 0.5) * 8.2;
                const double de = pe - east, dn = pn - north;
                const double dist = std::hypot(de, dn);
                if (dist > 4.6 + r) continue;

                const double tlat = wf->anchor_lat + north / wf->metres_lat;
                const double tlon = wf->anchor_lon + east / wf->metres_lon;
                double re = 0, mu = 0, rg = 0;
                std::string rid;
                if (query_runway(tlat, tlon, re, mu, rg, rid) ||
                    is_excluded_for_vegetation(*wf, tlat, tlon) ||
                    is_near_building(*wf, tlat, tlon, 2.2) ||
                    is_near_road(*wf, tlat, tlon, 2.4)) continue;

                const float lidar = sample_canopy_height(tlat, tlon);
                const Vec3 surf = sample_surface(tlat, tlon);
                const float eff_h = (lidar >= 2.4f) ? lidar : (surf.x > 0.45f ? (4.8f + surf.x * 6.0f) : 0.0f);
                if (eff_h < 2.4f || surf.z > 0.22f) continue;

                const uint32_t s2 = mix_bits(seed ^ 0x517cc1b7u);
                const double h = std::clamp(eff_h * (0.86 + frac01(s2) * 0.28), 4.5, 13.5);
                const double g = sample_elevation(tlat, tlon);
                if (alt_msl_m <= g - 0.5 || alt_msl_m >= g + h + r) continue;

                const double inv_d = (dist > 1e-4) ? (1.0 / dist) : 1.0;
                const double crown_base = g + h * 0.42;
                if (alt_msl_m < crown_base) {
                    const double trunk_r = 0.45 + r;
                    if (dist < trunk_r) {
                        const double pen = trunk_r - dist;
                        if (!best.hit || best.kind > 2 || pen > best.penetration_m) {
                            best.hit = true;
                            best.kind = 2;
                            best.normal_e = static_cast<float>(dist > 1e-4 ? de * inv_d : 1.0);
                            best.normal_up = 0.0f;
                            best.normal_n = static_cast<float>(dist > 1e-4 ? dn * inv_d : 0.0);
                            best.penetration_m = static_cast<float>(pen);
                        }
                    }
                } else {
                    const double hfrac = std::clamp((alt_msl_m - crown_base) / std::max(1.0, h * 0.58), 0.0, 1.0);
                    const double crown_r = h * 0.28 * (1.12 - 0.65 * hfrac) + r;
                    if (dist < crown_r) {
                        const double pen = std::min(crown_r - dist, g + h + r - alt_msl_m);
                        if (!best.hit || (best.kind == 3 && pen > best.penetration_m)) {
                            best.hit = true;
                            best.kind = 3;
                            best.normal_e = static_cast<float>(dist > 1e-4 ? de * inv_d * 0.55 : 0.0);
                            best.normal_up = 0.85f;
                            best.normal_n = static_cast<float>(dist > 1e-4 ? dn * inv_d * 0.55 : 0.0);
                            best.penetration_m = static_cast<float>(pen);
                        }
                    }
                }
            }
        }
    }
    return best;
}

} // namespace nsim
