#pragma once

#include "async_streamer.hpp"
#include "gl_loader.hpp"
#include "math3d.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace nsim {

struct CdlodDrawStats {
    int visible_nodes = 0;
    int max_active_lod = 0;
    int prefetch_requests = 0;
    int visible_by_lod[12]{};
};

struct TerrainGrassZone {
    Vec2 center_en{};
    Vec2 axis_en{1.0f, 0.0f};
    float half_length_m = 0.0f;
    float outer_half_width_m = 0.0f;
    float runway_half_width_m = 0.0f;
    bool grass_runway = false;
};

// Continuous Distance-Dependent LOD (CDLOD) Geomorphing Quadtree Terrain Renderer:
// - 1 single glDrawElementsInstanced call for all ~140-260 visible quads across 163.84 km
// - Smooth continuous vertex & parent-child texture geomorphing (ZERO cracks, ZERO ring jumps, ZERO lake seams!)
// - Velocity-predictive + AGL-adaptive LOD prefetching along the aircraft's flight path
// - True 16-layer GL_TEXTURE_2D_ARRAY Northern Spain satellite cascade + Tri-Planar Cliffs + Cliff-Pines + PMRF!
class CdlodTerrain {
public:
    static constexpr int MAX_LOD_DISTANCE_PERCENT = 450;
    static constexpr int PATCH_N = 65; // 65x65 vertices = 64x64 quads per CDLOD leaf (4x sharper ridges & cliffs!)
    static constexpr int MAX_VISIBLE_NODES = 1024;

    CdlodTerrain() = default;
    ~CdlodTerrain();

    bool init(const std::string &asset_root, const std::string &shader_root);
    void shutdown();

    // Select visible CDLOD leaves + queue velocity-predictive pages ahead of the aircraft!
    void update_and_render(
        AsyncTerrainStreamer &streamer,
        const Mat4 &vp,
        const Frustum &frustum,
        double cam_east_m, double cam_north_m, double cam_alt_m, double cam_agl_m,
        double vel_east_mps, double vel_north_mps,
        const Vec3 &sun_dir,
        const Vec3 &haze_cool, const Vec3 &haze_warm,
        float haze_density, float cloud_coverage, float time_sec,
        const Vec2 &wind_en_mps, GLuint refl_env_tex, int quality,
        int season, float snowline_m, int lod_distance_percent = 100,
        const TerrainGrassZone &grass_zone = {},
        const Vec4 &wx_env = {0.0f, 0.0f, 0.0f, 0.0f},
        int terrain_detail = -1, int veg_detail = -1,
        float lod_scale = 1.0f,
        float debug_lod_alpha = 0.0f
    );
    void set_anisotropy(int max_aniso);

    GLuint cloud_noise_tex() const { return cloud_noise_tex_; }
    GLuint cloud_vol_tex() const { return cloud_vol_tex_; }
    GLuint cloud_detail_tex() const { return cloud_detail_tex_; }
    GLuint rock_albedo_tex() const { return rock_albedo_tex_; }
    GLuint rock_normal_tex() const { return rock_normal_tex_; }
    GLuint lut_tex() const { return lut_tex_; }
    CdlodDrawStats last_stats() const { return stats_; }

private:
    struct NodeInstanceGpu {
        float min_e_rel;      // patch SW corner east relative to camera
        float min_n_rel;      // patch SW corner north relative to camera
        float size_m;         // patch world span in meters
        float morph_start;    // distance where geomorph begins (0 -> 1)
        float morph_end;      // distance where geomorph hits 1.0 (matches parent grid!)
        float page_slice;     // child GPU cache slice
        float parent_slice;   // parent GPU cache slice (for seamless morph & fade!)
        float fade_in;        // smooth arrival fade [0..1]
        float parent_uv_off_x;// 0.0 or 0.5
        float parent_uv_off_y;// 0.0 or 0.5
        float world_min_e;    // absolute anchor-relative east (for world-locked satellite/PMRF UVs!)
        float world_min_n;    // absolute anchor-relative north
    };

    void select_node(
        AsyncTerrainStreamer &streamer,
        QuadKey k,
        int parent_slice,
        float parent_uv_x, float parent_uv_y,
        const Frustum &frustum,
        double cam_e, double cam_n, double cam_alt, double cam_agl,
        double pred_e, double pred_n,
        int max_desired_lod,
        float now_sec,
        bool is_prefetch_only,
        int draw_limit
    );

    bool load_sat_array_16(const std::string &atlas_path);
    bool load_cloud_vol_3d(const std::string &atlas_path);
    GLuint load_tex2d(const std::string &path, bool srgb, bool mipmaps, bool repeat = true);
    void build_procedural_cloud_and_micro_tex();

    GLuint prog_ = 0;
    struct TerrainUniforms {
        GLint vp = -1, cam_alt = -1, cam_agl = -1, sun_dir = -1, haze_cool = -1, haze_warm = -1;
        GLint haze_density = -1, cloud_cov = -1, time = -1, detail_strength = -1, wind_en = -1, quality = -1, veg_detail = -1;
        GLint season = -1, snowline_m = -1;
        GLint grass_zone = -1, grass_zone_bounds = -1, wx_env = -1, debug_lod = -1;
    } u_{};
    GLuint vao_ = 0;
    GLuint vbo_ = 0;
    GLuint ebo_ = 0;
    GLuint inst_ssbo_ = 0;
    int index_count_ = 0;

    GLuint sat_array_tex_ = 0; // True 16-layer 512x512 GL_TEXTURE_2D_ARRAY (native mipmaps + repeat!)
    GLuint rock_albedo_tex_ = 0;
    GLuint rock_normal_tex_ = 0;
    GLuint grass_albedo_tex_ = 0;
    GLuint grass_normal_tex_ = 0;
    GLuint gravel_albedo_tex_ = 0;
    GLuint gravel_normal_tex_ = 0;
    GLuint cloud_noise_tex_ = 0;
    GLuint cloud_vol_tex_ = 0;
    GLuint cloud_detail_tex_ = 0;
    GLuint vegetation_volume_tex_ = 0; // Pre-baked tree-turf and ground-grass density volume.
    GLuint vegetation_sss_lut_tex_ = 0; // Pre-baked wrapped-light/thickness foliage scattering lookup.
    GLuint micro_detail_tex_ = 0;
    GLuint lut_tex_ = 0;

    float lod_ranges_[AsyncTerrainStreamer::MAX_LOD + 1]{};
    std::vector<NodeInstanceGpu> visible_;
    CdlodDrawStats stats_{};
};

} // namespace nsim
