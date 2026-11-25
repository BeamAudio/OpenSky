#include "runtime_checks.hpp"

#include "aircraft_hud.hpp"
#include "aircraft_session.hpp"
#include "async_streamer.hpp"
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
        auto pattern = (std::filesystem::temp_directory_path() / "opensky-runtime-XXXXXX").string();
        if (::mkdtemp(pattern.data())) directory = pattern;
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

} // namespace

int run_runtime_checks(PackReader &pack, bool gpu, bool full_outlook) {
    if (!profile_checks(pack)) { std::fprintf(stderr, "[RUNTIME] Fleet profile or parking check failed\n"); return 1; }
    if (!replay_check(pack, gpu)) { std::fprintf(stderr, "[RUNTIME] Replay failed\n"); return 1; }
    if (!world_check(pack, gpu, full_outlook)) { std::fprintf(stderr, "[RUNTIME] Region concurrency or outlook check failed\n"); return 1; }
    std::puts("[RUNTIME OK] Save, parking, replay, and world transitions");
    return 0;
}
} // namespace nsim
