#include "volumetric_clouds.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

namespace nsim {

namespace {
inline float smooth01(float lo, float hi, float v) {
    float t = std::clamp((v - lo) / std::max(1e-5f, hi - lo), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
}

SkyAndClouds::~SkyAndClouds() {
    shutdown();
}

bool SkyAndClouds::init(const std::string &shader_root, int win_w, int win_h, int quality,
                        int pixel_percent, int aa_mode, int weather_detail) {
    sky_prog_   = compile_program_files(shader_root + "/fullscreen.vert", shader_root + "/sky.frag");
    cloud_prog_ = compile_program_files(shader_root + "/fullscreen.vert", shader_root + "/clouds_qres.frag");
    post_prog_  = compile_program_files(shader_root + "/fullscreen.vert", shader_root + "/post_tonemap.frag");
    refl_prog_  = compile_program_files(shader_root + "/fullscreen.vert", shader_root + "/env_reflect.frag");
    if (!sky_prog_ || !cloud_prog_ || !post_prog_ || !refl_prog_) return false;
    u_sky_inv_vp_  = glGetUniformLocation(sky_prog_, "u_inv_vp");
    u_sky_sun_     = glGetUniformLocation(sky_prog_, "u_sun_dir");
    u_sky_cool_    = glGetUniformLocation(sky_prog_, "u_haze_cool");
    u_sky_warm_    = glGetUniformLocation(sky_prog_, "u_haze_warm");
    u_sky_cov_     = glGetUniformLocation(sky_prog_, "u_cloud_cov");
    u_cld_inv_vp_  = glGetUniformLocation(cloud_prog_, "u_inv_vp");
    u_cld_prev_vp_ = glGetUniformLocation(cloud_prog_, "u_prev_vp");
    u_cld_cam_delta_ = glGetUniformLocation(cloud_prog_, "u_camera_delta");
    u_cld_sun_     = glGetUniformLocation(cloud_prog_, "u_sun_dir");
    u_cld_cam_en_  = glGetUniformLocation(cloud_prog_, "u_cam_en");
    u_cld_cam_alt_ = glGetUniformLocation(cloud_prog_, "u_cam_alt_m");
    u_cld_base_    = glGetUniformLocation(cloud_prog_, "u_cloud_base_m");
    u_cld_thick_   = glGetUniformLocation(cloud_prog_, "u_cloud_thick_m");
    u_cld_cov_     = glGetUniformLocation(cloud_prog_, "u_coverage");
    u_cld_precip_  = glGetUniformLocation(cloud_prog_, "u_precip");
    u_cld_time_    = glGetUniformLocation(cloud_prog_, "u_time");
    u_cld_frame_   = glGetUniformLocation(cloud_prog_, "u_frame_idx");
    u_cld_tblend_  = glGetUniformLocation(cloud_prog_, "u_temporal_blend");
    u_cld_steps_   = glGetUniformLocation(cloud_prog_, "u_steps");
    u_cld_sparse_  = glGetUniformLocation(cloud_prog_, "u_sparse_period");
    u_cld_lit_     = glGetUniformLocation(cloud_prog_, "u_cloud_lit");
    u_cld_sh_      = glGetUniformLocation(cloud_prog_, "u_cloud_shadow");
    u_cld_cool_    = glGetUniformLocation(cloud_prog_, "u_haze_cool");
    u_cld_warm_    = glGetUniformLocation(cloud_prog_, "u_haze_warm");
    u_cld_dom_     = glGetUniformLocation(cloud_prog_, "u_atmo_domain");
    u_cld_wx_fx_   = glGetUniformLocation(cloud_prog_, "u_wx_fx");
    u_post_exp_    = glGetUniformLocation(post_prog_, "u_exposure");
    u_post_sun_uv_ = glGetUniformLocation(post_prog_, "u_sun_uv_vis");
    u_post_warm_   = glGetUniformLocation(post_prog_, "u_haze_warm");
    u_post_inv_vp_ = glGetUniformLocation(post_prog_, "u_inv_vp");
    u_post_prev_vp_ = glGetUniformLocation(post_prog_, "u_prev_vp");
    u_post_camera_delta_ = glGetUniformLocation(post_prog_, "u_camera_delta");
    u_post_history_valid_ = glGetUniformLocation(post_prog_, "u_history_valid");
    u_post_aa_mode_ = glGetUniformLocation(post_prog_, "u_aa_mode");
    u_refl_sun_    = glGetUniformLocation(refl_prog_, "u_sun_dir");
    u_refl_cool_   = glGetUniformLocation(refl_prog_, "u_haze_cool");
    u_refl_warm_   = glGetUniformLocation(refl_prog_, "u_haze_warm");
    u_refl_lit_    = glGetUniformLocation(refl_prog_, "u_cloud_lit");
    u_refl_sh_     = glGetUniformLocation(refl_prog_, "u_cloud_shadow");
    u_refl_cov_    = glGetUniformLocation(refl_prog_, "u_cloud_cov");
    u_refl_precip_ = glGetUniformLocation(refl_prog_, "u_precip");
    glCreateVertexArrays(1, &dummy_vao_);

    // Allocate pre-baked 256x128 RGBA16F equirectangular IBL reflection map + 8-level roughness mipmap chain!
    glCreateTextures(GL_TEXTURE_2D, 1, &refl_env_tex_);
    glTextureStorage2D(refl_env_tex_, 8, GL_RGBA16F, 256, 128);
    glTextureParameteri(refl_env_tex_, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTextureParameteri(refl_env_tex_, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(refl_env_tex_, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTextureParameteri(refl_env_tex_, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glCreateFramebuffers(1, &refl_fbo_);
    glNamedFramebufferTexture(refl_fbo_, GL_COLOR_ATTACHMENT0, refl_env_tex_, 0);
    GLenum db = GL_COLOR_ATTACHMENT0;
    glNamedFramebufferDrawBuffers(refl_fbo_, 1, &db);

    alloc_fbos(win_w, win_h, quality, pixel_percent, aa_mode, weather_detail);
    return true;
}

void SkyAndClouds::shutdown() {
    free_fbos();
    if (refl_fbo_)     { glDeleteFramebuffers(1, &refl_fbo_); refl_fbo_ = 0; }
    if (refl_env_tex_) { glDeleteTextures(1, &refl_env_tex_); refl_env_tex_ = 0; }
    if (sky_prog_)   { glDeleteProgram(sky_prog_);   sky_prog_ = 0; }
    if (cloud_prog_) { glDeleteProgram(cloud_prog_); cloud_prog_ = 0; }
    if (post_prog_)  { glDeleteProgram(post_prog_);  post_prog_ = 0; }
    if (refl_prog_)  { glDeleteProgram(refl_prog_);  refl_prog_ = 0; }
    if (dummy_vao_)  { glDeleteVertexArrays(1, &dummy_vao_); dummy_vao_ = 0; }
}

void SkyAndClouds::free_fbos() {
    if (scene_fbo_)   { glDeleteFramebuffers(1, &scene_fbo_); scene_fbo_ = 0; }
    if (scene_col_)   { glDeleteTextures(1, &scene_col_);     scene_col_ = 0; }
    if (scene_depth_) { glDeleteTextures(1, &scene_depth_);   scene_depth_ = 0; }
    if (cloud_occlusion_depth_) { glDeleteTextures(1, &cloud_occlusion_depth_); cloud_occlusion_depth_ = 0; }
    for (int i = 0; i < 2; i++) {
        if (cloud_fbo_[i]) { glDeleteFramebuffers(1, &cloud_fbo_[i]); cloud_fbo_[i] = 0; }
        if (cloud_tex_[i]) { glDeleteTextures(1, &cloud_tex_[i]);     cloud_tex_[i] = 0; }
        if (post_fbo_[i]) { glDeleteFramebuffers(1, &post_fbo_[i]); post_fbo_[i] = 0; }
        if (post_history_tex_[i]) { glDeleteTextures(1, &post_history_tex_[i]); post_history_tex_[i] = 0; }
    }
    if (display_fbo_) { glDeleteFramebuffers(1, &display_fbo_); display_fbo_ = 0; }
    if (post_display_tex_) { glDeleteTextures(1, &post_display_tex_); post_display_tex_ = 0; }
    if (display_depth_) { glDeleteTextures(1, &display_depth_); display_depth_ = 0; }
}

void SkyAndClouds::alloc_fbos(int win_w, int win_h, int quality, int pixel_percent, int aa_mode, int weather_detail) {
    free_fbos();
    cur_win_w_ = std::max(320, win_w);
    cur_win_h_ = std::max(240, win_h);
    cur_quality_ = std::clamp(quality, 0, 3);
    cloud_quality_ = weather_detail == 0 ? cur_quality_ : std::clamp(weather_detail - 1, 0, 2);
    cur_pixel_percent_ = std::clamp(pixel_percent, 40, 100);
    cur_aa_mode_ = std::clamp(aa_mode, 0, 2);

    // Keep the output native while letting users select the shaded pixel budget.
    // sqrt maps the requested pixel-area percentage onto both buffer dimensions.
    float scale = std::sqrt(cur_pixel_percent_ / 100.0f);
    scene_w_ = std::max(320, static_cast<int>(cur_win_w_ * scale));
    scene_h_ = std::max(180, static_cast<int>(cur_win_h_ * scale));

    // Render clouds at 1/2 internal res on High/Ultra (1/3 on Low/Medium) for crisp defined silhouettes at 60 FPS!
    int cdiv = (cloud_quality_ >= 2) ? 2 : 3;
    cloud_w_ = std::max(160, scene_w_ / cdiv);
    cloud_h_ = std::max(90,  scene_h_ / cdiv);

    glCreateTextures(GL_TEXTURE_2D, 1, &scene_col_);
    glTextureStorage2D(scene_col_, 1, GL_RGBA16F, scene_w_, scene_h_);
    glTextureParameteri(scene_col_, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(scene_col_, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(scene_col_, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(scene_col_, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glCreateTextures(GL_TEXTURE_2D, 1, &scene_depth_);
    glTextureStorage2D(scene_depth_, 1, GL_DEPTH_COMPONENT32F, scene_w_, scene_h_);
    glTextureParameteri(scene_depth_, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTextureParameteri(scene_depth_, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTextureParameteri(scene_depth_, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(scene_depth_, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glCreateTextures(GL_TEXTURE_2D, 1, &cloud_occlusion_depth_);
    glTextureStorage2D(cloud_occlusion_depth_, 1, GL_DEPTH_COMPONENT32F, scene_w_, scene_h_);
    glTextureParameteri(cloud_occlusion_depth_, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTextureParameteri(cloud_occlusion_depth_, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTextureParameteri(cloud_occlusion_depth_, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(cloud_occlusion_depth_, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glCreateFramebuffers(1, &scene_fbo_);
    glNamedFramebufferTexture(scene_fbo_, GL_COLOR_ATTACHMENT0, scene_col_, 0);
    glNamedFramebufferTexture(scene_fbo_, GL_DEPTH_ATTACHMENT, scene_depth_, 0);
    GLenum db = GL_COLOR_ATTACHMENT0;
    glNamedFramebufferDrawBuffers(scene_fbo_, 1, &db);

    for (int i = 0; i < 2; i++) {
        glCreateTextures(GL_TEXTURE_2D, 1, &cloud_tex_[i]);
        glTextureStorage2D(cloud_tex_[i], 1, GL_RGBA16F, cloud_w_, cloud_h_);
        glTextureParameteri(cloud_tex_[i], GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTextureParameteri(cloud_tex_[i], GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri(cloud_tex_[i], GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(cloud_tex_[i], GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glCreateFramebuffers(1, &cloud_fbo_[i]);
        glNamedFramebufferTexture(cloud_fbo_[i], GL_COLOR_ATTACHMENT0, cloud_tex_[i], 0);
        glNamedFramebufferDrawBuffers(cloud_fbo_[i], 1, &db);
    }

    // Full-resolution temporal history stores resolved HDR color and reverse-Z depth.
    glCreateTextures(GL_TEXTURE_2D, 1, &post_display_tex_);
    glTextureStorage2D(post_display_tex_, 1, GL_RGBA8, cur_win_w_, cur_win_h_);
    glTextureParameteri(post_display_tex_, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTextureParameteri(post_display_tex_, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glCreateFramebuffers(1, &display_fbo_);
    glNamedFramebufferTexture(display_fbo_, GL_COLOR_ATTACHMENT0, post_display_tex_, 0);
    // Compact native-resolution reverse-Z depth is used only when rendering the
    // cockpit after reconstruction; the cockpit meshes are close to the camera.
    glCreateTextures(GL_TEXTURE_2D, 1, &display_depth_);
    glTextureStorage2D(display_depth_, 1, GL_DEPTH_COMPONENT16, cur_win_w_, cur_win_h_);
    glTextureParameteri(display_depth_, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTextureParameteri(display_depth_, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTextureParameteri(display_depth_, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(display_depth_, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glNamedFramebufferTexture(display_fbo_, GL_DEPTH_ATTACHMENT, display_depth_, 0);
    glNamedFramebufferDrawBuffers(display_fbo_, 1, &db);
    for (int i = 0; i < 2; i++) {
        glCreateTextures(GL_TEXTURE_2D, 1, &post_history_tex_[i]);
        glTextureStorage2D(post_history_tex_[i], 1, GL_RGBA16F, cur_win_w_, cur_win_h_);
        glTextureParameteri(post_history_tex_[i], GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTextureParameteri(post_history_tex_[i], GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri(post_history_tex_[i], GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(post_history_tex_[i], GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glCreateFramebuffers(1, &post_fbo_[i]);
        glNamedFramebufferTexture(post_fbo_[i], GL_COLOR_ATTACHMENT0, post_display_tex_, 0);
        glNamedFramebufferTexture(post_fbo_[i], GL_COLOR_ATTACHMENT1, post_history_tex_[i], 0);
        GLenum post_db[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
        glNamedFramebufferDrawBuffers(post_fbo_[i], 2, post_db);
    }
    history_valid_ = false;
    post_history_valid_ = false;
    post_ping_ = 0;
}

void SkyAndClouds::resize_if_needed(int win_w, int win_h, int quality, int pixel_percent, int aa_mode, int weather_detail) {
    pixel_percent = std::clamp(pixel_percent, 40, 100);
    aa_mode = std::clamp(aa_mode, 0, 2);
    if (win_w != cur_win_w_ || win_h != cur_win_h_ || quality != cur_quality_ || pixel_percent != cur_pixel_percent_ ||
        cloud_quality_ != (weather_detail == 0 ? quality : std::clamp(weather_detail - 1, 0, 2))) {
        alloc_fbos(win_w, win_h, quality, pixel_percent, aa_mode, weather_detail);
    } else if (aa_mode != cur_aa_mode_) {
        cur_aa_mode_ = aa_mode;
        post_history_valid_ = false;
        post_ping_ = 0;
    }
}

void SkyAndClouds::update_solar_and_weather(
    double lat_deg, double lon_deg, double ground_m,
    int tod_slot, double sim_time_sec, float fog_regime_mult, float wx_advance01,
    const WeatherSnapshot *baked, int season_override, double day_cycle_elapsed_sec
) {
    atmo_.fair_vfr = (tod_slot < 0 || (tod_slot >= 0 && tod_slot <= 9));

    std::time_t now_t = std::time(nullptr);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &now_t);
#else
    gmtime_r(&now_t, &utc);
#endif
    int h = utc.tm_hour, m = utc.tm_min, s = utc.tm_sec;
    int real_doy = utc.tm_yday + 1;
    static constexpr int kSeasonRepresentativeDoy[4] = {15, 105, 196, 288};
    const int initial_doy = (season_override >= 0 && season_override < 4)
        ? kSeasonRepresentativeDoy[season_override] : real_doy;
    const double elapsed = std::max(0.0, day_cycle_elapsed_sec >= 0.0 ? day_cycle_elapsed_sec : sim_time_sec);
    constexpr double kRealSecondsPerGameDay = 5.0 * 60.0 * 60.0;
    constexpr double kCompressedNightFraction = 0.14;
    constexpr double kCompressedDayFraction = 1.0 - kCompressedNightFraction;
    const double day_count = std::floor(elapsed / kRealSecondsPerGameDay);
    const double cycle_phase = std::fmod(elapsed / kRealSecondsPerGameDay, 1.0);
    atmo_.day_cycle_phase = static_cast<float>(cycle_phase);
    atmo_.simulated_day = 1 + static_cast<int>(day_count);
    double phi = lat_deg * DEG2RAD_D;

    // Seed the accelerated clock at the current real UTC solar position so the
    // first frame does not jump. Every later 18,000 wall-clock seconds wraps
    // exactly one simulated day. The night occupies 14% of that cycle; its
    // hour angle advances faster while sunrise/sunset still follow latitude
    // and the seasonal declination.
    if (!cycle_clock_seeded_) {
        const double start_gamma = (2.0 * PI_D / 365.0) * (static_cast<double>(initial_doy - 1) +
            (h - 12.0 + (m + s / 60.0) / 60.0) / 24.0);
        const double start_eqtime = 229.18 * (0.000075 + 0.001868 * std::cos(start_gamma) - 0.032077 * std::sin(start_gamma)
            - 0.014615 * std::cos(2.0 * start_gamma) - 0.040849 * std::sin(2.0 * start_gamma));
        const double start_decl = 0.006918 - 0.399912 * std::cos(start_gamma) + 0.070257 * std::sin(start_gamma)
            - 0.006758 * std::cos(2.0 * start_gamma) + 0.000907 * std::sin(2.0 * start_gamma);
        const double start_phi = lat_deg * DEG2RAD_D;
        const double start_tst = std::fmod(h * 60.0 + m + s / 60.0 + start_eqtime + 4.0 * lon_deg + 1440.0, 1440.0);
        const double start_ha = (start_tst / 4.0 - 180.0) * DEG2RAD_D;
        const double start_ha_set = std::acos(std::clamp(-std::tan(start_phi) * std::tan(start_decl), -1.0, 1.0));
        if (start_ha < -start_ha_set) {
            cycle_seed_phase_ = kCompressedNightFraction * (start_ha + PI_D) / std::max(1e-6, PI_D - start_ha_set);
        } else if (start_ha > start_ha_set) {
            cycle_seed_phase_ = kCompressedDayFraction + kCompressedNightFraction *
                (start_ha - start_ha_set) / std::max(1e-6, PI_D - start_ha_set);
        } else {
            cycle_seed_phase_ = kCompressedNightFraction + kCompressedDayFraction *
                (start_ha + start_ha_set) / std::max(1e-6, 2.0 * start_ha_set);
        }
        cycle_seed_phase_ = std::clamp(cycle_seed_phase_, 0.0, std::nextafter(1.0, 0.0));
        cycle_seed_lon_ = lon_deg;
        cycle_seed_doy_ = initial_doy;
        cycle_seed_utc_ = now_t;
        cycle_clock_seeded_ = true;
    }
    int doy = cycle_seed_doy_;
    if (season_override < 0) {
        const std::time_t simulated_utc = cycle_seed_utc_ + static_cast<std::time_t>(day_count) * 86400;
        std::tm simulated_date{};
#ifdef _WIN32
        gmtime_s(&simulated_date, &simulated_utc);
#else
        gmtime_r(&simulated_utc, &simulated_date);
#endif
        doy = simulated_date.tm_yday + 1;
    }
    const double game_hour = std::fmod(cycle_phase * 24.0, 24.0);
    const double gamma = (2.0 * PI_D / 365.0) * (static_cast<double>(doy - 1) + (game_hour - 12.0) / 24.0);
    const double decl = 0.006918 - 0.399912 * std::cos(gamma) + 0.070257 * std::sin(gamma)
        - 0.006758 * std::cos(2.0 * gamma) + 0.000907 * std::sin(2.0 * gamma);
    const double ha_set = std::acos(std::clamp(-std::tan(phi) * std::tan(decl), -1.0, 1.0));

    double local_phase = std::fmod(cycle_seed_phase_ + elapsed / kRealSecondsPerGameDay +
                                   (lon_deg - cycle_seed_lon_) / 360.0 + 1.0, 1.0);
    double ha;
    if (local_phase < kCompressedNightFraction) {
        ha = -PI_D + (local_phase / kCompressedNightFraction) * (PI_D - ha_set);
    } else if (local_phase < kCompressedDayFraction) {
        ha = -ha_set + ((local_phase - kCompressedNightFraction) / kCompressedDayFraction) * (2.0 * ha_set);
    } else {
        ha = ha_set + ((local_phase - kCompressedDayFraction) / kCompressedNightFraction) * (PI_D - ha_set);
    }
    atmo_.is_night = std::abs(ha) > ha_set;

    double sin_el = std::clamp(std::sin(phi) * std::sin(decl) + std::cos(phi) * std::cos(decl) * std::cos(ha), -1.0, 1.0);
    double raw_el = std::asin(sin_el) * RAD2DEG_D;
    atmo_.sun_elev_deg = static_cast<float>(raw_el < 0.0 ? (-9.5 * (1.0 - std::exp(raw_el / 12.0))) : raw_el);

    double east = -std::cos(decl) * std::sin(ha);
    double north = std::cos(phi) * std::sin(decl) - std::sin(phi) * std::cos(decl) * std::cos(ha);
    double hl = std::max(1e-4, std::hypot(east, north));
    east /= hl; north /= hl;

    float wx = std::clamp(wx_advance01, 0.0f, 1.0f);
    const bool live_open_meteo = baked && baked->label.rfind("LIVE OPEN-METEO", 0) == 0;
    if (baked) {
        float dyn_osc = 0.015f * std::sin(static_cast<float>(sim_time_sec) * 0.015f);
        wx = std::clamp(wx + dyn_osc, 0.0f, 1.0f);
        atmo_.cloud_cov = std::clamp(baked->cloud_cov + 0.015f * std::sin(static_cast<float>(sim_time_sec) * 0.02f), 0.05f, 0.98f);
        atmo_.precip = baked->precip;
        double base = live_open_meteo
            ? ground_m + std::max(320.0, static_cast<double>(baked->cloud_base_m))
            : std::max(static_cast<double>(ground_m + 340.0), static_cast<double>(baked->cloud_base_m) - ground_m * 0.08);
        atmo_.cloud_base_m = static_cast<float>(std::clamp(base, 820.0, 3200.0));
        atmo_.cloud_thick_m = std::clamp(baked->cloud_thick_m, 900.0f, 3200.0f);
    } else {
        float dyn_osc = 0.055f * std::sin(static_cast<float>(sim_time_sec) * 0.028f) +
                        0.035f * std::cos(static_cast<float>(sim_time_sec) * 0.067f);
        wx = std::clamp(wx_advance01 + dyn_osc, 0.0f, 1.0f);
        atmo_.cloud_cov = std::lerp(0.16f, 0.94f, wx);
        atmo_.precip = std::clamp((wx - 0.56f) / 0.42f, 0.0f, 1.0f);
        double base_off = std::lerp(1350.0, 560.0, wx);
        atmo_.cloud_base_m = static_cast<float>(std::clamp(std::max(ground_m + base_off, std::lerp(2450.0, 1420.0, wx)), 1320.0, 2950.0));
        atmo_.cloud_thick_m = std::lerp(1450.0f, 2450.0f, wx);
    }

    char tod_tag[32];
    if (tod_slot >= 0 && tod_slot <= 9) {
        atmo_.sun_elev_deg = kTodElDeg[tod_slot];
        atmo_.is_night = false;
        atmo_.sun_dir = tod_slot_sun_dir(tod_slot);
        std::snprintf(tod_tag, sizeof(tod_tag), "TOD %d %s", tod_slot, kTodLabels[tod_slot]);
    } else if (tod_slot == 10) {
        atmo_.sun_elev_deg = -8.5f;
        atmo_.is_night = true;
        float az = 315.0f * DEG2RAD_F;
        float el = -8.5f * DEG2RAD_F;
        atmo_.sun_dir = normalize(Vec3(std::sin(az) * std::cos(el), std::sin(el), -std::cos(az) * std::cos(el)));
        std::snprintf(tod_tag, sizeof(tod_tag), "NIGHT RT-LIGHTS");
    } else {
        float er = atmo_.sun_elev_deg * DEG2RAD_F;
        atmo_.sun_dir = normalize(Vec3(static_cast<float>(east) * std::cos(er), std::sin(er), -static_cast<float>(north) * std::cos(er)));
        const int cycle_minute = static_cast<int>(cycle_phase * 300.0);
        std::snprintf(tod_tag, sizeof(tod_tag), "SIM DAY %03d | 5H %02d:%02d",
            atmo_.simulated_day, cycle_minute / 60, cycle_minute % 60);
    }

    float day_t = std::clamp((atmo_.sun_elev_deg + 2.0f) / 7.5f, 0.0f, 1.0f);
    float high_t = std::clamp(atmo_.sun_elev_deg / 26.0f, 0.0f, 1.0f);
    float storm_dim = std::lerp(1.0f, 0.66f, atmo_.precip);
    atmo_.haze_cool = mix(Vec3(0.004f, 0.008f, 0.022f), mix(Vec3(0.28f, 0.48f, 0.80f), Vec3(0.32f, 0.56f, 0.90f), high_t), day_t) * storm_dim;
    atmo_.haze_warm = mix(Vec3(0.014f, 0.016f, 0.028f), mix(Vec3(1.0f, 0.60f, 0.28f), Vec3(1.0f, 0.90f, 0.72f), high_t), day_t) * storm_dim;
    atmo_.cloud_lit = mix(Vec3(0.012f, 0.018f, 0.038f), mix(Vec3(1.0f, 0.76f, 0.48f), Vec3(1.0f, 0.98f, 0.94f), high_t), day_t);
    atmo_.cloud_shadow = mix(Vec3(0.003f, 0.005f, 0.012f), mix(Vec3(0.32f, 0.40f, 0.58f), Vec3(0.42f, 0.52f, 0.68f), high_t), day_t);
    float cloud_weather = std::clamp(smooth01(0.12f, 0.78f, atmo_.precip) * 0.76f +
                                     smooth01(0.54f, 0.92f, atmo_.cloud_cov) * 0.22f, 0.0f, 0.90f);
    atmo_.cloud_lit = mix(atmo_.cloud_lit, Vec3(0.54f, 0.61f, 0.69f) * std::lerp(0.72f, 1.0f, day_t), cloud_weather);
    atmo_.cloud_shadow = mix(atmo_.cloud_shadow, Vec3(0.075f, 0.092f, 0.125f) * std::lerp(0.72f, 1.0f, day_t), cloud_weather);
    float haze_mult = baked ? std::clamp(baked->haze_mult, 0.55f, 2.2f) : std::lerp(0.82f, 2.35f, wx);
    atmo_.haze_density = std::clamp(0.000014f * fog_regime_mult * haze_mult * std::lerp(1.22f, 0.88f, high_t), 0.000009f, 0.000095f);
    atmo_.exposure = std::lerp(1.12f, std::lerp(1.18f, 1.14f, high_t), day_t);

    char buf[160];
    const char *wx_tag = (atmo_.precip > 0.65f) ? "STORM!" : (atmo_.precip > 0.18f ? "RAIN+FOG" : (atmo_.cloud_cov > 0.72f ? "OVC" : (atmo_.cloud_cov > 0.46f ? "BKN" : "SCT")));
    std::snprintf(buf, sizeof(buf), "%s | SUN %+.1f deg | %s %d%% @ %dm%s",
        tod_tag, atmo_.sun_elev_deg, wx_tag,
        static_cast<int>(atmo_.cloud_cov * 100.0f), static_cast<int>(atmo_.cloud_base_m),
        baked ? (live_open_meteo ? " | LIVE OPEN-METEO" : " | BAKED ERA5") : "");
    atmo_.hud_wx_line = buf;
    update_reflection_map_if_needed(day_cycle_elapsed_sec >= 0.0 ? elapsed : sim_time_sec);
}

void SkyAndClouds::update_reflection_map_if_needed(double sim_time_sec) {
    if (!refl_fbo_ || !refl_prog_) return;
    // Reflection radiance is only 256x128, but keep its regeneration at or
    // below 0.5 Hz. The shader samples this baked environment every frame.
    // Also handle time-of-day scrubs and simulation resets that move the clock
    // backwards; otherwise the negative delta would suppress refresh forever.
    if (std::abs(sim_time_sec - refl_last_update_t_) < 2.0) return;
    auto changed3 = [](const Vec3 &a, const Vec3 &b) {
        return std::abs(a.x-b.x) + std::abs(a.y-b.y) + std::abs(a.z-b.z) > 0.025f;
    };
    const bool changed = dot(atmo_.sun_dir, refl_last_sun_) <= 0.9999f ||
        std::abs(atmo_.cloud_cov - refl_last_cov_) >= 0.02f ||
        std::abs(atmo_.precip - refl_last_precip_) >= 0.035f ||
        changed3(atmo_.haze_cool, refl_last_cool_) || changed3(atmo_.haze_warm, refl_last_warm_) ||
        changed3(atmo_.cloud_lit, refl_last_cloud_lit_) || changed3(atmo_.cloud_shadow, refl_last_cloud_shadow_);
    if (!changed) return;
    refl_last_sun_ = atmo_.sun_dir;
    refl_last_cov_ = atmo_.cloud_cov;
    refl_last_precip_ = atmo_.precip;
    refl_last_cool_ = atmo_.haze_cool;
    refl_last_warm_ = atmo_.haze_warm;
    refl_last_cloud_lit_ = atmo_.cloud_lit;
    refl_last_cloud_shadow_ = atmo_.cloud_shadow;
    refl_last_update_t_ = sim_time_sec;

    glBindFramebuffer(GL_FRAMEBUFFER, refl_fbo_);
    glViewport(0, 0, 256, 128);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glUseProgram(refl_prog_);
    glUniform3f(u_refl_sun_, atmo_.sun_dir.x, atmo_.sun_dir.y, atmo_.sun_dir.z);
    glUniform3f(u_refl_cool_, atmo_.haze_cool.x, atmo_.haze_cool.y, atmo_.haze_cool.z);
    glUniform3f(u_refl_warm_, atmo_.haze_warm.x, atmo_.haze_warm.y, atmo_.haze_warm.z);
    glUniform3f(u_refl_lit_, atmo_.cloud_lit.x, atmo_.cloud_lit.y, atmo_.cloud_lit.z);
    glUniform3f(u_refl_sh_, atmo_.cloud_shadow.x, atmo_.cloud_shadow.y, atmo_.cloud_shadow.z);
    glUniform1f(u_refl_cov_, atmo_.cloud_cov);
    glUniform1f(u_refl_precip_, atmo_.precip);
    glBindVertexArray(dummy_vao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    glGenerateTextureMipmap(refl_env_tex_);
}

void SkyAndClouds::begin_scene_pass(const Mat4 &inv_vp) {
    glBindFramebuffer(GL_FRAMEBUFFER, scene_fbo_);
    glViewport(0, 0, scene_w_, scene_h_);
    // Reverse-Z clear depth to 0.0 (far infinity) + test GL_GREATER!
    glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
    glDepthMask(GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClearDepth(0.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // Draw Rayleigh + Mie Sky fullscreen triangle with depth write disabled
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glUseProgram(sky_prog_);
    glUniformMatrix4fv(u_sky_inv_vp_, 1, GL_FALSE, inv_vp.m);
    glUniform3f(u_sky_sun_, atmo_.sun_dir.x, atmo_.sun_dir.y, atmo_.sun_dir.z);
    glUniform3f(u_sky_cool_, atmo_.haze_cool.x, atmo_.haze_cool.y, atmo_.haze_cool.z);
    glUniform3f(u_sky_warm_, atmo_.haze_warm.x, atmo_.haze_warm.y, atmo_.haze_warm.z);
    glUniform1f(u_sky_cov_, atmo_.cloud_cov);
    glBindVertexArray(dummy_vao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    // Restore Reverse-Z opaque depth state for terrain, cities, trees, runways & aircraft
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_GREATER);
    glDepthMask(GL_TRUE);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
}

void SkyAndClouds::capture_cloud_occlusion_depth() {
    if (!scene_depth_ || !cloud_occlusion_depth_ || scene_w_ <= 0 || scene_h_ <= 0) return;
    glCopyImageSubData(scene_depth_, GL_TEXTURE_2D, 0, 0, 0, 0,
                       cloud_occlusion_depth_, GL_TEXTURE_2D, 0, 0, 0, 0,
                       scene_w_, scene_h_, 1);
}

void SkyAndClouds::render_clouds_and_post(
    const Mat4 &vp, const Mat4 &inv_vp,
    const Vec3 &camera_forward,
    double cam_east_m, double cam_north_m, double cam_alt_m,
    GLuint cloud_noise_tex, GLuint cloud_vol_tex, GLuint cloud_detail_tex, GLuint lut_tex,
    GLuint atmo_field_tex, GLuint atmo_terrain_tex, const Vec4 &atmo_domain,
    float time_sec, int frame_idx, const Vec4 &wx_fx
) {
    Vec3 camera_delta{0.0f, 0.0f, 0.0f};
    if (prev_camera_valid_) {
        camera_delta = {
            static_cast<float>(cam_east_m - prev_cam_east_m_),
            static_cast<float>(cam_alt_m - prev_cam_alt_m_),
            static_cast<float>(-(cam_north_m - prev_cam_north_m_))
        };
    }
    const double camera_step_m = std::sqrt(
        (cam_east_m - prev_cam_east_m_) * (cam_east_m - prev_cam_east_m_) +
        (cam_north_m - prev_cam_north_m_) * (cam_north_m - prev_cam_north_m_) +
        (cam_alt_m - prev_cam_alt_m_) * (cam_alt_m - prev_cam_alt_m_));
    const float forward_dot = std::clamp(
        camera_forward.x * prev_camera_forward_.x +
        camera_forward.y * prev_camera_forward_.y +
        camera_forward.z * prev_camera_forward_.z,
        -1.0f, 1.0f);
    const float camera_turn_rad = prev_camera_valid_ ? std::acos(forward_dot) : 0.0f;
    const int sparse_period = 1;
    const float temporal_blend = (!history_valid_ || camera_step_m > 40.0)
        ? 0.0f
        : std::clamp(0.68f - static_cast<float>(camera_step_m) * 0.008f - camera_turn_rad * 2.5f, 0.35f, 0.68f);

    // 1. Quarter-Resolution Temporally Reprojected Living 3D Volumetric Cloud Pass (~0.35ms on Renoir iGPU!)
    int dst_idx = cloud_ping_;
    int src_idx = 1 - cloud_ping_;
    glBindFramebuffer(GL_FRAMEBUFFER, cloud_fbo_[dst_idx]);
    glViewport(0, 0, cloud_w_, cloud_h_);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);

    glUseProgram(cloud_prog_);
    glUniformMatrix4fv(u_cld_inv_vp_, 1, GL_FALSE, inv_vp.m);
    glUniformMatrix4fv(u_cld_prev_vp_, 1, GL_FALSE, prev_vp_.m);
    glUniform3f(u_cld_cam_delta_, camera_delta.x, camera_delta.y, camera_delta.z);
    glUniform3f(u_cld_sun_, atmo_.sun_dir.x, atmo_.sun_dir.y, atmo_.sun_dir.z);
    glUniform2f(u_cld_cam_en_, static_cast<float>(cam_east_m), static_cast<float>(cam_north_m));
    glUniform1f(u_cld_cam_alt_, static_cast<float>(cam_alt_m));
    glUniform1f(u_cld_base_, atmo_.cloud_base_m);
    glUniform1f(u_cld_thick_, atmo_.cloud_thick_m);
    glUniform1f(u_cld_cov_, atmo_.cloud_cov);
    glUniform1f(u_cld_precip_, atmo_.precip);
    glUniform1f(u_cld_time_, time_sec);
    glUniform1i(u_cld_frame_, frame_idx);
    glUniform1f(u_cld_tblend_, temporal_blend);
    static constexpr GLint kCloudSteps[4] = {9, 10, 12, 16};
    glUniform1i(u_cld_steps_, kCloudSteps[cloud_quality_]);
    glUniform1i(u_cld_sparse_, sparse_period);
    glUniform3f(u_cld_lit_, atmo_.cloud_lit.x, atmo_.cloud_lit.y, atmo_.cloud_lit.z);
    glUniform3f(u_cld_sh_, atmo_.cloud_shadow.x, atmo_.cloud_shadow.y, atmo_.cloud_shadow.z);
    glUniform3f(u_cld_cool_, atmo_.haze_cool.x, atmo_.haze_cool.y, atmo_.haze_cool.z);
    glUniform3f(u_cld_warm_, atmo_.haze_warm.x, atmo_.haze_warm.y, atmo_.haze_warm.z);
    glUniform4f(u_cld_dom_, atmo_domain.x, atmo_domain.y, atmo_domain.z, atmo_domain.w);
    glUniform4f(u_cld_wx_fx_, wx_fx.x, wx_fx.y, wx_fx.z, wx_fx.w);

    glBindTextureUnit(0, cloud_noise_tex);
    glBindTextureUnit(1, cloud_vol_tex);
    glBindTextureUnit(2, cloud_tex_[src_idx]);
    glBindTextureUnit(3, cloud_occlusion_depth_);
    glBindTextureUnit(4, atmo_field_tex);
    glBindTextureUnit(5, atmo_terrain_tex);
    glBindTextureUnit(6, cloud_detail_tex);
    glBindVertexArray(dummy_vao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    history_valid_ = true;
    cloud_ping_ = 1 - cloud_ping_;

    // 2. Reprojected temporal AA + AgX/LUT composite. The history stores linear HDR
    // and reverse-Z depth, so stable far terrain converges across subpixel jitter.
    int post_dst = post_ping_;
    int post_src = 1 - post_ping_;
    glBindFramebuffer(GL_FRAMEBUFFER, post_fbo_[post_dst]);
    glViewport(0, 0, cur_win_w_, cur_win_h_);
    glUseProgram(post_prog_);
    glUniform1f(u_post_exp_, atmo_.exposure);
    glUniformMatrix4fv(u_post_inv_vp_, 1, GL_FALSE, inv_vp.m);
    glUniformMatrix4fv(u_post_prev_vp_, 1, GL_FALSE, prev_vp_.m);
    glUniform3f(u_post_camera_delta_, camera_delta.x, camera_delta.y, camera_delta.z);
    glUniform1i(u_post_history_valid_, post_history_valid_ ? 1 : 0);
    glUniform1i(u_post_aa_mode_, cur_aa_mode_);
    // Project directional sun (at 100 km) into screen UV for lightweight Reverse-Z crepuscular scattering shafts!
    Vec4 sc = vp.mul({atmo_.sun_dir.x * 100000.0f, atmo_.sun_dir.y * 100000.0f, atmo_.sun_dir.z * 100000.0f, 1.0f});
    float su = 0.5f, sv = 0.5f, svis = 0.0f;
    if (sc.w > 0.001f) {
        su = (sc.x / sc.w) * 0.5f + 0.5f;
        sv = (sc.y / sc.w) * 0.5f + 0.5f;
        svis = std::clamp(1.25f - std::hypot(su - 0.5f, sv - 0.5f) * 0.85f, 0.0f, 1.0f);
    }
    float sun_el_vis = std::clamp((atmo_.sun_dir.y + 0.06f) / 0.22f, 0.0f, 1.0f);
    glUniform4f(u_post_sun_uv_, su, sv, svis, sun_el_vis);
    glUniform3f(u_post_warm_, atmo_.haze_warm.x, atmo_.haze_warm.y, atmo_.haze_warm.z);
    glBindTextureUnit(0, scene_col_);
    glBindTextureUnit(1, cloud_tex_[dst_idx]);
    glBindTextureUnit(2, lut_tex);
    glBindTextureUnit(3, scene_depth_);
    glBindTextureUnit(4, post_history_tex_[post_src]);
    glBindVertexArray(dummy_vao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    // Draw HUD and read captures from the resolved image. The display FBO
    // shares the color target and has no temporal history attachment.
    glBindFramebuffer(GL_FRAMEBUFFER, display_fbo_);
    glBindVertexArray(0);
    post_history_valid_ = true;
    post_ping_ = post_src;
    prev_vp_ = vp;
    prev_cam_east_m_ = cam_east_m;
    prev_cam_north_m_ = cam_north_m;
    prev_cam_alt_m_ = cam_alt_m;
    prev_camera_forward_ = camera_forward;
    prev_camera_valid_ = true;
}

void SkyAndClouds::begin_native_cockpit_pass() {
    glBindFramebuffer(GL_FRAMEBUFFER, display_fbo_);
    glViewport(0, 0, cur_win_w_, cur_win_h_);
    glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_GREATER);
    glDepthMask(GL_TRUE);
    glClearDepth(0.0);
    glClear(GL_DEPTH_BUFFER_BIT);
    glDisable(GL_BLEND);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
}

void SkyAndClouds::present(int x, int y, int width, int height, int window_w, int window_h) const {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (width < window_w || height < window_h) {
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    // PixelRect uses top-left coordinates like the X11 pointer. GL uses bottom-left.
    const int bottom = window_h - y - height;
    glBlitNamedFramebuffer(display_fbo_, 0, 0, 0, cur_win_w_, cur_win_h_,
        x, bottom, x + width, bottom + height, GL_COLOR_BUFFER_BIT, GL_LINEAR);
}

} // namespace nsim
