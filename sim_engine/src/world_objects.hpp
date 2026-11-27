#pragma once

#include "gl_loader.hpp"
#include "math3d.hpp"
#include "pack_reader.hpp"
#include "volumetric_clouds.hpp"

#include <cmath>
#include <cstdint>
#include <future>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace nsim {

struct GlbNode {
    std::string name;
    int parent = -1;
    Mat4 base_local = Mat4::identity();
};

struct GlbSubmesh {
    uint32_t first_index = 0;
    uint32_t index_count = 0;
    int32_t base_vertex = 0;
    int node_idx = 0;
    Vec4 base_color{1.0f, 1.0f, 1.0f, 1.0f};
    Vec3 emissive_col{0.0f, 0.0f, 0.0f};
    std::string material_name;
    float metallic = 0.0f;
    float roughness = 0.55f;
    GLuint albedo_tex = 0; // Embedded sRGB GLB albedo texture (0 => bind 1x1 white fallback)
    GLuint normal_tex = 0; // Embedded linear tangent-space normal map (0 => unused)
    GLuint mr_tex = 0;     // Embedded linear ORM/MR map (R=AO, G=Roughness, B=Metallic; 0 => unused)
    bool double_sided = false;
    bool unlit = false;
};

struct GlbMeshData {
    GLuint vao = 0;
    GLuint vbo = 0;
    GLuint ebo = 0;
    std::vector<GLuint> owned_textures;
    std::vector<GlbNode> nodes;
    std::unordered_map<std::string, int> node_index;
    std::vector<GlbSubmesh> submeshes;
    std::vector<float> cpu_verts;
    std::vector<uint32_t> cpu_indices;
};

// GPU-Driven World Objects:
//  1. All 79,372 pre-baked OSM .glb buildings uploaded ONCE to static GPU SSBO -> culled via OpenGL 4.6 Compute Shader
//     into glMultiDrawElementsIndirect (1 single draw call, ~0.10ms, ZERO CPU MultiMesh rebuild stalls!)
//  3. All 17 OurAirports paved/dirt/grass runways with ICAO markings & twilight/Short-Night edge lights!
class WorldObjects {
public:
    WorldObjects() = default;
    ~WorldObjects();

    struct BldgGpu {
        float pos_yaw[4];
        float scale_arch[4];
        float roof_ao[4];
    };
    struct PreparedReanchor {
        std::vector<BldgGpu> gpu_b;
        std::vector<float> rv;
        std::vector<float> lv;
    };
    struct StagedReanchorUpload {
        PreparedReanchor data;
        GLuint buildings = 0;
        GLuint runways = 0;
        GLuint landmarks = 0;
        size_t buildings_uploaded = 0;
        size_t runways_uploaded = 0;
        size_t landmarks_uploaded = 0;
        int phase = 0;
    };

    bool init(const PackReader *pack, const std::string &asset_root, const std::string &shader_root, const std::string &pack_dir = "packs");
    PreparedReanchor prepare_reanchor_cpu(const PackReader::WorldFrame &frame) const;
    void commit_reanchor(PreparedReanchor &&prep);
    void begin_staged_reanchor_upload(PreparedReanchor &&prep);
    void pump_staged_reanchor_upload(size_t byte_budget = 512 * 1024);
    bool staged_reanchor_upload_ready() const { return staged_reanchor_.phase >= 3; }
    void commit_staged_reanchor_upload();
    void cancel_staged_reanchor_upload();
    void on_reanchor();
    void shutdown();

    void update_and_render(
        const Mat4 &vp, const Frustum &frustum,
        double cam_east_m, double cam_north_m, double cam_alt_m,
        const AtmosphereState &atmo, float time_sec, const Vec2 &wind_en_mps, GLuint refl_env_tex, int quality,
        int max_new_tree_cells = 1, int world_detail = -1,
        float dist_scale = 1.0f, int road_quality = -1
    );
    void cancel_road_stream();
    void set_anisotropy(int max_aniso);

    // Load a colored .glb (used by AircraftHud for C172S exterior & cockpit!)
    static bool load_colored_glb(const std::string &path, GlbMeshData &out, bool keep_cpu_geom = false);

    int total_buildings() const { return total_bldgs_ + baked_city_bldgs_; }
    int active_cars() const { return active_cars_; }
    int active_city_regions() const {
        int n = 0;
        for (const auto &cr : city_regions_) if (cr.gpu_streamed) ++n;
        return n;
    }
    int streamed_city_chunks() const {
        int n = 0;
        for (const auto &cr : city_regions_) if (cr.gpu_streamed) n += static_cast<int>(cr.chunks.size());
        return n;
    }

private:
    struct DrawCmd {
        uint32_t count;
        uint32_t instanceCount;
        uint32_t firstIndex;
        int32_t baseVertex;
        uint32_t baseInstance;
    };
    struct RoadVert {
        float pos[3];
        float uv[2];
        float kind;
        uint32_t bake_u32;
    };
    struct RoadSeg {
        float e0, y0, n0;
        float e1, y1, n1;
        float len, yaw, lane_off;
        uint8_t kind;
        uint32_t node_a = 0, node_b = 0;
    };
    struct CarGpu {
        float pos_yaw[4];
        float scale_col[4];
    };
    struct TrafficAgent {
        std::vector<uint32_t> route; // Individual acyclic sequence of (seg_idx << 1 | dir_bit) edges.
        uint16_t step = 0;
        float dist_m = 0.0f;
        float cur_speed_mps = 10.0f;
        float speed_scale = 1.0f;
        float cur_yaw = 0.0f;
        float w = 1.42f, h = 1.02f, l = 3.25f;
        float col_id = 0.0f;
        uint32_t rng = 1u;
    };
    struct CityChunkInfo {
        uint32_t is_urban = 0; // 1 = dense urban chunk (>=25 buildings)
        uint32_t first_index = 0;
        uint32_t index_count = 0;
        int32_t base_vertex = 0;
        float ce = 0.0f, cy = 0.0f, cn = 0.0f, rad = 0.0f;
    };
    struct CityChunkDiskMeta {
        uint32_t is_urban = 0;
        uint32_t first_index = 0;
        uint32_t index_count = 0;
        uint32_t base_vertex = 0;
        uint32_t vert_count = 0;
        float ce = 0.0f, cy = 0.0f, cn = 0.0f, rad = 0.0f;
    };
    struct CityRegionGpu {
        std::string region_id, city_path;
        double lat_min = 0.0, lon_min = 0.0, lat_max = 0.0, lon_max = 0.0;
        double anchor_lat = 42.10, anchor_lon = -0.30, m_lat = 111132.0, m_lon = 82600.0;
        float off_e = 0.0f, off_n = 0.0f, scl_e = 1.0f, scl_n = 1.0f;
        int bldgs = 0;
        bool meta_loaded = false;
        uint64_t vert_file_off = 0, idx_file_off = 0;
        std::vector<CityChunkDiskMeta> disk_chunks;
        bool gpu_streamed = false;
        double stream_le = 1e18, stream_ln = 1e18;
        double stream_retry_time = 0.0;
        GLuint vao = 0, vbo = 0, ebo = 0;
        std::vector<CityChunkInfo> chunks;
        std::unordered_map<int64_t, std::vector<uint32_t>> macro_grid;
        std::unordered_map<int64_t, uint8_t> dense_cells;
    };
    struct CityRegionData {
        std::vector<uint8_t> vertices;
        std::vector<uint32_t> indices;
        std::vector<CityChunkInfo> chunks;
        std::unordered_map<int64_t, std::vector<uint32_t>> macro_grid;
    };
    struct CityStream {
        CityRegionGpu *region = nullptr;
        double le = 0.0, ln = 0.0;
        std::future<std::optional<CityRegionData>> job;
        CityRegionData data;
        GLuint vbo = 0, ebo = 0;
        size_t uploaded = 0;
    };
    struct PreparedRoads {
        std::vector<RoadVert> rv;
        std::vector<RoadSeg> segs;
        std::vector<std::vector<uint32_t>> adj;
        std::vector<TrafficAgent> agents;
        double ce = 0.0, cn = 0.0;
        int q = -1;
    };
    struct RoadStream {
        bool pending = false;
        double ce = 0.0, cn = 0.0;
        int q = -1;
        std::future<PreparedRoads> job;
    };

    static constexpr int MAX_PER_ARCH = 16000;
    static constexpr int MAX_LOD1 = 65536;
    static constexpr int MAX_ROAD_VERTS = 180000;
    static constexpr int MAX_CARS = 900;

    bool load_textured_glb_append(
        const std::string &path,
        std::vector<float> &verts, std::vector<uint32_t> &indices,
        DrawCmd &out_cmd
    );
    GLuint load_tex2d(const std::string &path, bool srgb, bool repeat = true);
    bool load_baked_cities(const std::string &pack_dir);
    bool load_city_region_meta(CityRegionGpu &cr);
    static std::optional<CityRegionData> read_city_region(const CityRegionGpu &cr, double cam_le, double cam_ln);
    void publish_city_region(CityRegionGpu &cr, CityRegionData &&data, double le, double ln, GLuint vbo, GLuint ebo);
    void reset_city_stream();
    void unload_city_region_gpu(CityRegionGpu &cr);
    void update_city_region_transforms();
    void ensure_city_regions_for_camera(double cam_east_m, double cam_north_m, bool preload = false);
    bool is_dense_urban_en(double e, double n) const;
    std::vector<BldgGpu> build_buildings_cpu(const PackReader::WorldFrame &frame) const;
    std::vector<float> build_runways_cpu(const PackReader::WorldFrame &frame) const;
    std::vector<float> build_landmarks_cpu(const PackReader::WorldFrame &frame) const;
    void upload_buildings_gpu(const std::vector<BldgGpu> &gpu_b);
    void upload_runways_gpu(const std::vector<float> &rv);
    void upload_landmarks_gpu(const std::vector<float> &lv);
    GLuint create_runway_vao(GLuint vbo);
    GLuint create_landmark_vao(GLuint vbo);
    void build_car_mesh();
    static PreparedRoads build_roads_cpu(const PackReader *pack, std::shared_ptr<const PackReader::WorldFrame> frame,
                                         double cam_e, double cam_n, int quality);
    static void plan_vehicle_route_on(const std::vector<RoadSeg> &segs, const std::vector<std::vector<uint32_t>> &adj,
                                      TrafficAgent &ag, bool random_progress);
    void rebuild_local_roads_and_traffic(double cam_e, double cam_n, int quality);
    void plan_vehicle_route(TrafficAgent &ag, bool random_progress);

    const PackReader *pack_ = nullptr;
    StagedReanchorUpload staged_reanchor_{};

    // GPU-driven OSM buildings + 1-call GPU-instanced directional ground shadows & contact AO
    GLuint cull_comp_ = 0;
    GLuint bldg_prog_ = 0;
    GLuint bldg_vao_ = 0, bldg_vbo_ = 0, bldg_ebo_ = 0;
    GLuint bldg_all_ssbo_ = 0, bldg_cmd_ssbo_ = 0, bldg_vis_ssbo_ = 0;
    DrawCmd bldg_base_cmds_[6]{};
    GLuint bldg_sh_prog_ = 0;
    GLuint bldg_sh_vao_ = 0, bldg_sh_vbo_ = 0, bldg_sh_ebo_ = 0, bldg_sh_cmd_ssbo_ = 0;
    DrawCmd bldg_sh_base_cmds_[6]{};
    GLuint bldg_lod1_prog_ = 0;
    GLuint bldg_lod1_vao_ = 0, bldg_lod1_vbo_ = 0, bldg_lod1_ebo_ = 0;
    GLuint bldg_lod1_cmd_ssbo_ = 0, bldg_lod1_vis_ssbo_ = 0;
    DrawCmd bldg_lod1_base_cmd_{};
    int total_bldgs_ = 0;
    int baked_city_bldgs_ = 0;
    GLuint bldg_alb_ = 0, bldg_nrm_ = 0, bldg_orm_ = 0, interior_tex_ = 0;

    // Offline Map-Wide Pre-Baked Exact-Polygon Populated Zones (streamed per region across ALL Spain!)
    GLuint city_prog_ = 0;
    GLuint city_far_prog_ = 0;
    std::vector<CityRegionGpu> city_regions_;
    CityStream city_stream_;
    GLuint mtn_gi_tex_ = 0; // 512x384 RG8: R=50km mountain 10-TOD shadow, G=valley Sky AO (updated only on TOD shift!)
    float mtn_gi_phase_ = -10.0f;
    std::vector<uint8_t> mtn_gi_cpu_;
    bool baked_map_wide_ = false;
    GLint u_city_vp_ = -1, u_city_cam_ = -1, u_city_sun_ = -1, u_city_cool_ = -1, u_city_warm_ = -1, u_city_hd_ = -1, u_city_alt_ = -1, u_city_ph_ = -1, u_city_mb_ = -1, u_city_roff_ = -1, u_city_rscl_ = -1;
    GLint u_cfar_vp_ = -1, u_cfar_cam_ = -1, u_cfar_sun_ = -1, u_cfar_cool_ = -1, u_cfar_warm_ = -1, u_cfar_hd_ = -1, u_cfar_alt_ = -1, u_cfar_fs_ = -1, u_cfar_fe_ = -1, u_cfar_ph_ = -1, u_cfar_mb_ = -1, u_cfar_roff_ = -1, u_cfar_rscl_ = -1;

    // Streamed real OpenStreetMap streets & avenues + moving 3D PBR traffic!
    GLuint road_prog_ = 0;
    GLuint road_vao_ = 0, road_vbo_ = 0;
    int road_vert_count_ = 0;
    double road_center_e_ = 1e12, road_center_n_ = 1e12;
    int road_last_q_ = -1;
    std::vector<RoadSeg> active_segs_;
    std::vector<std::vector<uint32_t>> road_adj_;
    RoadStream road_stream_;
    float prev_traffic_t_ = -1.0f;

    GLuint traffic_prog_ = 0;
    GLuint car_vao_ = 0, car_vbo_ = 0, car_ebo_ = 0, car_ssbo_ = 0;
    int car_idx_count_ = 0;
    std::vector<TrafficAgent> agents_;
    std::vector<CarGpu> car_gpu_buf_;
    int active_cars_ = 0;

    // Iconic Northern Spain Hero Landmarks (Basilica del Pilar, Puente de Piedra, Torre del Agua, Catedral Huesca, Loarre, Mediano!)
    GLuint landmark_prog_ = 0;
    GLuint lm_vao_ = 0, lm_vbo_ = 0;
    int lm_vert_count_ = 0;

    // OurAirports runways (full Cook-Torrance GGX PBR with normal maps!)
    GLuint rwy_prog_ = 0;
    GLuint rwy_vao_ = 0, rwy_vbo_ = 0;
    int rwy_vert_count_ = 0;
    GLuint asph_tex_ = 0, grav_tex_ = 0, asph_nrm_ = 0, grav_nrm_ = 0;

    // Cached GL uniform locations (ZERO per-frame glGetUniformLocation!)
    GLint u_rwy_vp_ = -1, u_rwy_cam_ = -1, u_rwy_sun_ = -1, u_rwy_cool_ = -1, u_rwy_warm_ = -1, u_rwy_hd_ = -1;
    GLint u_road_vp_ = -1, u_road_cam_ = -1, u_road_sun_ = -1, u_road_cool_ = -1, u_road_warm_ = -1, u_road_hd_ = -1, u_road_alt_ = -1, u_road_ph_ = -1;
    GLint u_car_vp_ = -1, u_car_cam_ = -1, u_car_sun_ = -1, u_car_cool_ = -1, u_car_warm_ = -1, u_car_hd_ = -1, u_car_alt_ = -1;
    GLint u_lm_vp_ = -1, u_lm_cam_ = -1, u_lm_sun_ = -1, u_lm_cool_ = -1, u_lm_warm_ = -1, u_lm_hd_ = -1, u_lm_alt_ = -1;
    GLint u_cull_tot_ = -1, u_cull_max_ = -1, u_cull_max_l1_ = -1, u_cull_cam_ = -1, u_cull_dist_ = -1, u_cull_far_ = -1, u_cull_frust_ = -1;
    GLint u_bldg_vp_ = -1, u_bldg_cam_ = -1, u_bldg_sun_ = -1, u_bldg_cool_ = -1, u_bldg_warm_ = -1, u_bldg_hd_ = -1, u_bldg_alt_ = -1, u_bldg_fs_ = -1, u_bldg_fe_ = -1;
    GLint u_bsh_vp_ = -1, u_bsh_cam_ = -1, u_bsh_sun_ = -1, u_bsh_fs_ = -1, u_bsh_fe_ = -1;
    GLint u_bl1_vp_ = -1, u_bl1_cam_ = -1, u_bl1_sun_ = -1, u_bl1_cool_ = -1, u_bl1_warm_ = -1, u_bl1_hd_ = -1, u_bl1_alt_ = -1, u_bl1_is_ = -1, u_bl1_ie_ = -1, u_bl1_fs_ = -1, u_bl1_fe_ = -1;

};

} // namespace nsim
