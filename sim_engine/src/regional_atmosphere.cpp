#include "regional_atmosphere.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <chrono>
#include <cstddef>
#include <utility>
#if defined(__AVX2__)
#include <immintrin.h>
#endif

namespace nsim {

namespace {

inline float smooth01(float e0, float e1, float x) {
    float d = e1 - e0;
    float t = std::abs(d) > 1e-5f ? std::clamp((x - e0) / d, 0.0f, 1.0f) : (x >= e1 ? 1.0f : 0.0f);
    return t * t * (3.0f - 2.0f * t);
}

inline float saturation_vapor_pressure_hpa(float temp_c) {
    const float t = std::clamp(temp_c, -75.0f, 55.0f);
    return 6.112f * std::exp(17.67f * t / (t + 243.5f));
}

inline float saturation_mixing_ratio_gkg(float temp_c, float pressure_hpa) {
    const float e = saturation_vapor_pressure_hpa(temp_c);
    return std::clamp(621.97f * e / std::max(120.0f, pressure_hpa - 0.378f * e), 0.02f, 40.0f);
}

bool json_number(const char *json, const char *key, double &value) {
    char needle[96];
    std::snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = json;
    while ((p = std::strstr(p, needle)) != nullptr) {
        const char *colon = std::strchr(p + std::strlen(needle), ':');
        if (!colon) return false;
        char *end = nullptr;
        double candidate = std::strtod(colon + 1, &end);
        if (end != colon + 1 && std::isfinite(candidate)) {
            value = candidate;
            return true;
        }
        p += std::strlen(needle);
    }
    return false;
}

double json_or(const char *json, const char *key, double fallback) {
    double v = fallback;
    return json_number(json, key, v) ? v : fallback;
}

std::string json_object_text(const char *json, const char *key) {
    char needle[96];
    std::snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = std::strstr(json, needle);
    if (!p) return {};
    p = std::strchr(p + std::strlen(needle), ':');
    if (!p) return {};
    while (*++p && std::isspace(static_cast<unsigned char>(*p))) {}
    if (*p != '{') return {};
    const char *begin = p;
    int depth = 0;
    bool quoted = false, escaped = false;
    for (; *p; ++p) {
        if (quoted) {
            if (escaped) escaped = false;
            else if (*p == '\\') escaped = true;
            else if (*p == '"') quoted = false;
            continue;
        }
        if (*p == '"') quoted = true;
        else if (*p == '{') ++depth;
        else if (*p == '}' && --depth == 0) return std::string(begin, p + 1);
    }
    return {};
}

WeatherSnapshot parse_live_weather(const char *json, double elevation_m) {
    WeatherSnapshot w{};
    const std::string current = json_object_text(json, "current");
    const char *source = current.empty() ? json : current.c_str();
    const double temp = json_or(source, "temperature_2m", 16.0);
    const double dew = json_or(source, "dew_point_2m", temp - 7.0);
    const double cover = std::clamp(json_or(source, "cloud_cover", 35.0), 0.0, 100.0);
    const double mm = std::max(json_or(source, "precipitation", 0.0),
                               json_or(source, "rain", 0.0) + json_or(source, "showers", 0.0));
    const int code = static_cast<int>(json_or(source, "weather_code", 0.0));
    float precip = static_cast<float>(std::clamp(mm / 1.2, 0.0, 1.0));
    if (code >= 95) precip = std::max(precip, 0.78f);
    else if (code >= 80) precip = std::max(precip, 0.52f);
    else if (code >= 51) precip = std::max(precip, 0.18f);
    w.cloud_cov = static_cast<float>(cover / 100.0);
    w.precip = precip;
    w.precip_mm_hr = static_cast<float>(std::clamp(mm, 0.0, 120.0));
    // Open-Meteo temperature_2m/dew_point_2m are local surface values. The
    // simulator stores sea-level references and applies elevation per column.
    w.oat_sl_c = static_cast<float>(temp + elevation_m * 0.0065);
    w.dewpoint_sl_c = static_cast<float>(dew + elevation_m * 0.0017);
    w.qnh_hpa = static_cast<float>(std::clamp(json_or(source, "pressure_msl", 1015.0), 940.0, 1060.0));
    w.wind_dir_from_deg = static_cast<float>(std::fmod(json_or(source, "wind_direction_10m", 270.0) + 360.0, 360.0));
    w.wind_speed_10m_mps = static_cast<float>(std::clamp(json_or(source, "wind_speed_10m", 4.0), 0.0, 45.0));
    w.cloud_base_m = static_cast<float>(std::clamp(125.0 * std::max(2.0, temp - dew), 350.0, 3400.0)); // LCL AGL estimate
    w.cloud_thick_m = std::lerp(850.0f, 3000.0f, std::clamp(0.56f * w.cloud_cov + 0.74f * precip, 0.0f, 1.0f));
    w.storm_int = std::clamp(std::max(precip, code >= 95 ? 0.80f : 0.0f), 0.0f, 1.0f);
    w.turb_scale = std::clamp(0.18f + w.wind_speed_10m_mps / 38.0f + w.storm_int * 0.40f, 0.15f, 1.0f);
    w.visibility_m = std::clamp(40000.0f - w.cloud_cov * 7000.0f - precip * 27000.0f, 900.0f, 40000.0f);
    w.haze_mult = std::clamp(1.0f + w.cloud_cov * 0.30f + precip * 0.55f, 0.8f, 2.0f);
    w.label = "LIVE OPEN-METEO | " + std::to_string(static_cast<int>(cover)) + "% cloud";
    w.summary = "OPEN-METEO CURRENT | WMO " + std::to_string(code) + " | T " +
                std::to_string(static_cast<int>(std::round(temp))) + "C";
    return w;
}

} // namespace

RegionalAtmosphere::~RegionalAtmosphere() {
    shutdown();
}

bool RegionalAtmosphere::init(const PackReader *pack, bool create_gpu_tex, const std::string &weather_path, bool enable_live_weather) {
    shutdown();
    pack_ = pack;
    gpu_enabled_ = create_gpu_tex;
    if (!pack_) return false;
    min_e_ = pack_->corridor_min_e();
    min_n_ = pack_->corridor_min_n();
    span_e_ = pack_->corridor_span_e();
    span_n_ = pack_->corridor_span_n();
    live_lat_ = pack_->anchor_lat();
    live_lon_ = pack_->anchor_lon();
    if (const auto *region = pack_->find_region_at(live_lat_, live_lon_)) active_region_id_ = region->region_id;
    else active_region_id_.clear();
    live_elevation_m_ = pack_->sample_elevation(live_lat_, live_lon_);
    last_model_history_sec_ = -1.0;
    model_history_.clear();
    live_last_success_utc_.store(0, std::memory_order_release);
    live_overlay_applied_ = false;

    std::string wpath = weather_path.empty() ? pack_->active_weather_path(pack_->anchor_lat(), pack_->anchor_lon()) : weather_path;
    if (gpu_enabled_) {
        glCreateTextures(GL_TEXTURE_2D_ARRAY, 1, &field_tex_);
        glTextureStorage3D(field_tex_, 1, GL_RGBA16F, GRID_W, GRID_H, nspain::NSWX_LEVELS + 1);
        glTextureParameteri(field_tex_, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTextureParameteri(field_tex_, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri(field_tex_, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(field_tex_, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTextureParameteri(field_tex_, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
        glCreateTextures(GL_TEXTURE_2D, 1, &terrain_field_tex_);
        glTextureStorage2D(terrain_field_tex_, 1, GL_R32F, GRID_W, GRID_H);
        glTextureParameteri(terrain_field_tex_, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTextureParameteri(terrain_field_tex_, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri(terrain_field_tex_, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(terrain_field_tex_, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    if (!wpath.empty() && lib_.load(wpath)) {
        loaded_wx_path_ = wpath;
        std::printf("[WX] %s | %d baked states | %s\n", wpath.c_str(),
                    nspain::NSWX_STATE_COUNT, lib_.provenance());
    } else {
        loaded_wx_path_.clear();
        std::printf("[WX] No .nswx weather pack for region — using lightweight synoptic default.\n");
    }
    dirty_ = true;
    manual_weather_override_ = false;
    if (solver_seed_) {
        dryden_filter_ = StochasticDrydenFilter{};
        dryden_filter_.set_seed(*solver_seed_);
    }
    active_weather_init_ = false;
    active_weather_time_ = -1.0;
    update_field(normalize(Vec3(0.86f, 0.32f, 0.38f)), 0.0, true, 0.28f);
    live_enabled_ = enable_live_weather;
    live_stop_.store(false, std::memory_order_release);
    if (live_enabled_) live_thread_ = std::thread(&RegionalAtmosphere::live_weather_worker, this);
    return true;
}

void RegionalAtmosphere::sync_region(double lat_deg, double lon_deg) {
    if (!pack_) return;
    const double min_e = pack_->corridor_min_e(), min_n = pack_->corridor_min_n();
    const double span_e = pack_->corridor_span_e(), span_n = pack_->corridor_span_n();
    // Weather follows the committed world frame, rather than swapping a
    // climatology underneath an unrelated terrain domain mid-flight.
    const auto *region = pack_->find_region_at(pack_->anchor_lat(), pack_->anchor_lon());
    const std::string region_id = region ? region->region_id : "";
    const bool region_changed = region_id != active_region_id_;
    {
        std::lock_guard<std::mutex> lk(live_mtx_);
        if (region_changed || std::abs(live_lat_ - lat_deg) > 0.015 || std::abs(live_lon_ - lon_deg) > 0.015) {
            live_lat_ = lat_deg;
            live_lon_ = lon_deg;
            live_elevation_m_ = pack_->sample_elevation(lat_deg, lon_deg);
            live_force_fetch_ = true;
            ++live_request_id_;
        }
        if (region_changed) {
            live_available_.store(false, std::memory_order_release);
            live_last_success_utc_.store(0, std::memory_order_release);
        }
    }
    live_cv_.notify_one();
    const std::string next_p = region ? region->weather_path : "";
    if (region_changed || next_p != loaded_wx_path_) {
        active_region_id_ = region_id;
        loaded_wx_path_.clear();
        lib_.shutdown();
        if (!next_p.empty() && lib_.load(next_p)) {
            loaded_wx_path_ = next_p;
            std::printf("[WX] Switched active weather library -> %s\n", next_p.c_str());
        }
        active_weather_init_ = false;
        live_overlay_applied_ = false;
        synoptic_weather_ = {};
        last_state_ = -1;
        dirty_ = true;
    }
    if (region_changed || min_e != min_e_ || min_n != min_n_ || span_e != span_e_ || span_n != span_n_ ||
        pack_->anchor_lat() != weather_anchor_lat_ || pack_->anchor_lon() != weather_anchor_lon_) {
        stop_weather_worker();
        min_e_ = min_e;
        min_n_ = min_n;
        span_e_ = span_e;
        span_n_ = span_n;
        dirty_ = true;
        weather_model_ready_ = false;
        weather_model_time_ = -1.0;
        weather_model_accum_ = 0.0;
    }
}

void RegionalAtmosphere::shutdown() {
    stop_weather_worker();
    live_stop_.store(true, std::memory_order_release);
    live_cv_.notify_all();
    if (live_thread_.joinable()) live_thread_.join();
    live_enabled_ = false;
    live_available_.store(false, std::memory_order_release);
    live_last_success_utc_.store(0, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(live_mtx_);
    }
    live_overlay_applied_ = false;
    if (field_tex_) { glDeleteTextures(1, &field_tex_); field_tex_ = 0; }
    if (terrain_field_tex_) { glDeleteTextures(1, &terrain_field_tex_); terrain_field_tex_ = 0; }
    lib_.shutdown();
    weather_model_ready_ = false;
    weather_model_time_ = -1.0;
    weather_model_accum_ = 0.0;
    solver_ = {};
    frame_weather_ = {};
    pending_weather_ = {};
    staging_weather_ = {};
}

void RegionalAtmosphere::live_weather_worker() {
    while (!live_stop_.load(std::memory_order_acquire)) {
        double lat = 0.0, lon = 0.0, elevation_m = 0.0;
        uint64_t request_id = 0;
        {
            std::unique_lock<std::mutex> lk(live_mtx_);
            if (live_stop_.load(std::memory_order_acquire)) break;
            live_force_fetch_ = false;
            lat = live_lat_;
            lon = live_lon_;
            elevation_m = live_elevation_m_;
            request_id = live_request_id_;
        }

        char url[1200];
        std::snprintf(url, sizeof(url),
            "https://api.open-meteo.com/v1/forecast?latitude=%.5f&longitude=%.5f"
            "&current=temperature_2m,relative_humidity_2m,dew_point_2m,precipitation,rain,showers,weather_code,cloud_cover,pressure_msl,wind_speed_10m,wind_direction_10m,wind_gusts_10m"
            "&wind_speed_unit=ms&timezone=UTC", lat, lon);
        char cmd[1500];
        std::snprintf(cmd, sizeof(cmd), "curl -sS -f -L --connect-timeout 3 --max-time 8 '%s' 2>/dev/null", url);
        FILE *pipe = popen(cmd, "r");
        std::array<char, 8192> response{};
        size_t used = 0;
        bool success = false;
        if (pipe) {
            while (used + 1 < response.size()) {
                size_t got = std::fread(response.data() + used, 1, response.size() - used - 1, pipe);
                used += got;
                if (!got) break;
            }
            int status = pclose(pipe);
            success = status == 0 && used > 32 && std::strstr(response.data(), "\"current\"");
            if (success) {
                const int64_t refreshed = static_cast<int64_t>(std::time(nullptr));
                WeatherSnapshot fresh = parse_live_weather(response.data(), elevation_m);
                {
                    std::lock_guard<std::mutex> lk(live_mtx_);
                    if (request_id == live_request_id_) {
                        live_target_ = std::move(fresh);
                        live_last_success_utc_.store(refreshed, std::memory_order_release);
                        live_available_.store(true, std::memory_order_release);
                        std::printf("[WX] Open-Meteo current conditions refreshed for %.3f, %.3f\n", lat, lon);
                    }
                }
            } else {
                std::fprintf(stderr, "[WX] Open-Meteo fetch failed; retaining the last available weather.\n");
            }
        }
        std::unique_lock<std::mutex> lk(live_mtx_);
        live_cv_.wait_for(lk, std::chrono::minutes(success ? 10 : 3), [&]() {
            return live_stop_.load(std::memory_order_acquire) || live_force_fetch_;
        });
    }
}

void RegionalAtmosphere::set_regime(WindRegime r) {
    if (r != regime_) { regime_ = r; dirty_ = true; }
}

void RegionalAtmosphere::cycle_regime() {
    set_regime(static_cast<WindRegime>((static_cast<int>(regime_) + 1) % static_cast<int>(WindRegime::COUNT)));
}

const char *RegionalAtmosphere::regime_name() const {
    if (regime_ == WindRegime::WINTER_ICING) return "Pyrenean Winter Icing & Snow";
    return WeatherLibrary::type_name(static_cast<int>(regime_));
}
const char *RegionalAtmosphere::regime_short() const {
    switch (regime_) {
        case WindRegime::PYRENEAN_THERMAL: return "THERMAL";
        case WindRegime::CIERZO_NW:        return "CIERZO NW";
        case WindRegime::FOEHN_SUR:        return "FOEHN SUR";
        case WindRegime::KATABATIC_DAWN:   return "DAWN FOG";
        case WindRegime::CONVECTIVE_STORM: return "STORM CELL";
        case WindRegime::WINTER_ICING:     return "WINTER ICE";
        default:                           return "VFR";
    }
}

namespace {

uint32_t hardware_entropy_seed(const void *salt) {
    std::random_device rd;
    uint64_t t = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    uint64_t p = reinterpret_cast<uintptr_t>(salt);
    uint64_t h = (static_cast<uint64_t>(rd()) << 32) ^ static_cast<uint64_t>(rd()) ^ t ^ (p * 0x9e3779b97f4a7c15ULL);
    h ^= h >> 30; h *= 0xbf58476d1ce4e5b9ULL;
    h ^= h >> 27; h *= 0x94d049bb133111ebULL;
    return static_cast<uint32_t>(h ^ (h >> 31));
}

} // namespace

StochasticDrydenFilter::StochasticDrydenFilter() : rng_(hardware_entropy_seed(this)) {}

DrydenGustSample StochasticDrydenFilter::step(double dt_sec, double tas_mps, double agl_m,
                                              double sigma_u, double sigma_v, double sigma_w,
                                              double wingspan_m) {
    const double dt = std::clamp(dt_sec, 1e-4, 0.05);
    const double va = std::max(10.0, tas_mps);
    const double h = std::max(6.0, agl_m);
    const double b = std::max(4.0, wingspan_m);

    // MIL-F-8785C altitude-dependent integral length scales (boundary layer -> 533 m free atmosphere).
    const double low_blend = std::clamp((h - 305.0) / 305.0, 0.0, 1.0);
    const double lw_low = std::max(8.0, h);
    const double lu_low = h / std::pow(0.177 + 0.0027 * h, 1.2);
    const double lw = std::lerp(lw_low, 533.0, low_blend);
    const double lu = std::lerp(std::clamp(lu_low, 12.0, 533.0), 533.0, low_blend);
    const double lv = lu;

    // 1st-order Dryden longitudinal gust u_g (exact OU transition, preserves variance sigma_u^2).
    const double tau_u = std::max(0.25, lu / va);
    const double a_u = std::exp(-dt / tau_u);
    u_s_ = a_u * u_s_ + std::sqrt(std::max(0.0, 1.0 - a_u * a_u)) * norm_(rng_);

    // Critically-damped 2nd-order Dryden lateral & vertical gusts (cascaded OU -> exact C^1 variance = sigma^2).
    const double om_v = std::sqrt(3.0) * va / std::max(12.0, lv);
    const double a_v = std::exp(-dt * om_v);
    v_s1_ = a_v * v_s1_ + std::sqrt(std::max(0.0, 1.0 - a_v * a_v)) * norm_(rng_);
    v_s2_ = a_v * v_s2_ + (1.0 - a_v) * v_s1_;

    const double om_w = std::sqrt(3.0) * va / std::max(8.0, lw);
    const double a_w = std::exp(-dt * om_w);
    w_s1_ = a_w * w_s1_ + std::sqrt(std::max(0.0, 1.0 - a_w * a_w)) * norm_(rng_);
    w_s2_ = a_w * w_s2_ + (1.0 - a_w) * w_s1_;

    const double ug = std::clamp(sigma_u * u_s_, -18.0, 18.0);
    const double vg = std::clamp(sigma_v * std::sqrt(2.0) * v_s2_, -18.0, 18.0);
    const double wg = std::clamp(sigma_w * std::sqrt(2.0) * w_s2_, -14.0, 14.0);

    // MIL-F-8785C wingspan-filtered rolling gust rate p_g + aerodynamic pitch/yaw eddy rates from C^1 dw/dt, dv/dt.
    const double tau_p = std::max(0.14, (4.0 * b) / (PI_D * va));
    const double a_p = std::exp(-dt / tau_p);
    p_s_ = a_p * p_s_ + std::sqrt(std::max(0.0, 1.0 - a_p * a_p)) * norm_(rng_);
    const double sigma_p = (sigma_w / b) * 0.72 * std::pow(std::max(0.25, (PI_D * lw) / (4.0 * b)), -1.0 / 6.0);
    const double pg = std::clamp(sigma_p * p_s_, -0.55, 0.55);

    const double dw_dt = (wg - prev_wg_) / dt;
    const double dv_dt = (vg - prev_vg_) / dt;
    prev_wg_ = wg;
    prev_vg_ = vg;
    // Low-pass over airframe length (~8 m) into angular gust rates.
    const double a_qr = std::exp(-dt * (va / 9.0));
    const double qg_raw = std::clamp(-dw_dt / va * 0.65, -0.35, 0.35);
    const double rg_raw = std::clamp(dv_dt / va * 0.50, -0.30, 0.30);
    last_.qg_rps = std::lerp(qg_raw, last_.qg_rps, a_qr);
    last_.rg_rps = std::lerp(rg_raw, last_.rg_rps, a_qr);
    last_.ug_mps = ug;
    last_.vg_mps = vg;
    last_.wg_mps = wg;
    last_.pg_rps = pg;
    return last_;
}

void RegionalAtmosphere::WeatherSolver::spawn_dynamic_cell(DynamicCell &c, bool initial_spread) {
    std::uniform_real_distribution<float> u01(0.0f, 1.0f);
    const bool storm = (regime_ == WindRegime::CONVECTIVE_STORM) || (synoptic_weather_.storm_int > 0.48f);
    const bool winter = (regime_ == WindRegime::WINTER_ICING);
    const bool foehn = (regime_ == WindRegime::FOEHN_SUR);
    const size_t cells = static_cast<size_t>(GRID_W * GRID_H);
    const size_t l2_off = 2 * cells;

    // Stochastically pick the most convectively unstable / moist point among 8 random candidates in the domain.
    float best_score = -1e9f;
    int best_x = GRID_W / 2, best_y = GRID_H / 2;
    for (int k = 0; k < 8; ++k) {
        int rx = 12 + static_cast<int>(u01(solver_rng_) * (GRID_W - 24));
        int ry = 12 + static_cast<int>(u01(solver_rng_) * (GRID_H - 24));
        size_t idx = static_cast<size_t>(ry) * GRID_W + rx;
        float score = u01(solver_rng_) * 2.2f;
        if (idx < terrain_ridge_exposure_.size()) score += terrain_ridge_exposure_[idx] * 2.4f;
        if (weather_grid_.size() >= (l2_off + cells)) {
            score += weather_grid_[idx].vapor_gkg * 0.25f + std::max(0.0f, weather_grid_[cells + idx].w) * 1.4f;
        }
        if (score > best_score) {
            best_score = score;
            best_x = rx;
            best_y = ry;
        }
    }
    c.east_m = static_cast<float>(min_e_ + span_e_ * best_x / (GRID_W - 1.0));
    c.north_m = static_cast<float>(min_n_ + span_n_ * best_y / (GRID_H - 1.0));
    if (weather_grid_.size() >= (l2_off + cells)) {
        const auto &sc = weather_grid_[l2_off + static_cast<size_t>(best_y) * GRID_W + best_x];
        c.vel_e = sc.u;
        c.vel_n = sc.v;
    } else {
        const double dir = synoptic_weather_.wind_dir_from_deg * DEG2RAD_D;
        c.vel_e = static_cast<float>(-std::sin(dir) * std::clamp(synoptic_weather_.wind_speed_10m_mps * 1.35f, 6.0f, 24.0f));
        c.vel_n = static_cast<float>(-std::cos(dir) * std::clamp(synoptic_weather_.wind_speed_10m_mps * 1.35f, 6.0f, 24.0f));
    }
    c.radius_m = (7200.0f + u01(solver_rng_) * 6200.0f) * (winter ? 1.30f : 1.0f);
    c.phase = initial_spread ? (0.14f + u01(solver_rng_) * 0.72f) : 0.02f;
    c.period_s = 540.0f + u01(solver_rng_) * 540.0f;
    const float regime_scale = storm ? 1.15f : (winter ? 0.62f : (foehn ? 0.48f : 0.24f));
    c.peak_rain = (18.0f + u01(solver_rng_) * 16.0f) * regime_scale;
    c.peak_updraft = (3.6f + u01(solver_rng_) * 2.8f) * (storm ? 1.15f : (winter ? 0.55f : 0.72f));
    c.lightning_flash = 0.0f;
    c.lightning_cooldown = 2.0f + u01(solver_rng_) * 8.0f;
}

void RegionalAtmosphere::WeatherSolver::seed_dynamic_cells() {
    const bool storm = (regime_ == WindRegime::CONVECTIVE_STORM) || (synoptic_weather_.storm_int > 0.48f);
    const bool winter = (regime_ == WindRegime::WINTER_ICING);
    const bool foehn = (regime_ == WindRegime::FOEHN_SUR);
    dyn_cell_count_ = storm ? 6 : (winter ? 5 : (foehn ? 4 : 3));
    for (int i = 0; i < 6; ++i) spawn_dynamic_cell(dyn_cells_[i], true);
}

void RegionalAtmosphere::WeatherSolver::advance_dynamic_cells(float dt, float solar) {
    if (dyn_cell_count_ <= 0 || weather_grid_.empty()) return;
    std::uniform_real_distribution<float> u01(0.0f, 1.0f);
    const size_t cells = static_cast<size_t>(GRID_W * GRID_H);
    const size_t l1_off = cells, l2_off = 2 * cells;
    const float span_e = static_cast<float>(std::max(10000.0, span_e_));
    const float span_n = static_cast<float>(std::max(10000.0, span_n_));

    for (int i = 0; i < dyn_cell_count_; ++i) {
        auto &c = dyn_cells_[i];
        // Sample real mid-tropospheric steering wind (lev=2, 1800m AGL) directly from the solved PDE grid!
        int gx = std::clamp(static_cast<int>((c.east_m - min_e_) / span_e * (GRID_W - 1)), 1, GRID_W - 2);
        int gy = std::clamp(static_cast<int>((c.north_m - min_n_) / span_n * (GRID_H - 1)), 1, GRID_H - 2);
        size_t idx = static_cast<size_t>(gy) * GRID_W + gx;
        const auto &steer = weather_grid_[l2_off + idx];
        const auto &conv = weather_grid_[l1_off + idx];
        c.vel_e = std::lerp(c.vel_e, steer.u, std::clamp(dt * 0.25f, 0.0f, 0.5f));
        c.vel_n = std::lerp(c.vel_n, steer.v, std::clamp(dt * 0.25f, 0.0f, 0.5f));
        c.east_m += c.vel_e * dt;
        c.north_m += c.vel_n * dt;

        // Instability & ridge feedback: moist updrafts sustain cells; dry Foehn descent decays them faster!
        const float qsat = saturation_mixing_ratio_gkg(conv.temp_c, conv.pressure_hpa);
        const float rh = conv.vapor_gkg / std::max(0.1f, qsat);
        const float growth_mod = std::clamp(1.18f - rh * 0.35f - std::max(0.0f, conv.w) * 0.08f - solar * 0.08f, 0.55f, 1.55f);
        c.phase += (dt / std::max(240.0f, c.period_s)) * growth_mod;

        const bool out_of_domain = (c.east_m < min_e_ - c.radius_m || c.east_m > min_e_ + span_e + c.radius_m ||
                                    c.north_m < min_n_ - c.radius_m || c.north_m > min_n_ + span_n + c.radius_m);
        if (c.phase >= 1.0f || out_of_domain) {
            spawn_dynamic_cell(c, false);
            continue;
        }

        // True Poisson stochastic lightning driven by cell maturity & rain/updraft charge separation!
        const float life = std::sin(c.phase * PI_F);
        c.lightning_flash = std::max(0.0f, c.lightning_flash - dt * 3.8f);
        c.lightning_cooldown = std::max(0.0f, c.lightning_cooldown - dt);
        const float eff_rain = c.peak_rain * life;
        if (eff_rain > 11.0f && c.lightning_cooldown <= 0.0f) {
            const float rate_hz = std::clamp((eff_rain - 10.0f) * 0.012f, 0.02f, 0.22f);
            if (u01(solver_rng_) < (1.0f - std::exp(-rate_hz * dt))) {
                c.lightning_flash = 0.72f + u01(solver_rng_) * 0.28f;
                c.lightning_cooldown = 2.5f + u01(solver_rng_) * 9.5f;
            }
        }
    }
}

void RegionalAtmosphere::evaluate_dynamic_cells(
    double east_m, double north_m, double agl_m,
    float &out_cloud_add, float &out_rain_add, float &out_w_add,
    float &out_outflow_e, float &out_outflow_n, float &out_tke_add
) const {
    out_cloud_add = out_rain_add = out_w_add = out_outflow_e = out_outflow_n = out_tke_add = 0.0f;
    if (!has_model()) return;
    const auto &snap = frame_weather_.cells;
    const int count = frame_weather_.cell_count;
    if (count <= 0) return;
    const float base_severity = std::clamp(
        std::max(frame_forcing().storm_int, frame_forcing().precip) * 0.75f +
        ((frame_regime() == WindRegime::CONVECTIVE_STORM) ? 0.65f :
         (frame_regime() == WindRegime::WINTER_ICING) ? 0.55f :
         (frame_regime() == WindRegime::FOEHN_SUR) ? 0.30f : 0.14f), 0.05f, 1.25f);

    for (int i = 0; i < count; ++i) {
        const auto &c = snap[i];
        const float de = static_cast<float>(east_m) - c.east_m;
        const float dn = static_cast<float>(north_m) - c.north_m;
        const float r2 = de * de + dn * dn;
        const float max_r = c.radius_m * 1.45f;
        if (r2 > max_r * max_r) continue;

        const float life = std::sin(std::clamp(c.phase, 0.0f, 1.0f) * PI_F);
        if (life < 0.06f) continue;

        const float dist = std::sqrt(r2);
        const float core = std::exp(-r2 / std::max(1e6f, c.radius_m * c.radius_m * 0.42f));
        const float env = smooth01(max_r, c.radius_m * 0.35f, dist) * life * base_severity;

        out_cloud_add += env * 0.52f;
        const float rain_h = 1.0f - smooth01(2200.0f, 4200.0f, static_cast<float>(agl_m));
        const float rain_here = c.peak_rain * core * life * base_severity * rain_h;
        out_rain_add += rain_here;

        // Mature cells have strong updraft on leading edge + cold rain-cooled downdraft core!
        const float downdraft = smooth01(0.45f, 0.85f, c.phase) * core * c.peak_updraft * 0.95f * base_severity;
        const float updraft = (1.0f - smooth01(0.55f, 0.92f, c.phase)) * env * c.peak_updraft * 0.85f;
        out_w_add += (updraft - downdraft) * (1.0f - smooth01(3400.0f, 5200.0f, static_cast<float>(agl_m)));

        // Low-level cold-pool radial outflow gust front beneath rain shaft (< 950m AGL)
        if (agl_m < 950.0 && dist > 120.0f && rain_here > 1.5f) {
            const float ring = smooth01(0.0f, c.radius_m * 0.55f, dist) * (1.0f - smooth01(c.radius_m * 0.65f, max_r, dist));
            const float gust = ring * std::min(11.0f, rain_here * 0.38f) * (1.0f - static_cast<float>(agl_m) / 950.0f);
            out_outflow_e += (de / dist) * gust;
            out_outflow_n += (dn / dist) * gust;
        }
        out_tke_add += env * (0.28f + std::min(0.55f, rain_here * 0.025f));
    }
}

float RegionalAtmosphere::lightning_flash_at(double east_m, double north_m, double /*sim_time_sec*/) const {
    if (frame_regime() != WindRegime::CONVECTIVE_STORM && frame_forcing().storm_int < 0.45f && active_weather_.precip < 0.45f)
        return 0.0f;
    if (!has_model()) return 0.0f;
    const auto &snap = frame_weather_.cells;
    const int count = frame_weather_.cell_count;
    float flash = 0.0f;
    for (int i = 0; i < count; ++i) {
        const auto &c = snap[i];
        if (c.lightning_flash <= 0.01f) continue;
        const float dist = std::hypot(static_cast<float>(east_m) - c.east_m, static_cast<float>(north_m) - c.north_m);
        if (dist > 38000.0f) continue;
        const float prox = 1.0f - smooth01(6000.0f, 38000.0f, dist);
        flash = std::max(flash, c.lightning_flash * prox);
    }
    return std::clamp(flash, 0.0f, 1.0f);
}

void RegionalAtmosphere::upload_field() {
    if (!gpu_enabled_ || !field_tex_ || !has_model()) return;
    const auto &field = frame_weather_.field;
    if (frame_weather_.generation == uploaded_weather_generation_ || field.empty()) return;
    uploaded_weather_generation_ = frame_weather_.generation;
    glTextureSubImage3D(field_tex_, 0, 0, 0, 0, GRID_W, GRID_H, nspain::NSWX_LEVELS + 1,
                        GL_RGBA, GL_FLOAT, field.data());
    ++field_uploads_;
}

void RegionalAtmosphere::refresh_climate_field() {
    const int sw = nspain::NSWX_GRID_W, sh = nspain::NSWX_GRID_H;
    const auto &src = lib_.field_rgba();
    const size_t cells = static_cast<size_t>(GRID_W) * GRID_H;
    climate_field_.assign(cells * 4, 0.0f);
    if (!lib_.valid() || src.size() != static_cast<size_t>(sw) * sh * 4) return;
    for (int y = 0; y < GRID_H; ++y) {
        const float gy = static_cast<float>(y) * (sh - 1) / (GRID_H - 1);
        const int y0 = std::min(sh - 2, static_cast<int>(gy));
        const float ty = gy - y0;
        for (int x = 0; x < GRID_W; ++x) {
            const float gx = static_cast<float>(x) * (sw - 1) / (GRID_W - 1);
            const int x0 = std::min(sw - 2, static_cast<int>(gx));
            const float tx = gx - x0;
            const size_t o = (static_cast<size_t>(y) * GRID_W + x) * 4;
            const size_t a = (static_cast<size_t>(y0) * sw + x0) * 4;
            const size_t b = a + 4;
            const size_t c = a + static_cast<size_t>(sw) * 4;
            const size_t d = c + 4;
            for (int k = 0; k < 4; ++k) {
                const float top = std::lerp(src[a + k], src[b + k], tx);
                const float bot = std::lerp(src[c + k], src[d + k], tx);
                climate_field_[o + k] = std::lerp(top, bot, ty);
            }
        }
    }
}

void RegionalAtmosphere::publish_weather_model() {
    if (solver_.weather_grid_.empty()) return;
    staging_weather_.grid = solver_.weather_grid_;
    if (gpu_enabled_) {
        const size_t cells = static_cast<size_t>(GRID_W * GRID_H);
        const size_t rain_offset = cells * nspain::NSWX_LEVELS * 4;
        staging_weather_.field.resize(rain_offset + cells * 4);
        const float de = static_cast<float>(solver_.span_e_ / (GRID_W - 1.0));
        const float dn = static_cast<float>(solver_.span_n_ / (GRID_H - 1.0));
        const bool dawn_fog = (solver_.regime_ == WindRegime::KATABATIC_DAWN);
        const float base_sev = std::clamp(
            std::max(solver_.synoptic_weather_.storm_int, solver_.synoptic_weather_.precip) * 0.75f +
            ((solver_.regime_ == WindRegime::CONVECTIVE_STORM) ? 0.65f :
             (solver_.regime_ == WindRegime::WINTER_ICING) ? 0.55f :
             (solver_.regime_ == WindRegime::FOEHN_SUR) ? 0.30f : 0.14f), 0.05f, 1.25f);
        struct ActiveCell { float e, n, r2_inv, max_r2, rain, cloud, up, down; };
        ActiveCell ac[6];
        int nac = 0;
        for (int ci = 0; ci < solver_.dyn_cell_count_; ++ci) {
            const auto &c = solver_.dyn_cells_[ci];
            const float life = std::sin(std::clamp(c.phase, 0.0f, 1.0f) * PI_F);
            if (life < 0.06f) continue;
            const float max_r = c.radius_m * 1.45f;
            ac[nac++] = {c.east_m, c.north_m, 1.0f / std::max(1e6f, c.radius_m * c.radius_m * 0.42f), max_r * max_r,
                         c.peak_rain * life * base_sev, 0.52f * life * base_sev,
                         (1.0f - smooth01(0.55f, 0.92f, c.phase)) * c.peak_updraft * 0.85f * life * base_sev,
                         smooth01(0.45f, 0.85f, c.phase) * c.peak_updraft * 0.95f * base_sev};
        }
        for (int y = 0; y < GRID_H; ++y) {
            const float north = static_cast<float>(solver_.min_n_) + y * dn;
            for (int x = 0; x < GRID_W; ++x) {
                const size_t i = static_cast<size_t>(y) * GRID_W + x;
                const float east = static_cast<float>(solver_.min_e_) + x * de;
                const float basin = solver_.terrain_valley_basin_[i];
                float cell_cloud = 0.0f, cell_rain = 0.0f, cell_w = 0.0f;
                for (int ci = 0; ci < nac; ++ci) {
                    const float dx_c = east - ac[ci].e, dy_c = north - ac[ci].n;
                    const float r2 = dx_c * dx_c + dy_c * dy_c;
                    if (r2 > ac[ci].max_r2) continue;
                    const float core = std::exp(-r2 * ac[ci].r2_inv);
                    cell_cloud += ac[ci].cloud * core;
                    cell_rain += ac[ci].rain * core;
                    cell_w += (ac[ci].up - ac[ci].down) * core;
                }
                for (int level = 0; level < nspain::NSWX_LEVELS; ++level) {
                    const size_t k = static_cast<size_t>(level) * cells + i;
                    const SimWeatherCell &c = solver_.weather_grid_[k];
                    const float rain_h = (level <= 2) ? 1.0f : 0.25f;
                    const float fog_add = (level == 0 && dawn_fog) ? (basin * 0.70f) : 0.0f;
                    const float c_frac = std::clamp(c.cloud_fraction + cell_cloud + fog_add, 0.0f, 1.0f);
                    staging_weather_.field[k * 4 + 0] = c.u;
                    staging_weather_.field[k * 4 + 1] = c.v;
                    staging_weather_.field[k * 4 + 2] = c.w + cell_w * rain_h;
                    staging_weather_.field[k * 4 + 3] = std::clamp(
                        (c_frac - solver_.weather_worker_active_.cloud_cov) * 0.92f + (level == 0 ? fog_add * 0.45f : 0.0f),
                        -0.45f, 0.55f);
                    staging_weather_.field[rain_offset + i * 4 + level] = std::clamp(c.rain_mm_hr + cell_rain * rain_h, 0.0f, 48.0f);
                }
            }
        }
    }
    staging_weather_.cells = solver_.dyn_cells_;
    staging_weather_.cell_count = solver_.dyn_cell_count_;
    staging_weather_.summary = solver_.weather_worker_active_;
    staging_weather_.forcing = solver_.synoptic_weather_;
    staging_weather_.source = solver_.source;
    staging_weather_.regime = solver_.regime_;
    staging_weather_.solved_time = solver_.solved_time;
    staging_weather_.region_id = solver_.region_id;
    staging_weather_.anchor_lat = solver_.weather_anchor_lat_;
    staging_weather_.anchor_lon = solver_.anchor_lon;
    staging_weather_.metres_lat = solver_.metres_lat;
    staging_weather_.metres_lon = solver_.metres_lon;
    staging_weather_.min_e = solver_.min_e_;
    staging_weather_.min_n = solver_.min_n_;
    staging_weather_.span_e = solver_.span_e_;
    staging_weather_.span_n = solver_.span_n_;
    std::lock_guard<std::mutex> lk(weather_publish_mtx_);
    staging_weather_.generation = ++weather_generation_;
    std::swap(staging_weather_, pending_weather_);
}

WeatherModelHistoryEntry RegionalAtmosphere::summarize(double time, const WeatherSnapshot &model,
                                                       const std::string &region) {
    return {time, model.oat_sl_c, model.wind_dir_from_deg, model.wind_speed_10m_mps,
            model.cloud_cov * 100.0f, model.precip_mm_hr, region};
}

void RegionalAtmosphere::latch_weather() {
    {
        std::lock_guard<std::mutex> lk(weather_publish_mtx_);
        if (pending_weather_.generation <= frame_weather_.generation) return;
        std::swap(frame_weather_, pending_weather_);
    }
    active_weather_ = frame_weather_.summary;
    if (model_history_.empty() || frame_weather_.region_id != model_history_.back().region_id ||
        frame_weather_.solved_time - last_model_history_sec_ >= 30.0) {
        model_history_.push_back(summarize(frame_weather_.solved_time, active_weather_, frame_weather_.region_id));
        if (model_history_.size() > 48) model_history_.erase(model_history_.begin());
        last_model_history_sec_ = frame_weather_.solved_time;
    }
}

bool RegionalAtmosphere::covers(double lat, double lon) const {
    if (!has_model()) return false;
    const auto &f = frame_weather_;
    const double e = (lon - f.anchor_lon) * f.metres_lon;
    const double n = (lat - f.anchor_lat) * f.metres_lat;
    return e >= f.min_e && e <= f.min_e + f.span_e && n >= f.min_n && n <= f.min_n + f.span_n;
}

void RegionalAtmosphere::request_outlook() {
    if (!has_model() || synchronous_solver_) return;
    {
        std::lock_guard<std::mutex> lock(weather_job_mtx_);
        ++outlook_request_id_;
        outlook_ = {WeatherOutlook::Status::Running, frame_weather_.solved_time, frame_weather_.region_id, {}};
        outlook_requested_ = true;
    }
    weather_job_cv_.notify_one();
}

void RegionalAtmosphere::cancel_outlook() {
    {
        std::lock_guard<std::mutex> lock(weather_job_mtx_);
        ++outlook_request_id_;
        outlook_ = {};
        outlook_requested_ = false;
    }
    weather_job_cv_.notify_one();
}

WeatherOutlook RegionalAtmosphere::outlook() const {
    std::lock_guard<std::mutex> lock(weather_job_mtx_);
    return outlook_;
}

void RegionalAtmosphere::weather_worker_loop() {
    using Clock = std::chrono::steady_clock;
    static constexpr float step_sec = 0.25f;
    std::optional<WeatherSolver> projection;
    uint64_t request = 0;
    int projection_steps = 0;
    while (!weather_worker_stop_.load(std::memory_order_acquire)) {
        int live_steps = 0;
        bool start_projection = false;
        WeatherSnapshot forcing;
        Vec3 sun;
        {
            std::unique_lock<std::mutex> lock(weather_job_mtx_);
            weather_job_cv_.wait(lock, [&] {
                return weather_worker_stop_.load(std::memory_order_acquire) ||
                       weather_pending_steps_ > 0 || outlook_requested_ || projection.has_value();
            });
            if (weather_worker_stop_.load(std::memory_order_acquire)) break;
            if (request != outlook_request_id_) projection.reset();
            live_steps = std::exchange(weather_pending_steps_, 0);
            if (live_steps > 0) {
                forcing = weather_worker_forcing_;
                sun = weather_worker_sun_;
                solver_.regime_ = weather_worker_regime_;
                solver_.source = weather_worker_source_;
                if (std::exchange(weather_pending_climate_, false))
                    solver_.weather_worker_climate_field_.swap(weather_pending_climate_field_);
            } else if (outlook_requested_) {
                start_projection = true;
                outlook_requested_ = false;
                request = outlook_request_id_;
            }
        }
        if (live_steps > 0) {
            const auto start = Clock::now();
            for (int i = 0; i < live_steps; ++i) solver_.step(step_sec, sun, forcing);
            const auto solved = Clock::now();
            publish_weather_model();
            weather_solve_us_.fetch_add(std::chrono::duration_cast<std::chrono::microseconds>(solved - start).count(), std::memory_order_relaxed);
            weather_publish_us_.fetch_add(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - solved).count(), std::memory_order_relaxed);
            weather_steps_.fetch_add(live_steps, std::memory_order_relaxed);
            continue;
        }
        if (start_projection) {
            projection = solver_;
            projection_steps = 0;
            std::lock_guard<std::mutex> lock(weather_job_mtx_);
            if (request == outlook_request_id_) outlook_.issue_sim_time = projection->solved_time;
        }
        if (!projection) continue;

        // Run one step before polling live work again. This caps the delay
        // an outlook can introduce into the live worker queue.
        projection->step(step_sec, projection->sun, projection->synoptic_weather_);
        ++projection_steps;
        if (projection_steps == 240 || projection_steps == 720 || projection_steps == 1200) {
            std::lock_guard<std::mutex> lock(weather_job_mtx_);
            if (request == outlook_request_id_) {
                outlook_.predictions.push_back(summarize(projection->solved_time,
                    projection->weather_worker_active_, projection->region_id));
                if (projection_steps == 1200) outlook_.status = WeatherOutlook::Status::Complete;
            }
        }
        if (projection_steps == 1200) projection.reset();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

void RegionalAtmosphere::start_weather_worker() {
    if (weather_worker_.joinable() || !weather_model_ready_) return;
    weather_worker_stop_.store(false, std::memory_order_release);
    weather_worker_ = std::thread(&RegionalAtmosphere::weather_worker_loop, this);
}

void RegionalAtmosphere::stop_weather_worker() {
    weather_worker_stop_.store(true, std::memory_order_release);
    weather_job_cv_.notify_all();
    if (weather_worker_.joinable()) weather_worker_.join();
    std::lock_guard<std::mutex> lk(weather_job_mtx_);
    weather_pending_steps_ = 0;
    weather_pending_climate_ = false;
    outlook_requested_ = false;
    outlook_ = {};
    ++outlook_request_id_;
}

void RegionalAtmosphere::initialize_weather_model() {
    const size_t cells = static_cast<size_t>(GRID_W * GRID_H);
    const size_t total = cells * nspain::NSWX_LEVELS;
    solver_.weather_grid_.assign(total, {});
    solver_.weather_next_.assign(total, {});
    solver_.weather_rain_flux_.assign(total, 0.0f);
    solver_.terrain_height_.assign(cells, 0.0f);
    solver_.terrain_slope_e_.assign(cells, 0.0f);
    solver_.terrain_slope_n_.assign(cells, 0.0f);
    solver_.terrain_ridge_exposure_.assign(cells, 0.0f);
    solver_.terrain_valley_basin_.assign(cells, 0.0f);
    refresh_climate_field();

    // Resample the pre-baked corridor DEM once. The four atmospheric layers are
    // terrain-relative, so every column carries its own valley/ridge elevation.
    if (pack_) {
        const auto &dem = pack_->corridor_dem();
        const int dw = pack_->corridor_dem_w(), dh = pack_->corridor_dem_h();
        const double dmin_e = pack_->corridor_min_e(), dmin_n = pack_->corridor_min_n();
        const double dspan_e = std::max(1.0, pack_->corridor_span_e());
        const double dspan_n = std::max(1.0, pack_->corridor_span_n());
        const bool have_dem = static_cast<int>(dem.size()) == dw * dh && dw > 1 && dh > 1;
        for (int y = 0; y < GRID_H; ++y) {
            for (int x = 0; x < GRID_W; ++x) {
                const double e = min_e_ + span_e_ * x / (GRID_W - 1.0);
                const double n = min_n_ + span_n_ * y / (GRID_H - 1.0);
                const size_t i = static_cast<size_t>(y) * GRID_W + x;
                if (have_dem) {
                    double fx = std::clamp((e - dmin_e) / dspan_e * (dw - 1), 0.0, static_cast<double>(dw - 1));
                    double fy = std::clamp((n - dmin_n) / dspan_n * (dh - 1), 0.0, static_cast<double>(dh - 1));
                    int x0 = std::min(dw - 2, static_cast<int>(fx));
                    int y0 = std::min(dh - 2, static_cast<int>(fy));
                    float tx = static_cast<float>(fx - x0), ty = static_cast<float>(fy - y0);
                    float a = std::lerp(dem[static_cast<size_t>(y0) * dw + x0], dem[static_cast<size_t>(y0) * dw + x0 + 1], tx);
                    float b = std::lerp(dem[static_cast<size_t>(y0 + 1) * dw + x0], dem[static_cast<size_t>(y0 + 1) * dw + x0 + 1], tx);
                    solver_.terrain_height_[i] = std::lerp(a, b, ty);
                } else {
                    double lat = pack_->anchor_lat() + n / pack_->anchor_m_lat();
                    double lon = pack_->anchor_lon() + e / pack_->anchor_m_lon();
                    solver_.terrain_height_[i] = static_cast<float>(pack_->sample_elevation(lat, lon));
                }
            }
        }
    }

    const float dx = static_cast<float>(span_e_ / (GRID_W - 1));
    const float dy = static_cast<float>(span_n_ / (GRID_H - 1));
    for (int y = 0; y < GRID_H; ++y) {
        for (int x = 0; x < GRID_W; ++x) {
            const int xl = std::max(0, x - 1), xr = std::min(GRID_W - 1, x + 1);
            const int yd = std::max(0, y - 1), yu = std::min(GRID_H - 1, y + 1);
            size_t i = static_cast<size_t>(y) * GRID_W + x;
            float dzdx = solver_.terrain_height_[static_cast<size_t>(y) * GRID_W + xr] - solver_.terrain_height_[static_cast<size_t>(y) * GRID_W + xl];
            float dzdy = solver_.terrain_height_[static_cast<size_t>(yu) * GRID_W + x] - solver_.terrain_height_[static_cast<size_t>(yd) * GRID_W + x];
            solver_.terrain_slope_e_[i] = dzdx / std::max(1.0f, dx * (xr - xl));
            solver_.terrain_slope_n_[i] = dzdy / std::max(1.0f, dy * (yu - yd));
        }
    }
    // Separable 9x9 spatial mean separates exposed ridges from sheltered basins cleanly per region bake.
    std::vector<float> horiz_mean(cells, 0.0f);
    for (int y = 0; y < GRID_H; ++y) {
        const size_t row = static_cast<size_t>(y) * GRID_W;
        for (int x = 0; x < GRID_W; ++x) {
            float sum = 0.0f;
            for (int ox = -4; ox <= 4; ++ox) {
                sum += solver_.terrain_height_[row + static_cast<size_t>(std::clamp(x + ox, 0, GRID_W - 1))];
            }
            horiz_mean[row + x] = sum * (1.0f / 9.0f);
        }
    }
    for (int y = 0; y < GRID_H; ++y) {
        for (int x = 0; x < GRID_W; ++x) {
            const size_t i = static_cast<size_t>(y) * GRID_W + x;
            float mean_h = 0.0f;
            for (int oy = -4; oy <= 4; ++oy) {
                mean_h += horiz_mean[static_cast<size_t>(std::clamp(y + oy, 0, GRID_H - 1)) * GRID_W + x];
            }
            mean_h *= (1.0f / 9.0f);
            const float slope = std::hypot(solver_.terrain_slope_e_[i], solver_.terrain_slope_n_[i]);
            const float relative_h = solver_.terrain_height_[i] - mean_h;
            solver_.terrain_ridge_exposure_[i] = std::clamp(
                std::max(smooth01(0.025f, 0.22f, slope) * 0.72f,
                         smooth01(35.0f, 240.0f, relative_h) * 0.62f), 0.0f, 1.0f);
            solver_.terrain_valley_basin_[i] = std::clamp(
                smooth01(25.0f, 190.0f, -relative_h) * (1.0f - smooth01(0.035f, 0.18f, slope)), 0.0f, 1.0f);
        }
    }
    terrain_upload_ = solver_.terrain_height_;
    if (gpu_enabled_ && terrain_field_tex_ && terrain_upload_.size() == cells)
        glTextureSubImage2D(terrain_field_tex_, 0, 0, 0, GRID_W, GRID_H, GL_RED, GL_FLOAT, terrain_upload_.data());

    const double dir = synoptic_weather_.wind_dir_from_deg * DEG2RAD_D;
    const float global_u = static_cast<float>(-std::sin(dir) * synoptic_weather_.wind_speed_10m_mps);
    const float global_v = static_cast<float>(-std::cos(dir) * synoptic_weather_.wind_speed_10m_mps);
    static constexpr float layer_wind_scale[nspain::NSWX_LEVELS] = {0.82f, 0.94f, 1.08f, 1.28f};
    for (int lev = 0; lev < nspain::NSWX_LEVELS; ++lev) {
        const float agl = nspain::NSWX_LEVEL_AGL_M[lev];
        for (size_t i = 0; i < cells; ++i) {
            const float ref_u = climate_field_[i * 4 + 0];
            const float ref_v = climate_field_[i * 4 + 1];
            const float ref_w = climate_field_[i * 4 + 2];
            const float ref_cloud = climate_field_[i * 4 + 3];
            SimWeatherCell &c = solver_.weather_grid_[static_cast<size_t>(lev) * cells + i];
            c.u = std::clamp(std::lerp(ref_u, global_u, 0.34f) * layer_wind_scale[lev], -60.0f, 60.0f);
            c.v = std::clamp(std::lerp(ref_v, global_v, 0.34f) * layer_wind_scale[lev], -60.0f, 60.0f);
            c.w = std::clamp(ref_w * 0.70f, -8.0f, 8.0f);
            c.temp_c = synoptic_weather_.oat_sl_c - (solver_.terrain_height_[i] + agl) * 0.0065f;
            c.pressure_hpa = synoptic_weather_.qnh_hpa * std::exp(-agl / 8500.0f);
            const float dew = synoptic_weather_.dewpoint_sl_c - agl * 0.0017f;
            const float saturation = saturation_mixing_ratio_gkg(c.temp_c, c.pressure_hpa);
            const float x = static_cast<float>(i % GRID_W), y = static_cast<float>(i / GRID_W);
            const float plume = std::sin(x * 0.47f + y * 0.32f + lev * 0.30f) *
                                std::sin(x * 0.29f - y * 0.71f - lev * 0.22f);
            const float humidity = saturation_mixing_ratio_gkg(dew, c.pressure_hpa) / saturation;
            const float ridge = solver_.terrain_ridge_exposure_[i];
            const float basin = solver_.terrain_valley_basin_[i];
            const float rh = std::clamp(std::min(humidity, 1.02f) + 0.11f * plume + 0.07f * ref_cloud +
                                        ridge * 0.025f + basin * 0.035f, 0.08f, 1.15f);
            c.vapor_gkg = rh * saturation;
            c.liquid_gkg = std::clamp((rh - 0.70f) * 2.4f + synoptic_weather_.cloud_cov * 0.16f, 0.0f, 2.0f);
            c.cloud_fraction = std::clamp(synoptic_weather_.cloud_cov + ref_cloud * 0.18f +
                plume * 0.09f + (smooth01(0.87f, 1.04f, rh) - 0.5f) * 0.14f +
                ridge * 0.05f + basin * 0.035f, 0.0f, 1.0f);
            c.rain_mm_hr = synoptic_weather_.precip_mm_hr * c.cloud_fraction * 0.40f;
            c.tke = 0.06f + synoptic_weather_.turb_scale * 0.24f;
        }
    }
    solver_.weather_next_ = solver_.weather_grid_; // Keep fixed boundary cells in both advection buffers.
    weather_model_ready_ = true;
    solver_.weather_worker_active_ = synoptic_weather_;
    solver_.weather_worker_climate_field_ = climate_field_;
    if (pack_) {
        weather_anchor_lat_ = pack_->anchor_lat();
        weather_anchor_lon_ = pack_->anchor_lon();
    }
    if (weather_model_time_ < 0.0) weather_model_time_ = last_update_t_;
    solver_.min_e_ = min_e_;
    solver_.min_n_ = min_n_;
    solver_.span_e_ = span_e_;
    solver_.span_n_ = span_n_;
    solver_.weather_anchor_lat_ = weather_anchor_lat_;
    solver_.anchor_lon = weather_anchor_lon_;
    solver_.metres_lat = pack_->anchor_m_lat();
    solver_.metres_lon = pack_->anchor_m_lon();
    solver_.region_id = active_region_id_;
    solver_.solved_time = last_update_t_;
    solver_.regime_ = regime_;
    solver_.solver_rng_.seed(solver_seed_ ? *solver_seed_ : hardware_entropy_seed(this));
    solver_.seed_dynamic_cells();
    publish_weather_model();
    latch_weather();
    upload_field();
    std::printf("[WX-SIM] CPU mesoscale field %dx%dx%d, terrain-following layers at 10/550/1800/4500m AGL\n",
                GRID_W, GRID_H, nspain::NSWX_LEVELS);
}

WeatherWorkStats RegionalAtmosphere::work_stats() const {
    return {weather_steps_.load(std::memory_order_relaxed), field_uploads_,
            weather_solve_us_.load(std::memory_order_relaxed) / 1000.0,
            weather_publish_us_.load(std::memory_order_relaxed) / 1000.0};
}

WeatherDebugSlice RegionalAtmosphere::debug_slice(double east_m, double north_m, int level, int sample_stride) const {
    WeatherDebugSlice slice;
    if (level < 0 || level >= nspain::NSWX_LEVELS || !weather_model_ready_) return slice;
    const int stride = std::clamp(sample_stride, 1, 8);
    const double de = frame_weather_.span_e / (GRID_W - 1), dn = frame_weather_.span_n / (GRID_H - 1);
    const int x_mid = static_cast<int>(std::round((east_m - frame_weather_.min_e) / de));
    const int y_mid = static_cast<int>(std::round((north_m - frame_weather_.min_n) / dn));
    slice.agl_m = nspain::NSWX_LEVEL_AGL_M[level];
    slice.step_e_m = static_cast<float>(de * stride);
    slice.step_n_m = static_cast<float>(dn * stride);
    if (frame_weather_.grid.empty()) return slice;
    slice.generation = frame_weather_.generation;
    const size_t offset = static_cast<size_t>(level) * GRID_W * GRID_H;
    for (int row = 0; row < slice.width; ++row) {
        const int y = y_mid + (slice.width / 2 - row) * stride;
        for (int col = 0; col < slice.width; ++col) {
            const int x = x_mid + (col - slice.width / 2) * stride;
            if (x < 0 || x >= GRID_W || y < 0 || y >= GRID_H) continue;
            const size_t i = static_cast<size_t>(y) * GRID_W + x;
            const auto &src = frame_weather_.grid[offset + i];
            slice.cells[row * slice.width + col] = {
                {static_cast<float>(frame_weather_.min_e + x * de), static_cast<float>(frame_weather_.min_n + y * dn), solver_.terrain_height_[i] + slice.agl_m},
                {src.u, src.v, src.w},
                src.cloud_fraction, src.liquid_gkg, src.rain_mm_hr,
                solver_.terrain_ridge_exposure_[i], solver_.terrain_valley_basin_[i], true
            };
        }
    }
    return slice;
}

RegionalAtmosphere::SimWeatherCell RegionalAtmosphere::sample_weather_model(double east_m, double north_m, double agl_m) const {
    SimWeatherCell zero{};
    if (!weather_model_ready_) return zero;
    const auto &src_grid = frame_weather_.grid;
    if (src_grid.empty()) return zero;
    const double gx = std::clamp((east_m - frame_weather_.min_e) / std::max(1.0, frame_weather_.span_e) * (GRID_W - 1), 0.0, static_cast<double>(GRID_W - 1));
    const double gy = std::clamp((north_m - frame_weather_.min_n) / std::max(1.0, frame_weather_.span_n) * (GRID_H - 1), 0.0, static_cast<double>(GRID_H - 1));
    const int x0 = std::min(GRID_W - 2, static_cast<int>(gx)), y0 = std::min(GRID_H - 2, static_cast<int>(gy));
    const float tx = static_cast<float>(gx - x0), ty = static_cast<float>(gy - y0);
    int l0 = 0;
    while (l0 < nspain::NSWX_LEVELS - 2 && agl_m > nspain::NSWX_LEVEL_AGL_M[l0 + 1]) ++l0;
    const int l1 = std::min(nspain::NSWX_LEVELS - 1, l0 + 1);
    const float tz = static_cast<float>(std::clamp(
        (agl_m - nspain::NSWX_LEVEL_AGL_M[l0]) /
        std::max(1.0f, nspain::NSWX_LEVEL_AGL_M[l1] - nspain::NSWX_LEVEL_AGL_M[l0]), 0.0, 1.0));
    const size_t cells = static_cast<size_t>(GRID_W * GRID_H);
    auto sample_level = [&](int lev) {
        SimWeatherCell out{};
        const size_t off = static_cast<size_t>(lev) * cells;
        auto field = [&](float SimWeatherCell::*m) {
            float a = std::lerp(src_grid[off + static_cast<size_t>(y0) * GRID_W + x0].*m,
                                src_grid[off + static_cast<size_t>(y0) * GRID_W + x0 + 1].*m, tx);
            float b = std::lerp(src_grid[off + static_cast<size_t>(y0 + 1) * GRID_W + x0].*m,
                                src_grid[off + static_cast<size_t>(y0 + 1) * GRID_W + x0 + 1].*m, tx);
            return std::lerp(a, b, ty);
        };
        out.u = field(&SimWeatherCell::u); out.v = field(&SimWeatherCell::v); out.w = field(&SimWeatherCell::w);
        out.vapor_gkg = field(&SimWeatherCell::vapor_gkg); out.liquid_gkg = field(&SimWeatherCell::liquid_gkg);
        out.temp_c = field(&SimWeatherCell::temp_c); out.pressure_hpa = field(&SimWeatherCell::pressure_hpa);
        out.rain_mm_hr = field(&SimWeatherCell::rain_mm_hr); out.cloud_fraction = field(&SimWeatherCell::cloud_fraction);
        out.tke = field(&SimWeatherCell::tke);
        return out;
    };
    const SimWeatherCell a = sample_level(l0), b = sample_level(l1);
    auto mix_cell = [tz](const SimWeatherCell &x, const SimWeatherCell &y) {
        SimWeatherCell r{};
        r.u = std::lerp(x.u, y.u, tz); r.v = std::lerp(x.v, y.v, tz); r.w = std::lerp(x.w, y.w, tz);
        r.vapor_gkg = std::lerp(x.vapor_gkg, y.vapor_gkg, tz); r.liquid_gkg = std::lerp(x.liquid_gkg, y.liquid_gkg, tz);
        r.temp_c = std::lerp(x.temp_c, y.temp_c, tz); r.pressure_hpa = std::lerp(x.pressure_hpa, y.pressure_hpa, tz);
        r.rain_mm_hr = std::lerp(x.rain_mm_hr, y.rain_mm_hr, tz); r.cloud_fraction = std::lerp(x.cloud_fraction, y.cloud_fraction, tz);
        r.tke = std::lerp(x.tke, y.tke, tz);
        return r;
    };
    return mix_cell(a, b);
}

void RegionalAtmosphere::WeatherSolver::step(float dt, const Vec3 &sun_dir, const WeatherSnapshot &forcing) {
    if (weather_grid_.empty()) return;
    synoptic_weather_ = forcing;
    sun = sun_dir;
    solved_time += dt;
    dt = std::clamp(dt, 0.02f, 0.35f);
    const size_t cells = static_cast<size_t>(GRID_W * GRID_H);
    const float dx = static_cast<float>(span_e_ / (GRID_W - 1));
    const float dy = static_cast<float>(span_n_ / (GRID_H - 1));
    const double lat = weather_anchor_lat_ != 0.0 ? weather_anchor_lat_ * DEG2RAD_D : 42.0 * DEG2RAD_D;
    const float coriolis = static_cast<float>(2.0 * 7.2921159e-5 * std::sin(lat));
    const float solar = std::clamp(sun_dir.y, 0.0f, 1.0f);
    const double dir = forcing.wind_dir_from_deg * DEG2RAD_D;
    const float global_u = static_cast<float>(-std::sin(dir) * forcing.wind_speed_10m_mps);
    const float global_v = static_cast<float>(-std::cos(dir) * forcing.wind_speed_10m_mps);
    static constexpr float kWindScale[nspain::NSWX_LEVELS] = {0.82f, 0.94f, 1.08f, 1.28f};
    static constexpr float kWindTau[nspain::NSWX_LEVELS] = {48.0f, 130.0f, 320.0f, 720.0f};
    static constexpr float kVerticalMix[nspain::NSWX_LEVELS] = {0.0f, 540.0f, 1250.0f, 2700.0f};
    const SimWeatherCell * __restrict old = weather_grid_.data();
    SimWeatherCell * __restrict next = weather_next_.data();
    std::fill(weather_rain_flux_.begin(), weather_rain_flux_.end(), 0.0f);

    const float back_e = dt / std::max(1.0f, dx), back_n = dt / std::max(1.0f, dy);
    const float inv_2dx = 1.0f / std::max(2.0f, 2.0f * dx);
    const float inv_2dy = 1.0f / std::max(2.0f, 2.0f * dy);
    // All transported quantities share the same four neighboring cells.
    auto advect = [&](size_t offset, float x, float y) {
        x = std::clamp(x, 0.0f, static_cast<float>(GRID_W - 1));
        y = std::clamp(y, 0.0f, static_cast<float>(GRID_H - 1));
        const int x0 = std::min(GRID_W - 2, static_cast<int>(x));
        const int y0 = std::min(GRID_H - 2, static_cast<int>(y));
        const float tx = x - x0, ty = y - y0;
        const auto *a = old + offset + static_cast<size_t>(y0) * GRID_W + x0;
        const auto *b = a + 1, *c = a + GRID_W, *d = c + 1;
        SimWeatherCell out{};
        auto field = [&](float SimWeatherCell::*m) {
            return std::lerp(std::lerp(a->*m, b->*m, tx), std::lerp(c->*m, d->*m, tx), ty);
        };
#if defined(__AVX2__) && defined(__FMA__)
        // Eight adjacent quantities within a cell fit one AVX register. Copying
        // their representation avoids treating struct members as a float array.
        static_assert(offsetof(SimWeatherCell, cloud_fraction) == sizeof(__m256));
        auto load = [](const SimWeatherCell *p) {
            __m256 v;
            std::memcpy(&v, p, sizeof(v));
            return v;
        };
        auto lerp = [](__m256 lhs, __m256 rhs, __m256 t) {
            return _mm256_fmadd_ps(_mm256_sub_ps(rhs, lhs), t, lhs);
        };
        const __m256 u = _mm256_set1_ps(tx), v = _mm256_set1_ps(ty);
        const __m256 values = lerp(lerp(load(a), load(b), u), lerp(load(c), load(d), u), v);
        std::memcpy(static_cast<void *>(&out), &values, sizeof(values));
#else
        out.u = field(&SimWeatherCell::u);
        out.v = field(&SimWeatherCell::v);
        out.w = field(&SimWeatherCell::w);
        out.vapor_gkg = field(&SimWeatherCell::vapor_gkg);
        out.liquid_gkg = field(&SimWeatherCell::liquid_gkg);
        out.temp_c = field(&SimWeatherCell::temp_c);
        out.pressure_hpa = field(&SimWeatherCell::pressure_hpa);
        out.rain_mm_hr = field(&SimWeatherCell::rain_mm_hr);
#endif
        out.cloud_fraction = field(&SimWeatherCell::cloud_fraction);
        out.tke = field(&SimWeatherCell::tke);
        return out;
    };

    for (int lev = nspain::NSWX_LEVELS - 1; lev >= 0; --lev) {
        const float agl = nspain::NSWX_LEVEL_AGL_M[lev];
        const size_t layer_off = static_cast<size_t>(lev) * cells;
        const size_t above_off = static_cast<size_t>(std::min(nspain::NSWX_LEVELS - 1, lev + 1)) * cells;
        const float pressure_curve = std::exp(-agl / 8500.0f);
        const float density_height = 1.225f * pressure_curve * 288.15f;
        const float p_target = forcing.qnh_hpa * pressure_curve;
        const float ridge_scale = std::exp(-agl / 760.0f);
        const float thermal_scale = 0.18f * solar * std::exp(-agl / 2100.0f);
        for (int y = 1; y < GRID_H - 1; ++y) {
            for (int x = 1; x < GRID_W - 1; ++x) {
                const size_t i = static_cast<size_t>(y) * GRID_W + x;
                const size_t k = layer_off + i;
                const SimWeatherCell &old_here = old[k];
                SimWeatherCell c = advect(layer_off, x - old_here.u * back_e, y - old_here.v * back_n);

                const SimWeatherCell &wl = old[layer_off + i - 1], &wr = old[layer_off + i + 1];
                const SimWeatherCell &ws = old[layer_off + i - GRID_W], &wn = old[layer_off + i + GRID_W];
                const float dpdx = (wr.pressure_hpa - wl.pressure_hpa) * inv_2dx;
                const float dpdy = (wn.pressure_hpa - ws.pressure_hpa) * inv_2dy;
                const float div = (wr.u - wl.u) * inv_2dx +
                                  (wn.v - ws.v) * inv_2dy;
                const float rho = std::clamp(density_height /
                                             std::max(190.0f, c.temp_c + 273.15f), 0.32f, 1.45f);
                const float pressure_accel = 100.0f / rho;
                float du = -pressure_accel * dpdx + coriolis * c.v;
                float dv = -pressure_accel * dpdy - coriolis * c.u;
                c.pressure_hpa += (-1.38f * c.pressure_hpa * div -
                                   (c.pressure_hpa - p_target) / 900.0f) * dt;
                c.pressure_hpa = std::clamp(c.pressure_hpa, p_target - 9.0f, p_target + 9.0f);

                const float ref_u = weather_worker_climate_field_[i * 4 + 0];
                const float ref_v = weather_worker_climate_field_[i * 4 + 1];
                const float ref_w = weather_worker_climate_field_[i * 4 + 2];
                const float ref_cloud = weather_worker_climate_field_[i * 4 + 3];
                const float veer = (lev == 0) ? 0.0f : static_cast<float>(lev) * 0.0065f;
                const float target_u = (std::lerp(ref_u, global_u, 0.38f) * kWindScale[lev]) - global_v * veer;
                const float target_v = (std::lerp(ref_v, global_v, 0.38f) * kWindScale[lev]) + global_u * veer;
                c.u = std::clamp(c.u + (du + (target_u - c.u) / kWindTau[lev]) * dt, -62.0f, 62.0f);
                c.v = std::clamp(c.v + (dv + (target_v - c.v) / kWindTau[lev]) * dt, -62.0f, 62.0f);

                const float terrain_h = terrain_height_[i];
                const float base_temp = forcing.oat_sl_c - (terrain_h + agl) * 0.0065f;
                const float ridge_exposure = terrain_ridge_exposure_[i];
                const float valley_basin = terrain_valley_basin_[i];
                const float sensible_heat = (lev <= 1) ? solar * (lev == 0 ? 0.00038f : 0.00016f) : 0.000025f;
                const float slope_e = terrain_slope_e_[i], slope_n = terrain_slope_n_[i];
                const float ridge_lift = (c.u * slope_e + c.v * slope_n) * ridge_scale;
                const float low_wind = 1.0f - smooth01(2.0f, 10.0f, std::hypot(c.u, c.v));
                const float valley_stability = valley_basin * (1.0f - solar) * low_wind;
                const float thermal_lift = std::max(0.0f, c.temp_c - base_temp) * thermal_scale *
                                           (1.0f - 0.72f * valley_stability);
                const float target_w = ref_w * 0.48f + ridge_lift * (0.72f + 0.42f * ridge_exposure) + thermal_lift;
                // Brunt-Vaisala static stability restoring force generates genuine standing lee waves
                // downwind of Pyrenean crests on mid-tropospheric layers (lev 1..2).
                float bv_restore = 0.0f;
                if (lev >= 1 && lev <= 2) {
                    const SimWeatherCell &below = old[static_cast<size_t>(lev - 1) * cells + i];
                    const float dz_l = std::max(200.0f, agl - nspain::NSWX_LEVEL_AGL_M[lev - 1]);
                    const float dtheta_dz = (c.temp_c - below.temp_c) / dz_l + 0.0098f;
                    const float n2 = std::clamp(9.81f * std::max(0.0f, dtheta_dz) / std::max(220.0f, c.temp_c + 273.15f), 0.0f, 4.0e-4f);
                    bv_restore = -n2 * 140.0f * (c.temp_c - base_temp);
                }
                c.w = std::clamp(c.w + ((target_w - c.w) / (8.0f + agl * 0.002f) + bv_restore) * dt, -10.0f, 10.0f);
                // True Dry Adiabatic Lapse Rate (9.8 C/km) for unsaturated vertical motion; latent heat
                // below converts saturated ascent to the Moist Adiabatic Lapse Rate (~5.5 C/km -> real Foehn warming!).
                c.temp_c += ((base_temp - c.temp_c) / 1800.0f + sensible_heat - c.w * 0.0098f) * dt;

                const float qsat = saturation_mixing_ratio_gkg(c.temp_c, c.pressure_hpa);
                const float lower_mix = (lev > 0) ? std::clamp(std::max(0.0f, c.w) * dt /
                                          std::max(100.0f, kVerticalMix[lev]), 0.0f, 0.08f) : 0.0f;
                if (lev > 0 && lower_mix > 0.0f) {
                    const SimWeatherCell &lower = old[static_cast<size_t>(lev - 1) * cells + i];
                    c.vapor_gkg = std::lerp(c.vapor_gkg, lower.vapor_gkg, lower_mix);
                }
                if (lev == 0) c.vapor_gkg += solar * std::max(0.0f, 1.0f - c.vapor_gkg / std::max(0.1f, qsat)) * 0.00022f * dt;
                float supersat = c.vapor_gkg - qsat;
                // Latent heat coefficient L_v / c_p ~= 2.49 C per g/kg (dampened for discrete time step stability).
                constexpr float kLatentCPerGkg = 2.49f;
                if (supersat > 0.0f) {
                    float condense = std::min(supersat, supersat * std::clamp(dt / 2.8f, 0.0f, 0.14f));
                    c.vapor_gkg -= condense;
                    c.liquid_gkg += condense;
                    c.temp_c += condense * kLatentCPerGkg * 0.55f;
                } else if (c.liquid_gkg > 0.0f) {
                    float evaporate = std::min(c.liquid_gkg, -supersat * std::clamp(dt / 7.0f, 0.0f, 0.06f));
                    c.liquid_gkg -= evaporate;
                    c.vapor_gkg += evaporate;
                    c.temp_c -= evaporate * kLatentCPerGkg * 0.55f;
                }
                c.liquid_gkg = std::clamp(c.liquid_gkg, 0.0f, 5.0f);
                c.vapor_gkg = std::clamp(c.vapor_gkg, 0.02f, 35.0f);
                const float rh = std::clamp(c.vapor_gkg / std::max(0.1f, qsat) + valley_stability * 0.025f, 0.0f, 1.2f);
                const float micro_cloud = std::clamp(smooth01(0.10f, 1.10f, c.liquid_gkg) * 0.78f +
                                                      smooth01(0.84f, 1.0f, rh) * 0.22f, 0.0f, 1.0f);
                const float base_cloud = std::clamp(forcing.cloud_cov + ref_cloud * 0.18f, 0.0f, 1.0f);
                const float orographic_cloud = ridge_exposure * smooth01(0.30f, 1.45f, std::max(0.0f, c.w)) *
                                               smooth01(0.78f, 0.99f, rh);
                const float valley_mist = (lev == 0 ? 1.0f : 0.12f) * valley_stability * smooth01(0.94f, 1.08f, rh);
                const float cloud_target = std::clamp(std::lerp(base_cloud, micro_cloud, 0.57f) +
                                                       orographic_cloud * 0.30f + valley_mist * 0.24f, 0.0f, 1.0f);
                c.cloud_fraction = std::lerp(c.cloud_fraction, cloud_target, dt / 8.0f);

                float incoming_rain = (lev < nspain::NSWX_LEVELS - 1)
                    ? weather_rain_flux_[above_off + i] * 0.62f : 0.0f;
                // Sub-cloud virga evaporation cools dry lower air into realistic storm cold pools!
                if (incoming_rain > 0.2f && rh < 0.85f) {
                    const float evap_frac = std::clamp((0.85f - rh) * 0.35f, 0.0f, 0.28f);
                    const float rain_evap = incoming_rain * evap_frac;
                    incoming_rain -= rain_evap;
                    c.temp_c -= rain_evap * 0.008f * dt;
                    c.vapor_gkg = std::clamp(c.vapor_gkg + rain_evap * 0.0025f * dt, 0.02f, 35.0f);
                }
                c.temp_c = std::clamp(c.temp_c, base_temp - 9.0f, base_temp + 10.0f);
                float precip_gen = (std::max(0.0f, c.liquid_gkg - 0.95f) * 1.65f +
                                    forcing.precip_mm_hr * 0.26f) * c.cloud_fraction *
                                   (1.0f + 0.55f * orographic_cloud);
                weather_rain_flux_[k] = std::clamp(precip_gen + incoming_rain, 0.0f, 45.0f);
                c.rain_mm_hr = std::clamp(std::lerp(c.rain_mm_hr, precip_gen + incoming_rain, std::clamp(dt / 5.0f, 0.0f, 0.08f)), 0.0f, 45.0f);
                const float shear = std::abs(wr.u - wl.u) / std::max(1.0f, 2.0f * dx) +
                                    std::abs(wn.v - ws.v) / std::max(1.0f, 2.0f * dy);
                const float tke_target = std::clamp((0.08f + forcing.turb_scale * 0.30f + shear * 110.0f +
                                                     std::abs(c.w) * 0.018f) * (1.0f - 0.62f * valley_stability), 0.03f, 1.2f);
                c.tke = std::lerp(c.tke, tke_target, std::clamp(dt / 18.0f, 0.0f, 0.04f));
                next[k] = c;
            }
        }
    }
    weather_grid_.swap(weather_next_);

    // Inject lightweight non-deterministic sub-grid eddy forcing into sparse randomized cells
    // so thermal plumes & wind fluctuations nucleate organically across the 384x288x4 domain.
    std::uniform_real_distribution<float> u11(-1.0f, 1.0f);
    std::uniform_int_distribution<int> rx_d(2, GRID_W - 3);
    std::uniform_int_distribution<int> ry_d(2, GRID_H - 3);
    for (int p = 0; p < 48; ++p) {
        const size_t idx = static_cast<size_t>(ry_d(solver_rng_)) * GRID_W + rx_d(solver_rng_);
        const float amp = (0.35f + solar * 0.65f) * std::sqrt(dt);
        for (int lev = 0; lev < 3; ++lev) {
            auto &cell = weather_grid_[static_cast<size_t>(lev) * cells + idx];
            const float s = std::clamp(cell.tke, 0.05f, 1.2f) * amp;
            cell.u = std::clamp(cell.u + u11(solver_rng_) * s * 1.4f, -62.0f, 62.0f);
            cell.v = std::clamp(cell.v + u11(solver_rng_) * s * 1.4f, -62.0f, 62.0f);
            cell.w = std::clamp(cell.w + u11(solver_rng_) * s * 0.75f, -10.0f, 10.0f);
        }
    }
    advance_dynamic_cells(dt, solar);

    double mean_oat = 0.0, mean_cloud = 0.0, mean_rain = 0.0, mean_u = 0.0, mean_v = 0.0, mean_tke = 0.0;
    double cover_low = 0.0, cover_mid = 0.0, cover_high = 0.0;
    size_t n = 0;
    for (int y = 1; y < GRID_H - 1; ++y) {
        for (int x = 1; x < GRID_W - 1; ++x) {
            size_t i = static_cast<size_t>(y) * GRID_W + x;
            const SimWeatherCell &l0 = weather_grid_[i];
            const SimWeatherCell &l1 = weather_grid_[cells + i];
            const SimWeatherCell &l2 = weather_grid_[2 * cells + i];
            const SimWeatherCell &l3 = weather_grid_[3 * cells + i];
            mean_cloud += 0.16 * l0.cloud_fraction + 0.39 * l1.cloud_fraction + 0.34 * l2.cloud_fraction + 0.11 * l3.cloud_fraction;
            mean_rain += l0.rain_mm_hr;
            mean_oat += l0.temp_c + (terrain_height_[i] + nspain::NSWX_LEVEL_AGL_M[0]) * 0.0065f;
            mean_u += l0.u; mean_v += l0.v;
            mean_tke += 0.45 * l1.tke + 0.40 * l2.tke + 0.15 * l3.tke;
            cover_low += l0.cloud_fraction;
            cover_mid += l1.cloud_fraction;
            cover_high += 0.60 * l2.cloud_fraction + 0.40 * l3.cloud_fraction;
            ++n;
        }
    }
    if (n > 0) {
        const float cloud_target = std::clamp(0.66f * forcing.cloud_cov + 0.34f * static_cast<float>(mean_cloud / n), 0.02f, 0.99f);
        const float rain_target = std::clamp(std::max(forcing.precip * 0.58f, static_cast<float>(mean_rain / n) / 5.0f), 0.0f, 1.0f);
        weather_worker_active_.oat_sl_c = static_cast<float>(mean_oat / n);
        const float summary_alpha = std::clamp(dt / 42.0f, 0.0f, 0.015f);
        weather_worker_active_.cloud_cov = std::lerp(weather_worker_active_.cloud_cov, cloud_target, summary_alpha);
        weather_worker_active_.precip = std::lerp(weather_worker_active_.precip, rain_target, std::clamp(dt / 18.0f, 0.0f, 0.04f));
        weather_worker_active_.precip_mm_hr = std::lerp(weather_worker_active_.precip_mm_hr,
            static_cast<float>(mean_rain / n), std::clamp(dt / 18.0f, 0.0f, 0.04f));
        weather_worker_active_.storm_int = std::clamp(std::max(forcing.storm_int * 0.70f, weather_worker_active_.precip), 0.0f, 1.0f);
        // Espy's Lifting Condensation Level (LCL ~= 125 * (T - Td) m AGL) coupled with solver liquid deck
        const float dew_spread = std::max(0.5f, forcing.oat_sl_c - forcing.dewpoint_sl_c);
        const float lcl_agl = std::clamp(125.0f * dew_spread, 180.0f, 2600.0f);
        const float low_cloud = static_cast<float>(cover_low / n), mid_cloud = static_cast<float>(cover_mid / n);
        float base_target = std::lerp(forcing.cloud_base_m, lcl_agl + 420.0f, 0.42f);
        if (low_cloud > 0.55f) base_target = std::min(base_target, 320.0f);
        else if (mid_cloud > 0.60f) base_target = std::min(base_target, 850.0f);
        weather_worker_active_.cloud_base_m = std::lerp(weather_worker_active_.cloud_base_m, base_target, std::clamp(dt / 65.0f, 0.0f, 0.01f));
        const float vertical_depth = 800.0f + static_cast<float>(cover_high / n) * 2300.0f + weather_worker_active_.precip * 600.0f;
        weather_worker_active_.cloud_thick_m = std::lerp(weather_worker_active_.cloud_thick_m,
                                                   std::clamp(std::max(forcing.cloud_thick_m, vertical_depth), 700.0f, 4200.0f),
                                                   std::clamp(dt / 50.0f, 0.0f, 0.012f));
        mean_u /= n; mean_v /= n;
        weather_worker_active_.wind_speed_10m_mps = std::lerp(weather_worker_active_.wind_speed_10m_mps,
                                                        static_cast<float>(std::hypot(mean_u, mean_v)),
                                                        std::clamp(dt / 30.0f, 0.0f, 0.02f));
        if (weather_worker_active_.wind_speed_10m_mps > 0.05f)
            weather_worker_active_.wind_dir_from_deg = static_cast<float>(std::fmod(std::atan2(-mean_u, -mean_v) * RAD2DEG_D + 360.0, 360.0));
        weather_worker_active_.turb_scale = std::lerp(weather_worker_active_.turb_scale,
                                                std::clamp(static_cast<float>(mean_tke / n), 0.12f, 1.0f),
                                                std::clamp(dt / 25.0f, 0.0f, 0.02f));
        weather_worker_active_.haze_mult = std::clamp(1.0f + weather_worker_active_.cloud_cov * 0.30f + weather_worker_active_.precip * 0.55f, 0.8f, 2.0f);
        weather_worker_active_.visibility_m = std::clamp(forcing.visibility_m - weather_worker_active_.precip * 27000.0f, 900.0f, 40000.0f);
        weather_worker_active_.label = forcing.label + " | CPU 4L MESOSCALE";
        weather_worker_active_.summary = forcing.summary;
    }
}

void RegionalAtmosphere::update_field(const Vec3 &sun_dir, double sim_time_sec, bool force, float wx_advance01) {
    if (weather_model_ready_ && pack_ &&
        (std::abs(pack_->anchor_lat() - weather_anchor_lat_) > 1e-7 ||
         std::abs(pack_->anchor_lon() - weather_anchor_lon_) > 1e-7)) {
        stop_weather_worker();
        weather_model_ready_ = false;
        weather_model_time_ = -1.0;
        weather_model_accum_ = 0.0;
    }
    if (weather_model_ready_) latch_weather();
    if (!force && !dirty_ && (sim_time_sec - last_update_t_) < 0.25 && active_weather_init_) {
        upload_field();
        return;
    }
    last_update_t_ = sim_time_sec;
    const bool was_dirty = dirty_;
    dirty_ = false;

    int season = season_override_;
    if (season < 0) {
        std::time_t now = std::time(nullptr);
        std::tm utc{};
#ifdef _WIN32
        gmtime_s(&utc, &now);
#else
        gmtime_r(&now, &utc);
#endif
        season = WeatherLibrary::season_from_month(utc.tm_mon + 1);
    }
    float slot = slot_override_;
    if (slot < 0.0f) {
        slot = (sun_dir.y > 0.015f) ? std::clamp(sun_dir_to_phase10(sun_dir), 0.0f, 9.0f)
                                    : static_cast<float>(nspain::NSWX_NIGHT_SLOT);
    }
    float severity = std::clamp(wx_advance01, 0.0f, 1.0f);
    bool selected_field = false;
    if (lib_.valid() && (force || was_dirty || season != last_season_ || std::abs(slot - last_slot_) > 0.02f ||
        std::abs(severity - last_severity_) > 0.02f)) {
        const int lib_regime = std::min(static_cast<int>(regime_), 4);
        lib_.select(season, slot, lib_regime, severity);
        last_state_ = lib_.active_state();
        last_season_ = season;
        last_slot_ = slot;
        last_severity_ = severity;
        synoptic_weather_ = lib_.snapshot();
        // Shape distinct VFR synoptic regimes so every regime creates genuine pilot decision-making
        if (regime_ == WindRegime::CONVECTIVE_STORM) {
            // Keep broken-to-overcast (0.72..0.82) so towering Cb walls & dark bases sculpt in 3D instead of a flat gray lid!
            synoptic_weather_.cloud_cov = std::clamp(synoptic_weather_.cloud_cov, 0.72f, 0.82f);
            synoptic_weather_.precip = std::max(synoptic_weather_.precip, 0.78f);
            synoptic_weather_.precip_mm_hr = std::max(synoptic_weather_.precip_mm_hr, 16.0f);
            synoptic_weather_.storm_int = std::max(synoptic_weather_.storm_int, 0.88f);
            synoptic_weather_.turb_scale = std::max(synoptic_weather_.turb_scale, 0.80f);
            synoptic_weather_.cloud_base_m = std::min(synoptic_weather_.cloud_base_m, 1020.0f);
            synoptic_weather_.cloud_thick_m = std::max(synoptic_weather_.cloud_thick_m, 3400.0f);
            synoptic_weather_.wind_speed_10m_mps = std::max(synoptic_weather_.wind_speed_10m_mps, 11.5f);
            synoptic_weather_.label = "PYRENEAN STORM FRONT";
        } else if (regime_ == WindRegime::WINTER_ICING) {
            synoptic_weather_.oat_sl_c = -0.5f;      // Freezing across both Ebro basin (-3.8C) and Pyrenees (-7..-15C)!
            synoptic_weather_.dewpoint_sl_c = -1.1f; // Saturated freezing snow & active structural icing airmass
            synoptic_weather_.cloud_cov = std::clamp(synoptic_weather_.cloud_cov, 0.76f, 0.85f);
            synoptic_weather_.precip = std::max(synoptic_weather_.precip, 0.68f);
            synoptic_weather_.precip_mm_hr = std::max(synoptic_weather_.precip_mm_hr, 9.5f);
            synoptic_weather_.storm_int = std::max(synoptic_weather_.storm_int, 0.45f);
            synoptic_weather_.turb_scale = std::max(synoptic_weather_.turb_scale, 0.58f);
            synoptic_weather_.cloud_base_m = std::min(synoptic_weather_.cloud_base_m, 880.0f);
            synoptic_weather_.cloud_thick_m = std::max(synoptic_weather_.cloud_thick_m, 2600.0f);
            synoptic_weather_.wind_speed_10m_mps = std::max(synoptic_weather_.wind_speed_10m_mps, 9.5f);
            synoptic_weather_.label = "WINTER ICING & SNOW FRONT";
        } else if (regime_ == WindRegime::KATABATIC_DAWN) {
            synoptic_weather_.dewpoint_sl_c = std::max(synoptic_weather_.dewpoint_sl_c, synoptic_weather_.oat_sl_c - 1.2f);
            synoptic_weather_.cloud_base_m = std::min(synoptic_weather_.cloud_base_m, 750.0f);
            synoptic_weather_.haze_mult = std::max(synoptic_weather_.haze_mult, 1.55f);
            synoptic_weather_.label = "VALLEY RADIATION FOG";
        }
        refresh_climate_field();
        selected_field = true;
    }
    if (!active_weather_init_) {
        if (lib_.valid()) {
            synoptic_weather_ = lib_.snapshot();
            refresh_climate_field();
        }
        active_weather_ = synoptic_weather_;
        active_weather_init_ = true;
    }

    bool live_expired = false;
    const bool live_fresh = live_enabled_ && live_weather_available() && !manual_weather_override_;
    if (live_overlay_applied_ && !live_fresh) {
        synoptic_weather_ = lib_.valid() ? lib_.snapshot() : WeatherSnapshot{};
        refresh_climate_field();
        live_overlay_applied_ = false;
        live_expired = true;
    }
    if (live_fresh) {
        WeatherSnapshot target;
        {
            std::lock_guard<std::mutex> lk(live_mtx_);
            target = live_target_;
        }
        double dt = active_weather_time_ >= 0.0 ? std::clamp(sim_time_sec - active_weather_time_, 0.0, 5.0) : 0.0;
        float blend = active_weather_time_ < 0.0 ? 0.0f : static_cast<float>(1.0 - std::exp(-dt / 35.0));
        auto lerpf = [blend](float a, float b) { return std::lerp(a, b, blend); };
        synoptic_weather_.cloud_cov = lerpf(synoptic_weather_.cloud_cov, target.cloud_cov);
        synoptic_weather_.precip = lerpf(synoptic_weather_.precip, target.precip);
        synoptic_weather_.precip_mm_hr = lerpf(synoptic_weather_.precip_mm_hr, target.precip_mm_hr);
        synoptic_weather_.cloud_base_m = lerpf(synoptic_weather_.cloud_base_m, target.cloud_base_m);
        synoptic_weather_.cloud_thick_m = lerpf(synoptic_weather_.cloud_thick_m, target.cloud_thick_m);
        synoptic_weather_.oat_sl_c = lerpf(synoptic_weather_.oat_sl_c, target.oat_sl_c);
        synoptic_weather_.dewpoint_sl_c = lerpf(synoptic_weather_.dewpoint_sl_c, target.dewpoint_sl_c);
        synoptic_weather_.qnh_hpa = lerpf(synoptic_weather_.qnh_hpa, target.qnh_hpa);
        synoptic_weather_.haze_mult = lerpf(synoptic_weather_.haze_mult, target.haze_mult);
        synoptic_weather_.turb_scale = lerpf(synoptic_weather_.turb_scale, target.turb_scale);
        synoptic_weather_.visibility_m = lerpf(synoptic_weather_.visibility_m, target.visibility_m);
        synoptic_weather_.wind_dir_from_deg = target.wind_dir_from_deg;
        synoptic_weather_.wind_speed_10m_mps = lerpf(synoptic_weather_.wind_speed_10m_mps, target.wind_speed_10m_mps);
        synoptic_weather_.storm_int = lerpf(synoptic_weather_.storm_int, target.storm_int);
        synoptic_weather_.label = target.label;
        synoptic_weather_.summary = target.summary;
        live_overlay_applied_ = true;
    }
    active_weather_time_ = sim_time_sec;

    if (!weather_model_ready_ || force || live_expired || (was_dirty && selected_field)) {
        stop_weather_worker();
        solver_.synoptic_weather_ = synoptic_weather_;
        solver_.sun = sun_dir;
        solver_.source = manual_weather_override_ ? WeatherSource::Manual
            : live_fresh ? WeatherSource::Live : lib_.valid() ? WeatherSource::Baked : WeatherSource::Default;
        initialize_weather_model();
        weather_model_time_ = sim_time_sec;
    }

    int steps = 0;
    if (weather_model_time_ < 0.0) weather_model_time_ = sim_time_sec;
    else {
        double elapsed = std::max(0.0, sim_time_sec - weather_model_time_);
        weather_model_time_ = sim_time_sec;
        weather_model_accum_ += std::min(elapsed, 0.75);
        if (weather_model_accum_ > 1.0) weather_model_accum_ = 1.0;
        steps = std::min(4, static_cast<int>(weather_model_accum_ / 0.25));
        weather_model_accum_ -= steps * 0.25;
    }

    if (!weather_model_ready_) return;
    if (synchronous_solver_) {
        if (steps > 0) {
            solver_.regime_ = regime_;
            solver_.source = manual_weather_override_ ? WeatherSource::Manual
                : live_fresh ? WeatherSource::Live : lib_.valid() ? WeatherSource::Baked : WeatherSource::Default;
            if (selected_field) solver_.weather_worker_climate_field_ = climate_field_;
            for (int i = 0; i < steps; ++i) solver_.step(0.25f, sun_dir, synoptic_weather_);
            publish_weather_model();
            latch_weather();
        }
    } else {
        {
            std::lock_guard<std::mutex> lk(weather_job_mtx_);
            weather_worker_forcing_ = synoptic_weather_;
            weather_worker_regime_ = regime_;
            weather_worker_source_ = manual_weather_override_ ? WeatherSource::Manual
                : live_fresh ? WeatherSource::Live : lib_.valid() ? WeatherSource::Baked : WeatherSource::Default;
            weather_worker_sun_ = sun_dir;
            if (selected_field) {
                weather_pending_climate_field_ = climate_field_;
                weather_pending_climate_ = true;
            }
            weather_pending_steps_ = std::min(4, weather_pending_steps_ + steps);
        }
        start_weather_worker();
        weather_job_cv_.notify_one();
    }
    upload_field();
}

LocalAirSample RegionalAtmosphere::sample_at(
    double lat_deg, double lon_deg, double alt_msl_m,
    const Vec3 &sun_dir, double sim_time_sec, float wx_advance01,
    double dt_sec, double heading_deg, double tas_mps
) const {
    LocalAirSample out{};
    if ((!lib_.valid() && !weather_model_ready_) || !pack_) {
        const WeatherSnapshot &wx = synoptic_weather_;
        const double dir = wx.wind_dir_from_deg * DEG2RAD_D;
        out.wind_east_mps = -std::sin(dir) * wx.wind_speed_10m_mps;
        out.wind_north_mps = -std::cos(dir) * wx.wind_speed_10m_mps;
        out.qnh_hpa = wx.qnh_hpa;
        out.oat_c = wx.oat_sl_c - alt_msl_m * 0.0065;
        return out;
    }
    const WeatherSnapshot &ws = frame_forcing();

    const bool model_here = covers(lat_deg, lon_deg);
    const double east_m = (lon_deg - pack_->anchor_lon()) * pack_->anchor_m_lon();
    const double north_m = (lat_deg - pack_->anchor_lat()) * pack_->anchor_m_lat();
    const double d_lat = 80.0 / pack_->anchor_m_lat();
    const double d_lon = 80.0 / pack_->anchor_m_lon();
    const double g_here = pack_->sample_elevation(lat_deg, lon_deg);
    const double dz_de = (pack_->sample_elevation(lat_deg, lon_deg + d_lon) -
                          pack_->sample_elevation(lat_deg, lon_deg - d_lon)) / 160.0;
    const double dz_dn = (pack_->sample_elevation(lat_deg + d_lat, lon_deg) -
                          pack_->sample_elevation(lat_deg - d_lat, lon_deg)) / 160.0;
    const double agl = std::max(0.0, alt_msl_m - g_here);
    const Vec3 surf = pack_->sample_surface(lat_deg, lon_deg); // x=forest, z=water

    if (agl < 550.0) out.atmo_layer = 0;
    else if (alt_msl_m < 2650.0) out.atmo_layer = 1;
    else if (alt_msl_m < 5400.0) out.atmo_layer = 2;
    else out.atmo_layer = 3;

    SimWeatherCell sim_cell = sample_weather_model(east_m, north_m, agl);
    WeatherCell cell = lib_.valid() ? lib_.sample(east_m, north_m, agl) : WeatherCell{};
    if (model_here) {
        cell.u = sim_cell.u;
        cell.v = sim_cell.v;
        cell.w = sim_cell.w;
        cell.cloud = sim_cell.cloud_fraction - ws.cloud_cov;
    }
    // Add live grid-steered storm/shower cells (outflow gusts, updraft/downdraft, rain shafts)
    float dyn_cloud = 0.0f, dyn_rain = 0.0f, dyn_w = 0.0f, dyn_oe = 0.0f, dyn_on = 0.0f, dyn_tke = 0.0f;
    if (model_here) evaluate_dynamic_cells(east_m, north_m, agl,
                           dyn_cloud, dyn_rain, dyn_w, dyn_oe, dyn_on, dyn_tke);
    double we = cell.u + dyn_oe, wn = cell.v + dyn_on;
    if (!model_here && live_weather_available() && !manual_weather_override_) {
        const double dir = ws.wind_dir_from_deg * DEG2RAD_D;
        const double live_e = -std::sin(dir) * ws.wind_speed_10m_mps;
        const double live_n = -std::cos(dir) * ws.wind_speed_10m_mps;
        we = std::lerp(we, live_e, 0.55);
        wn = std::lerp(wn, live_n, 0.55);
    }

    // Diagnostic decomposition of the solved + dynamic vertical velocity field.
    const double ridge_decay = std::exp(-agl / 680.0);
    out.ridge_lift_mps = std::clamp((we * dz_de + wn * dz_dn) * ridge_decay * 1.18, -6.5, 6.5);
    double baked_wave = model_here ? sim_cell.w * 0.30 : cell.w_upper * 0.55;
    out.wave_lift_mps = std::clamp(baked_wave * std::exp(-agl / 2600.0), -4.0, 4.0);
    out.thermal_lift_mps = std::clamp(static_cast<double>(cell.w + dyn_w) - out.ridge_lift_mps - out.wave_lift_mps, -7.5, 7.5);
    const float solar = smooth01(0.04f, 0.38f, sun_dir.y);
    if (surf.z > 0.22f && solar > 0.25f) out.thermal_lift_mps = std::min(out.thermal_lift_mps, 0.0);

    out.in_lee_rotor = (out.wave_lift_mps < -1.0 && ws.turb_scale > 0.50 && agl < 950.0);
    out.in_thermal = (out.thermal_lift_mps > 0.75);
    const double local_precip = std::clamp<double>((model_here ? sim_cell.rain_mm_hr : ws.precip_mm_hr) + dyn_rain, 0.0, 48.0);
    out.precipitation_mm_hr = local_precip;
    const double local_cloud = std::clamp<double>((model_here ? sim_cell.cloud_fraction : ws.cloud_cov + cell.cloud) + dyn_cloud, 0.0, 1.0);
    out.local_cloud_frac = static_cast<float>(local_cloud);
    const double c_base_msl = std::max(g_here + 150.0, static_cast<double>(ws.cloud_base_m));
    const double c_top_msl = c_base_msl + std::max(600.0, static_cast<double>(ws.cloud_thick_m));
    out.in_cloud = (alt_msl_m >= c_base_msl && alt_msl_m <= c_top_msl && local_cloud > 0.52) ||
                   (frame_regime() == WindRegime::KATABATIC_DAWN && agl < 320.0 && local_cloud > 0.62);
    out.in_storm_cell = (std::max(ws.storm_int, active_weather_.storm_int) > 0.42f &&
                         local_precip > 2.5 && local_cloud > 0.35) || (dyn_rain > 6.0f);

    // Real vertical wind shear + multi-point spanwise differential shear directly from the 4-layer grid!
    double grid_roll_shear = 0.0, grid_vert_shear = 0.0;
    if (model_here) {
        const SimWeatherCell upper = sample_weather_model(east_m, north_m, agl + 180.0);
        grid_vert_shear = std::hypot(upper.u - sim_cell.u, upper.v - sim_cell.v) / 180.0;
        const double hdg_r = heading_deg * DEG2RAD_D;
        // Right-wing unit vector in (East, North) is (cos(hdg), -sin(hdg)); probe grid gradient across +-180 m
        const double re = std::cos(hdg_r) * 180.0, rn = -std::sin(hdg_r) * 180.0;
        const SimWeatherCell rw = sample_weather_model(east_m + re, north_m + rn, agl);
        const SimWeatherCell lw = sample_weather_model(east_m - re, north_m - rn, agl);
        // dw/dy across the wing creates an immediate aerodynamic roll torque (left updraft rolls right -> +p)
        grid_roll_shear = std::clamp((lw.w - rw.w) / 360.0 * 4.5, -0.18, 0.18);
    }

    // Physical turbulence intensity derived directly from solved grid TKE, vertical shear, rotor & convective cores!
    const double cell_tke = (model_here ? sim_cell.tke : ws.turb_scale) + dyn_tke;
    out.turb_rms = 0.04 + cell_tke * 0.30 + std::abs(cell.w + dyn_w) * 0.022 +
                   grid_vert_shear * 2.2 + (out.in_lee_rotor ? 0.24 : 0.0) + (out.in_storm_cell ? 0.22 : 0.0);

    // Hardware-seeded MIL-F-8785C 3D Stochastic Dryden Turbulence (ZERO deterministic sine waves!)
    const double sigma_w = std::clamp(std::sqrt(std::max(0.0, cell_tke * 1.45)) * (0.45 + 0.55 * out.turb_rms) +
                                      (out.in_lee_rotor ? 1.45 : 0.0) + (out.in_storm_cell ? 1.85 : 0.0), 0.06, 6.2);
    const double abl_aniso = (agl < 305.0) ? (1.0 / std::pow(0.177 + 0.0027 * std::max(6.0, agl), 0.4)) : 1.0;
    const double sigma_u = std::clamp(sigma_w * std::clamp(abl_aniso, 1.0, 1.85), 0.08, 8.5);
    const double sigma_v = sigma_u;

    DrydenGustSample gust = (dt_sec > 1e-5)
        ? dryden_filter_.step(dt_sec, tas_mps, agl, sigma_u, sigma_v, sigma_w, 11.0)
        : DrydenGustSample{};
    // Project aircraft-aligned (ug, vg) back into ENU wind components + add stochastic vertical gust wg!
    const double hr = heading_deg * DEG2RAD_D;
    const double fwd_e = std::sin(hr), fwd_n = std::cos(hr);
    const double rgt_e = std::cos(hr), rgt_n = -std::sin(hr);
    we += fwd_e * gust.ug_mps + rgt_e * gust.vg_mps;
    wn += fwd_n * gust.ug_mps + rgt_n * gust.vg_mps;
    out.gust_mps = std::hypot(gust.ug_mps, gust.vg_mps) * (gust.ug_mps >= 0.0 ? 1.0 : -1.0);
    out.p_turb_rps = std::clamp(gust.pg_rps + grid_roll_shear, -0.60, 0.60);
    out.q_turb_rps = gust.qg_rps;
    out.r_turb_rps = gust.rg_rps;

    // Solved temperature + nocturnal valley inversion + rain cold pool, plus real local barometric QNH!
    const double night_inv = (1.0 - solar) * 2.1 * smooth01(40.0f, 420.0f, static_cast<float>(agl)) *
                             (1.0 - smooth01(420.0f, 950.0f, static_cast<float>(agl)));
    out.oat_c = (model_here ? sim_cell.temp_c : ws.oat_sl_c - alt_msl_m * 0.0065) +
                night_inv - std::clamp(local_precip / 12.0, 0.0, 1.0) * 1.8 - (1.0 - solar) * 3.2;
    if (model_here) {
        // Reduce local solved pressure back to sea-level QNH so troughs/ridges shift altimeter pressure!
        const double local_qnh = sim_cell.pressure_hpa * std::exp(agl / 8500.0);
        out.qnh_hpa = std::clamp(local_qnh, 975.0, 1045.0);
    } else {
        out.qnh_hpa = ws.qnh_hpa;
    }
    out.cloud_delta = static_cast<float>(std::clamp(cell.cloud + dyn_cloud, -0.5f, 0.65f));
    if (model_here) {
        const float qsat = saturation_mixing_ratio_gkg(sim_cell.temp_c, sim_cell.pressure_hpa);
        out.relative_humidity = std::clamp(sim_cell.vapor_gkg / std::max(0.1f, qsat) + (local_precip > 0.5 ? 0.12 : 0.0), 0.0, 1.0);
    } else {
        const double dew_c = ws.dewpoint_sl_c - alt_msl_m * 0.0017;
        const float vapor = saturation_vapor_pressure_hpa(static_cast<float>(dew_c));
        const float sat = saturation_vapor_pressure_hpa(static_cast<float>(out.oat_c));
        out.relative_humidity = std::clamp(static_cast<double>(vapor / std::max(0.1f, sat)), 0.0, 1.0);
    }
    out.snow_factor = smooth01(1.8f, -1.2f, static_cast<float>(out.oat_c));
    const float freezing = smooth01(0.8f, -3.5f, static_cast<float>(out.oat_c));
    const float vis_moist = std::clamp(static_cast<float>(local_precip / 4.0) + (out.in_cloud ? 0.85f : 0.0f) +
                                       smooth01(0.86f, 0.98f, static_cast<float>(out.relative_humidity)) * 0.45f, 0.0f, 1.0f);
    out.icing_intensity = freezing * vis_moist;
    out.lightning_flash = lightning_flash_at(east_m, north_m, sim_time_sec);
    out.wind_east_mps = we;
    out.wind_north_mps = wn;
    out.wind_up_mps = std::clamp(out.ridge_lift_mps + out.wave_lift_mps + out.thermal_lift_mps + gust.wg_mps, -12.0, 12.0);
    return out;
}

} // namespace nsim
