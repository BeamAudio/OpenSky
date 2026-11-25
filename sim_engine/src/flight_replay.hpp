#pragma once

#include "aircraft_hud.hpp"
#include "aircraft_session.hpp"
#include "pack_reader.hpp"
#include "regional_atmosphere.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace nsim {

struct ScopedTempDir {
    std::filesystem::path dir;
    explicit ScopedTempDir(const char *prefix = "opensky-flight-XXXXXX");
    ~ScopedTempDir();
};

struct ReplayStep {
    double dt = 1.0 / 60.0;
    uint8_t ap_master = 0;
    uint8_t ap_nav_hold = 0;
    uint8_t ap_hdg_hold = 0;
    uint8_t ap_alt_hold = 0;
    float ap_hdg_deg = 0.0f;
    float ap_alt_ft = 0.0f;
    float flaps_deg = 0.0f;
    float roll_cmd = 0.0f;
    float pitch_cmd = 0.0f;
    float throttle_tgt = 0.0f;
    float brake_cmd = 0.0f;
};

struct FlightReplayTrace {
    std::string start_airport = "ES-0205";
    std::string dest_airport = "LESU";
    int weather_regime = 0; // WindRegime::PYRENEAN_THERMAL
    int season = 2;         // Summer clear daylight
    float wx_advance = 0.22f;
    std::vector<FleetRoutePoint> route;
    std::vector<ReplayStep> steps;
};

struct BoundaryMetrics {
    bool crossed_region = false;
    std::string start_region;
    std::string crossed_to_region;
    double cross_sim_time_s = -1.0;
    double cross_wall_ms = -1.0;
    double commit_wall_ms = -1.0;
    double sharp_terrain_wall_ms = -1.0;
    int pre_cross_max_lod = 0;
    double time_to_sharp_since_cross_ms = -1.0;
    double time_to_sharp_since_commit_ms = -1.0;
};

bool save_flight_replay(const std::filesystem::path &path, const FlightReplayTrace &trace);
bool load_flight_replay(const std::filesystem::path &path, FlightReplayTrace &out);

bool prepare_replay_session(
    const PackReader &pack,
    const FlightReplayTrace &trace,
    const std::filesystem::path &profile_path,
    AircraftSession &session,
    AircraftAndHud &aircraft,
    double ambient_temp_c
);

void apply_replay_step(
    AircraftAndHud &aircraft,
    AircraftSession &session,
    const ReplayStep &step,
    const AtmosphereState &atmo,
    const RegionalAtmosphere &reg_atmo,
    double sim_time_sec
);

bool verify_replay_persistence(
    const PackReader &pack,
    AircraftSession &session,
    AircraftAndHud &aircraft,
    const std::filesystem::path &profile_path,
    const FlightReplayTrace &trace,
    double start_fuel_gal,
    bool crossed_region
);

bool record_castejon_catalonia_flight(PackReader &pack, const std::filesystem::path &trace_path);
bool replay_flight_headless(PackReader &pack, const std::filesystem::path &trace_path);

struct RouteVideoFrame {
    double sim_t = 0.0;
    double lat_deg = 0.0;
    double lon_deg = 0.0;
    double alt_msl_m = 0.0;
    double heading_deg = 0.0;
    double pitch_deg = 0.0;
    double roll_deg = 0.0;
    double ias_kt = 0.0;
    double rpm = 0.0;
    double flaps_deg = 0.0;
    double throttle = 0.0;
    double aileron = 0.0;
    double elevator = 0.0;
    double rudder = 0.0;
};

struct RouteVideoRecording {
    std::string title;
    std::vector<RouteVideoFrame> frames;
    double duration_sec() const {
        return frames.size() >= 2 ? std::max(0.0, frames.back().sim_t - frames.front().sim_t) : 0.0;
    }
};

bool save_route_video(const std::filesystem::path &path, const RouteVideoRecording &rec);
bool load_route_video(const std::filesystem::path &path, RouteVideoRecording &out);
RouteVideoFrame sample_route_video(const RouteVideoRecording &rec, double playback_t);

} // namespace nsim
