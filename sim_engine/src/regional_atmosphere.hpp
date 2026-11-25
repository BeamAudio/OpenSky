#pragma once

#include "gl_loader.hpp"
#include "math3d.hpp"
#include "pack_reader.hpp"
#include "weather_library.hpp"

#include <array>
#include <algorithm>
#include <cstdint>
#include <ctime>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace nsim {

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
    static constexpr int width = 9;
    std::array<WeatherDebugCell, width * width> cells{};
    uint64_t generation = 0;
    float agl_m = 0.0f, step_e_m = 0.0f, step_n_m = 0.0f;
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
    DrydenGustSample current() const { return last_; }

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
class RegionalAtmosphere {
public:
    // Regional weather is a mesoscale field; shader noise supplies finer cloud structure.
    static constexpr int GRID_W = 384;
    static constexpr int GRID_H = 288;

    RegionalAtmosphere() = default;
    ~RegionalAtmosphere();

    bool init(const PackReader *pack, bool create_gpu_tex = true,
              const std::string &weather_path = "", bool enable_live_weather = true);
    void sync_region(double lat_deg, double lon_deg);
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
    WeatherDebugSlice debug_slice(double east_m, double north_m, int level) const;
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

    void upload_field();
    void live_weather_worker();
    void initialize_weather_model();
    void advance_weather_model(float dt, const Vec3 &sun_dir, const WeatherSnapshot &forcing);
    void weather_worker_loop();
    void start_weather_worker();
    void stop_weather_worker();
    void publish_weather_model();
    void refresh_climate_field();
    void seed_dynamic_cells();
    void spawn_dynamic_cell(DynamicCell &c, bool initial_spread);
    void advance_dynamic_cells(float dt, float solar);
    void evaluate_dynamic_cells(double east_m, double north_m, double agl_m,
                                float &out_cloud_add, float &out_rain_add, float &out_w_add,
                                float &out_outflow_e, float &out_outflow_n, float &out_tke_add) const;
    SimWeatherCell sample_weather_model(double east_m, double north_m, double agl_m) const;

    const PackReader *pack_ = nullptr;
    WindRegime regime_ = WindRegime::PYRENEAN_THERMAL;
    bool gpu_enabled_ = false;
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
    WeatherSnapshot live_target_{};
    WeatherSnapshot synoptic_weather_{}; // Baked/observed boundary conditions fed into the runtime solve
    WeatherSnapshot active_weather_{};
    bool active_weather_init_ = false;
    bool manual_weather_override_ = false;
    double active_weather_time_ = -1.0;
    double last_live_field_upload_ = -999.0;
    std::vector<float> field_upload_;
    std::vector<float> terrain_upload_;
    std::vector<float> climate_field_;
    std::vector<SimWeatherCell> weather_grid_;
    std::vector<SimWeatherCell> weather_next_;
    mutable std::mutex weather_publish_mtx_;
    std::vector<SimWeatherCell> weather_published_;
    std::vector<SimWeatherCell> weather_staging_;
    std::vector<float> field_published_;
    std::vector<float> field_staging_;
    WeatherSnapshot weather_published_snapshot_{};
    uint64_t weather_generation_ = 0;
    uint64_t uploaded_weather_generation_ = 0;
    uint64_t field_uploads_ = 0;
    std::atomic<uint64_t> weather_steps_{0}, weather_solve_us_{0}, weather_publish_us_{0};
    std::vector<float> weather_rain_flux_;
    std::vector<float> terrain_height_;
    std::vector<float> terrain_slope_e_;
    std::vector<float> terrain_slope_n_;
    std::vector<float> terrain_ridge_exposure_;
    std::vector<float> terrain_valley_basin_;
    bool weather_model_ready_ = false;
    double weather_model_time_ = -1.0;
    double weather_model_accum_ = 0.0;
    double weather_anchor_lat_ = 0.0, weather_anchor_lon_ = 0.0;
    std::thread weather_worker_;
    std::mutex weather_job_mtx_;
    std::condition_variable weather_job_cv_;
    std::atomic<bool> weather_worker_stop_{false};
    int weather_pending_steps_ = 0;
    bool weather_pending_climate_ = false;
    std::vector<float> weather_pending_climate_field_;
    std::vector<float> weather_worker_climate_field_;
    WeatherSnapshot weather_worker_forcing_{};
    WeatherSnapshot weather_worker_active_{};
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
    std::array<DynamicCell, 6> dyn_cells_{};
    std::array<DynamicCell, 6> dyn_cells_published_{};
    int dyn_cell_count_ = 0;
    int dyn_cell_count_published_ = 0;
    double dyn_cell_time_ = 0.0;
    std::mt19937 solver_rng_{};
    bool solver_rng_seeded_ = false;
    mutable StochasticDrydenFilter dryden_filter_{};
};

} // namespace nsim
