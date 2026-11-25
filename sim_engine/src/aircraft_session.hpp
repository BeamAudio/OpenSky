#pragma once

#include "fleet_profile.hpp"

namespace nsim {

class AircraftAndHud;
class GameFrontend;
class PackReader;
struct AirportInfo;
struct LocalAirSample;

enum class AircraftService {
    TopOil, ChangeOil, CleanPlugs, RepairProp, Overhaul, FillLeft, FillBoth,
    DrainSumps, TogglePitot, ToggleTieDowns, ToggleChocks, RepairGear,
    RepairAirframe, ReplaceWindshield, ChargeBattery, CleanWindshield, ToggleSecuring
};

class AircraftSession {
public:
    void init(const PackReader &pack, std::filesystem::path profile_file);
    std::string begin_walkaround(const AirportInfo &airport, const LocalAirSample *weather);
    void board(AircraftAndHud &aircraft, double ambient_temp_c);
    bool can_park(const AircraftAndHud &aircraft) const;
    bool park(AircraftAndHud &aircraft, const AirportInfo &airport);
    void step_flight(AircraftAndHud &aircraft, double dt);
    void save_profile() const;
    static bool full_service(const std::string &airport_id);
    bool can_add_fuel(bool left_tank) const;
    void adjust_fuel(bool left_tank, bool add);
    void service(AircraftService action);
    bool add_route_point(FleetRoutePoint point);
    void remove_route_point(size_t index);
    void clear_route();
    const FleetProfile &profile() const { return profile_; }
    size_t active_leg() const { return active_leg_; }
    const std::string &profile_error() const { return profile_error_; }

private:
    friend class GameFrontend;
    void prepare_parking(const AirportInfo &airport);
    const PackReader *pack_ = nullptr;
    std::filesystem::path profile_file_;
    mutable std::string profile_error_;
    bool preserve_invalid_profile_ = false;
    FleetProfile profile_{};
    bool flight_active_ = false;
    size_t active_leg_ = 0;
    double remote_fuel_added_ = 0.0; // Caps remote-strip jerrycan refills per visit
    // Flight tracking & touchdown debrief
    std::string dep_airport_id_;
    double flight_elapsed_s_ = 0.0, flight_airborne_total_s_ = 0.0;
    double flight_dist_m_ = 0.0, flight_start_fuel_ = 0.0;
    double prev_lat_ = 0.0, prev_lon_ = 0.0;
    double airborne_seconds_ = 0.0;
    double last_sink_ = 0.0, last_g_ = 1.0, save_elapsed_ = 0.0;
    bool was_grounded_ = true;
    bool has_touchdown_ = false;
    double td_fpm_ = 0.0, td_xwind_kt_ = 0.0, td_g_ = 1.0, td_banner_s_ = 0.0;
    std::string td_surface_, td_grade_;
};

} // namespace nsim
