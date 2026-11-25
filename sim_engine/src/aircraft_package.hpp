#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace nsim {

// A directory based aircraft add-on. Paths in aircraft.ini are relative to the
// package unless they start with @shared/, which addresses the game's shared
// asset tree (intended for built-in packages only).
struct AircraftPackage {
    std::string id;
    std::string name;
    std::string manufacturer;
    std::string model;
    std::string registration;
    std::string systems_profile;
    std::filesystem::path root;
    std::filesystem::path flight_model_dir;
    std::string exterior_model;
    std::string cockpit_model;
    std::string rain_front;
    std::string rain_side;
    double cockpit_eye_height_m = 0.635;
    double max_flaps_deg = 30.0;
    double left_tank_capacity_gal = 26.5;
    double right_tank_capacity_gal = 26.5;
    double oil_capacity_quarts = 8.0;
    double bus_voltage = 28.0;
    std::unordered_map<std::string, std::string> nodes;
};

struct AircraftPackageResult {
    std::vector<AircraftPackage> packages;
    std::vector<std::string> warnings;
};

AircraftPackageResult discover_aircraft_packages(const std::vector<std::filesystem::path> &roots);
bool resolve_aircraft_asset(const AircraftPackage &package, const std::string &asset,
                            const std::filesystem::path &shared_asset_root,
                            std::filesystem::path &resolved, std::string &error);

} // namespace nsim
