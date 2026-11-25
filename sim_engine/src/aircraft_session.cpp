#include "aircraft_session.hpp"
#include "aircraft_hud.hpp"
#include "pack_reader.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <utility>

namespace nsim {
namespace {
constexpr double nm_m = 1852.0;
double sample_aircraft_ground(const PackReader &pack, double lat, double lon, double heading_deg) {
    double mlat, mlon;
    nspain::meters_per_degree(lat, mlat, mlon);
    const double hr = heading_deg * DEG2RAD_D;
    const double fe = std::sin(hr), fn = std::cos(hr);
    const double re = std::cos(hr), rn = -std::sin(hr);
    const double pts[4][2] = {{0.0, 0.0}, {fe * 2.0, fn * 2.0}, {-fe * 0.6 + re * 1.5, -fn * 0.6 + rn * 1.5},
                              {-fe * 0.6 - re * 1.5, -fn * 0.6 - rn * 1.5}};
    double gnd = -1e9;
    for (const auto &p : pts) {
        const double plat = lat + p[1] / mlat;
        const double plon = lon + p[0] / mlon;
        double e = pack.sample_elevation(plat, plon);
        double rwy_e = 0.0, mu = 0.0, rough = 0.0;
        std::string rwy_id;
        if (pack.query_runway(plat, plon, rwy_e, mu, rough, rwy_id)) e = std::max(e, rwy_e + 0.35);
        gnd = std::max(gnd, e);
    }
    return gnd;
}

double bearing(double a_lat, double a_lon, double b_lat, double b_lon) {
    const double a = a_lat * DEG2RAD_D, b = b_lat * DEG2RAD_D;
    const double d = (b_lon - a_lon) * DEG2RAD_D;
    return std::fmod(360.0 + std::atan2(std::sin(d) * std::cos(b),
        std::cos(a) * std::sin(b) - std::sin(a) * std::cos(b) * std::cos(d)) * RAD2DEG_D, 360.0);
}

} // namespace

void AircraftSession::init(const PackReader &pack, std::filesystem::path profile_file) {
    pack_ = &pack;
    profile_file_ = std::move(profile_file);
    preserve_invalid_profile_ = !profile_file_.empty() &&
        read_fleet_profile(profile_file_, profile_) == ProfileLoad::Invalid;
    if (profile_.parked_id_.empty()) {
        const auto *home = pack.find_airport("ES-0205");
        if (!home) home = pack.find_airport("LECS");
        if (home) profile_.parked_id_ = home->id;
    }
    if (preserve_invalid_profile_) {
        profile_error_ = "Aircraft save could not be read. The original file has been kept.";
        std::fprintf(stderr, "[FLEET] Invalid or unreadable profile: %s\n", profile_file_.c_str());
    }
}

void AircraftSession::save_profile() const {
    if (profile_file_.empty() || preserve_invalid_profile_) return;
    if (!write_fleet_profile(profile_file_, profile_, profile_error_))
        std::fprintf(stderr, "[FLEET] %s (%s)\n", profile_error_.c_str(), profile_file_.c_str());
}

bool AircraftSession::full_service(const std::string &id) {
    return id == "ES-0205" || id == "LECS" || id == "LECI" || id == "LEHC" || id == "LEZG";
}

bool AircraftSession::can_add_fuel(bool left_tank) const {
    const auto &care = profile_.care_;
    return !flight_active_ && (left_tank ? care.left_fuel_gal : care.right_fuel_gal) < 26.45 &&
           (full_service(profile_.parked_id_) || remote_fuel_added_ < 10.0);
}

void AircraftSession::adjust_fuel(bool left_tank, bool add) {
    if (flight_active_) return;
    auto &care = profile_.care_;
    auto &tank = left_tank ? care.left_fuel_gal : care.right_fuel_gal;
    if (!add) {
        tank = std::max(0.0, tank - 5.0);
    } else {
        if (!can_add_fuel(left_tank)) return;
        const bool hangar = full_service(profile_.parked_id_);
        const double amount = std::min({5.0, 26.5 - tank, hangar ? 5.0 : 10.0 - remote_fuel_added_});
        tank += amount;
        if (!hangar) remote_fuel_added_ += amount;
    }
    save_profile();
}

void AircraftSession::service(AircraftService action) {
    if (flight_active_) return;
    const bool hangar = full_service(profile_.parked_id_);
    auto &c = profile_.care_;
    switch (action) {
        case AircraftService::TopOil: c.oil_quarts = std::min(8.0, c.oil_quarts + 1.0); break;
        case AircraftService::ChangeOil:
            if (!hangar) return;
            c.oil_quarts = 7.6;
            c.oil_hours = 0;
            break;
        case AircraftService::CleanPlugs: c.spark_fouling = 0; break;
        case AircraftService::RepairProp:
            c.prop_nicks = hangar ? 0 : std::max(0.0, c.prop_nicks - 0.35);
            break;
        case AircraftService::Overhaul:
            if (!hangar) return;
            c.cylinder_wear = c.shock_cooling = 0;
            break;
        case AircraftService::FillLeft:
        case AircraftService::FillBoth:
            if (!hangar) return;
            c.left_fuel_gal = 26.5;
            if (action == AircraftService::FillBoth) c.right_fuel_gal = 26.5;
            break;
        case AircraftService::DrainSumps: c.sump_water = 0; break;
        case AircraftService::TogglePitot: c.pitot_cover_on = !c.pitot_cover_on; break;
        case AircraftService::ToggleTieDowns: c.tie_downs_on = !c.tie_downs_on; break;
        case AircraftService::ToggleChocks: c.chocks_on = !c.chocks_on; break;
        case AircraftService::RepairGear:
            c.tire_wear = hangar ? 0 : std::min(c.tire_wear, 0.25);
            c.brake_wear = hangar ? 0 : std::min(c.brake_wear, 0.25);
            c.gear_alignment = hangar ? 0 : c.gear_alignment * 0.4;
            break;
        case AircraftService::RepairAirframe:
            c.airframe_damage = hangar ? 0 : std::min(c.airframe_damage, 0.25);
            break;
        case AircraftService::ChargeBattery:
            c.battery_soc = hangar ? 0.98 : std::max(c.battery_soc, 0.55);
            break;
        case AircraftService::CleanWindshield: c.windshield_dirt = 0; break;
        case AircraftService::ToggleSecuring:
            c.chocks_on = c.pitot_cover_on = c.tie_downs_on =
                !(c.chocks_on || c.pitot_cover_on || c.tie_downs_on);
            break;
    }
    save_profile();
}

bool AircraftSession::add_route_point(FleetRoutePoint point) {
    auto &route = profile_.route_;
    if (flight_active_ || route.size() >= max_route_points ||
        (!route.empty() && nspain::haversine_m(route.back().lat, route.back().lon, point.lat, point.lon) < 30))
        return false;
    route.push_back(std::move(point));
    save_profile();
    return true;
}

void AircraftSession::remove_route_point(size_t index) {
    if (flight_active_ || index >= profile_.route_.size()) return;
    profile_.route_.erase(profile_.route_.begin() + index);
    save_profile();
}

void AircraftSession::clear_route() {
    if (flight_active_) return;
    profile_.route_.clear();
    save_profile();
}

void AircraftSession::prepare_parking(const AirportInfo &ap) {
    const bool valid_custom_park = profile_.has_custom_park_ && profile_.parked_id_ == ap.id &&
        nspain::haversine_m(profile_.parking_spot_.lat, profile_.parking_spot_.lon, ap.lat, ap.lon) <= 2500.0;
    if (!valid_custom_park) {
        if (profile_.parked_id_ == ap.id) profile_.has_custom_park_ = false;
        const auto stands = pack_->compute_parking_stands(ap, 1);
        const ParkingStand &st = stands.front();
        // C172 exterior GLB tires reach -1.358 m below origin; clear the highest of all 3 wheel footprints.
        profile_.parking_spot_ = {st.lat, st.lon, sample_aircraft_ground(*pack_, st.lat, st.lon, st.heading_deg) + 1.38, st.heading_deg, 0};
    }

    if (valid_custom_park) {
        auto &p = profile_.parking_spot_;
        p.alt = sample_aircraft_ground(*pack_, p.lat, p.lon, p.heading) + 1.38;
    }
}

std::string AircraftSession::begin_walkaround(const AirportInfo &ap, const LocalAirSample *parked_weather) {
    prepare_parking(ap);
    std::string notice;
    // Apply real-time parked exposure (battery drain if Master left on, rain/dew sumps, untied wind wear)
    const int64_t now_epoch = static_cast<int64_t>(std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
    if (profile_.parked_id_ == ap.id && profile_.last_park_epoch_ > 0 && now_epoch > profile_.last_park_epoch_ + 120) {
        const double gap_hr = std::min(24.0, static_cast<double>(now_epoch - profile_.last_park_epoch_) / 3600.0);
        if (profile_.care_.master_on) {
            profile_.care_.battery_soc = std::max(0.02, profile_.care_.battery_soc - gap_hr * 0.18);
            notice = "Master switch was left ON while parked - battery drained!";
        }
        if (parked_weather) {
            const double wind_kt = std::hypot(parked_weather->wind_east_mps, parked_weather->wind_north_mps) * 1.94384;
            if ((parked_weather->precipitation_mm_hr > 0.25 || parked_weather->relative_humidity > 0.84) && gap_hr > 0.25) {
                profile_.care_.sump_water = std::min(1.0, profile_.care_.sump_water + 0.28);
                profile_.care_.windshield_dirt = std::min(1.0, profile_.care_.windshield_dirt + 0.18);
                if (notice.empty()) notice = "Moisture while parked - drain wing fuel sumps.";
            }
            if (!profile_.care_.tie_downs_on && wind_kt > 16.0) {
                profile_.care_.airframe_damage = std::min(0.65, profile_.care_.airframe_damage + 0.06);
                notice = "Gusty wind buffeted untied aircraft! Inspect tail & tie down.";
            }
        }
        profile_.last_park_epoch_ = now_epoch;
        save_profile();
    } else if (parked_weather) {
        const double wind_kt = std::hypot(parked_weather->wind_east_mps, parked_weather->wind_north_mps) * 1.94384;
        if (parked_weather->precipitation_mm_hr > 0.5 && profile_.care_.sump_water < 0.25)
            profile_.care_.sump_water = std::min(1.0, profile_.care_.sump_water + 0.20);
        if (parked_weather->oat_c < 6.0)
            notice = "Cold air outside - prime engine and warm oil before full power.";
        else if (wind_kt > 16.0 && !profile_.care_.tie_downs_on)
            notice = "Strong gusts (>16 kt) - use wing tie-downs when leaving aircraft.";
    }
    remote_fuel_added_ = 0.0;
    return notice;
}

void AircraftSession::board(AircraftAndHud &aircraft, double ambient_temp_c) {
    flight_active_ = true;
    was_grounded_ = true;
    active_leg_ = 0;
    dep_airport_id_ = profile_.parked_id_;
    flight_elapsed_s_ = flight_airborne_total_s_ = flight_dist_m_ = 0.0;
    flight_start_fuel_ = profile_.care_.left_fuel_gal + profile_.care_.right_fuel_gal;
    prev_lat_ = profile_.parking_spot_.lat;
    prev_lon_ = profile_.parking_spot_.lon;
    save_elapsed_ = last_sink_ = airborne_seconds_ = 0.0;
    last_g_ = 1.0;
    has_touchdown_ = false;
    td_banner_s_ = 0.0;
    // Always board at the parked location; taxi manually to the runway for departure.
    aircraft.start_owned_flight(profile_.parking_spot_.lat, profile_.parking_spot_.lon, profile_.parking_spot_.alt,
        profile_.parking_spot_.heading, profile_.care_, ambient_temp_c, false);
}

bool AircraftSession::can_park(const AircraftAndHud &aircraft) const {
    if (!flight_active_ || !aircraft.jsb_state().on_ground || aircraft.state().speed_mps > 1.8 ||
        aircraft.state().slew_fast) return false;
    // Must taxi clear of the active runway before parking and stepping outside.
    double re = 0.0, mu = 0.0, rough = 0.0;
    std::string rid;
    if (pack_->query_runway(aircraft.state().lat_deg, aircraft.state().lon_deg, re, mu, rough, rid))
        return false;
    double distance = 0;
    return pack_->find_nearest_airport(aircraft.state().lat_deg, aircraft.state().lon_deg, &distance) && distance <= 900.0;
}

bool AircraftSession::park(AircraftAndHud &aircraft, const AirportInfo &airport) {
    if (!can_park(aircraft)) return false;
    const auto *nearest = pack_->find_nearest_airport(aircraft.state().lat_deg, aircraft.state().lon_deg);
    if (nearest != &airport) return false;
    aircraft.sync_care_snapshot(profile_.care_);
    flight_active_ = false;
    profile_.parked_id_ = airport.id;
    profile_.last_park_epoch_ = static_cast<int64_t>(std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));

    // Log completed flight & stamp the Aragon Airfield Passport if the aircraft actually flew
    if (flight_airborne_total_s_ >= 10.0) {
        FleetFlightLogEntry entry;
        entry.dep_id = dep_airport_id_.empty() ? airport.id : dep_airport_id_;
        entry.arr_id = airport.id;
        entry.grade = has_touchdown_ ? td_grade_ : "LOGGED";
        entry.minutes = std::max(1.0, flight_elapsed_s_ / 60.0);
        entry.distance_nm = flight_dist_m_ / nm_m;
        entry.fuel_gal = std::max(0.0, flight_start_fuel_ - (profile_.care_.left_fuel_gal + profile_.care_.right_fuel_gal));
        entry.touchdown_fpm = has_touchdown_ ? td_fpm_ : 0.0;
        entry.crosswind_kt = has_touchdown_ ? td_xwind_kt_ : 0.0;
        profile_.logbook_.insert(profile_.logbook_.begin(), entry);
        if (profile_.logbook_.size() > 20) profile_.logbook_.resize(20);

        auto st = std::find_if(profile_.passport_.begin(), profile_.passport_.end(), [&](const auto &stamp) {
            return stamp.airport_id == airport.id;
        });
        if (st == profile_.passport_.end()) {
            profile_.passport_.push_back({airport.id, 0, 9999.0});
            st = std::prev(profile_.passport_.end());
        }
        st->visits += 1;
        if (has_touchdown_ && td_fpm_ > 0.0 && td_fpm_ < st->best_fpm) st->best_fpm = td_fpm_;
    }

    // Keep the aircraft right where the pilot taxied and parked it (no teleporting).
    const auto &s = aircraft.state();
    profile_.parking_spot_ = {s.lat_deg, s.lon_deg, sample_aircraft_ground(*pack_, s.lat_deg, s.lon_deg, s.heading_deg) + 1.38, s.heading_deg, 0};
    profile_.has_custom_park_ = true;
    return true;
}

void AircraftSession::step_flight(AircraftAndHud &aircraft, double dt) {
    if (!flight_active_) return;
    aircraft.sync_care_snapshot(profile_.care_);
    if (aircraft.state().paused || aircraft.state().slew_fast) return;

    flight_elapsed_s_ += dt;
    if (td_banner_s_ > 0.0) td_banner_s_ = std::max(0.0, td_banner_s_ - dt);
    if (aircraft.systems_.is_engine_running()) profile_.hours_ += dt / 3600.0;

    const auto &flight = aircraft.jsb_state();
    const double step_m = nspain::haversine_m(prev_lat_, prev_lon_, flight.lat_deg, flight.lon_deg);
    if (step_m < 500.0) flight_dist_m_ += step_m;
    prev_lat_ = flight.lat_deg;
    prev_lon_ = flight.lon_deg;

    // Feed live VFR route waypoint bearing directly into the cockpit Autopilot NAV bus!
    if (active_leg_ < profile_.route_.size()) {
        const auto &wp = profile_.route_[active_leg_];
        aircraft.state().ap_nav_brg_deg = bearing(flight.lat_deg, flight.lon_deg, wp.lat, wp.lon);
        aircraft.state().ap_nav_valid = true;
    } else {
        aircraft.state().ap_nav_valid = false;
    }

    // Surface-aware wear during ground roll (gravel/rough grass wear tires & nick prop at high RPM)
    if (flight.on_ground) {
        double re = 0.0, mu = 0.0, rough = 0.12;
        std::string rid;
        const bool on_rwy = pack_->query_runway(flight.lat_deg, flight.lon_deg, re, mu, rough, rid);
        const double spd = flight.ground_speed_mps;
        if (spd > 2.0) {
            const double surf_mult = on_rwy ? (0.4 + rough * 3.0) : 2.2;
            profile_.care_.tire_wear = std::min(1.0, profile_.care_.tire_wear + spd * surf_mult * 0.000018 * dt);
            if ((aircraft.state().parking_brake || aircraft.fdm_.controls().brake_left > 0.2) && spd > 3.0)
                profile_.care_.brake_wear = std::min(1.0, profile_.care_.brake_wear + spd * 0.00012 * dt);
            if (flight.rpm > 1800.0 && (rough > 0.14 || !on_rwy))
                profile_.care_.prop_nicks = std::min(1.0, profile_.care_.prop_nicks + 0.00035 * dt);
        }
    } else {
        flight_airborne_total_s_ += dt;
        // Bugs & rain slowly build windshield grime in flight
        if (flight.agl_m < 600.0 || aircraft.last_air().precipitation_mm_hr > 0.2)
            profile_.care_.windshield_dirt = std::min(1.0, profile_.care_.windshield_dirt + 0.00012 * dt);
    }

    // Touchdown detection, debrief grading & realistic gear/tire/airframe consequences
    if (flight.on_ground && !was_grounded_ && airborne_seconds_ > 2.0) {
        const double sink_mps = std::abs(std::min(0.0, last_sink_));
        const double fpm = sink_mps * 196.8504;
        const double hdg = flight.heading_deg * DEG2RAD_D;
        const double xwind_mps = std::abs(aircraft.last_air().wind_east_mps * std::cos(hdg) -
                                          aircraft.last_air().wind_north_mps * std::sin(hdg));
        td_fpm_ = fpm;
        td_xwind_kt_ = xwind_mps * 1.94384;
        td_g_ = std::max(1.0, last_g_);

        std::string rid;
        double re = 0.0, mu = 0.0, rough = 0.0;
        const bool on_rwy = pack_->query_runway(flight.lat_deg, flight.lon_deg, re, mu, rough, rid);
        const AirportInfo *ap = pack_->find_nearest_airport(flight.lat_deg, flight.lon_deg, nullptr);
        td_surface_ = (on_rwy && ap && !ap->surface.empty()) ? ap->surface : (on_rwy ? "runway" : "off-field");
        if (fpm < 120.0) td_grade_ = "GREASED";
        else if (fpm < 240.0) td_grade_ = "SMOOTH";
        else if (fpm < 420.0) td_grade_ = "FIRM";
        else td_grade_ = "HARD - INSPECT GEAR";

        has_touchdown_ = true;
        td_banner_s_ = 14.0;
        profile_.total_landings_ += 1;
        if (fpm < profile_.best_landing_fpm_) profile_.best_landing_fpm_ = fpm;

        // Hard or off-field touchdowns wear tires, knock gear alignment, and damage airframe
        if (sink_mps > 2.0 || !on_rwy) {
            const double excess = std::max(0.0, sink_mps - 2.0) + (on_rwy ? 0.0 : 0.8);
            profile_.care_.tire_wear = std::min(1.0, profile_.care_.tire_wear + excess * 0.08);
            const double pull_dir = (flight.roll_deg >= 0.0) ? 1.0 : -1.0;
            profile_.care_.gear_alignment = std::clamp(profile_.care_.gear_alignment + pull_dir * excess * 0.14, -1.0, 1.0);
        }
        if (sink_mps > 3.0) {
            profile_.care_.airframe_damage = std::min(1.0, profile_.care_.airframe_damage + (sink_mps - 3.0) * 0.10);
        }
    }

    airborne_seconds_ = flight.on_ground ? 0.0 : airborne_seconds_ + dt;
    if (aircraft.systems_.has_overtemp_failure() || aircraft.systems_.has_oil_starvation_failure())
        profile_.care_.airframe_damage = std::max(profile_.care_.airframe_damage, 0.85);

    aircraft.damage_ = profile_.care_.airframe_damage;
    aircraft.tire_wear_ = profile_.care_.tire_wear;
    aircraft.brake_wear_ = profile_.care_.brake_wear;
    aircraft.gear_alignment_ = profile_.care_.gear_alignment;
    aircraft.prop_nicks_ = profile_.care_.prop_nicks;
    aircraft.windshield_dirt_ = profile_.care_.windshield_dirt;

    was_grounded_ = flight.on_ground;
    last_sink_ = flight.vsi_mps;
    last_g_ = flight.g_load;
    if (active_leg_ < profile_.route_.size()) {
        const auto &p = profile_.route_[active_leg_];
        if (nspain::haversine_m(flight.lat_deg, flight.lon_deg, p.lat, p.lon) < 650.0) ++active_leg_;
    }
    save_elapsed_ += dt;
    if (save_elapsed_ >= 15.0) {
        save_profile();
        save_elapsed_ = 0.0;
    }
}

} // namespace nsim
