#include "flight_replay.hpp"

#include "flight_model_jsb.hpp"
#include "platform_io.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace nsim {
namespace {

constexpr double tick = nspain::FlightModel::step_sec;
constexpr double DEG2RAD = 3.14159265358979323846 / 180.0;
constexpr double RAD2DEG = 180.0 / 3.14159265358979323846;
const Vec3 replay_sun = normalize(Vec3(0.75f, 0.58f, 0.30f));

double bearing_deg(double lat1, double lon1, double lat2, double lon2) {
    const double p1 = lat1 * DEG2RAD, p2 = lat2 * DEG2RAD;
    const double dl = (lon2 - lon1) * DEG2RAD;
    const double y = std::sin(dl) * std::cos(p2);
    const double x = std::cos(p1) * std::sin(p2) - std::sin(p1) * std::cos(p2) * std::cos(dl);
    double deg = std::atan2(y, x) * RAD2DEG;
    if (deg < 0.0) deg += 360.0;
    return deg;
}

double ground_step_steer(double lat, double lon, double hdg_deg, double tgt_lat, double tgt_lon) {
    const double tgt_brg = bearing_deg(lat, lon, tgt_lat, tgt_lon);
    const double err = std::remainder(tgt_brg - hdg_deg, 360.0);
    return std::clamp(err / 24.0, -0.82, 0.82);
}

void add_default_route(FlightReplayTrace &trace, const AirportInfo &lesu) {
    const double dn = (lesu.rwy_lat_b - lesu.rwy_lat_a) * 111132.0;
    const double de = (lesu.rwy_lon_b - lesu.rwy_lon_a) * 111320.0 * std::cos(lesu.rwy_lat_a * DEG2RAD);
    const double rwy_rad = std::atan2(de, dn);
    const auto rwy_ext = [&](double back_m) -> std::pair<double, double> {
        return {
            lesu.rwy_lat_a - std::cos(rwy_rad) * back_m / 111132.0,
            lesu.rwy_lon_a - std::sin(rwy_rad) * back_m / (111320.0 * std::cos(lesu.rwy_lat_a * DEG2RAD))
        };
    };
    const auto iaf = rwy_ext(7800.0);
    const auto fin = rwy_ext(2800.0);
    const double mid_lat = 0.5 * (lesu.rwy_lat_a + lesu.rwy_lat_b);
    const double mid_lon = 0.5 * (lesu.rwy_lon_a + lesu.rwy_lon_b);

    trace.route = {
        {"ES0205_EXIT", 42.4780, 0.4120},
        {"CAMPO_W",     42.4300, 0.3820},
        {"GRAUS_N",     42.3400, 0.3850},
        {"BENABARRE",   42.2400, 0.4600},
        {"BORDER_065",  42.1950, 0.6500},
        {"TREMP",       42.1850, 0.9000},
        {"ISONA",       42.1850, 1.1500},
        {"ORGANYA",     42.2350, 1.3340},
        {"LESU_IAF",    iaf.first, iaf.second},
        {"LESU_FIN",    fin.first, fin.second},
        {"LESU_MID",    mid_lat,   mid_lon},
    };
}

} // namespace

ScopedTempDir::ScopedTempDir(const char *prefix) {
    dir = create_unique_temp_dir(prefix);
}

ScopedTempDir::~ScopedTempDir() {
    std::error_code ec;
    if (!dir.empty()) std::filesystem::remove_all(dir, ec);
}

bool save_flight_replay(const std::filesystem::path &path, const FlightReplayTrace &trace) {
    if (path.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    std::ofstream out(path);
    if (!out.is_open()) return false;
    out << "# OpenSky VFR Replay v1\n";
    out << "START_AIRPORT " << trace.start_airport << "\n";
    out << "DEST_AIRPORT " << trace.dest_airport << "\n";
    out << "WEATHER " << trace.weather_regime << " " << trace.season << " " << trace.wx_advance << "\n";
    for (const auto &wp : trace.route) {
        out.precision(9);
        out << "ROUTE " << wp.label << " " << wp.lat << " " << wp.lon << "\n";
    }
    out << "STEPS " << trace.steps.size() << "\n";
    out.setf(std::ios::fixed);
    for (const auto &s : trace.steps) {
        out.precision(10);
        out << s.dt << '\t'
            << static_cast<int>(s.ap_master) << '\t'
            << static_cast<int>(s.ap_nav_hold) << '\t'
            << static_cast<int>(s.ap_hdg_hold) << '\t'
            << static_cast<int>(s.ap_alt_hold) << '\t';
        out.precision(4);
        out << s.ap_hdg_deg << '\t' << s.ap_alt_ft << '\t' << s.flaps_deg << '\t';
        out.precision(6);
        out << s.roll_cmd << '\t' << s.pitch_cmd << '\t' << s.throttle_tgt << '\t' << s.brake_cmd << '\n';
    }
    return out.good();
}

bool load_flight_replay(const std::filesystem::path &path, FlightReplayTrace &out) {
    std::ifstream in(path);
    if (!in.is_open()) return false;
    out = FlightReplayTrace{};
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        if (std::isdigit(static_cast<unsigned char>(line[0])) || line[0] == '.') {
            ReplayStep s{};
            int am = 0, an = 0, ah = 0, aa = 0;
            if (ss >> s.dt >> am >> an >> ah >> aa >> s.ap_hdg_deg >> s.ap_alt_ft >>
                s.flaps_deg >> s.roll_cmd >> s.pitch_cmd >> s.throttle_tgt >> s.brake_cmd) {
                s.ap_master = static_cast<uint8_t>(am);
                s.ap_nav_hold = static_cast<uint8_t>(an);
                s.ap_hdg_hold = static_cast<uint8_t>(ah);
                s.ap_alt_hold = static_cast<uint8_t>(aa);
                out.steps.push_back(s);
            }
            continue;
        }
        std::string key;
        ss >> key;
        if (key == "START_AIRPORT") ss >> out.start_airport;
        else if (key == "DEST_AIRPORT") ss >> out.dest_airport;
        else if (key == "WEATHER") ss >> out.weather_regime >> out.season >> out.wx_advance;
        else if (key == "ROUTE") {
            FleetRoutePoint wp{};
            if (ss >> wp.label >> wp.lat >> wp.lon) out.route.push_back(std::move(wp));
        }
    }
    return !out.steps.empty();
}

bool prepare_replay_session(
    const PackReader &pack,
    const FlightReplayTrace &trace,
    const std::filesystem::path &profile_path,
    AircraftSession &session,
    AircraftAndHud &aircraft,
    double ambient_temp_c
) {
    const AirportInfo *dep = pack.find_airport(trace.start_airport);
    if (!dep) return false;
    session.init(pack, profile_path);
    session.begin_walkaround(*dep, nullptr);
    const auto &c = session.profile().care_;
    if (c.chocks_on || c.tie_downs_on || c.pitot_cover_on) {
        session.service(AircraftService::ToggleSecuring);
    }
    for (const auto &wp : trace.route) session.add_route_point(wp);
    session.board(aircraft, ambient_temp_c);
    if (!aircraft.systems().is_engine_running()) aircraft.quick_start_or_shutdown();
    aircraft.state().parking_brake = false;
    aircraft.state().menu_open = false;
    aircraft.state().paused = false;
    return true;
}

void apply_replay_step(
    AircraftAndHud &aircraft,
    AircraftSession &session,
    const ReplayStep &step,
    const AtmosphereState &atmo,
    const RegionalAtmosphere &reg_atmo,
    double sim_time_sec
) {
    aircraft.state().ap_master = step.ap_master != 0;
    aircraft.state().ap_nav_hold = step.ap_nav_hold != 0;
    aircraft.state().ap_hdg_hold = step.ap_hdg_hold != 0;
    aircraft.state().ap_alt_hold = step.ap_alt_hold != 0;
    aircraft.state().ap_hdg_deg = step.ap_hdg_deg;
    aircraft.state().ap_alt_ft = step.ap_alt_ft;
    aircraft.systems().set_flaps_target_deg(step.flaps_deg);
    aircraft.step_autonomous_assist(step.dt, step.roll_cmd, step.pitch_cmd, step.throttle_tgt, step.brake_cmd,
                                    atmo, reg_atmo, sim_time_sec);
    session.step_flight(aircraft, step.dt);
}

bool verify_replay_persistence(
    const PackReader &pack,
    AircraftSession &session,
    AircraftAndHud &aircraft,
    const std::filesystem::path &profile_path,
    const FlightReplayTrace &trace,
    double start_fuel_gal,
    bool crossed_region
) {
    const AirportInfo *dest = pack.find_airport(trace.dest_airport);
    if (!dest) {
        std::fprintf(stderr, "[REPLAY] Missing destination airport %s\n", trace.dest_airport.c_str());
        return false;
    }
    if (!crossed_region) {
        std::fprintf(stderr, "[REPLAY] Continuous flight did not cross region boundary\n");
        return false;
    }
    if (!session.can_park(aircraft) || !session.park(aircraft, *dest)) {
        const auto &js = aircraft.jsb_state();
        std::fprintf(stderr, "[REPLAY] Park rejected at %s: ground=%d speed=%.2f pos=(%.5f,%.5f) alt=%.2f agl=%.2f hdg=%.1f pitch=%.1f roll=%.1f vsi=%.2f\n",
                     dest->id.c_str(), js.on_ground ? 1 : 0, aircraft.state().speed_mps,
                     js.lat_deg, js.lon_deg, js.alt_msl_m, js.agl_m, js.heading_deg,
                     js.pitch_deg, js.roll_deg, js.vsi_mps);
        return false;
    }
    session.save_profile();

    FleetProfile raw_check;
    if (read_fleet_profile(profile_path, raw_check) != ProfileLoad::Loaded) {
        std::fprintf(stderr, "[REPLAY] Saved profile unreadable at %s\n", profile_path.string().c_str());
        return false;
    }

    AircraftSession restarted;
    restarted.init(pack, profile_path);
    const auto &saved = restarted.profile();
    const auto &expected = session.profile();
    const double end_fuel = saved.care_.left_fuel_gal + saved.care_.right_fuel_gal;
    const double fuel_burned = start_fuel_gal - end_fuel;

    const bool ok = saved.parked_id_ == dest->id &&
                    saved.has_custom_park_ &&
                    std::abs(saved.parking_spot_.lat - expected.parking_spot_.lat) < 1e-6 &&
                    std::abs(saved.parking_spot_.lon - expected.parking_spot_.lon) < 1e-6 &&
                    std::abs(saved.parking_spot_.heading - expected.parking_spot_.heading) < 1e-4 &&
                    std::abs(saved.care_.left_fuel_gal - expected.care_.left_fuel_gal) < 1e-5 &&
                    std::abs(saved.care_.right_fuel_gal - expected.care_.right_fuel_gal) < 1e-5 &&
                    fuel_burned > 0.4 &&
                    saved.logbook_.size() == 1 &&
                    saved.logbook_.front().dep_id == trace.start_airport &&
                    saved.logbook_.front().arr_id == dest->id &&
                    saved.logbook_.front().distance_nm > 30.0 &&
                    saved.logbook_.front().fuel_gal > 0.4 &&
                    !saved.passport_.empty();

    std::printf("[REPLAY-PERSIST] ok=%d parked=%s spot=(%.5f,%.5f hdg=%.1f) fuel=%.2f->%.2f (-%.2f gal) "
                "log=(%s->%s %.1fnm %.0ffpm %s)\n",
                ok ? 1 : 0, saved.parked_id_.c_str(),
                saved.parking_spot_.lat, saved.parking_spot_.lon, saved.parking_spot_.heading,
                start_fuel_gal, end_fuel, fuel_burned,
                saved.logbook_.empty() ? "-" : saved.logbook_.front().dep_id.c_str(),
                saved.logbook_.empty() ? "-" : saved.logbook_.front().arr_id.c_str(),
                saved.logbook_.empty() ? 0.0 : saved.logbook_.front().distance_nm,
                saved.logbook_.empty() ? 0.0 : saved.logbook_.front().touchdown_fpm,
                saved.logbook_.empty() ? "-" : saved.logbook_.front().grade.c_str());
    return ok;
}

bool record_castejon_catalonia_flight(PackReader &pack, const std::filesystem::path &trace_path) {
    const AirportInfo *dep = pack.find_airport("ES-0205");
    const AirportInfo *dest = pack.find_airport("LESU");
    if (!dep || !dest) return false;
    pack.reanchor_to(dep->lat, dep->lon);

    ScopedTempDir temp("opensky-record-XXXXXX");
    if (temp.dir.empty()) return false;
    const auto profile_path = temp.dir / "fleet.ini";

    FlightReplayTrace trace;
    trace.start_airport = dep->id;
    trace.dest_airport = dest->id;
    trace.weather_regime = 0;
    trace.season = 2;
    trace.wx_advance = 0.22f;
    add_default_route(trace, *dest);

    RegionalAtmosphere weather;
    weather.set_solver_seed(20261125, true);
    weather.set_manual_weather_override(true);
    weather.set_regime(WindRegime::PYRENEAN_THERMAL);
    weather.set_season(trace.season);
    if (!weather.init(&pack, false, "", false)) return false;
    weather.sync_region(dep->lat, dep->lon);
    weather.update_field(replay_sun, 0.0, false, trace.wx_advance);

    AircraftAndHud aircraft;
    if (!aircraft.init_physics(&pack, "game/aircraft/c172s")) return false;
    AircraftSession session;
    if (!prepare_replay_session(pack, trace, profile_path, session, aircraft, 15.0)) return false;

    const double start_fuel = session.profile().care_.left_fuel_gal + session.profile().care_.right_fuel_gal;

    // Departure RWY 16 vector (b -> a) at ES-0205
    const double d16_n = (dep->rwy_lat_a - dep->rwy_lat_b) * 111132.0;
    const double d16_e = (dep->rwy_lon_a - dep->rwy_lon_b) * 111320.0 * std::cos(dep->rwy_lat_b * DEG2RAD);
    const double hdg_16 = bearing_deg(dep->rwy_lat_b, dep->rwy_lon_b, dep->rwy_lat_a, dep->rwy_lon_a);
    const double hdg_34 = bearing_deg(dep->rwy_lat_a, dep->rwy_lon_a, dep->rwy_lat_b, dep->rwy_lon_b);
    const double dep_rwy_len = std::hypot(d16_n, d16_e);
    const double pad_lat = dep->rwy_lat_b + (dep->rwy_lat_a - dep->rwy_lat_b) * (60.0 / dep_rwy_len);
    const double pad_lon = dep->rwy_lon_b + (dep->rwy_lon_a - dep->rwy_lon_b) * (60.0 / dep_rwy_len);
    const double line_lat = dep->rwy_lat_b + (dep->rwy_lat_a - dep->rwy_lat_b) * 0.45;
    const double line_lon = dep->rwy_lon_b + (dep->rwy_lon_a - dep->rwy_lon_b) * 0.45;
    const double enter_lat = dep->rwy_lat_b + (dep->rwy_lat_a - dep->rwy_lat_b) * 0.72;
    const double enter_lon = dep->rwy_lon_b + (dep->rwy_lon_a - dep->rwy_lon_b) * 0.72;

    // Arrival RWY 03 vector (a -> b) at LESU
    const double d03_n = (dest->rwy_lat_b - dest->rwy_lat_a) * 111132.0;
    const double d03_e = (dest->rwy_lon_b - dest->rwy_lon_a) * 111320.0 * std::cos(dest->rwy_lat_a * DEG2RAD);
    const double lesu_len = std::hypot(d03_n, d03_e);
    const double rwy_03_hdg = bearing_deg(dest->rwy_lat_a, dest->rwy_lon_a, dest->rwy_lat_b, dest->rwy_lon_b);
    const double rwy_03_rad = rwy_03_hdg * DEG2RAD;

    enum class Phase {
        TaxiToPad, Align16, HoldBeforeTakeoff, TakeoffRoll, InitClimb,
        CruiseNav, ApproachGlide, FlareRollout, TaxiClear, StopForPark, Done
    } phase = Phase::TaxiToPad;

    AtmosphereState sky{};
    sky.sun_dir = replay_sun;
    double sim_time = 0.0;
    double phase_timer = 0.0;
    bool crossed_region = false;
    std::string init_region = "es_aragon_pyrenees";
    if (const auto *r0 = pack.find_region_at(dep->lat, dep->lon)) init_region = r0->region_id;

    constexpr int max_steps = 160000;
    double next_wx_t = 45.0;
    for (int i = 0; i < max_steps && phase != Phase::Done; ++i) {
        const auto &fs = aircraft.jsb_state();
        const double d_lesu_thr = nspain::haversine_m(fs.lat_deg, fs.lon_deg, dest->rwy_lat_a, dest->rwy_lon_a);
        const bool fine_dt =
            (phase == Phase::TakeoffRoll || (phase == Phase::InitClimb && fs.agl_m < 90.0)) ||
            (fs.lon_deg >= 0.618 && fs.lon_deg <= 0.708) ||
            d_lesu_thr < 2500.0 ||
            phase >= Phase::ApproachGlide;
        const double dt = (fine_dt ? 2.0 : 6.0) * tick;
        sim_time += dt;
        phase_timer += dt;

        // Track region crossing & anchor shifts identically to live game loop.
        const auto *fr = pack.find_region_at(fs.lat_deg, fs.lon_deg);
        const auto *ar = pack.find_region_at(pack.anchor_lat(), pack.anchor_lon());
        if (fr && fr->region_id != init_region) crossed_region = true;
        if (nspain::haversine_m(fs.lat_deg, fs.lon_deg, pack.anchor_lat(), pack.anchor_lon()) > 60000.0 ||
            (fr && fr != ar)) {
            if (pack.reanchor_to(fs.lat_deg, fs.lon_deg)) {
                weather.sync_region(fs.lat_deg, fs.lon_deg);
                weather.update_field(replay_sun, sim_time, false, trace.wx_advance);
                next_wx_t = sim_time + 45.0;
            }
        }
        if (sim_time >= next_wx_t) {
            weather.update_field(replay_sun, sim_time, false, trace.wx_advance);
            next_wx_t = sim_time + 45.0;
        }

        ReplayStep s{};
        s.dt = dt;
        switch (phase) {
            case Phase::TaxiToPad: {
                const double d_enter = nspain::haversine_m(fs.lat_deg, fs.lon_deg, enter_lat, enter_lon);
                const double d_pad = nspain::haversine_m(fs.lat_deg, fs.lon_deg, pad_lat, pad_lon);
                const bool passed_enter = (fs.lat_deg > enter_lat) || d_enter < 28.0;
                const double tlat = passed_enter ? pad_lat : enter_lat;
                const double tlon = passed_enter ? pad_lon : enter_lon;
                s.roll_cmd = static_cast<float>(ground_step_steer(fs.lat_deg, fs.lon_deg, fs.heading_deg, tlat, tlon));
                const double tgt_spd = (d_pad < 55.0) ? 3.5 : 6.0;
                s.throttle_tgt = fs.ground_speed_mps < tgt_spd ? 0.56f : 0.14f;
                s.brake_cmd = fs.ground_speed_mps > tgt_spd + 1.5 ? 0.40f : 0.0f;
                if (d_pad < 22.0) { phase = Phase::Align16; phase_timer = 0.0; }
                break;
            }
            case Phase::Align16: {
                const double herr = std::remainder(hdg_16 - fs.heading_deg, 360.0);
                s.roll_cmd = (std::abs(herr) > 65.0)
                    ? 0.72f
                    : static_cast<float>(std::clamp(herr / 24.0, -0.72, 0.72));
                s.throttle_tgt = fs.ground_speed_mps < 3.6 ? 0.62f : 0.18f;
                if (std::abs(herr) < 10.0) { phase = Phase::HoldBeforeTakeoff; phase_timer = 0.0; }
                break;
            }
            case Phase::HoldBeforeTakeoff: {
                s.throttle_tgt = 0.0f;
                s.brake_cmd = 1.0f;
                if (fs.ground_speed_mps < 0.6 && phase_timer > 0.6) {
                    phase = Phase::TakeoffRoll;
                    phase_timer = 0.0;
                }
                break;
            }
            case Phase::TakeoffRoll: {
                const double aim = bearing_deg(fs.lat_deg, fs.lon_deg, line_lat, line_lon);
                const double herr = std::remainder(hdg_16 - fs.heading_deg, 360.0);
                // Steer gently to runway heading down RWY 16
                s.roll_cmd = (fs.on_ground && fs.ground_speed_mps > 3.0)
                    ? static_cast<float>(std::clamp(herr / 18.0, -0.35, 0.35))
                    : 0.0f;
                (void)aim;
                (void)hdg_34;
                s.pitch_cmd = (fs.on_ground && fs.ias_kt < 53.0) ? 0.0f : (fs.ias_kt < 66.0 ? 0.56f : 0.66f);
                s.throttle_tgt = 1.0f;
                if (!fs.on_ground && fs.agl_m > 35.0) {
                    phase = Phase::InitClimb;
                    phase_timer = 0.0;
                }
                break;
            }
            case Phase::InitClimb: {
                // Step down the river notch: (42.505,0.475) -> (42.492,0.465) -> (42.478,0.412)
                const bool in_north = (fs.lat_deg > 42.493 && fs.lon_deg > 0.464);
                const double tgt_lat = in_north ? 42.4915 : 42.4780;
                const double tgt_lon = in_north ? 0.4660 : 0.4120;
                const double climb_brg = bearing_deg(fs.lat_deg, fs.lon_deg, tgt_lat, tgt_lon);
                const double herr = std::remainder(climb_brg - fs.heading_deg, 360.0);
                s.roll_cmd = static_cast<float>(std::clamp(herr / 24.0, -0.78, 0.78));
                s.pitch_cmd = (fs.ias_kt < 78.0) ? 0.34f : 0.54f;
                s.throttle_tgt = (fs.agl_m < 180.0) ? 1.0f : 0.86f;
                if (fs.lon_deg < 0.435 || fs.alt_msl_m > 1360.0) {
                    phase = Phase::CruiseNav;
                    phase_timer = 0.0;
                }
                break;
            }
            case Phase::CruiseNav: {
                s.ap_master = 1;
                s.ap_nav_hold = 1;
                s.ap_alt_hold = 1;
                const size_t leg = session.active_leg();
                float target_ft = 5950.0f;
                float thr = 0.86f;
                float flp = 0.0f;
                if (leg >= 8) { // Inside open Segre gorge -> stabilized descent onto LESU RWY 03
                    if (leg == 8) { target_ft = 3550.0f; thr = 0.56f; flp = 20.0f; }
                    else { target_ft = 2950.0f; thr = 0.46f; flp = 20.0f; }
                } else if (leg >= 6) {
                    target_ft = (leg == 7) ? 5850.0f : 6050.0f;
                }
                const double cur_ft = fs.alt_msl_m * 3.28084;
                if (cur_ft < target_ft - 60.0) {
                    thr = 0.90f;
                    // Climb smoothly above 90 kt so terrain clearances over Pyrenean passes never stall on light wind.
                    target_ft = (fs.ias_kt >= 90.0)
                        ? static_cast<float>(std::min<double>(target_ft, cur_ft + 100.0))
                        : static_cast<float>(cur_ft);
                }
                s.ap_alt_ft = target_ft;
                s.throttle_tgt = thr;
                s.flaps_deg = flp;
                if (leg >= 9 && d_lesu_thr < 4300.0) {
                    phase = Phase::ApproachGlide;
                    phase_timer = 0.0;
                }
                break;
            }
            case Phase::ApproachGlide: {
                const double rel_n = (fs.lat_deg - dest->rwy_lat_a) * 111132.0;
                const double rel_e = (fs.lon_deg - dest->rwy_lon_a) * 111320.0 * std::cos(dest->rwy_lat_a * DEG2RAD);
                const double along = rel_e * std::sin(rwy_03_rad) + rel_n * std::cos(rwy_03_rad);
                const double xtrack = rel_e * std::cos(rwy_03_rad) - rel_n * std::sin(rwy_03_rad);
                const double aim_hdg = rwy_03_hdg - std::atan2(xtrack, 240.0) * RAD2DEG;
                const double h_err = std::remainder(aim_hdg - fs.heading_deg, 360.0);
                s.roll_cmd = static_cast<float>(std::clamp(h_err / 24.0, -0.55, 0.55));
                const double rwy_frac = std::clamp(along / lesu_len, 0.0, 1.0);
                const double rwy_msl = dest->rwy_elev_a * (1.0 - rwy_frac) + dest->rwy_elev_b * rwy_frac + 0.35;
                const double h_rwy = fs.alt_msl_m - rwy_msl;
                if (along < 40.0 || h_rwy > 5.5) {
                    const double tgt_h = std::max(0.8, (160.0 - along) * 0.055);
                    const double tgt_vsi = std::clamp(-2.1 + (tgt_h - h_rwy) * 0.16, -3.8, -0.3);
                    s.pitch_cmd = static_cast<float>(std::clamp((tgt_vsi - fs.vsi_mps) * 0.24 - 0.12, -0.50, 0.28));
                    s.throttle_tgt = (along > 30.0 && h_rwy < 10.0) ? 0.0f
                        : static_cast<float>(std::clamp(0.18 + (58.0 - fs.ias_kt) * 0.032, 0.0, 0.54));
                } else {
                    s.throttle_tgt = 0.0f;
                    const double tgt_vsi = (h_rwy > 2.4) ? -1.0 : -0.45;
                    s.pitch_cmd = static_cast<float>(std::clamp((tgt_vsi - fs.vsi_mps) * 0.28 + 0.06, -0.30, 0.42));
                }
                s.flaps_deg = 20.0f;
                if (fs.on_ground && phase_timer > 1.0) {
                    phase = Phase::FlareRollout;
                    phase_timer = 0.0;
                }
                break;
            }
            case Phase::FlareRollout: {
                const double herr = std::remainder(rwy_03_hdg - fs.heading_deg, 360.0);
                s.roll_cmd = static_cast<float>(std::clamp(herr / 22.0, -0.4, 0.4));
                s.throttle_tgt = 0.0f;
                s.brake_cmd = 0.65f;
                s.flaps_deg = 20.0f;
                if (fs.ground_speed_mps < 4.5) {
                    phase = Phase::TaxiClear;
                    phase_timer = 0.0;
                }
                break;
            }
            case Phase::TaxiClear: {
                double re = 0.0, mu = 0.0, rough = 0.0;
                std::string rid;
                const bool on_rwy = pack.query_runway(fs.lat_deg, fs.lon_deg, re, mu, rough, rid);
                if (!on_rwy && phase_timer > 0.6) {
                    phase = Phase::StopForPark;
                    phase_timer = 0.0;
                    break;
                }
                // Turn ~90 deg left off RWY 03 (hdg 301 deg) onto LESU's flat west apron plateau; east drops sharply.
                const double exit_hdg = std::fmod(rwy_03_hdg + 270.0, 360.0);
                const double herr = std::remainder(exit_hdg - fs.heading_deg, 360.0);
                s.roll_cmd = static_cast<float>(std::clamp(herr / 45.0, -0.33, 0.33));
                s.throttle_tgt = fs.ground_speed_mps < 3.0 ? 0.44f : 0.10f;
                s.brake_cmd = fs.ground_speed_mps > 3.6 ? 0.35f : 0.0f;
                break;
            }
            case Phase::StopForPark: {
                s.throttle_tgt = 0.0f;
                s.brake_cmd = 1.0f;
                if (fs.on_ground && fs.ground_speed_mps < 0.12 && phase_timer > 11.5) {
                    phase = Phase::Done;
                }
                break;
            }
            case Phase::Done: break;
        }

        apply_replay_step(aircraft, session, s, sky, weather, sim_time);
        trace.steps.push_back(s);
        if ((i % 2000) == 0) {
            std::printf("[REC t=%.0fs ph=%d] lat=%.4f lon=%.4f alt=%.0fm agl=%.0fm ias=%.1fkt rpm=%.0f hp=%.0f cht=%.0f oil=%.0f cyl=%.2f pwr=%.2f ice=%.2f hdg=%.0f leg=%zu\n",
                        sim_time, static_cast<int>(phase),
                        aircraft.jsb_state().lat_deg, aircraft.jsb_state().lon_deg,
                        aircraft.jsb_state().alt_msl_m, aircraft.jsb_state().agl_m,
                        aircraft.jsb_state().ias_kt, aircraft.jsb_state().rpm, aircraft.jsb_state().shaft_hp,
                        aircraft.systems().get_cht_f(), aircraft.systems().get_oil_temp_f(), aircraft.systems().get_cylinder_wear(),
                        aircraft.systems().get_engine_power_mult(), aircraft.systems().get_carb_ice_level(),
                        aircraft.jsb_state().heading_deg, session.active_leg());
        }
        if (aircraft.jsb_state().agl_m < -2.0) {
            std::fprintf(stderr, "[RECORD] Unexpected below-ground contact at step %d (lat=%.4f lon=%.4f)\n",
                         i, aircraft.jsb_state().lat_deg, aircraft.jsb_state().lon_deg);
            return false;
        }
    }

    if (phase != Phase::Done) {
        std::fprintf(stderr, "[RECORD] Flight timed out in phase %d (lat=%.4f lon=%.4f alt=%.0fm steps=%zu)\n",
                     static_cast<int>(phase), aircraft.jsb_state().lat_deg, aircraft.jsb_state().lon_deg,
                     aircraft.jsb_state().alt_msl_m, trace.steps.size());
        return false;
    }

    if (!verify_replay_persistence(pack, session, aircraft, profile_path, trace, start_fuel, crossed_region))
        return false;
    if (!save_flight_replay(trace_path, trace)) {
        std::fprintf(stderr, "[RECORD] Could not save trace to %s\n", trace_path.string().c_str());
        return false;
    }
    std::printf("[RECORD-FLIGHT OK] Saved %zu steps (%.1f sim min) -> %s\n",
                trace.steps.size(), sim_time / 60.0, trace_path.string().c_str());
    return true;
}

bool replay_flight_headless(PackReader &pack, const std::filesystem::path &trace_path) {
    FlightReplayTrace trace;
    if (!load_flight_replay(trace_path, trace)) {
        std::fprintf(stderr, "[REPLAY] Failed to load %s\n", trace_path.string().c_str());
        return false;
    }
    const AirportInfo *dep = pack.find_airport(trace.start_airport);
    if (!dep) return false;
    pack.reanchor_to(dep->lat, dep->lon);

    ScopedTempDir temp("opensky-record-XXXXXX");
    if (temp.dir.empty()) return false;
    const auto profile_path = temp.dir / "fleet.ini";

    RegionalAtmosphere weather;
    weather.set_solver_seed(20261125, true);
    weather.set_manual_weather_override(true);
    weather.set_regime(WindRegime::PYRENEAN_THERMAL);
    weather.set_season(trace.season);
    if (!weather.init(&pack, false, "", false)) return false;
    weather.sync_region(dep->lat, dep->lon);
    weather.update_field(replay_sun, 0.0, false, trace.wx_advance);

    AircraftAndHud aircraft;
    if (!aircraft.init_physics(&pack, "game/aircraft/c172s")) return false;
    AircraftSession session;
    if (!prepare_replay_session(pack, trace, profile_path, session, aircraft, 15.0)) return false;
    const double start_fuel = session.profile().care_.left_fuel_gal + session.profile().care_.right_fuel_gal;

    AtmosphereState sky{};
    sky.sun_dir = replay_sun;
    double sim_time = 0.0;
    bool crossed_region = false;
    std::string init_region = "es_aragon_pyrenees";
    if (const auto *r0 = pack.find_region_at(dep->lat, dep->lon)) init_region = r0->region_id;

    double next_wx_t = 45.0;
    for (const auto &s : trace.steps) {
        sim_time += s.dt;
        const auto &fs = aircraft.jsb_state();
        const auto *fr = pack.find_region_at(fs.lat_deg, fs.lon_deg);
        const auto *ar = pack.find_region_at(pack.anchor_lat(), pack.anchor_lon());
        if (fr && fr->region_id != init_region) crossed_region = true;
        if (nspain::haversine_m(fs.lat_deg, fs.lon_deg, pack.anchor_lat(), pack.anchor_lon()) > 60000.0 ||
            (fr && fr != ar)) {
            if (pack.reanchor_to(fs.lat_deg, fs.lon_deg)) {
                weather.sync_region(fs.lat_deg, fs.lon_deg);
                weather.update_field(replay_sun, sim_time, false, trace.wx_advance);
                next_wx_t = sim_time + 45.0;
            }
        }
        if (sim_time >= next_wx_t) {
            weather.update_field(replay_sun, sim_time, false, trace.wx_advance);
            next_wx_t = sim_time + 45.0;
        }
        apply_replay_step(aircraft, session, s, sky, weather, sim_time);
    }

    return verify_replay_persistence(pack, session, aircraft, profile_path, trace, start_fuel, crossed_region);
}

bool save_route_video(const std::filesystem::path &path, const RouteVideoRecording &rec) {
    if (rec.frames.size() < 2) return false;
    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    const std::filesystem::path tmp = path.string() + ".tmp";
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;
    static constexpr char kMagic[8] = {'N', 'S', 'R', 'T', '0', '1', '\0', '\0'};
    const uint32_t title_len = static_cast<uint32_t>(std::min<size_t>(rec.title.size(), 255));
    const uint32_t count = static_cast<uint32_t>(rec.frames.size());
    out.write(kMagic, sizeof(kMagic));
    out.write(reinterpret_cast<const char *>(&title_len), sizeof(title_len));
    out.write(reinterpret_cast<const char *>(&count), sizeof(count));
    if (title_len > 0) out.write(rec.title.data(), title_len);
    out.write(reinterpret_cast<const char *>(rec.frames.data()),
              static_cast<std::streamsize>(rec.frames.size() * sizeof(RouteVideoFrame)));
    out.flush();
    const bool ok = out.good();
    out.close();
    if (!ok) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

bool load_route_video(const std::filesystem::path &path, RouteVideoRecording &out) {
    out = {};
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return false;
    char magic[8]{};
    uint32_t title_len = 0, count = 0;
    in.read(magic, sizeof(magic));
    in.read(reinterpret_cast<char *>(&title_len), sizeof(title_len));
    in.read(reinterpret_cast<char *>(&count), sizeof(count));
    static constexpr char kMagic[8] = {'N', 'S', 'R', 'T', '0', '1', '\0', '\0'};
    if (!in.good() || std::memcmp(magic, kMagic, sizeof(kMagic)) != 0 ||
        title_len > 255 || count < 2 || count > 500000u) {
        return false;
    }
    out.title.resize(title_len);
    if (title_len > 0) in.read(out.title.data(), title_len);
    out.frames.resize(count);
    in.read(reinterpret_cast<char *>(out.frames.data()),
            static_cast<std::streamsize>(out.frames.size() * sizeof(RouteVideoFrame)));
    return in.good();
}

RouteVideoFrame sample_route_video(const RouteVideoRecording &rec, double playback_t) {
    if (rec.frames.empty()) return {};
    if (rec.frames.size() == 1 || playback_t <= rec.frames.front().sim_t) return rec.frames.front();
    if (playback_t >= rec.frames.back().sim_t) return rec.frames.back();
    auto it = std::lower_bound(rec.frames.begin(), rec.frames.end(), playback_t,
                               [](const RouteVideoFrame &f, double t) { return f.sim_t < t; });
    if (it == rec.frames.begin()) return *it;
    if (it == rec.frames.end()) return rec.frames.back();
    const RouteVideoFrame &b = *it;
    const RouteVideoFrame &a = *(it - 1);
    const double span = std::max(1e-6, b.sim_t - a.sim_t);
    const double u = std::clamp((playback_t - a.sim_t) / span, 0.0, 1.0);
    auto lerp_d = [u](double x, double y) { return x + (y - x) * u; };
    RouteVideoFrame out = a;
    out.sim_t = playback_t;
    out.lat_deg = lerp_d(a.lat_deg, b.lat_deg);
    out.lon_deg = lerp_d(a.lon_deg, b.lon_deg);
    out.alt_msl_m = lerp_d(a.alt_msl_m, b.alt_msl_m);
    out.heading_deg = std::fmod(a.heading_deg + std::remainder(b.heading_deg - a.heading_deg, 360.0) * u + 360.0, 360.0);
    out.pitch_deg = lerp_d(a.pitch_deg, b.pitch_deg);
    out.roll_deg = lerp_d(a.roll_deg, b.roll_deg);
    out.ias_kt = lerp_d(a.ias_kt, b.ias_kt);
    out.rpm = lerp_d(a.rpm, b.rpm);
    out.flaps_deg = lerp_d(a.flaps_deg, b.flaps_deg);
    out.throttle = lerp_d(a.throttle, b.throttle);
    out.aileron = lerp_d(a.aileron, b.aileron);
    out.elevator = lerp_d(a.elevator, b.elevator);
    out.rudder = lerp_d(a.rudder, b.rudder);
    return out;
}

} // namespace nsim
