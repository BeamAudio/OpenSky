#pragma once

#include "gl_loader.hpp"
#include "math3d.hpp"
#include "pack_reader.hpp"
#include "weather_library.hpp"

#include <optional>
#include <array>
#include <algorithm>
#include <cstdint>
#include <ctime>
#include <future>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace nsim {

// Smooth dividing-streamline terrain deflection shared by the 3D solver and World map previews:
// blocks cross-slope flow, splits around peaks, channels along valleys, and adds lee wake & slope breezes.
// h_fac = exp(-agl_m / 1350.0f) is hoisted per vertical layer to avoid per-cell exp() overhead.
inline void orographic_deflect_wind(float u0, float v0, float ge, float gn,
                                    float ridge, float basin, float h_fac, float solar,
                                    float &out_u, float &out_v) {
    const float s2 = ge * ge + gn * gn;
    float u = u0, v = v0;
    if (s2 > 3.6e-5f && h_fac > 0.04f) {
        const float s = std::sqrt(s2);
        const float inv_s = 1.0f / s;
        const float nx = ge * inv_s, ny = gn * inv_s; // upslope normal
        const float tx = -ny, ty = nx;                // contour tangent
        const float un = u0 * nx + v0 * ny;
        const float ut = u0 * tx + v0 * ty;
        const float spd = std::sqrt(u0 * u0 + v0 * v0);
        const float t_s = std::clamp((s - 0.006f) / 0.085f, 0.0f, 1.0f);
        const float beta = (t_s * t_s * (3.0f - 2.0f * t_s)) * h_fac;
        const float un_blocked = un * (1.0f - 0.90f * beta);
        const float eps = 0.14f * spd + 0.28f;
        const float smooth_sign = ut / std::sqrt(ut * ut + eps * eps);
        const float ut_deflected = ut * (1.0f + 0.42f * beta) + smooth_sign * (0.95f * beta * std::abs(un));
        const float lee = (un < 0.0f) ? (beta * std::clamp(-un / (spd + 1.0f), 0.0f, 1.0f)) : 0.0f;
        u = nx * un_blocked + tx * ut_deflected;
        v = ny * un_blocked + ty * ut_deflected;
        u = u * (1.0f - 0.34f * lee) + tx * (0.46f * lee * spd);
        v = v * (1.0f - 0.34f * lee) + ty * (0.46f * lee * spd);
        const float slope_drv = (solar - 0.22f) * 2.8f * beta;
        u += nx * slope_drv;
        v += ny * slope_drv;
    }
    const float gain = 1.0f + (0.26f * ridge + 0.22f * basin) * h_fac;
    out_u = std::clamp(u * gain, -60.0f, 60.0f);
    out_v = std::clamp(v * gain, -60.0f, 60.0f);
}

enum class WindRegime : int {
    PYRENEAN_THERMAL = 0, // Sunny anabatic up-valley breeze + cliff thermals
    CIERZO_NW        = 1, // Iconic Aragon NW Ebro Valley Venturi jet + Santa Cilia lee waves
    FOEHN_SUR        = 2, // Southerly Pyrenean crest crossing -> orographic clouds + lee Foehn
    KATABATIC_DAWN   = 3, // Cool stable downslope valley drainage currents + basin radiation fog
    CONVECTIVE_STORM = 4, // Active Pyrenean storm cells / heavy rain shafts / lightning / gusts
    WINTER_ICING     = 5, // Freezing Pyrenean winter front: snow, freezing rain & structural rime ice
    COUNT            = 6
};

struct LocalAirSample {
    // Total 3D airmass velocity in local ENU (m/s)
    double wind_east_mps = 0.0;
    double wind_north_mps = 0.0;
    double wind_up_mps = 0.0;

    // Aerodynamic turbulence & asymmetric thermal/rotor wing torque (rad/s)
    double p_turb_rps = 0.0;
    double q_turb_rps = 0.0;
    double r_turb_rps = 0.0;

    // Physics decomposition for cockpit variometer & avionics HUD
    double ridge_lift_mps = 0.0;
    double wave_lift_mps = 0.0;
    double thermal_lift_mps = 0.0;
    double gust_mps = 0.0;
    double precipitation_mm_hr = 0.0;
    double turb_rms = 0.0;
    double oat_c = 14.0;
    double relative_humidity = 0.55;
    double qnh_hpa = 1015.0;
    float cloud_delta = 0.0f; // Baked orographic/thermal condensation modifier
    float local_cloud_frac = 0.0f; // [0..1] local cloud / valley fog density at aircraft
    float icing_intensity = 0.0f;  // [0..1] freezing visible moisture / supercooled rain intensity
    float snow_factor = 0.0f;      // [0..1] 0=liquid rain, 1=snow/sleet
    float lightning_flash = 0.0f;  // [0..1] instantaneous storm sheet-lightning pulse
    int atmo_layer = 0;       // 0=Boundary/Inversion, 1=Convective, 2=Lee-Wave, 3=Upper Jet
    bool in_lee_rotor = false;
    bool in_thermal = false;
    bool in_storm_cell = false;
    bool in_cloud = false;

    const char *layer_name() const {
        switch (atmo_layer) {
            case 0: return "L0-ABL";
            case 1: return "L1-CONV";
            case 2: return "L2-WAVE";
            default: return "L3-JET";
        }
    }

    double speed_kt() const { return std::hypot(wind_east_mps, wind_north_mps) * 1.94384; }
    double from_dir_deg() const {
        double d = std::atan2(-wind_east_mps, -wind_north_mps) * RAD2DEG_D;
        return std::fmod(d + 360.0, 360.0);
    }
};

struct WeatherDebugCell {
    Vec3 position_enu_m;
    Vec3 wind_enu_mps;
    float cloud_fraction = 0.0f;
    float liquid_gkg = 0.0f;
    float rain_mm_hr = 0.0f;
    float ridge_exposure = 0.0f;
    float valley_basin = 0.0f;
    bool valid = false;
};

struct WeatherDebugSlice {
    static constexpr int width = 13;
    std::array<WeatherDebugCell, width * width> cells{};
    uint64_t generation = 0;
    float agl_m = 0.0f, step_e_m = 0.0f, step_n_m = 0.0f;
    bool locally_refined = false;
};

struct WeatherWorkStats {
    uint64_t steps = 0, uploads = 0;
    double solve_ms = 0.0, publish_ms = 0.0;
};

struct DrydenGustSample {
    double ug_mps = 0.0, vg_mps = 0.0, wg_mps = 0.0;
    double pg_rps = 0.0, qg_rps = 0.0, rg_rps = 0.0;
};

// Non-deterministic hardware-seeded MIL-F-8785C 3D continuous-time Dryden turbulence filter.
// Converts solved weather-grid TKE & boundary-layer integral scales into physically correlated
// 3D velocity gusts and wingspan-filtered angular gust rates (zero deterministic sine waves).
class StochasticDrydenFilter {
public:
    StochasticDrydenFilter();
    DrydenGustSample step(double dt_sec, double tas_mps, double agl_m,
                          double sigma_u, double sigma_v, double sigma_w,
                          double wingspan_m = 11.0);
    void set_seed(uint32_t seed) { rng_.seed(seed); norm_.reset(); }

private:
    std::mt19937 rng_;
    std::normal_distribution<double> norm_{0.0, 1.0};
    double u_s_ = 0.0;
    double v_s1_ = 0.0, v_s2_ = 0.0;
    double w_s1_ = 0.0, w_s2_ = 0.0;
    double p_s_ = 0.0, prev_vg_ = 0.0, prev_wg_ = 0.0;
    DrydenGustSample last_{};
};

// Advances a terrain-following regional model on a worker thread using
// baked or live boundary conditions. The renderer consumes completed fields.
enum class WeatherSource { Baked, Live, Manual, Default };

struct WeatherOutlook {
    enum class Status { Idle, Running, Complete };
    Status status = Status::Idle;
    double issue_sim_time = 0;
    std::string region_id;
    std::vector<WeatherModelHistoryEntry> predictions;
};

class RegionalAtmosphere {
public:
    // Regional weather is a mesoscale field; shader noise supplies finer cloud structure.
    static constexpr int GRID_W = 384;
    static constexpr int GRID_H = 288;

    RegionalAtmosphere() = default;
    ~RegionalAtmosphere();

    bool init(const PackReader *pack, bool create_gpu_tex = true,
              const std::string &weather_path = "", bool enable_live_weather = true);
    void set_grid_detail(int detail, double aircraft_lat, double aircraft_lon);
    void update_refinement_center(double aircraft_lat, double aircraft_lon);
    int grid_detail() const { return weather_grid_detail_; }
    int grid_width() const { return grid_w_; }
    int grid_height() const { return grid_h_; }
    int local_grid_width() const { return frame_weather_.local_grid_w; }
    int local_grid_height() const { return frame_weather_.local_grid_h; }
    GLuint local_field_tex() const { return local_field_tex_; }
    GLuint local_terrain_tex() const { return local_terrain_tex_; }
    Vec4 local_domain_bounds() const;
    void sync_region(double lat_deg, double lon_deg);
    void finish_pending_initialization();
    void shutdown();

    void set_regime(WindRegime r);
    void cycle_regime();
    WindRegime regime() const { return regime_; }
    const char *regime_name() const;
    const char *regime_short() const;

    // Select / blend baked states from UTC season, sun azimuth/elevation and severity.
    void update_field(const Vec3 &sun_dir, double sim_time_sec, bool force = false, float wx_advance01 = 0.28f);
    bool has_weather() const { return lib_.valid() || live_weather_available(); }
    bool live_weather_available() const {
        if (!live_available_.load(std::memory_order_acquire)) return false;
        const int64_t last = live_last_success_utc_.load(std::memory_order_acquire);
        return last > 0 && static_cast<int64_t>(std::time(nullptr)) - last <= 1800;
    }
    int64_t live_weather_age_seconds() const {
        if (!live_available_.load(std::memory_order_acquire)) return -1;
        const int64_t last = live_last_success_utc_.load(std::memory_order_acquire);
        return last > 0 ? std::max<int64_t>(0, static_cast<int64_t>(std::time(nullptr)) - last) : -1;
    }
    void set_solver_seed(uint32_t seed, bool synchronous = false) { solver_seed_ = seed; synchronous_solver_ = synchronous; }
    void request_outlook();
    void cancel_outlook();
    WeatherOutlook outlook() const;
    WeatherSource model_source() const { return frame_weather_.source; }
    bool has_model() const { return weather_model_ready_ && !frame_weather_.grid.empty(); }
    bool covers(double lat, double lon) const;
    const std::vector<WeatherModelHistoryEntry> &model_history() const { return model_history_; }
    void set_manual_weather_override(bool enabled) {
        if (manual_weather_override_ != enabled) { manual_weather_override_ = enabled; dirty_ = true; }
    }
    bool manual_weather_override() const { return manual_weather_override_; }

    // Interpolated sample from the four-layer regional mesoscale weather field.
    // Pass dt_sec > 0 during live flight to step the hardware-seeded Dryden turbulence filter
    // and evaluate spanwise wingtip wind shear along aircraft heading_deg at tas_mps.
    LocalAirSample sample_at(double lat_deg, double lon_deg, double alt_msl_m, const Vec3 &sun_dir,
                             double sim_time_sec, float wx_advance01 = 0.28f,
                             double dt_sec = 0.0, double heading_deg = 0.0, double tas_mps = 50.0) const;
    WeatherDebugSlice debug_slice(double east_m, double north_m, int level, int sample_stride = 3) const;
    WeatherDebugCell debug_sample(double east_m, double north_m, int level) const;
    WeatherWorkStats work_stats() const;

    GLuint field_tex() const { return field_tex_; }
    GLuint terrain_field_tex() const { return terrain_field_tex_; }
    Vec4 domain_bounds() const {
        return {static_cast<float>(min_e_), static_cast<float>(min_n_),
                static_cast<float>(1.0 / std::max(1.0, span_e_)),
                static_cast<float>(1.0 / std::max(1.0, span_n_))};
    }

    const WeatherLibrary &library() const { return lib_; }
    const WeatherSnapshot &weather() const { return active_weather_; }
    void set_season(int s) { season_override_ = s; dirty_ = true; }
    void set_slot(float s) { slot_override_ = s; dirty_ = true; }
    int active_state() const { return lib_.active_state(); }
    bool has_library() const { return lib_.valid(); }
    float lightning_flash_at(double east_m, double north_m, double sim_time_sec) const;

private:
    struct DynamicCell {
        float east_m = 0.0f, north_m = 0.0f;
        float vel_e = 14.0f, vel_n = 4.0f;
        float radius_m = 8500.0f;
        float phase = 0.5f;         // 0..1 lifecycle (grow -> mature -> dissipate)
        float period_s = 720.0f;
        float peak_rain = 20.0f;    // mm/hr at cell core
        float peak_updraft = 4.5f;  // m/s convective core
        float lightning_flash = 0.0f; // [0..1] live Poisson stochastic bolt intensity
        float lightning_cooldown = 3.0f;
    };
    struct SimWeatherCell {
        float u = 0.0f, v = 0.0f, w = 0.0f;
        float vapor_gkg = 5.0f, liquid_gkg = 0.0f;
        float temp_c = 10.0f, pressure_hpa = 950.0f;
        float rain_mm_hr = 0.0f, cloud_fraction = 0.0f, tke = 0.1f;
    };

    struct WeatherSolver {
        int grid_w_ = GRID_W, grid_h_ = GRID_H;
        bool suppress_dynamic_advance_ = false;
        std::vector<SimWeatherCell> weather_grid_, weather_next_;
        std::vector<float> weather_rain_flux_, terrain_height_, terrain_slope_e_, terrain_slope_n_;
        std::vector<float> flow_slope_e_, flow_slope_n_;
        std::vector<float> terrain_ridge_exposure_, terrain_valley_basin_, weather_worker_climate_field_;
        WeatherSnapshot weather_worker_active_{}, synoptic_weather_{};
        double min_e_ = 0, min_n_ = 0, span_e_ = 1, span_n_ = 1;
        double weather_anchor_lat_ = 0, anchor_lon = 0, metres_lat = 1, metres_lon = 1;
        double solved_time = 0;
        std::string region_id;
        WeatherSource source = WeatherSource::Default;
        Vec3 sun{0, 1, 0};
        WindRegime regime_ = WindRegime::PYRENEAN_THERMAL;
        std::array<DynamicCell, 6> dyn_cells_{};
        int dyn_cell_count_ = 0;
        std::mt19937 solver_rng_{};
        void spawn_dynamic_cell(DynamicCell &c, bool initial_spread);
        void seed_dynamic_cells();
        void advance_dynamic_cells(float dt, float solar);
        bool step(float dt, const Vec3 &sun_dir, const WeatherSnapshot &forcing,
                  const std::atomic<bool> *stop_requested = nullptr);
    };

    struct WeatherPublication {
        int grid_w = GRID_W, grid_h = GRID_H;
        std::vector<SimWeatherCell> grid;
        std::vector<uint16_t> field;
        std::vector<float> terrain;
        int local_grid_w = 0, local_grid_h = 0;
        std::vector<SimWeatherCell> local_grid;
        std::vector<uint16_t> local_field;
        std::vector<float> local_terrain;
        std::vector<float> local_ridge_exposure, local_valley_basin;
        std::array<DynamicCell, 6> cells{};
        int cell_count = 0;
        WeatherSnapshot summary, forcing;
        WeatherSource source = WeatherSource::Default;
        WindRegime regime = WindRegime::PYRENEAN_THERMAL;
        uint64_t generation = 0;
        uint64_t local_terrain_generation = 0;
        double solved_time = 0;
        double anchor_lat = 0, anchor_lon = 0, metres_lat = 1, metres_lon = 1;
        double min_e = 0, min_n = 0, span_e = 1, span_n = 1;
        double local_min_e = 0, local_min_n = 0, local_span_e = 0, local_span_n = 0;
        std::string region_id;
    };

    void latch_weather();
    static WeatherModelHistoryEntry summarize(double time, const WeatherSnapshot &weather, const std::string &region);
    const WeatherSnapshot &frame_forcing() const {
        return has_model() ? frame_weather_.forcing : synoptic_weather_;
    }
    WindRegime frame_regime() const { return has_model() ? frame_weather_.regime : regime_; }
    void upload_field();
    void allocate_weather_textures();
    void allocate_local_weather_textures();
    void initialize_local_weather_model();
    void refresh_local_refinement_climate();
    void sync_local_refinement_boundary();
    bool advance_weather_with_refinement(float dt, const Vec3 &sun, const WeatherSnapshot &forcing,
                                         const std::atomic<bool> *stop_requested = nullptr);
    void build_weather_texture_field(const WeatherSolver &solver, std::vector<uint16_t> &out) const;
    void live_weather_worker();
    void initialize_weather_model();
    bool poll_weather_model_initialization();
    void finish_weather_model_initialization();
    void weather_worker_loop();
    void start_weather_worker();
    void stop_weather_worker();
    void publish_weather_model();
    void refresh_climate_field();
    void evaluate_dynamic_cells(double east_m, double north_m, double agl_m,
                                float &out_cloud_add, float &out_rain_add, float &out_w_add,
                                float &out_outflow_e, float &out_outflow_n, float &out_tke_add) const;
    SimWeatherCell sample_weather_model(double east_m, double north_m, double agl_m) const;

    const PackReader *pack_ = nullptr;
    WindRegime regime_ = WindRegime::PYRENEAN_THERMAL;
    bool gpu_enabled_ = false;
    int weather_grid_detail_ = 0; // 0: off, 1: 192^2 local tile, 2: 384^2 local tile
    int grid_w_ = GRID_W, grid_h_ = GRID_H;
    int texture_grid_w_ = 0, texture_grid_h_ = 0;
    GLuint local_field_tex_ = 0, local_terrain_tex_ = 0;
    int local_texture_grid_w_ = 0, local_texture_grid_h_ = 0;
    GLuint field_tex_ = 0;
    GLuint terrain_field_tex_ = 0;

    WeatherLibrary lib_;
    std::string loaded_wx_path_;
    std::thread live_thread_;
    mutable std::mutex live_mtx_;
    std::condition_variable live_cv_;
    std::atomic<bool> live_stop_{false};
    std::atomic<bool> live_available_{false};
    std::atomic<int64_t> live_last_success_utc_{0};
    double live_elevation_m_ = 0.0;
    bool live_overlay_applied_ = false;
    bool live_enabled_ = false;
    bool live_force_fetch_ = false;
    double live_lat_ = 42.1, live_lon_ = -0.3;
    double last_model_history_sec_ = -1.0;
    std::vector<WeatherModelHistoryEntry> model_history_;
    WeatherSnapshot live_target_{};
    WeatherSnapshot synoptic_weather_{}; // Baked/observed boundary conditions fed into the runtime solve
    WeatherSnapshot active_weather_{};
    bool active_weather_init_ = false;
    bool manual_weather_override_ = false;
    double active_weather_time_ = -1.0;
    std::vector<float> terrain_upload_;
    std::vector<float> climate_field_;
    WeatherSolver solver_; // Mutated by the weather worker, or with that worker stopped.
    WeatherSolver local_solver_; // Fine tile around the aircraft; advanced with the regional boundary field.
    bool local_refinement_pending_ = false;
    double refinement_center_lat_ = 42.53131, refinement_center_lon_ = 0.47676;
    double local_center_e_ = 0.0, local_center_n_ = 0.0;
    WeatherPublication frame_weather_, pending_weather_, staging_weather_;
    mutable std::mutex weather_publish_mtx_;
    uint64_t weather_generation_ = 0;
    uint64_t local_terrain_generation_ = 0;
    uint64_t uploaded_weather_generation_ = 0;
    uint64_t uploaded_local_weather_generation_ = 0;
    uint64_t uploaded_local_terrain_generation_ = 0;
    uint64_t field_uploads_ = 0;
    std::atomic<uint64_t> weather_steps_{0}, weather_solve_us_{0}, weather_publish_us_{0};
    std::optional<uint32_t> solver_seed_;
    bool synchronous_solver_ = false;
    std::string active_region_id_;
    uint64_t live_request_id_ = 0; // guarded by live_mtx_
    WeatherOutlook outlook_; // guarded by weather_job_mtx_
    uint64_t outlook_request_id_ = 0;
    bool outlook_requested_ = false;
    bool weather_model_ready_ = false;
    double weather_model_time_ = -1.0;
    double weather_model_accum_ = 0.0;
    double weather_anchor_lat_ = 0.0, weather_anchor_lon_ = 0.0;
    std::thread weather_worker_;
    std::future<void> weather_init_future_;
    bool weather_init_pending_ = false;
    bool weather_terrain_upload_pending_ = false;
    mutable std::mutex weather_job_mtx_;
    std::condition_variable weather_job_cv_;
    std::atomic<bool> weather_worker_stop_{false};
    int weather_pending_steps_ = 0;
    bool weather_pending_climate_ = false;
    std::vector<float> weather_pending_climate_field_;
    WeatherSnapshot weather_worker_forcing_{};
    WeatherSource weather_worker_source_ = WeatherSource::Default;
    WindRegime weather_worker_regime_ = WindRegime::PYRENEAN_THERMAL;
    Vec3 weather_worker_sun_{0.0f, 1.0f, 0.0f};
    double min_e_ = 0.0, min_n_ = 0.0, span_e_ = 163840.0, span_n_ = 122880.0;
    int season_override_ = -1;
    int last_season_ = -1;
    float slot_override_ = -1.0f;
    float last_severity_ = -1.0f;
    float last_slot_ = -1.0f;
    int last_state_ = -1;
    double last_update_t_ = -999.0;
    bool dirty_ = true;
    mutable StochasticDrydenFilter dryden_filter_{};
};

} // namespace nsim
