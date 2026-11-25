#pragma once

#include "aircraft_condition.hpp"
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace nsim {

struct AircraftLocation {
    double lat = 0, lon = 0, alt = 0, heading = 0, pitch = 0;
};
struct FleetFlightLogEntry {
    std::string dep_id, arr_id, grade;
    double minutes = 0.0, distance_nm = 0.0, fuel_gal = 0.0;
    double touchdown_fpm = 0.0, crosswind_kt = 0.0;
};
struct FleetPassportStamp {
    std::string airport_id;
    int visits = 0;
    double best_fpm = 9999.0;
};
struct FleetRoutePoint { std::string label; double lat, lon; };
inline constexpr size_t max_route_points = 12;

struct FleetProfile {
    std::string parked_id_;
    std::vector<FleetRoutePoint> route_;
    AircraftLocation parking_spot_{};
    bool has_custom_park_ = false;
    AircraftCareSnapshot care_{};
    double hours_ = 0;
    int64_t last_park_epoch_ = 0;
    int total_landings_ = 0;
    double best_landing_fpm_ = 9999;
    std::vector<FleetPassportStamp> passport_;
    std::vector<FleetFlightLogEntry> logbook_;
};

enum class ProfileLoad { Missing, Loaded, Invalid };
ProfileLoad read_fleet_profile(const std::filesystem::path &path, FleetProfile &out);
bool write_fleet_profile(const std::filesystem::path &path, const FleetProfile &profile, std::string &error);

} // namespace nsim
