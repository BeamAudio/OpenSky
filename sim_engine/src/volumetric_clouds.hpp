#pragma once

#include "gl_loader.hpp"
#include "math3d.hpp"
#include "weather_library.hpp"

#include <ctime>
#include <string>

namespace nsim {

struct AtmosphereState {
    Vec3 sun_dir{0.86f, 0.32f, 0.38f};
    Vec3 haze_cool{0.34f, 0.52f, 0.80f};
    Vec3 haze_warm{1.0f, 0.82f, 0.56f};
    Vec3 cloud_lit{1.0f, 0.96f, 0.90f};
    Vec3 cloud_shadow{0.44f, 0.52f, 0.68f};
    float haze_density = 0.000018f;
    float cloud_cov = 0.44f;
    float cloud_base_m = 2250.0f;
    float cloud_thick_m = 1650.0f;
    float precip = 0.0f;
    float exposure = 1.0f;
    float sun_elev_deg = 19.0f;
    float day_cycle_phase = 0.0f;
    int simulated_day = 1;
    bool is_night = false;
    bool fair_vfr = true;
    std::string hud_wx_line;
};

// Manages:
//  1. Real-time UTC NOAA Solar Ephemeris + Compressed Short-Night VFR Cycle (no Midnight Sun clamp!)
//  2. Reverse-Z Main Scene FBO (GL_RGBA16F + GL_DEPTH_COMPONENT32F, quality-scaled 720p..1440p)
//  3. Quarter-Resolution Temporally Reprojected 3D Volumetric Raymarched Clouds (~0.35ms on iGPU, ZERO 2D sprites!)
//  4. Rayleigh + Mie Sky pass + AgX Filmic Tonemap + 1D Spain LUT Composite!
class SkyAndClouds {
public:
    SkyAndClouds() = default;
    ~SkyAndClouds();

    bool init(const std::string &shader_root, int win_w, int win_h, int quality,
              int pixel_percent = 60, int aa_mode = 2, int weather_detail = 0);
    void shutdown();
    void resize_if_needed(int win_w, int win_h, int quality, int pixel_percent = 60, int aa_mode = 2, int weather_detail = 0);

    void update_solar_and_weather(
        double lat_deg, double lon_deg, double ground_m,
        int tod_slot, double sim_time_sec, float fog_regime_mult = 1.0f, float wx_advance01 = 0.28f,
        const WeatherSnapshot *baked = nullptr, int season_override = -1,
        double day_cycle_elapsed_sec = -1.0
    );
    void reset_temporal_history() { history_valid_ = false; post_history_valid_ = false; prev_camera_valid_ = false; }
    void set_thermal_sparse_clouds(bool sparse) { thermal_sparse_clouds_ = sparse; }
    void update_reflection_map_if_needed(double sim_time_sec);

    // Begin opaque 3D pass into Reverse-Z Main FBO & draw Rayleigh/Mie sky background
    void begin_scene_pass(const Mat4 &inv_vp);
    // Copy terrain-only depth before dynamic scene objects enter the depth buffer.
    // Cloud history must not retain aircraft/car-shaped holes from prior frames.
    void capture_cloud_occlusion_depth();
    // Run 1/4-res temporally reprojected 3D volumetric cloud raymarch + AgX/LUT composite to backbuffer!
    void render_clouds_and_post(
        const Mat4 &vp, const Mat4 &inv_vp,
        const Vec3 &camera_forward,
        double cam_east_m, double cam_north_m, double cam_alt_m,
        GLuint cloud_noise_tex, GLuint cloud_vol_tex, GLuint cloud_detail_tex, GLuint lut_tex,
        GLuint atmo_field_tex, GLuint atmo_terrain_tex, const Vec4 &atmo_domain,
        float time_sec, int frame_idx,
        const Vec4 &wx_fx = {0.0f, 0.0f, 0.0f, 0.0f}
    );
    // Draw a native-resolution cockpit over the reconstructed world image.
    void begin_native_cockpit_pass();
    void present(int x, int y, int width, int height, int window_w, int window_h) const;

    const AtmosphereState &atmo() const { return atmo_; }
    GLuint refl_env_tex() const { return refl_env_tex_; }
    GLuint world_hdr_tex() const { return post_history_tex_[1 - post_ping_]; }
    int scene_w() const { return scene_w_; }
    int scene_h() const { return scene_h_; }
    int cloud_w() const { return cloud_w_; }
    int cloud_h() const { return cloud_h_; }

private:
    void alloc_fbos(int win_w, int win_h, int quality, int pixel_percent, int aa_mode, int weather_detail);
    void free_fbos();

    GLuint sky_prog_ = 0;
    GLuint cloud_prog_ = 0;
    GLuint post_prog_ = 0;
    GLuint refl_prog_ = 0;
    GLuint dummy_vao_ = 0;

    // Cached GL uniform locations (ZERO per-frame glGetUniformLocation string lookups!)
    GLint u_sky_inv_vp_ = -1, u_sky_sun_ = -1, u_sky_cool_ = -1, u_sky_warm_ = -1, u_sky_cov_ = -1;
    GLint u_cld_inv_vp_ = -1, u_cld_prev_vp_ = -1, u_cld_sun_ = -1, u_cld_cam_en_ = -1, u_cld_cam_alt_ = -1;
    GLint u_cld_cam_delta_ = -1;
    GLint u_cld_base_ = -1, u_cld_thick_ = -1, u_cld_cov_ = -1, u_cld_precip_ = -1, u_cld_time_ = -1;
    GLint u_cld_frame_ = -1, u_cld_tblend_ = -1, u_cld_steps_ = -1, u_cld_sparse_ = -1;
    GLint u_cld_lit_ = -1, u_cld_sh_ = -1, u_cld_cool_ = -1, u_cld_warm_ = -1, u_cld_dom_ = -1, u_cld_wx_fx_ = -1;
    GLint u_post_exp_ = -1, u_post_sun_uv_ = -1, u_post_warm_ = -1;
    GLint u_post_inv_vp_ = -1, u_post_prev_vp_ = -1, u_post_camera_delta_ = -1;
    GLint u_post_history_valid_ = -1, u_post_aa_mode_ = -1;
    GLint u_refl_sun_ = -1, u_refl_cool_ = -1, u_refl_warm_ = -1, u_refl_lit_ = -1, u_refl_sh_ = -1, u_refl_cov_ = -1, u_refl_precip_ = -1;

    GLuint scene_fbo_ = 0;
    GLuint scene_col_ = 0;
    GLuint scene_depth_ = 0;
    GLuint cloud_occlusion_depth_ = 0;

    GLuint refl_fbo_ = 0;
    GLuint refl_env_tex_ = 0;
    Vec3 refl_last_sun_{0.0f, -2.0f, 0.0f};
    float refl_last_cov_ = -1.0f;
    float refl_last_precip_ = -1.0f;
    Vec3 refl_last_cool_{-2.0f, 0.0f, 0.0f}, refl_last_warm_{-2.0f, 0.0f, 0.0f};
    Vec3 refl_last_cloud_lit_{-2.0f, 0.0f, 0.0f}, refl_last_cloud_shadow_{-2.0f, 0.0f, 0.0f};
    double refl_last_update_t_ = -1e9;

    GLuint cloud_fbo_[2]{};
    GLuint cloud_tex_[2]{};
    int cloud_ping_ = 0;
    bool history_valid_ = false;
    Mat4 prev_vp_ = Mat4::identity();
    double prev_cam_east_m_ = 0.0, prev_cam_north_m_ = 0.0, prev_cam_alt_m_ = 0.0;
    Vec3 prev_camera_forward_{0.0f, 0.0f, -1.0f};
    bool prev_camera_valid_ = false;
    bool cycle_clock_seeded_ = false;
    double cycle_seed_phase_ = 0.0;
    double cycle_seed_lon_ = 0.0;
    int cycle_seed_doy_ = 1;
    std::time_t cycle_seed_utc_ = 0;

    GLuint post_fbo_[2]{};
    GLuint display_fbo_ = 0;
    GLuint post_display_tex_ = 0;
    GLuint display_depth_ = 0;
    GLuint post_history_tex_[2]{};
    int post_ping_ = 0;
    bool post_history_valid_ = false;

    int scene_w_ = 0, scene_h_ = 0;
    int cloud_w_ = 0, cloud_h_ = 0;
    int cur_quality_ = -1;
    int cur_win_w_ = 0, cur_win_h_ = 0;
    int cloud_quality_ = 2;
    int cur_pixel_percent_ = 60, cur_aa_mode_ = 2;
    bool thermal_sparse_clouds_ = false;

    AtmosphereState atmo_{};
};

} // namespace nsim
