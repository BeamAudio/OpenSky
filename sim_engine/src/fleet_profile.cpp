#include "fleet_profile.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <unistd.h>

namespace nsim {
namespace {

bool valid_position(double lat, double lon) {
    return std::isfinite(lat) && std::isfinite(lon) && std::abs(lat) <= 90 && std::abs(lon) <= 180;
}

bool read_profile(std::istream &in, FleetProfile &profile) {
    int version = 0;
    auto &care = profile.care_;
    size_t route_count = 0;
    if (!(in >> version) || (version != 1 && version != 2) ||
        !(in >> std::quoted(profile.parked_id_) >> care.left_fuel_gal >> care.right_fuel_gal >>
          care.airframe_damage >> profile.hours_ >> route_count) ||
        route_count > max_route_points || profile.parked_id_.size() > 64) return false;
    for (size_t i = 0; i < route_count; ++i) {
        FleetRoutePoint point{};
        if (!(in >> std::quoted(point.label) >> point.lat >> point.lon) ||
            point.label.size() > 32 || !valid_position(point.lat, point.lon)) return false;
        profile.route_.push_back(std::move(point));
    }

    if (version == 2) {
        bool chocks, pitot, tied, master, avionics, running, custom_park;
        auto &pos = profile.parking_spot_;
        if (!(in >> care.oil_quarts >> care.oil_hours >> care.battery_soc >>
              care.spark_fouling >> care.shock_cooling >> care.cylinder_wear >>
              care.tire_wear >> care.brake_wear >> care.gear_alignment >> care.prop_nicks >>
              care.windshield_dirt >> care.sump_water >> chocks >> pitot >> tied >>
              master >> avionics >> care.magnetos >> care.fuel_selector >> care.mixture >>
              care.flaps_deg >> care.carb_heat >> running >> custom_park >>
              pos.lat >> pos.lon >> pos.heading >> profile.last_park_epoch_ >>
              profile.total_landings_ >> profile.best_landing_fpm_) ||
            !valid_position(pos.lat, pos.lon) || !std::isfinite(pos.heading) ||
            profile.last_park_epoch_ < 0 || profile.total_landings_ < 0) return false;
        care.chocks_on = chocks;
        care.pitot_cover_on = pitot;
        care.tie_downs_on = tied;
        // Keep the parked restart cold and dark. The original on-disk fields remain readable.
        care.master_on = care.avionics_on = care.engine_running = false;
        care.magnetos = 0;
        profile.has_custom_park_ = custom_park;

        size_t count = 0;
        if (!(in >> count) || count > 128) return false;
        for (size_t i = 0; i < count; ++i) {
            FleetPassportStamp stamp;
            if (!(in >> std::quoted(stamp.airport_id) >> stamp.visits >> stamp.best_fpm) ||
                stamp.airport_id.size() > 64 || stamp.visits < 0 || !std::isfinite(stamp.best_fpm)) return false;
            profile.passport_.push_back(std::move(stamp));
        }
        if (!(in >> count) || count > 32) return false;
        for (size_t i = 0; i < count; ++i) {
            FleetFlightLogEntry entry;
            if (!(in >> std::quoted(entry.dep_id) >> std::quoted(entry.arr_id) >> std::quoted(entry.grade) >>
                  entry.minutes >> entry.distance_nm >> entry.fuel_gal >> entry.touchdown_fpm >> entry.crosswind_kt) ||
                entry.dep_id.size() > 64 || entry.arr_id.size() > 64 || entry.grade.size() > 64) return false;
            for (double value : {entry.minutes, entry.distance_nm, entry.fuel_gal, entry.touchdown_fpm, entry.crosswind_kt})
                if (!std::isfinite(value) || value < 0) return false;
            profile.logbook_.push_back(std::move(entry));
        }
    }

    for (double value : {care.left_fuel_gal, care.right_fuel_gal, care.oil_quarts, care.oil_hours,
                         care.airframe_damage, profile.hours_, care.gear_alignment, care.mixture,
                         care.flaps_deg, profile.best_landing_fpm_})
        if (!std::isfinite(value)) return false;
    for (double *value : {&care.battery_soc, &care.spark_fouling, &care.shock_cooling,
                          &care.cylinder_wear, &care.tire_wear, &care.brake_wear, &care.prop_nicks,
                          &care.windshield_dirt, &care.sump_water, &care.carb_heat}) {
        if (!std::isfinite(*value)) return false;
        *value = std::clamp(*value, 0.0, 1.0);
    }
    care.left_fuel_gal = std::clamp(care.left_fuel_gal, 0.0, 26.5);
    care.right_fuel_gal = std::clamp(care.right_fuel_gal, 0.0, 26.5);
    care.oil_quarts = std::clamp(care.oil_quarts, 0.0, 8.0);
    care.oil_hours = std::max(0.0, care.oil_hours);
    care.airframe_damage = std::clamp(care.airframe_damage, 0.0, 1.0);
    care.gear_alignment = std::clamp(care.gear_alignment, -1.0, 1.0);
    care.fuel_selector = std::clamp(care.fuel_selector, 0, 3);
    care.flaps_deg = std::clamp(care.flaps_deg, 0.0, 30.0);
    care.mixture = std::clamp(care.mixture, 0.0, 1.0);
    profile.hours_ = std::max(0.0, profile.hours_);
    in >> std::ws;
    return in.eof();
}

} // namespace

ProfileLoad read_fleet_profile(const std::filesystem::path &path, FleetProfile &out) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return ec ? ProfileLoad::Invalid : ProfileLoad::Missing;
    std::ifstream in(path);
    FleetProfile loaded;
    if (!in || !read_profile(in, loaded)) return ProfileLoad::Invalid;
    out = std::move(loaded);
    return ProfileLoad::Loaded;
}

bool write_fleet_profile(const std::filesystem::path &path, const FleetProfile &profile, std::string &error) {
    std::error_code ec;
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        error = "Could not create the aircraft save directory.";
        return false;
    }
    // Use a unique temporary path in the destination directory so rename is atomic.
    auto pattern = path.string() + ".tmp.XXXXXX";
    const int fd = ::mkstemp(pattern.data());
    if (fd < 0) {
        error = "Could not create a temporary aircraft save.";
        return false;
    }
    ::close(fd);
    std::ofstream out(pattern, std::ios::trunc);
    const auto &c = profile.care_;
    const auto &pos = profile.parking_spot_;
    out << std::setprecision(12) << 2 << '\n' << std::quoted(profile.parked_id_) << '\n'
        << c.left_fuel_gal << ' ' << c.right_fuel_gal << ' ' << c.airframe_damage << ' ' << profile.hours_ << '\n'
        << profile.route_.size() << '\n';
    for (const auto &p : profile.route_) out << std::quoted(p.label) << ' ' << p.lat << ' ' << p.lon << '\n';
    out << c.oil_quarts << ' ' << c.oil_hours << ' ' << c.battery_soc << ' ' << c.spark_fouling << ' '
        << c.shock_cooling << ' ' << c.cylinder_wear << ' ' << c.tire_wear << ' ' << c.brake_wear << ' '
        << c.gear_alignment << ' ' << c.prop_nicks << ' ' << c.windshield_dirt << ' ' << c.sump_water << '\n'
        << c.chocks_on << ' ' << c.pitot_cover_on << ' ' << c.tie_downs_on << ' ' << c.master_on << ' '
        << c.avionics_on << ' ' << c.magnetos << ' ' << c.fuel_selector << ' ' << c.mixture << ' ' << c.flaps_deg
        << ' ' << c.carb_heat << ' ' << c.engine_running << '\n'
        << profile.has_custom_park_ << ' ' << pos.lat << ' ' << pos.lon << ' ' << pos.heading << '\n'
        << profile.last_park_epoch_ << ' ' << profile.total_landings_ << ' ' << profile.best_landing_fpm_ << '\n'
        << profile.passport_.size() << '\n';
    for (const auto &s : profile.passport_)
        out << std::quoted(s.airport_id) << ' ' << s.visits << ' ' << s.best_fpm << '\n';
    out << profile.logbook_.size() << '\n';
    for (const auto &e : profile.logbook_)
        out << std::quoted(e.dep_id) << ' ' << std::quoted(e.arr_id) << ' ' << std::quoted(e.grade) << ' '
            << e.minutes << ' ' << e.distance_nm << ' ' << e.fuel_gal << ' ' << e.touchdown_fpm << ' ' << e.crosswind_kt << '\n';
    out.close();
    if (out) std::filesystem::rename(pattern, path, ec);
    if (!out || ec) {
        std::filesystem::remove(pattern, ec);
        error = "Could not save the aircraft. The previous save has been kept.";
        return false;
    }
    error.clear();
    return true;
}

} // namespace nsim
