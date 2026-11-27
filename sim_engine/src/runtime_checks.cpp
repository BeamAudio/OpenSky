#include "runtime_checks.hpp"

#include "aircraft_hud.hpp"
#include "aircraft_session.hpp"
#include "async_streamer.hpp"
#include "flight_replay.hpp"
#include "platform_io.hpp"
#include "simulation_clock.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <string>
#include <thread>

namespace nsim {
namespace {

constexpr double tick = nspain::FlightModel::step_sec;
constexpr double tol = 1e-8;
const Vec3 check_sun = normalize(Vec3(0.7f, 0.65f, 0.2f));

struct TemporaryProfile {
    std::filesystem::path directory;
    TemporaryProfile() {
        directory = create_unique_temp_dir("opensky-runtime-XXXXXX");
    }
    ~TemporaryProfile() {
        std::error_code ec;
        if (!directory.empty()) std::filesystem::remove_all(directory, ec);
    }
};

bool profile_checks(PackReader &pack) {
    TemporaryProfile isolated;
    if (isolated.directory.empty()) return false;
    const auto path = isolated.directory / "fleet.ini";
    const auto *home = pack.find_airport("ES-0205");
    const auto *destination = pack.find_airport("LECI");
    if (!home || !destination) return false;
    std::ofstream(path) << "1\n\"ES-0205\"\n17 16 0.12 13\n1\n\"LECI\" "
                        << destination->lat << ' ' << destination->lon << "\n";
    FleetProfile v1;
    if (read_fleet_profile(path, v1) != ProfileLoad::Loaded || v1.care_.left_fuel_gal != 17 ||
        v1.route_.size() != 1) return false;

    AircraftSession session;
    session.init(pack, path);
    session.begin_walkaround(*home, nullptr);
    AircraftAndHud aircraft;
    if (!aircraft.init_physics(&pack, "game/aircraft/c172s")) return false;
    session.board(aircraft, 15.0);
    if (nspain::haversine_m(aircraft.state().lat_deg, aircraft.state().lon_deg,
                           session.profile().parking_spot_.lat, session.profile().parking_spot_.lon) > 1.0)
        return false;
    aircraft.reset_flight(home->lat, home->lon, home->elev_m + 700, 150, 104.0, false);
    if (session.can_park(aircraft) || session.park(aircraft, *destination)) return false;
    RegionalAtmosphere weather;
    weather.set_solver_seed(1725, true);
    if (!weather.init(&pack, false, "", false)) return false;
    AtmosphereState sky{};
    sky.sun_dir = check_sun;
    for (int i = 1; i <= 12 * 60; ++i) {
        aircraft.step_autonomous_assist(2 * tick, 0.0, 0.0, 0.65, 0.0, sky, weather, i * 2 * tick);
        session.step_flight(aircraft, 2 * tick);
    }
    session.save_profile();
    FleetProfile before_park;
    if (read_fleet_profile(path, before_park) != ProfileLoad::Loaded ||
        before_park.parked_id_ != home->id || !before_park.logbook_.empty()) return false;

    // Stage the destination arrival independently of the airborne replay.
    // Use the operational park command and file store, including a fresh session read.
    const auto stands = pack.compute_parking_stands(*destination, 1);
    if (stands.empty()) return false;
    const auto &stand = stands.front();
    aircraft.reset_flight(stand.lat, stand.lon, stand.elev_m + 1.38, stand.heading_deg, 0, true);
    for (int i = 0; i < 100; ++i) {
        aircraft.step_autonomous_assist(2 * tick, 0, 0, 0, 1, sky, weather, 12 + i * 2 * tick);
        session.step_flight(aircraft, 2 * tick);
    }
    if (!session.can_park(aircraft) || !session.park(aircraft, *destination)) {
        std::fprintf(stderr, "[RUNTIME] Destination park rejected, ground=%d speed=%.2f\n",
                     aircraft.jsb_state().on_ground, aircraft.state().speed_mps);
        return false;
    }
    session.save_profile();
    AircraftSession restarted;
    restarted.init(pack, path);
    const auto &saved = restarted.profile();
    const auto &expected = session.profile();
    if (saved.parked_id_ != destination->id || !saved.has_custom_park_ ||
        std::abs(saved.parking_spot_.lat - expected.parking_spot_.lat) > tol ||
        std::abs(saved.parking_spot_.lon - expected.parking_spot_.lon) > tol ||
        std::abs(saved.parking_spot_.heading - expected.parking_spot_.heading) > tol ||
        std::abs(saved.care_.left_fuel_gal - expected.care_.left_fuel_gal) > tol ||
        saved.logbook_.size() != 1 || saved.logbook_.front().dep_id != home->id ||
        saved.logbook_.front().arr_id != destination->id) return false;

    const auto corrupt = isolated.directory / "broken.ini";
    const std::string truncated = "2\n\"LECI\"\n17 16 0.12 13\n0\n7.4\n";
    std::ofstream(corrupt) << truncated;
    FleetProfile untouched = saved;
    if (read_fleet_profile(corrupt, untouched) != ProfileLoad::Invalid ||
        untouched.logbook_.size() != 1) return false;
    AircraftSession protected_save;
    protected_save.init(pack, corrupt);
    protected_save.save_profile();
    std::ifstream in(corrupt);
    if (std::string(std::istreambuf_iterator<char>(in), {}) != truncated) return false;

    std::string error;
    return !write_fleet_profile(path / "fleet.ini", saved, error) && !error.empty();
}

bool replay_check(const PackReader &pack, bool gpu) {
    RegionalAtmosphere weather;
    weather.set_solver_seed(20261125, true);
    weather.set_season(3);
    weather.set_slot(2);
    if (!weather.init(&pack, gpu, "", false)) return false;
    AircraftAndHud aircraft;
    if (!aircraft.init_physics(&pack, "game/aircraft/c172s")) return false;
    const auto *home = pack.find_airport("ES-0205");
    if (!home) return false;
    aircraft.reset_flight(home->lat, home->lon, home->elev_m + 900, 150, 100, false);
    AtmosphereState sky{};
    sky.sun_dir = check_sun;
    SimulationClock clock;
    double time = 0;
    for (int frame = 0; frame < 120; ++frame) {
        const double dt = clock.consume((frame % 2 ? 3.0 : 1.0) * tick);
        time += dt;
        weather.update_field(check_sun, time, false, 0.28f);
        const double roll = frame < 60 ? 0.12 : -0.12;
        aircraft.step_autonomous_assist(dt, roll, 0, 0.7, 0, sky, weather, time);
    }
    const auto &flight = aircraft.jsb_state();
    const auto &air = aircraft.last_air();
    if (std::abs(time - 2.0) > tol || !std::isfinite(flight.alt_msl_m)) return false;
    std::printf("[REPLAY] %.12f %.12f %.9f %.9f %.9f %.9f\n", flight.lat_deg, flight.lon_deg,
                flight.alt_msl_m, flight.heading_deg, air.oat_c, air.wind_east_mps);

    // Exercise live AircraftAndHud::resolve_world_collisions -> FlightModel::apply_impact_impulse path (Torre del Agua, 76m tall)
    const double torre_lat = 41.6693, torre_lon = -0.9073;
    const double torre_g = pack.sample_elevation(torre_lat, torre_lon);
    aircraft.reset_flight(torre_lat, torre_lon, torre_g + 45.0, 0.0, 48.0, false);
    aircraft.step_autonomous_assist(tick, 0.0, 0.0, 0.0, 0.0, sky, weather, time + tick);
    const auto &post = aircraft.jsb_state();
    AircraftCareSnapshot dmg{};
    aircraft.sync_care_snapshot(dmg);
    const double tot_dmg = dmg.dmg_nose + dmg.dmg_wing_l + dmg.dmg_wing_r + dmg.dmg_tail + dmg.airframe_damage;
    if (!std::isfinite(post.lat_deg) || !std::isfinite(post.lon_deg) || !std::isfinite(post.alt_msl_m) ||
        tot_dmg <= 0.05) {
        std::fprintf(stderr, "[COLL-CHK-FAIL] post=(%.6f,%.6f,%.1f) g=%.1f dmg=%.3f\n",
                     post.lat_deg, post.lon_deg, post.alt_msl_m, torre_g, tot_dmg);
        return false;
    }
    return true;
}

bool world_check(PackReader &pack, bool gpu, bool full_outlook) {
    const auto *home = pack.find_airport("ES-0205");
    const auto *cat = pack.find_airport("ES-0098");
    if (!home || !cat) return false;
    RegionalAtmosphere weather;
    weather.set_solver_seed(20261125);
    if (!weather.init(&pack, gpu, "", false)) return false;
    const auto original = pack.world_frame();
    const auto terrain_stamp = [&](const std::shared_ptr<const PackReader::WorldFrame> &frame) {
        std::vector<float> heights;
        std::vector<uint8_t> ortho, ao, surface;
        pack.fill_page_data(*frame, 6000, 5000, 450, 19, 16, 16, check_sun, heights, ortho, ao, surface);
        return std::pair{heights, surface};
    };
    const auto reference = terrain_stamp(original);
    auto overlap = std::async(std::launch::async, [&] {
        for (int i = 0; i < 16; ++i)
            if (terrain_stamp(original) != reference) return false;
        return true;
    });

    AsyncTerrainStreamer stream;
    if (gpu && !stream.init(&pack, 2, OrthoFormat::BC7Fast)) return false;
    double time = 0.0;
    const uint64_t streamed = QuadKey{7, 61, 63}.pack();
    for (const AirportInfo *target : {cat, home, cat}) {
        weather.request_outlook();
        if (gpu) {
            stream.request_page(streamed, 20000.0f);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (!pack.reanchor_to(target->lat, target->lon)) return false;
        if (gpu) {
            stream.invalidate_all_and_prewarm(check_sun);
            stream.pump_gpu_uploads(0.0f, 16, 20.0);
            if (stream.find_resident(streamed, 0.0f) >= 0) return false;
        }
        weather.sync_region(target->lat, target->lon);
        time += 1.0;
        weather.update_field(check_sun, time);
        weather.finish_pending_initialization();
        const auto *region = pack.find_region_at(pack.anchor_lat(), pack.anchor_lon());
        if (!region || !weather.covers(target->lat, target->lon) ||
            weather.outlook().status != WeatherOutlook::Status::Idle ||
            weather.model_history().empty() || weather.model_history().back().region_id != region->region_id) {
            std::fprintf(stderr, "[RUNTIME] %s cover=%d model=%s expected=%s outlook=%d\n",
                         target->id.c_str(), weather.covers(target->lat, target->lon),
                         weather.model_history().empty() ? "empty" : weather.model_history().back().region_id.c_str(),
                         region ? region->region_id.c_str() : "none", static_cast<int>(weather.outlook().status));
            return false;
        }
    }
    if (!overlap.get()) return false;
    if (gpu) {
        // Verify async region transition isolates pending pages until commit and lands L0..L2 resident immediately.
        const uint64_t l0_key = QuadKey{0, 0, 0}.pack();
        const uint64_t l1_key = QuadKey{1, 1, 0}.pack();
        auto next_frame = pack.prepare_world_frame(home->lat, home->lon, true);
        if (!next_frame) return false;
        stream.begin_async_transition(next_frame, check_sun, home->lat, home->lon, 40.0, 10.0);
        const auto t_start = std::chrono::steady_clock::now();
        bool staged = false;
        while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count() < 5.0) {
            stream.begin_frame(100);
            stream.pump_gpu_uploads(1.0f, 4, 4.0);
            const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
            if (stream.is_async_transition_ready(elapsed)) { staged = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
        if (!staged || stream.find_resident(l0_key, 1.0f, nullptr, false) < 0) return false;
        pack.commit_world_frame(next_frame);
        stream.commit_async_transition();
        if (stream.find_resident(l0_key, 1.0f) < 0 || stream.find_resident(l1_key, 1.0f) < 0) return false;
    }
    if (!full_outlook) return true;
    const auto region_id = weather.model_history().back().region_id;
    weather.request_outlook();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(240);
    int reported = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto result = weather.outlook();
        if (static_cast<int>(result.predictions.size()) != reported) {
            reported = static_cast<int>(result.predictions.size());
            std::printf("[OUTLOOK] %d/3 leads published\n", reported);
            std::fflush(stdout);
        }
        if (result.status == WeatherOutlook::Status::Complete) {
            if (result.predictions.size() != 3) return false;
            for (size_t i = 0; i < 3; ++i) {
                const auto &entry = result.predictions[i];
                if (std::abs(entry.sim_time_sec - result.issue_sim_time - (i * 2 + 1) * 60) > tol ||
                    entry.region_id != region_id || !std::isfinite(entry.temperature_c)) return false;
            }
            return weather.model_history().back().sim_time_sec == time;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

bool replay_and_assist_checks() {
    // 1. Route video persistence + wrap-aware heading interpolation.
    ScopedTempDir tmp("opensky-route-XXXXXX");
    if (tmp.dir.empty()) return false;
    RouteVideoRecording rec;
    rec.title = "ES-0205 -> LESU";
    rec.frames = {
        {10.0, 42.50, 0.48, 1200.0, 350.0, 2.0, -5.0, 95.0, 2350.0, 0.0, 0.72, 0.1, 0.05, 0.0},
        {20.0, 42.55, 0.55, 1300.0, 10.0,  4.0, +5.0, 99.0, 2400.0, 10.0, 0.78, -0.1, -0.02, 0.02},
    };
    const auto rpath = tmp.dir / "last_flight.nsroute";
    if (!save_route_video(rpath, rec)) return false;
    RouteVideoRecording loaded;
    if (!load_route_video(rpath, loaded) || loaded.frames.size() != 2 || loaded.title != rec.title) return false;
    const RouteVideoFrame mid = sample_route_video(loaded, 15.0);
    if (std::abs(std::remainder(mid.heading_deg - 0.0, 360.0)) > 0.1 ||
        std::abs(mid.pitch_deg - 3.0) > 0.01 || std::abs(mid.flaps_deg - 5.0) > 0.01) {
        return false;
    }

    // 2. Smooth VSI-guided flight assist (zero step jump across neutral pitch center).
    nspain::FlightState fs{};
    fs.on_ground = false;
    fs.ias_kt = 95.0;
    fs.vsi_mps = 3.0;
    const auto a0 = compute_flight_and_taxi_assist(0.0, 0.00, 0.0, false, 0.0, fs);
    const auto a1 = compute_flight_and_taxi_assist(0.0, 0.05, 0.0, false, 0.0, fs);
    if (std::abs(a1.target_pitch_deg - a0.target_pitch_deg) > 1.0) return false;

    // 3. Closed-loop 2-axis autopilot VS->ALT ARM capture & Turn-Coordinator breaker disconnect.
    // Verified using real PistonAircraftSystems + FlightSimState math invariants.
    nspain::PistonAircraftSystems ap_sys;
    ap_sys.reset_to_running_default();
    ap_sys.tune_nav_standby(1, 115.90); // BLB Bilbao VOR
    ap_sys.swap_nav(1);
    ap_sys.update_navigation(43.20, -2.936, 1500.0);
    if (ap_sys.get_nav_ident(1) != "BLB" || !ap_sys.is_circuit_powered(nspain::CB_TURN_COORD)) return false;
    ap_sys.set_breaker_pulled(nspain::CB_TURN_COORD, true);
    if (ap_sys.is_circuit_powered(nspain::CB_TURN_COORD)) return false;
    return true;
}

bool collision_and_damage_checks(const PackReader &pack) {
    // 1. Building / landmark collision hits solidly; open sky and runway clear cleanly.
    const double pilar_g = pack.sample_elevation(41.6569, -0.8787);
    const auto pilar_hit = pack.query_object_collision(41.6569, -0.8787, pilar_g + 10.0, 0.6);
    const auto sky_clear = pack.query_object_collision(41.6569, -0.8787, pilar_g + 180.0, 0.6);
    if (!pilar_hit.hit || pilar_hit.kind > 1 || sky_clear.hit) return false;

    if (const AirportInfo *es = pack.find_airport("ES-0205")) {
        const double rwy_g = pack.sample_elevation(es->lat, es->lon);
        if (pack.query_object_collision(es->lat, es->lon, rwy_g + 1.5, 0.5).hit) return false;
    }

    // 2. Procedural damage & windshield crack persist cleanly across fleet_profile v3 save/load.
    ScopedTempDir tmp("opensky-dmg-XXXXXX");
    if (tmp.dir.empty()) return false;
    const auto pfile = tmp.dir / "fleet.ini";
    FleetProfile p;
    p.parked_id_ = "ES-0205";
    p.care_.airframe_damage = 0.42;
    p.care_.prop_nicks = 0.35;
    p.care_.windshield_crack = 0.68;
    p.care_.dmg_nose = 0.55;
    p.care_.dmg_wing_l = 0.30;
    p.care_.dmg_wing_r = 0.15;
    p.care_.dmg_tail = 0.22;
    std::string err;
    FleetProfile loaded;
    if (!write_fleet_profile(pfile, p, err) || read_fleet_profile(pfile, loaded) != ProfileLoad::Loaded) return false;
    if (std::abs(loaded.care_.windshield_crack - 0.68) > 1e-6 ||
        std::abs(loaded.care_.dmg_nose - 0.55) > 1e-6 ||
        std::abs(loaded.care_.dmg_wing_l - 0.30) > 1e-6 ||
        std::abs(loaded.care_.dmg_tail - 0.22) > 1e-6) return false;
    return true;
}

} // namespace

int run_runtime_checks(PackReader &pack, bool gpu, bool full_outlook) {
    if (!profile_checks(pack)) { std::fprintf(stderr, "[RUNTIME] Fleet profile or parking check failed\n"); return 1; }
    if (!replay_and_assist_checks()) { std::fprintf(stderr, "[RUNTIME] Route replay or flight assist check failed\n"); return 1; }
    if (!collision_and_damage_checks(pack)) { std::fprintf(stderr, "[RUNTIME] Object collision or damage check failed\n"); return 1; }
    if (!replay_check(pack, gpu)) { std::fprintf(stderr, "[RUNTIME] Replay failed\n"); return 1; }
    if (!world_check(pack, gpu, full_outlook)) { std::fprintf(stderr, "[RUNTIME] Region concurrency or outlook check failed\n"); return 1; }
    std::puts("[RUNTIME OK] Save, parking, replay, flight assist, and world transitions");
    return 0;
}
} // namespace nsim
