#pragma once

#include "gl_loader.hpp"
#include "math3d.hpp"
#include "platform_io.hpp"
#include "../../protocol/weather_format.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace nsim {

struct WeatherSnapshot {
    float cloud_cov = 0.30f;
    float precip = 0.0f;
    float precip_mm_hr = 0.0f;
    float cloud_base_m = 2200.0f;
    float cloud_thick_m = 1600.0f;
    float oat_sl_c = 15.0f;
    float dewpoint_sl_c = 8.0f;
    float qnh_hpa = 1016.0f;
    float haze_mult = 1.0f;
    float turb_scale = 0.30f;
    float visibility_m = 30000.0f;
    float wind_dir_from_deg = 270.0f;
    float wind_speed_10m_mps = 5.0f;
    float storm_int = 0.0f;
    float wave_amp = 0.0f;
    float snowline_m = 2600.0f;
    int season = 1;
    int slot = 2;
    int wtype = 0;
    std::string label;
    std::string summary;
};

struct WeatherModelHistoryEntry {
    double sim_time_sec = 0.0;
    float temperature_c = 0.0f;
    float wind_direction_from_deg = 0.0f;
    float wind_speed_10m_mps = 0.0f;
    float cloud_cover_pct = 0.0f;
    float precipitation_mm_hr = 0.0f;
    std::string region_id;
};

struct WeatherCell {
    float u = 0.0f, v = 0.0f, w = 0.0f, cloud = 0.0f, w_upper = 0.0f;
};

// Replays a pre-baked .nswx state library. Nothing is solved at runtime: states
// are decompressed on demand (bounded LRU), interpolated bi-linearly in space,
// tri-linearly in AGL, and blended across adjacent solar slots / severity.
class WeatherLibrary {
public:
    ~WeatherLibrary();
    bool load(const std::string &path);
    void shutdown();
    bool valid() const { return valid_; }
    const char *provenance() const { return hdr_.provenance; }

    void select(int season, float slot_f, int wtype, float severity);
    const WeatherSnapshot &snapshot() const { return snapshot_; }
    const std::string &label() const { return snapshot_.label; }

    WeatherCell sample(double east_m, double north_m, double agl_m) const;
    const std::vector<float> &field_rgba() const { return field_rgba_; }
    int active_state() const { return active_state_; }

    static const char *season_name(int s);
    static const char *slot_name(int s);
    static const char *type_name(int t);
    static int season_from_month(int month);

private:
    struct DecodedState {
        std::vector<float> f; // LEVELS * H * W * 4
    };
    const DecodedState *state(int idx) const;

    FileHandle fd_ = kInvalidFileHandle;
    bool valid_ = false;
    nspain::NswxHeader hdr_{};
    std::vector<nspain::NswxStateEntry> entries_;
    mutable std::unordered_map<int, DecodedState> cache_;
    mutable std::vector<int> lru_;

    struct Blend {
        int idx = 0;
        float w = 1.0f;
    };
    Blend blend_[4]{};
    int blend_n_ = 1;
    WeatherSnapshot snapshot_{};
    std::vector<float> field_rgba_;
    int active_state_ = 0;

    double min_e_ = 0.0, min_n_ = 0.0, span_e_ = 1.0, span_n_ = 1.0;
};

} // namespace nsim
