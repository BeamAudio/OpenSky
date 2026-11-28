#pragma once

#include "../../engine/src/geo_math.hpp"
#include "../../protocol/pack_format.h"
#include "math3d.hpp"
#include "platform_io.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <fstream>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace nsim {

enum class OnlineOrthoProvider : int {
    OFF  = 0,
    PNOA = 1 // IGN Spain PNOA Maxima Actualidad WMTS (CC-BY 4.0, Z14..Z20 distance tiers)
};

inline int online_zoom_for_distance_m(double dist_m) {
    if (dist_m <= 200.0) return 20;
    if (dist_m <= 500.0) return 19;
    if (dist_m <= 2000.0) return 18;
    if (dist_m <= 4000.0) return 17;
    if (dist_m <= 8000.0) return 16;
    if (dist_m <= 12000.0) return 15;
    if (dist_m <= 28000.0) return 14;
    if (dist_m <= 48000.0) return 13;
    return 0;
}

struct PageImagery {
    uint64_t source_revision = 0;
    int target_zoom = 0;
    int target_tiles_ready = 0;
    int target_tiles_missing = 0;
};

struct AirportRunway {
    double lat_a = 42.0, lon_a = -0.3, elev_a_m = 400.0;
    double lat_b = 42.005, lon_b = -0.3, elev_b_m = 400.0;
    double width_m = 24.0;
    std::string surface = "unknown";
};

struct AirportGroundFeature {
    enum class Kind : uint8_t { Taxiway, Apron, AccessRoad };
    Kind kind = Kind::Taxiway;
    double width_m = 15.0;
    std::string surface = "unknown";
    std::vector<std::pair<double, double>> points_lat_lon;
};

struct AirportFence {
    std::string kind = "fence"; // fence | wall | hedge
    std::vector<std::pair<double, double>> points_lat_lon;
};

struct ParkingStand {
    double lat = 0.0;
    double lon = 0.0;
    double elev_m = 0.0;
    double heading_deg = 0.0;
    bool paved_apron = false;
};

struct AirportInfo {
    std::string id;
    std::string name;
    std::string kind = "small_airport";
    std::string surface = "unknown";
    std::string freq_type = "CTAF";
    std::string region_id;
    double lat = 42.0;
    double lon = -0.3;
    double elev_m = 400.0;
    double rwy_lat_a = 42.0, rwy_lon_a = -0.3, rwy_elev_a = 400.0;
    double rwy_lat_b = 42.005, rwy_lon_b = -0.3, rwy_elev_b = 400.0;
    double rwy_width_m = 24.0;
    double freq_mhz = 123.5;
    std::vector<AirportRunway> runways;
    std::vector<AirportGroundFeature> ground_features;
    std::vector<AirportFence> fences;
    std::vector<std::pair<double, double>> windsocks;
};

struct BuildingRec {
    double lat = 0.0;
    double lon = 0.0;
    float width_m = 10.0f;
    float depth_m = 8.0f;
    float height_m = 7.0f;
    float yaw_rad = 0.0f;
    float ground_m = 0.0f;
    uint8_t archetype = 0;
    uint8_t color_var = 0;
    uint8_t roof_r = 148;
    uint8_t roof_g = 124;
    uint8_t roof_b = 104;
    uint8_t canyon_ao = 210;
    bool airport_feature = false; // Imported aeroway building footprint; keep visible over city bake.
};

struct RoadRec {
    uint32_t offset = 0;
    uint16_t count = 0;
    uint8_t kind = 2; // 0=motorway/highway, 1=primary/avenue, 2=secondary/street, 3=residential/track
};

struct RoadPt {
    int32_t lat_e6 = 0;
    int32_t lon_e6 = 0;
    float ground_m = 0.0f;
};

struct DecodedElevTile {
    uint16_t width = 0;
    uint16_t height = 0;
    std::vector<float> heights;
};

struct DecodedRgbTile {
    uint16_t width = 0;
    uint16_t height = 0;
    std::vector<uint8_t> rgb;
};

// Thread-safe pure-C++20 reader for .nspack (IGN PNOA/MDT05/Hydrography/OurAirports) + .nsscn v2 (79,372 OSM buildings).
class PackReader {
public:
    struct CityShEntry { uint32_t offset = 0; uint32_t comp_size = 0; };
    struct LoadedRegion {
        uint32_t idx = 0;
        std::string region_id;
        std::string stem_base;
        std::string pack_path;
        std::string city_path;
        std::string scenery_path;
        std::string weather_path;
        nspain::PackHeader hdr{};
        FileHandle pack_fd = kInvalidFileHandle;
        double anchor_lat = 42.10, anchor_lon = -0.30;
        double m_lat = 111132.0, m_lon = 82600.0;
        FileHandle sh_fd = kInvalidFileHandle;
        int sh_res = 128;
        float sh_chunk_m = 500.0f;
        std::unordered_map<int64_t, CityShEntry> sh_idx;
    };

    struct WorldFrame {
        double anchor_lat = 42.10, anchor_lon = -0.30;
        double metres_lat = 111132.0, metres_lon = 82600.0;
        double min_e = 0.0, min_n = 0.0, span_e = 1.0, span_n = 1.0;
        std::vector<float> dem;
        std::vector<uint32_t> lighting;
    };

    // Thread-safe atomic capture for workers + async transition staging.
    std::shared_ptr<const WorldFrame> world_frame() const {
        return world_frame_atomic_.load(std::memory_order_acquire);
    }
    std::shared_ptr<const WorldFrame> prepare_world_frame(double lat_deg, double lon_deg, bool snap_to_region = true) const;
    void commit_world_frame(std::shared_ptr<const WorldFrame> frame);

    PackReader() = default;
    ~PackReader();

    bool load_pack(const std::string &nspack_path);
    bool load_packs_dir(const std::string &packs_dir, const std::string &only_region_id = "", const std::string &prefer_region_id = "");
    bool load_packs_dirs(const std::vector<std::string> &packs_dirs, const std::string &only_region_id = "", const std::string &prefer_region_id = "");
    bool load_scenery(const std::string &nsscn_path, bool append = false);
    bool load_city_shadows(const std::string &nsshd_path, uint32_t region_idx = 0);
    int city_shadow_tile_count() const;

    bool reanchor_to(double lat_deg, double lon_deg);
    const LoadedRegion *find_region_at(double lat_deg, double lon_deg) const;
    const LoadedRegion *find_region_by_id(const std::string &region_id) const;
    const std::deque<LoadedRegion> &regions() const { return regions_; }
    std::string active_weather_path(double lat_deg, double lon_deg) const;

    double anchor_lat() const { return world_frame_->anchor_lat; }
    double anchor_lon() const { return world_frame_->anchor_lon; }
    double anchor_m_lat() const { return world_frame_->metres_lat; }
    double anchor_m_lon() const { return world_frame_->metres_lon; }
    double lon_min() const { return header_.lon_min; }
    double lat_min() const { return header_.lat_min; }
    double lon_max() const { return header_.lon_max; }
    double lat_max() const { return header_.lat_max; }
    int tile_count() const { return static_cast<int>(index_.size()); }

    const std::vector<AirportInfo> &airports() const { return airports_; }
    const std::vector<BuildingRec> &buildings() const { return buildings_; }
    const std::vector<RoadRec> &roads() const { return roads_; }
    const std::vector<RoadPt> &road_pts() const { return road_pts_; }
    // Query OSM road indices near (lat, lon) within radius_deg using the pre-built spatial hash grid
    void query_roads_in_box(double min_lat, double max_lat, double min_lon, double max_lon, std::vector<uint32_t> &out_indices) const;
    const AirportInfo *find_airport(const std::string &id) const;
    const AirportInfo *find_nearest_airport(double lat_deg, double lon_deg, double *out_dist_m = nullptr) const;
    std::vector<ParkingStand> compute_parking_stands(const AirportInfo &ap, int max_stands = 6) const;
    bool is_excluded_for_vegetation(double lat_deg, double lon_deg) const;
    bool is_excluded_for_vegetation(const WorldFrame &frame, double lat_deg, double lon_deg) const;
    bool is_near_building(double lat_deg, double lon_deg, double pad_m = 4.5) const;
    bool is_near_building(const WorldFrame &frame, double lat_deg, double lon_deg, double pad_m = 4.5) const;
    bool is_near_road(double lat_deg, double lon_deg, double pad_m = 8.5) const;
    bool is_near_road(const WorldFrame &frame, double lat_deg, double lon_deg, double pad_m = 8.5) const;
    // Fast O(1) cell-local rasterized occupancy mask (bit0=building+pad, bit1=road+pad) for tree placement!
    void fill_cell_occupancy(double min_e, double min_n, double span_m, int res, uint8_t *out_mask,
                             bool include_verges = true) const;

    // Thread-safe sampling (callable from async worker threads & flight physics simultaneously!)
    double sample_elevation(double lat_deg, double lon_deg) const;
    void fill_elevation_grid(const WorldFrame &frame, double center_east_m, double center_north_m,
                            double half_span_m, int width, int height,
                            std::vector<float> &out_height) const;
    float sample_canopy_height(double lat_deg, double lon_deg) const;
    Vec3 sample_surface(double lat_deg, double lon_deg) const;
    Vec3 sample_imagery_rgb(double lat_deg, double lon_deg) const;

    // Fill a terrain page covering [center_e - half_span .. +half_span, center_n - half_span .. +half_span]
    // Height grid has border gutter (grid_n = 35 for 33x33 vertices); ortho has Kramer-Bruckner shock de-blurring!
    void fill_page_data(
        const WorldFrame &frame,
        double center_east_m, double center_north_m, double half_span_m,
        int height_n, int ortho_res, int aux_res,
        const Vec3 &sun_dir,
        std::vector<float> &out_height,
        std::vector<uint8_t> &out_ortho_rgba,
        std::vector<uint8_t> &out_norm_ao_rgba,
        std::vector<uint8_t> &out_surf_rgba,
        PageImagery *out_imagery = nullptr
    ) const;

    // Pre-bake whole-corridor 50km planetary DEM 10-time-of-day horizon shadows + 8-dir Valley Sky AO (uint32 packed!)
    uint32_t sample_baked_gi_u32_en(double east_m, double north_m) const;
    uint32_t sample_baked_gi_u32_en(const WorldFrame &frame, double east_m, double north_m) const;
    void fill_corridor_gi_rg8(float phase10, std::vector<uint8_t> &out_rg) const;
    const std::vector<uint32_t> &corridor_gi_u32() const { return world_frame_->lighting; }

    // Splatters real OSM building footprints & heights into [0..255] alpha mask for PMRF far villages
    void rasterize_building_mask(
        double center_lat, double center_lon, double half_span_m,
        int res, std::vector<uint8_t> &out_alpha
    ) const;

    // Read-only access to the baked 512x384 corridor DEM (shared by RegionalAtmosphere mesoscale wind/wave solver!)
    int corridor_dem_w() const { return SH_W; }
    int corridor_dem_h() const { return SH_H; }
    const std::vector<float> &corridor_dem() const { return world_frame_->dem; }
    double corridor_min_e() const { return world_frame_->min_e; }
    double corridor_min_n() const { return world_frame_->min_n; }
    double corridor_span_e() const { return world_frame_->span_e; }
    double corridor_span_n() const { return world_frame_->span_n; }

    // Check if aircraft wheel is on any OurAirports runway (returns runway elevation + friction mu)
    bool query_runway(double lat_deg, double lon_deg, double &out_elev_m, double &out_mu, double &out_rough, std::string &out_id) const;

    struct ObjectCollisionHit {
        bool hit = false;
        uint8_t kind = 0; // 0=building, 1=landmark/bridge/wall, 2=tree_trunk, 3=tree_canopy
        float normal_e = 0.0f, normal_up = 0.0f, normal_n = 0.0f;
        float penetration_m = 0.0f;
    };
    ObjectCollisionHit query_object_collision(double lat_deg, double lon_deg, double alt_msl_m, double probe_r_m) const;

    // Live online PNOA ortho streaming: bounded RAM LRU + async disk-backed downloads, no worker stalls.
    void set_online_provider(OnlineOrthoProvider p);
    OnlineOrthoProvider online_provider() const { return online_provider_.load(std::memory_order_relaxed); }
    const char *online_provider_name() const;
    int online_tiles_fetched() const { return online_fetched_count_.load(std::memory_order_relaxed); }
    int online_tiles_used() const { return online_used_count_.load(std::memory_order_relaxed); }
    uint64_t online_revision() const { return online_revision_.load(std::memory_order_acquire); }
    int online_page_zoom(double lat, double lon, double half_span_m, int ortho_res) const;
    void set_online_camera(double lat_deg, double lon_deg);
    // Point ahead of the aircraft along its velocity; PNOA downloads are ranked by the nearer of camera/lookahead.
    void set_online_lookahead(double lat_deg, double lon_deg, bool valid);
    void prefetch_online_box(double center_lat, double center_lon, double half_span_m, int zoom = 17) const;

private:
    struct IndexedTile {
        uint64_t key = 0;
        uint32_t pack_idx = 0;
        nspain::PackTileEntry entry{};
    };

    void bake_corridor_dem(WorldFrame &frame) const;
    static float sample_global_shadow(const WorldFrame &frame, double east_m, double north_m, float phase10);
    void fill_cell_occupancy(const WorldFrame &frame, double min_e, double min_n,
                             double span_m, int res, uint8_t *out_mask, bool include_verges) const;
    void clear_regions();
    bool append_pack_internal(const std::string &nspack_path);
    void finalize_packs(double anchor_lat, double anchor_lon);
    const IndexedTile *find_entry(uint64_t key) const;
    std::shared_ptr<DecodedElevTile> get_elev_tile(int z, int x, int y) const;
    std::shared_ptr<DecodedRgbTile> get_rgb_tile(uint8_t layer, int z, int x, int y) const;
    std::shared_ptr<DecodedRgbTile> get_online_rgb_tile(OnlineOrthoProvider prov, int z, int x, int y,
                                                        bool queue_download = true) const;
    bool download_online_tile_to_disk(OnlineOrthoProvider prov, int z, int x, int y, const std::string &cache_path) const;
    void online_dl_worker() const;
    const LoadedRegion *find_sh_region_at_ll(double lat_deg, double lon_deg) const;
    std::shared_ptr<std::vector<uint32_t>> get_city_sh_tile(const LoadedRegion &reg, int32_t cx, int32_t cy) const;
    void parse_airports_from_manifest(const std::string &json, const std::string &region_id);

    static constexpr double BLDG_CELL_DEG = 0.025;
    static int64_t pack_cell(int32_t cx, int32_t cy) {
        return (static_cast<int64_t>(cx) << 32) | static_cast<uint32_t>(cy);
    }

    template <typename T>
    struct SyncLru {
        size_t cap = 512;
        mutable std::mutex mtx;
        std::list<uint64_t> order;
        std::unordered_map<uint64_t, std::pair<std::shared_ptr<T>, std::list<uint64_t>::iterator>> map;
        explicit SyncLru(size_t c) : cap(c) {}

        std::shared_ptr<T> get(uint64_t key) {
            std::lock_guard<std::mutex> lk(mtx);
            auto it = map.find(key);
            if (it == map.end()) return nullptr;
            order.splice(order.begin(), order, it->second.second);
            return it->second.first;
        }
        void put(uint64_t key, std::shared_ptr<T> val) {
            std::lock_guard<std::mutex> lk(mtx);
            auto it = map.find(key);
            if (it != map.end()) {
                it->second.first = std::move(val);
                order.splice(order.begin(), order, it->second.second);
                return;
            }
            order.push_front(key);
            map[key] = {std::move(val), order.begin()};
            while (map.size() > cap) {
                uint64_t old = order.back();
                order.pop_back();
                map.erase(old);
            }
        }
    };

    std::deque<LoadedRegion> regions_;
    nspain::PackHeader header_{};
    std::vector<IndexedTile> index_;
    std::vector<int> elev_zooms_desc_;
    std::vector<int> img_zooms_desc_;
    std::vector<int> surf_zooms_desc_;
    std::vector<int> vegetation_zooms_desc_;
    std::shared_ptr<const WorldFrame> world_frame_ = std::make_shared<const WorldFrame>();
    std::atomic<std::shared_ptr<const WorldFrame>> world_frame_atomic_{world_frame_};

    mutable SyncLru<DecodedElevTile> elev_cache_{1024};
    mutable SyncLru<DecodedRgbTile> img_cache_{1024};
    mutable SyncLru<DecodedRgbTile> online_cache_{512};
    mutable SyncLru<DecodedRgbTile> surf_cache_{768};

    // 50km corridor DEM 10-time-of-day horizon shadow + Valley Sky AO table (uint32 packed!)
    static constexpr int SH_W = 512;
    static constexpr int SH_H = 384;
    std::vector<AirportInfo> airports_;
    std::vector<BuildingRec> buildings_;
    std::unordered_map<int64_t, std::vector<uint32_t>> bldg_grid_;
    std::vector<RoadRec> roads_;
    std::vector<RoadPt> road_pts_;
    std::unordered_map<int64_t, std::vector<uint32_t>> road_grid_;

    mutable SyncLru<std::vector<uint32_t>> city_sh_cache_{2048};

    // Distance-tiered PNOA Z14..Z20 async streamer: bounded RAM LRU + persistent disk cache.
    mutable std::atomic<OnlineOrthoProvider> online_provider_{OnlineOrthoProvider::OFF};
    mutable std::atomic<int> online_fetched_count_{0};
    mutable std::atomic<int> online_used_count_{0};
    mutable std::atomic<uint64_t> online_revision_{0};
    mutable std::atomic<bool> online_stop_{false};
    mutable std::mutex online_mtx_;
    mutable std::condition_variable online_cv_;
    mutable std::vector<uint64_t> online_dl_queue_;
    mutable std::unordered_set<uint64_t> online_dl_seen_;
    mutable std::unordered_map<uint64_t, double> online_failed_until_;
    mutable std::vector<std::thread> online_workers_;
    mutable std::atomic<uint64_t> online_cam_lat_bits_{0};
    mutable std::atomic<uint64_t> online_cam_lon_bits_{0};
    mutable std::atomic<uint64_t> online_ahead_lat_bits_{0};
    mutable std::atomic<uint64_t> online_ahead_lon_bits_{0};
    mutable std::atomic<bool> online_ahead_valid_{false};
};

} // namespace nsim
