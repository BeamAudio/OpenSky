#include "aircraft_hud.hpp"
#include "aircraft_package.hpp"
#include "app_paths.hpp"
#include "airfield_vegetation.hpp"
#include "async_streamer.hpp"
#include "audio_engine.hpp"
#include "cdlod_terrain.hpp"
#include "gl_loader.hpp"
#include "game_frontend.hpp"
#include "flight_profiler.hpp"
#include "flight_replay.hpp"
#include "fleet_profile.hpp"
#include "living_traffic.hpp"
#include "pack_reader.hpp"
#include "platform_window.hpp"
#include "regional_atmosphere.hpp"
#include "runtime_checks.hpp"
#include "simulation_clock.hpp"
#include "volumetric_clouds.hpp"
#include "world_objects.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <chrono>
#include <thread>
#include <unordered_set>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <numeric>
#include <string>
#include <vector>

using namespace nsim;

namespace {

// Self-check unit test for our pure-C++ PackReader, PMRF building count & velocity-predictive CDLOD math
int run_selftest(const PackReader &pack) {
    double ez = pack.sample_elevation(41.6662, -1.0416);
    double eh = pack.sample_elevation(42.0810, -0.3234);
    double ec = pack.sample_elevation(42.5210,  0.4830);
    if (!(ez > 220.0 && ez < 300.0)) { std::fprintf(stderr, "[FAIL] LEZG elev=%.1f\n", ez); return 1; }
    if (!(eh > 500.0 && eh < 580.0)) { std::fprintf(stderr, "[FAIL] LEHC elev=%.1f\n", eh); return 1; }
    if (!(ec > 880.0 && ec < 935.0)) { std::fprintf(stderr, "[FAIL] ES-0205 elev=%.1f\n", ec); return 1; }
    const int zoom_checks[][2] = {{100, 20}, {200, 20}, {201, 19}, {500, 19}, {501, 18},
                                  {2000, 18}, {4000, 17}, {8000, 16}, {12000, 15},
                                  {28000, 14}, {28001, 13}, {48000, 13}, {48001, 0}};
    for (const auto &zc : zoom_checks) {
        if (online_zoom_for_distance_m(static_cast<double>(zc[0])) != zc[1]) {
            std::fprintf(stderr, "[FAIL] online zoom at %dm = %d (expected %d)\n",
                         zc[0], online_zoom_for_distance_m(static_cast<double>(zc[0])), zc[1]);
            return 1;
        }
    }
    if (pack.buildings().size() < 95000) { std::fprintf(stderr, "[FAIL] bldgs=%zu\n", pack.buildings().size()); return 1; }
    if (pack.roads().size() < 40000) { std::fprintf(stderr, "[FAIL] roads=%zu\n", pack.roads().size()); return 1; }
    std::vector<uint32_t> zgz_roads;
    pack.query_roads_in_box(41.64, 41.67, -0.90, -0.86, zgz_roads);
    if (zgz_roads.size() < 400) { std::fprintf(stderr, "[FAIL] zaragoza road query=%zu\n", zgz_roads.size()); return 1; }
    if (pack.is_near_building(42.3175, 0.2037, 4.0) || pack.is_near_road(42.3175, 0.2037, 8.0)) {
        std::fprintf(stderr, "[FAIL] unexpected building/road in Mediano water\n"); return 1;
    }
    // Verify fast O(1) cell occupancy mask catches Zaragoza buildings/roads and leaves Mediano water empty
    uint8_t occ_chk[32 * 32];
    double ze_m = (-0.880 - pack.anchor_lon()) * pack.anchor_m_lon();
    double zn_m = (41.652 - pack.anchor_lat()) * pack.anchor_m_lat();
    pack.fill_cell_occupancy(ze_m - 250.0, zn_m - 250.0, 500.0, 32, occ_chk);
    int z_occ = 0;
    for (uint8_t v : occ_chk) if (v) z_occ++;
    if (z_occ < 200) { std::fprintf(stderr, "[FAIL] Zaragoza cell occupancy too sparse=%d\n", z_occ); return 1; }
    // Verify map-wide pre-baked populated-zone static pack ("NSCT" v3) + sparse shadow pack ("NSSH" v2) + 10-TOD RT Night GI!
    {
        std::ifstream cf("packs/es_aragon_cities.nscity", std::ios::binary);
        char magic[4]{}; uint32_t ver = 0, btot = 0, ch = 0;
        cf.read(magic, 4); cf.read(reinterpret_cast<char *>(&ver), 4);
        cf.read(reinterpret_cast<char *>(&btot), 4); cf.read(reinterpret_cast<char *>(&ch), 4);
        if (!cf || std::memcmp(magic, "NSCT", 4) != 0 || ver != 3 || btot < 80000 || ch < 5000) {
            std::fprintf(stderr, "[FAIL] es_aragon_cities.nscity v3 check failed (ver=%u btot=%u chunks=%u)\n", ver, btot, ch);
            return 1;
        }
        if (pack.city_shadow_tile_count() < 5000) {
            std::fprintf(stderr, "[FAIL] es_aragon_city_sh.nsshd v2 tiles=%d\n", pack.city_shadow_tile_count());
            return 1;
        }
        // Verify 10-TOD + Sky AO + 4-bit RT Night Light bitpack round-trip & phase mapping across ALL 10 slots!
        float tv[10];
        for (int k = 0; k < 10; k++) {
            tv[k] = static_cast<float>(k % 4) / 3.0f;
            float p10 = sun_dir_to_phase10(tod_slot_sun_dir(k));
            if (std::abs(p10 - static_cast<float>(k)) > 0.02f) {
                std::fprintf(stderr, "[FAIL] sun_dir_to_phase10 slot %d -> %.3f\n", k, p10);
                return 1;
            }
        }
        uint32_t pk = pack_tod10_u32(tv, 180.0f / 255.0f, 11.0f / 15.0f);
        for (int k = 0; k < 10; k++) {
            if (std::abs(unpack_tod10_vis(pk, static_cast<float>(k)) - tv[k]) > 0.02f) {
                std::fprintf(stderr, "[FAIL] tod10 unpack slot %d failed\n", k);
                return 1;
            }
        }
        if (std::abs(unpack_tod10_ao(pk) - 180.0f / 255.0f) > 0.01f || std::abs(unpack_tod10_night(pk) - 11.0f / 15.0f) > 0.01f) {
            std::fprintf(stderr, "[FAIL] tod10 ao/night unpack failed\n");
            return 1;
        }
        // Verify Zaragozan street receives nonzero RT night light & Pyrenees corridor has real mountain shadow variance!
        uint32_t z_gi = pack.sample_baked_gi_u32_en(ze_m, zn_m);
        if (unpack_tod10_ao(z_gi) >= 0.99f) {
            std::fprintf(stderr, "[FAIL] Zaragoza baked Sky AO expected canyon occlusion (<0.99), got %.3f\n", unpack_tod10_ao(z_gi));
            return 1;
        }
        int sh_dark = 0;
        for (uint32_t w : pack.corridor_gi_u32()) if (unpack_tod10_vis(w, 0.0f) < 0.7f) sh_dark++;
        if (sh_dark < 5000) {
            std::fprintf(stderr, "[FAIL] Corridor 50km Dawn shadow pixels too low=%d\n", sh_dark);
            return 1;
        }
        if (!std::filesystem::exists("game/assets/pbr/hud_font.png")) {
            std::fprintf(stderr, "[FAIL] Missing HiDPI HUD font atlas game/assets/pbr/hud_font.png\n");
            return 1;
        }
    }
    const AirportInfo *leci = pack.find_airport("LECI");
    if (!leci || std::abs(leci->rwy_lon_b - leci->rwy_lon_a) <= std::abs(leci->rwy_lat_b - leci->rwy_lat_a)) {
        std::fprintf(stderr, "[FAIL] LECI runway orientation parse check failed\n");
        return 1;
    }
    const AirportInfo *es0205 = pack.find_airport("ES-0205");
    if (!es0205 || es0205->surface != "grass") {
        std::fprintf(stderr, "[FAIL] ES-0205 expected grass surface, got %s\n", es0205 ? es0205->surface.c_str() : "missing");
        return 1;
    }
    if (leci->fences.empty() || leci->ground_features.empty()) {
        std::fprintf(stderr, "[FAIL] LECI expected OSM fences and ground_features\n");
        return 1;
    }

    // Verify QuadKey pack/unpack round-trip & parent relationship
    QuadKey k{7, 83, 41};
    QuadKey ku = QuadKey::unpack(k.pack());
    if (ku.lod != 7 || ku.qx != 83 || ku.qy != 41 || ku.parent().lod != 6 || ku.parent().qx != 41 || ku.parent().qy != 20) {
        std::fprintf(stderr, "[FAIL] QuadKey pack/parent check failed\n");
        return 1;
    }
    // Verify unified trackpad+keyboard Flight & Taxi Assist (wings-level, VSI hold, stall guard, taxi heading-hold & diff brake)
    {
        nspain::FlightState air{};
        air.on_ground = false; air.ias_kt = 95.0; air.roll_deg = 18.0; air.pitch_deg = 2.0; air.vsi_mps = 3.0;
        auto lvl = compute_flight_and_taxi_assist(0.0, 0.0, 0.0, false, 160.0, air);
        if (!(lvl.aileron < -0.25 && std::abs(lvl.target_bank_deg) < 0.01 && lvl.target_pitch_deg < 0.0)) {
            std::fprintf(stderr, "[FAIL] flight assist wings/VSI hold failed (ail=%.3f tgt_p=%.1f)\n", lvl.aileron, lvl.target_pitch_deg);
            return 1;
        }
        air.roll_deg = 0.0; air.vsi_mps = 0.0;
        auto turn = compute_flight_and_taxi_assist(0.65, 0.0, 0.0, false, 160.0, air);
        if (!(turn.target_bank_deg > 12.0 && turn.aileron > 0.20 && turn.rudder > 0.05)) {
            std::fprintf(stderr, "[FAIL] flight assist turn/coordination failed (bank=%.1f ail=%.3f rud=%.3f)\n",
                         turn.target_bank_deg, turn.aileron, turn.rudder);
            return 1;
        }
        air.ias_kt = 41.0; air.alpha_deg = 16.0;
        auto stall = compute_flight_and_taxi_assist(0.0, 1.0, 0.0, false, 160.0, air);
        if (!(stall.target_pitch_deg < 4.5)) {
            std::fprintf(stderr, "[FAIL] flight assist stall guard failed (tgt_pitch=%.1f)\n", stall.target_pitch_deg);
            return 1;
        }
        // Verify normal 58 kt rotation/climb is NOT suppressed by stall guard!
        air.ias_kt = 58.0; air.alpha_deg = 9.0; air.cl = 1.0;
        auto climb = compute_flight_and_taxi_assist(0.0, 0.65, 0.0, false, 160.0, air);
        if (!(climb.target_pitch_deg > 7.5)) {
            std::fprintf(stderr, "[FAIL] 58kt rotation climb suppressed (tgt_pitch=%.1f)\n", climb.target_pitch_deg);
            return 1;
        }
        nspain::FlightState gnd{};
        gnd.on_ground = true; gnd.ground_speed_mps = 5.0; gnd.heading_deg = 152.0;
        auto hold = compute_flight_and_taxi_assist(0.0, 0.0, 0.0, false, 160.0, gnd);
        if (!(hold.holding_heading && hold.rudder > 0.20)) {
            std::fprintf(stderr, "[FAIL] taxi heading-hold failed (hold=%d rud=%.3f)\n", hold.holding_heading, hold.rudder);
            return 1;
        }
        auto steer = compute_flight_and_taxi_assist(0.85, 0.5, 0.0, false, 160.0, gnd);
        if (!(steer.rudder > 0.35 && steer.brake_right_add > 0.15 && steer.elevator > 0.15)) {
            std::fprintf(stderr, "[FAIL] taxi steer/diff-brake failed (rud=%.3f brk_r=%.3f elv=%.3f)\n",
                         steer.rudder, steer.brake_right_add, steer.elevator);
            return 1;
        }
    }
    // Verify Reverse-Z infinite perspective maps z=-near -> 1.0 and z=-100km -> ~0.0
    Mat4 proj = Mat4::perspective_reverse_z(50.0f * DEG2RAD_F, 16.0f / 9.0f, 0.18f);
    Vec4 near_p = proj.mul({0.0f, 0.0f, -0.18f, 1.0f});
    Vec4 far_p  = proj.mul({0.0f, 0.0f, -100000.0f, 1.0f});
    float z_near = near_p.z / near_p.w;
    float z_far  = far_p.z / far_p.w;
    if (std::abs(z_near - 1.0f) > 1e-4f || z_far < 0.0f || z_far > 1e-4f) {
        std::fprintf(stderr, "[FAIL] Reverse-Z depth check: near=%f far=%f\n", z_near, z_far);
        return 1;
    }

    // Verify physics-based RegionalAtmosphere mesoscale currents (Cierzo Venturi, Thermals vs Lake Sink, Diurnal Reversal)
    RegionalAtmosphere atmo;
    if (!atmo.init(&pack, false, "packs/es_aragon_weather.nswx", false)) {
        std::fprintf(stderr, "[FAIL] RegionalAtmosphere init (missing packs/es_aragon_weather.nswx?)\n");
        return 1;
    }
    Vec3 sun_day = normalize(Vec3(0.55f, 0.52f, 0.65f));
    Vec3 sun_night = normalize(Vec3(-0.35f, -0.22f, 0.91f));

    // Baked-library smoke: regimes select, layers are stable, storm increases turbulence, and library is active.
    atmo.set_regime(WindRegime::CIERZO_NW);
    atmo.update_field(sun_day, 0.0, true, 0.20f);
    LocalAirSample z_cierzo = atmo.sample_at(41.6662, -1.0416, ez + 250.0, sun_day, 0.0, 0.20f);
    if (std::isnan(z_cierzo.speed_kt()) || z_cierzo.speed_kt() < 0.0) {
        std::fprintf(stderr, "[FAIL] Cierzo sample invalid speed\n");
        return 1;
    }

    atmo.set_regime(WindRegime::PYRENEAN_THERMAL);
    atmo.update_field(sun_day, 0.0, true, 0.10f);
    LocalAirSample med_water = atmo.sample_at(42.3120, 0.1940, pack.sample_elevation(42.3120, 0.1940) + 120.0, sun_day, 0.0, 0.10f);
    if (!(med_water.thermal_lift_mps <= 0.35)) {
        std::fprintf(stderr, "[FAIL] Mediano lake thermal check: thr=%.2f\n", med_water.thermal_lift_mps);
        return 1;
    }

    atmo.update_field(sun_night, 0.0, true, 0.10f);
    LocalAirSample pyr_night = atmo.sample_at(42.5313, 0.4768, ec + 250.0, sun_night, 0.0, 0.10f);

    atmo.set_regime(WindRegime::PYRENEAN_THERMAL);
    atmo.update_field(sun_day, 12.0, true, 0.10f);
    LocalAirSample pyr_day = atmo.sample_at(42.5313, 0.4768, ec + 250.0, sun_day, 12.0, 0.10f);
    LocalAirSample l0_clear = atmo.sample_at(42.5313, 0.4768, ec + 150.0,  sun_day, 12.0, 0.10f);
    LocalAirSample l2_wave  = atmo.sample_at(42.5313, 0.4768, 3600.0,      sun_day, 12.0, 0.45f);
    LocalAirSample l3_jet   = atmo.sample_at(42.5313, 0.4768, 6200.0,      sun_day, 12.0, 0.45f);
    if (l0_clear.atmo_layer != 0 || l2_wave.atmo_layer != 2 || l3_jet.atmo_layer != 3) {
        std::fprintf(stderr, "[FAIL] Baked layer assignment failed\n");
        return 1;
    }
    atmo.update_field(sun_day, 12.0, true, 0.95f);
    LocalAirSample l1_storm = atmo.sample_at(42.5313, 0.4768, ec + 1100.0, sun_day, 12.0, 0.95f);
    if (!(l1_storm.turb_rms >= l0_clear.turb_rms - 0.01)) {
        std::fprintf(stderr, "[FAIL] Storm turb not >= clear turb (%.2f vs %.2f)\n", l1_storm.turb_rms, l0_clear.turb_rms);
        return 1;
    }
    atmo.set_manual_weather_override(true);
    atmo.set_regime(WindRegime::CONVECTIVE_STORM);
    atmo.update_field(sun_day, 25.0, true, 0.90f);
    LocalAirSample storm_s = atmo.sample_at(42.5313, 0.4768, ec + 600.0, sun_day, 25.0, 0.90f);
    if (!(storm_s.precipitation_mm_hr >= 3.0 && storm_s.turb_rms >= 0.32)) {
        std::fprintf(stderr, "[FAIL] CONVECTIVE_STORM sample too weak (rain=%.2f turb=%.2f)\n",
                     storm_s.precipitation_mm_hr, storm_s.turb_rms);
        return 1;
    }
    atmo.set_regime(WindRegime::WINTER_ICING);
    atmo.update_field(sun_day, 25.0, true, 0.78f);
    LocalAirSample ice_s = atmo.sample_at(42.5313, 0.4768, ec + 600.0, sun_day, 25.0, 0.78f);
    if (!(ice_s.oat_c < 0.0 && ice_s.snow_factor > 0.5 && ice_s.icing_intensity > 0.15)) {
        std::fprintf(stderr, "[FAIL] WINTER_ICING sample check failed (oat=%.1f snow=%.2f ice=%.2f)\n",
                     ice_s.oat_c, ice_s.snow_factor, ice_s.icing_intensity);
        return 1;
    }
    // Verify hardware-seeded MIL-F-8785C Dryden turbulence is non-deterministic and scales with weather TKE!
    {
        StochasticDrydenFilter d1, d2, d_calm, d_storm;
        double diff_sum = 0.0, rms_calm = 0.0, rms_storm = 0.0;
        for (int i = 0; i < 2400; ++i) {
            auto g1 = d1.step(0.02, 52.0, 220.0, 2.2, 2.2, 1.6);
            auto g2 = d2.step(0.02, 52.0, 220.0, 2.2, 2.2, 1.6);
            diff_sum += std::abs(g1.ug_mps - g2.ug_mps) + std::abs(g1.wg_mps - g2.wg_mps);
            auto gc = d_calm.step(0.02, 52.0, 220.0, 0.25, 0.25, 0.20);
            auto gs = d_storm.step(0.02, 52.0, 220.0, 3.80, 3.80, 2.80);
            rms_calm += gc.wg_mps * gc.wg_mps;
            rms_storm += gs.wg_mps * gs.wg_mps;
        }
        if (!(diff_sum > 25.0 && rms_storm > rms_calm * 8.0)) {
            std::fprintf(stderr, "[FAIL] Stochastic Dryden check failed (diff=%.1f calm=%.2f storm=%.2f)\n",
                         diff_sum, rms_calm, rms_storm);
            return 1;
        }
    }
    if (!AudioEngine::run_selftest()) return 1;
    if (!LivingTraffic::run_selftest(pack)) return 1;
    const auto &w = atmo.weather();
    std::printf("[SELFTEST OK] LEZG=%.1fm LEHC=%.1fm ES-0205=%.1fm | baked %s | WX %s | Cierzo=%.1fkt pyr_night_n=%.1f dVn(day-night)=%.1f | stormTurb=%.2f clearTurb=%.2f | ice=%.2f snow=%.2f\n",
                ez, eh, ec, atmo.library().provenance(), w.summary.c_str(), z_cierzo.speed_kt(),
                pyr_night.wind_north_mps, pyr_day.wind_north_mps - pyr_night.wind_north_mps,
                l1_storm.turb_rms, l0_clear.turb_rms, ice_s.icing_intensity, ice_s.snow_factor);
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    bool selftest_only = false;
    bool runtime_check = false, runtime_check_gpu = false, outlook_check = false;
    bool only_region = false;
    bool bench_only = false;
    bool cloud_qa = false;
    bool cockpit_qa = false;
    bool promo_only = false;
    bool season_qa = false;
    std::string req_region;
    std::string req_airport;
    std::string req_aircraft;
    bool list_aircraft = false;
    std::string packs_dir = "packs";
    bool packs_dir_explicit = false;
    std::string autotest_dir;
    std::string topdown_dir;
    std::string airport_qa_dir;
    std::string world_qa_png;
    std::string profile_dir;
    std::string region_test_dir;
    std::string record_crossing_path;
    std::string replay_flight_path;
    bool replay_headless = false;
    std::string season_arg = "auto";
    std::string weather_arg = "auto";
    bool quick_profile = false;
    bool debug_overlays = false;
    bool no_audio = false;
    // Mode-6 BC7 keeps the same 16-byte blocks while releasing L7-L9 pages
    // about four times faster than the partition-search encoder on Renoir.
    OrthoFormat ortho_format = OrthoFormat::BC7Fast;
    OnlineOrthoProvider init_online = OnlineOrthoProvider::PNOA;
    bool ortho_explicit = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--selftest") selftest_only = true;
        else if (a == "--runtime-check") runtime_check = true;
        else if (a == "--runtime-check-gpu") runtime_check = runtime_check_gpu = true;
        else if (a == "--outlook-check") runtime_check = outlook_check = true;
        else if (a == "--bench-only") bench_only = true;
        else if (a == "--promo-only") promo_only = true;
        else if (a == "--debug-overlays") debug_overlays = true;
        else if (a == "--no-audio") no_audio = true;
        else if ((a == "--profile" || a == "--quick-profile") && i + 1 < argc) {
            profile_dir = argv[++i];
            quick_profile = a == "--quick-profile";
        } else if (a == "--ortho-format" && i + 1 < argc) {
            std::string value = argv[++i];
            if (value == "bc7") ortho_format = OrthoFormat::BC7;
            else if (value == "bc7-fast") ortho_format = OrthoFormat::BC7Fast;
            else if (value == "rgba8") ortho_format = OrthoFormat::RGBA8;
            else {
                std::fprintf(stderr, "[PROFILE] --ortho-format expects bc7, bc7-fast, or rgba8\n");
                return 2;
            }
        }
        else if (a == "--cloud-qa") cloud_qa = true;
        else if (a == "--cockpit-qa" && i + 1 < argc) { cockpit_qa = true; autotest_dir = argv[++i]; }
        else if (a == "--season-qa" && i + 1 < argc) { season_qa = true; autotest_dir = argv[++i]; }
        else if (a == "--only-region") only_region = true;
        else if (a == "--region" && i + 1 < argc) req_region = argv[++i];
        else if (a == "--airport" && i + 1 < argc) req_airport = argv[++i];
        else if (a == "--aircraft" && i + 1 < argc) req_aircraft = argv[++i];
        else if (a == "--list-aircraft") list_aircraft = true;
        else if (a == "--season" && i + 1 < argc) season_arg = argv[++i];
        else if (a == "--weather" && i + 1 < argc) weather_arg = argv[++i];
        else if (a == "--packs-dir" && i + 1 < argc) { packs_dir = argv[++i]; packs_dir_explicit = true; }
        else if (a == "--autotest" && i + 1 < argc) autotest_dir = argv[++i];
        else if (a == "--topdown"  && i + 1 < argc) topdown_dir  = argv[++i];
        else if (a == "--world-qa" && i + 1 < argc) world_qa_png = argv[++i];
        else if (a == "--airport-qa" && i + 1 < argc) airport_qa_dir = argv[++i];
        else if (a == "--region-flight-test" && i + 1 < argc) region_test_dir = argv[++i];
        else if (a == "--record-crossing-flight" && i + 1 < argc) record_crossing_path = argv[++i];
        else if (a == "--replay-flight" && i + 1 < argc) replay_flight_path = argv[++i];
        else if (a == "--replay-headless") replay_headless = true;
        else if (a == "--online-ortho" && i + 1 < argc) {
            ortho_explicit = true;
            std::string p = argv[++i];
            if (p == "pnoa") init_online = OnlineOrthoProvider::PNOA;
            else if (p == "off") init_online = OnlineOrthoProvider::OFF;
            else {
                std::fprintf(stderr, "[OPENSKY] --online-ortho expects pnoa or off\n");
                return 2;
            }
        }
    }
    if (quick_profile && (!autotest_dir.empty() || !topdown_dir.empty() || selftest_only)) {
        std::fprintf(stderr, "[PROFILE] --quick-profile is a standalone capture mode\n");
        return 2;
    }
    int season_override = -1;
    if (season_arg == "winter") season_override = 0;
    else if (season_arg == "spring") season_override = 1;
    else if (season_arg == "summer") season_override = 2;
    else if (season_arg == "autumn") season_override = 3;
    else if (season_arg != "auto") {
        std::fprintf(stderr, "[SEASON] --season expects auto, winter, spring, summer, or autumn\n");
        return 2;
    }
    const std::string root = ".";
    std::vector<std::string> packs_dirs;
    if (packs_dir_explicit) {
        packs_dirs.push_back(packs_dir);
    } else {
        // User-installed regions are loaded first, so they can override a shipped region with the same id.
        packs_dirs.push_back((opensky_data_dir() / "packs").string());
        packs_dirs.push_back(packs_dir);
    }
    const bool interactive_run = autotest_dir.empty() && topdown_dir.empty() && !quick_profile && airport_qa_dir.empty() && region_test_dir.empty() && world_qa_png.empty() && replay_flight_path.empty();
    // The profiler owns GL queries, so it must be destroyed before the window.
    PlatformWindow win;
    FlightProfiler profiler;
    const std::string &trace_dir = profile_dir.empty() && !cockpit_qa ? autotest_dir : profile_dir;
    if (!trace_dir.empty() && !profiler.open(trace_dir)) {
        std::fprintf(stderr, "[PROFILE] Unable to open output directory %s\n", trace_dir.c_str());
        return 1;
    }
    PackReader pack;
    if (!pack.load_packs_dirs(packs_dirs, only_region ? req_region : "", req_region)) return 1;
    profiler.startup_stage("pack_load_and_corridor_bake");
    if (!req_region.empty() && !pack.find_region_by_id(req_region)) {
        std::fprintf(stderr, "[OPENSKY] Region '%s' is not installed. Open the World Editor to build or install it.\n", req_region.c_str());
        return 1;
    }
    GraphicsSettings graphics_settings = load_graphics_settings();
    if (promo_only) {
        // Promotional captures deliberately render the highest quality tier
        // and a wider terrain radius without changing the user's saved settings.
        graphics_settings.quality_preset = 3;
        graphics_settings.pixel_percent = 100;
        graphics_settings.aa_mode = 2;
        graphics_settings.terrain_lod_percent = 500;
        graphics_settings.weather_detail = 3;
        graphics_settings.aircraft_detail = 1;
    }
    const auto profile_file = interactive_run
        ? graphics_settings_path().parent_path() / "fleet.ini" : std::filesystem::path{};
    const auto aircraft_roots = std::vector<std::filesystem::path>{
        std::filesystem::path(root) / "game/aircraft",
        graphics_settings_path().parent_path() / "aircraft"
    };
    auto aircraft_catalog = discover_aircraft_packages(aircraft_roots);
    for (const auto &warning : aircraft_catalog.warnings)
        std::fprintf(stderr, "[AIRCRAFT] Skipping package: %s\n", warning.c_str());
    if (list_aircraft) {
        for (const auto &item : aircraft_catalog.packages)
            std::printf("%s\t%s\t%s\n", item.id.c_str(), item.name.c_str(), item.systems_profile.c_str());
        return aircraft_catalog.packages.empty() ? 1 : 0;
    }
    std::string selected_aircraft_id = req_aircraft;
    if (selected_aircraft_id.empty() && !profile_file.empty()) {
        FleetProfile saved_profile;
        if (read_fleet_profile(profile_file, saved_profile) == ProfileLoad::Loaded)
            selected_aircraft_id = saved_profile.aircraft_id_;
    }
    if (selected_aircraft_id.empty()) selected_aircraft_id = "c172s";
    const auto selected_aircraft = std::find_if(aircraft_catalog.packages.begin(), aircraft_catalog.packages.end(),
        [&](const AircraftPackage &item) { return item.id == selected_aircraft_id; });
    if (selected_aircraft == aircraft_catalog.packages.end()) {
        std::fprintf(stderr, "[AIRCRAFT] Package '%s' is not installed. Use --list-aircraft or install it under game/aircraft or %s/aircraft.\n",
                     selected_aircraft_id.c_str(), graphics_settings_path().parent_path().c_str());
        return 1;
    }
    const AircraftPackage aircraft_package = *selected_aircraft;
    if (ortho_explicit) graphics_settings.terrain_streaming = init_online == OnlineOrthoProvider::PNOA;
    else if (!graphics_settings.terrain_streaming) init_online = OnlineOrthoProvider::OFF;
    pack.set_online_provider(init_online);

    const AirportInfo *start_ap = nullptr;
    if (!req_airport.empty()) {
        start_ap = pack.find_airport(req_airport);
        if (!start_ap) {
            std::fprintf(stderr, "[OPENSKY] Airfield '%s' is not present in the selected region.\n", req_airport.c_str());
            return 1;
        }
    } else if (!req_region.empty()) {
        if (req_region == "es_aragon_pyrenees") start_ap = pack.find_airport("ES-0205");
        if (!start_ap) {
            for (const auto &ap : pack.airports()) {
                if (ap.region_id == req_region) { start_ap = &ap; break; }
            }
        }
    } else {
        start_ap = pack.find_airport("ES-0205");
        if (!start_ap && !pack.airports().empty()) start_ap = &pack.airports().front();
    }

    if (start_ap && world_qa_png.empty()) {
        const double distance = nspain::haversine_m(start_ap->lat, start_ap->lon, pack.anchor_lat(), pack.anchor_lon());
        if (!req_airport.empty() || distance > 65000.0) pack.reanchor_to(start_ap->lat, start_ap->lon);
    }
    if (selftest_only) return run_selftest(pack);
    if (runtime_check) {
        if (runtime_check_gpu && !win.create("OpenSky runtime check", 960, 540)) return 1;
        return run_runtime_checks(pack, runtime_check_gpu, outlook_check);
    }
    if (!record_crossing_path.empty()) {
        return record_castejon_catalonia_flight(pack, record_crossing_path) ? 0 : 1;
    }
    if (!replay_flight_path.empty() && replay_headless) {
        return replay_flight_headless(pack, replay_flight_path) ? 0 : 1;
    }
    profiler.startup_stage("spawn_anchor");

    const int output_w = !topdown_dir.empty() ? 1024 : GAME_OUTPUT_W;
    const int output_h = !topdown_dir.empty() ? 1024 : GAME_OUTPUT_H;
    if (!win.create("OpenSky VFR Simulator", output_w, output_h,
                    static_cast<DisplayMode>(graphics_settings.display_mode))) {
        return 1;
    }
    // Cap FPS to monitor refresh rate (<= 60 Hz for iGPU thermal stability <80C) and enable VSync interactively.
    const double fps_cap = std::clamp(win.refresh_hz(), 30.0, 60.0);
    const double frame_period = 1.0 / fps_cap;
    if (interactive_run) win.set_vsync(true);
    std::printf("[OPENSKY] OpenGL %s | Renderer: %s | %dx%d output, %dx%d window (%.0f Hz, limit %.0f FPS) | %zu region(s) | %d tiles | %zu airfields | %zu OSM bldgs | %zu OSM roads\n",
                reinterpret_cast<const char *>(glGetString(GL_VERSION)),
                reinterpret_cast<const char *>(glGetString(GL_RENDERER)),
                output_w, output_h, win.width(), win.height(), win.refresh_hz(), fps_cap,
                pack.regions().size(), pack.tile_count(), pack.airports().size(), pack.buildings().size(), pack.roads().size());

    profiler.init_gpu();
    profiler.set_overlay(debug_overlays);
    profiler.startup_stage("window_and_gl");

    int quality = interactive_run ? graphics_settings.quality_preset : 2; // 0=LOW iGPU, 1=MEDIUM, 2=HIGH, 3=ULTRA
    if (promo_only) quality = 3;
    int tod_slot = (quick_profile || !autotest_dir.empty() || !topdown_dir.empty() || !airport_qa_dir.empty()) ? 2 : graphics_settings.time_of_day;
    // CLI selection enables repeatable QA captures; N cycles the same states during play.
    float wx_advance = 0.28f; // 0.0=Clear, 0.28=Fair SCT, 0.60=Broken, 0.92=Overcast/Rain!
    bool show_diagnostics = !autotest_dir.empty();
    int weather_debug_level = debug_overlays ? 1 : -1;
    bool physics_debug = debug_overlays;

    AsyncTerrainStreamer streamer;
    CdlodTerrain terrain;
    SkyAndClouds sky_clouds;
    RegionalAtmosphere reg_atmo;
    WorldObjects objects;
    LivingTraffic living_traffic;
    AirfieldVegetation vegetation;
    AudioEngine audio;
    AircraftAndHud ac_hud;
    GameFrontend frontend;

    if (!streamer.init(&pack, 0, ortho_format)) return 1;
    profiler.startup_stage("terrain_streamer_allocate");
    const char *format_name = ortho_format == OrthoFormat::BC7 ? "bc7"
                           : ortho_format == OrthoFormat::BC7Fast ? "bc7-fast" : "rgba8";
    profiler.metadata(output_w, output_h, format_name,
                      pack.online_provider_name(), streamer.worker_count(), streamer.stats().gpu_page_bytes);
    std::printf("[MULTICORE] AsyncTerrainStreamer: %d parallel workers | PackReader 10-TOD DEM: parallelized\n", streamer.worker_count());
    if (!terrain.init(root + "/game/assets", root + "/sim_engine/shaders")) return 1;
    profiler.startup_stage("terrain_resources");
    if (!sky_clouds.init(root + "/sim_engine/shaders", output_w, output_h, quality,
                         graphics_settings.pixel_percent, graphics_settings.aa_mode)) return 1;
    profiler.startup_stage("sky_cloud_resources");
    if (!interactive_run) reg_atmo.set_solver_seed(0x1725);
    if (!reg_atmo.init(&pack, true, "", interactive_run)) return 1;
    if (weather_arg != "auto") {
        reg_atmo.set_manual_weather_override(true);
        if (weather_arg == "thermal" || weather_arg == "clear") {
            reg_atmo.set_regime(WindRegime::PYRENEAN_THERMAL);
            wx_advance = 0.25f;
        } else if (weather_arg == "cierzo") {
            reg_atmo.set_regime(WindRegime::CIERZO_NW);
            wx_advance = 0.28f;
        } else if (weather_arg == "foehn") {
            reg_atmo.set_regime(WindRegime::FOEHN_SUR);
            wx_advance = 0.45f;
        } else if (weather_arg == "fog") {
            reg_atmo.set_regime(WindRegime::KATABATIC_DAWN);
            wx_advance = 0.42f;
        } else if (weather_arg == "storm") {
            reg_atmo.set_regime(WindRegime::CONVECTIVE_STORM);
            wx_advance = 0.90f;
        } else if (weather_arg == "icing") {
            reg_atmo.set_regime(WindRegime::WINTER_ICING);
            wx_advance = 0.80f;
            if (season_override < 0) season_override = 0;
        } else {
            std::fprintf(stderr, "[WEATHER] --weather expects auto, thermal, cierzo, foehn, fog, storm, or icing\n");
            return 2;
        }
    }
    reg_atmo.set_season(season_override);
    profiler.startup_stage("weather_init");
    if (!cockpit_qa && !objects.init(&pack, root + "/game/assets", root + "/sim_engine/shaders", packs_dir)) return 1;
    if (!cockpit_qa && !living_traffic.init(&pack, root + "/sim_engine/shaders")) {
        std::fprintf(stderr, "[LIVING-TRAFFIC] Warning: aerial/maritime traffic failed to initialize.\n");
    }
    if (!cockpit_qa && topdown_dir.empty() && !vegetation.init(root + "/game/assets", root + "/sim_engine/shaders")) {
        std::fprintf(stderr, "[VEG] Warning: volumetric airfield vegetation failed to initialize.\n");
    }
    profiler.startup_stage("world_resources");
    if (!ac_hud.init(&pack, root + "/game/assets", aircraft_package, root + "/sim_engine/shaders")) return 1;
    ac_hud.set_graphics_settings(graphics_settings);
    profiler.startup_stage("aircraft_resources");
    const bool interactive = interactive_run;
    audio.init(interactive && !no_audio);
    // An empty path gives scripted runs an in-memory profile with no read/write access.
    if ((interactive || !airport_qa_dir.empty() || !world_qa_png.empty()) &&
        !frontend.init(pack, root + "/sim_engine/shaders", root + "/game/assets", profile_file,
                       aircraft_package)) return 1;
    auto show_loading = [&](const char *title, const char *stage, float progress01) {
        if (!interactive) return;
        frontend.draw_loading_screen(ac_hud, win.width(), win.height(), title, stage, progress01);
        win.swap_buffers();
        win.poll_events();
    };
    show_loading("OpenSky VFR - Starting Up", "Initializing flight & atmosphere...", 0.25f);
    if (!autotest_dir.empty() || !topdown_dir.empty() || !airport_qa_dir.empty() || quick_profile || !replay_flight_path.empty()) {
        ac_hud.state().menu_open = false;
        ac_hud.state().paused = false;
    }

    if (!world_qa_png.empty()) {
        if (!start_ap) return 1;
        const FlightSimState before = ac_hud.state();
        const auto *parked = frontend.parked_airport();
        frontend.focus_map(*start_ap);
        const bool captured = frontend.capture_world(ac_hud, sky_clouds, win, tod_slot, &reg_atmo, world_qa_png);
        const auto &after = ac_hud.state();
        const bool stayed_put = before.lat_deg == after.lat_deg && before.lon_deg == after.lon_deg &&
            before.alt_msl_m == after.alt_msl_m && parked == frontend.parked_airport();
        if (!stayed_put) std::fprintf(stderr, "[WORLD] Map browsing moved the aircraft\n");
        return captured && stayed_put ? 0 : 1;
    }
    if (start_ap) {
        reg_atmo.sync_region(start_ap->lat, start_ap->lon);
        if (!interactive) ac_hud.teleport_to_airport(*start_ap, 1200.0, 145.0);
    } else if (!req_region.empty()) {
        const auto *reg = pack.find_region_by_id(req_region);
        const double lat = 0.5 * (reg->hdr.lat_min + reg->hdr.lat_max);
        const double lon = 0.5 * (reg->hdr.lon_min + reg->hdr.lon_max);
        reg_atmo.sync_region(lat, lon);
        ac_hud.reset_flight(lat, lon, pack.sample_elevation(lat, lon) + 1200.0, 0.0, 80.0, false);
        std::printf("[OPENSKY] Region '%s' has no runway; starting at its center.\n", req_region.c_str());
    }
    profiler.startup_stage("initialize_flight");
    const auto &spawn = ac_hud.state();
    const WeatherSnapshot *baked0 = reg_atmo.has_weather() ? &reg_atmo.weather() : nullptr;
    sky_clouds.update_solar_and_weather(spawn.lat_deg, spawn.lon_deg, spawn.ground_m,
                                      tod_slot, 0.0, 1.0f, wx_advance, baked0, season_override, 0.0);
    show_loading("OpenSky VFR - Starting Up", "Streaming coarse terrain horizon...", 0.55f);
    streamer.prewarm_coarse(sky_clouds.atmo().sun_dir);
    show_loading("OpenSky VFR - Starting Up", "Ready for flight.", 1.0f);
    Vec3 last_stream_sun = sky_clouds.atmo().sun_dir;
    profiler.startup_stage("terrain_coarse_prewarm");

    double sim_time = 0.0;
    uint64_t frame_idx = 1;
    struct AsyncPrepareResult {
        std::shared_ptr<const PackReader::WorldFrame> frame;
        WorldObjects::PreparedReanchor world_objs;
    };
    struct AsyncCrossingJob {
        bool active = false;
        bool stage2_streaming = false;
        double start_wall_t = 0.0;
        std::future<AsyncPrepareResult> cpu_future;
        AsyncPrepareResult prepared;
    } async_cross;
    auto bbox_contains_region = [](const PackReader::LoadedRegion &r, double lat, double lon) {
        return lat >= r.hdr.lat_min && lat <= r.hdr.lat_max &&
               lon >= r.hdr.lon_min && lon <= r.hdr.lon_max;
    };
    auto cancel_async_crossing = [&]() {
        if (async_cross.active) {
            if (async_cross.cpu_future.valid()) async_cross.cpu_future.wait();
            async_cross.cpu_future = {};
            async_cross.prepared = {};
            async_cross.active = false;
            async_cross.stage2_streaming = false;
        }
        streamer.cancel_async_transition();
    };
    auto transition_world = [&](double lat, double lon, const char *loading_title = nullptr) {
        cancel_async_crossing();
        if (loading_title) show_loading(loading_title, "Preparing world coordinates...", 0.20f);
        const bool changed = pack.reanchor_to(lat, lon);
        if (changed) {
            vegetation.invalidate();
            if (loading_title) show_loading(loading_title, "Streaming coarse terrain & IGN PNOA...", 0.50f);
            streamer.invalidate_all_and_prewarm(sky_clouds.atmo().sun_dir);
            if (loading_title) show_loading(loading_title, "Loading local buildings, roads & traffic...", 0.78f);
            objects.on_reanchor();
            living_traffic.on_reanchor();
            sky_clouds.reset_temporal_history();
        }
        reg_atmo.sync_region(lat, lon);
        reg_atmo.update_field(sky_clouds.atmo().sun_dir, sim_time, false, wx_advance);
        return changed;
    };

    auto step_flight_world_transition = [&](double lat, double lon, double vel_e = 0.0, double vel_n = 0.0) -> bool {
        if (async_cross.active && async_cross.stage2_streaming) {
            const double elapsed = PlatformWindow::now_sec() - async_cross.start_wall_t;
            if (!streamer.is_async_transition_ready(elapsed)) return false;
            if (async_cross.prepared.frame) {
                pack.commit_world_frame(async_cross.prepared.frame);
                streamer.commit_async_transition();
                vegetation.invalidate();
                objects.commit_reanchor(std::move(async_cross.prepared.world_objs));
                living_traffic.on_reanchor();
                reg_atmo.sync_region(pack.anchor_lat(), pack.anchor_lon());
                reg_atmo.update_field(sky_clouds.atmo().sun_dir, sim_time, false, wx_advance);
            } else {
                streamer.cancel_async_transition();
            }
            async_cross.prepared = {};
            async_cross.stage2_streaming = false;
            async_cross.active = false;
            return true;
        }
        if (async_cross.active && !async_cross.stage2_streaming) {
            if (async_cross.cpu_future.valid() &&
                async_cross.cpu_future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
                async_cross.prepared = async_cross.cpu_future.get();
                async_cross.cpu_future = {};
                if (async_cross.prepared.frame) {
                    async_cross.stage2_streaming = true;
                    streamer.begin_async_transition(async_cross.prepared.frame, sky_clouds.atmo().sun_dir,
                                                    lat, lon, vel_e, vel_n);
                } else {
                    async_cross.active = false;
                }
            }
            return false;
        }
        const double anchor_drift_m = nspain::haversine_m(lat, lon, pack.anchor_lat(), pack.anchor_lon());
        const auto *flight_region = pack.find_region_at(lat, lon);
        const auto *anchor_region = pack.find_region_at(pack.anchor_lat(), pack.anchor_lon());
        const bool out_of_anchor_box = anchor_region && !bbox_contains_region(*anchor_region, lat, lon);
        const bool region_crossed = flight_region && flight_region != anchor_region && out_of_anchor_box;
        if (anchor_drift_m > 60000.0 || region_crossed) {
            async_cross.active = true;
            async_cross.stage2_streaming = false;
            async_cross.start_wall_t = PlatformWindow::now_sec();
            async_cross.cpu_future = std::async(std::launch::async, [&pack, &objects, lat, lon]() {
                AsyncPrepareResult res;
                res.frame = pack.prepare_world_frame(lat, lon, true);
                if (res.frame) res.world_objs = objects.prepare_reanchor_cpu(*res.frame);
                return res;
            });
        }
        return false;
    };

    double last_t = PlatformWindow::now_sec();
    const double cycle_clock_origin = last_t;
    double menu_clock_refresh = -1.0;
    std::vector<double> frame_hist(60, 16.6);
    int hist_pos = 0;
    double cur_fps = 60.0, cur_avg_ms = 16.6, cur_p95_ms = 16.6;

    auto regime_fog_mult = [&]() -> float {
        const int clarity = ac_hud.graphics_settings().atmosphere_clarity;
        const float clarity_mult = (clarity == 0) ? 0.85f : (clarity == 1 ? 1.15f : 1.55f);
        switch (reg_atmo.regime()) {
            case WindRegime::CIERZO_NW:        return 0.72f * clarity_mult;
            case WindRegime::PYRENEAN_THERMAL: return 1.00f * clarity_mult;
            case WindRegime::FOEHN_SUR:        return 1.35f * clarity_mult;
            case WindRegime::KATABATIC_DAWN:   return 1.85f * clarity_mult;
            case WindRegime::CONVECTIVE_STORM: return 1.55f * clarity_mult;
            case WindRegime::WINTER_ICING:     return 1.70f * clarity_mult;
            default:                           break;
        }
        return clarity_mult;
    };

    auto step_flight = [&](double dt) {
        const double start = profiler.cpu_start();
        ac_hud.step_inputs_and_physics(win, dt, sky_clouds.atmo(), reg_atmo, sim_time);
        profiler.cpu_end(FlightProfiler::InputPhysics, start);
    };
    bool first_presented = false;
    int thermal_pixel_cap = 72;
    double thermal_fps_cap = 60.0;
    double last_thermal_check_t = -10.0;
    struct RoutePlaybackUiState {
        bool active = false;
        bool playing = true;
        float speed = 1.0f;
        double cur_t = 0.0;
        double first_t = 0.0;
        double dur_t = 0.0;
        double seek_t = -1.0;
        bool saved_walking = false;
        bool saved_menu_open = true;
        double saved_sim_time = 0.0;
        FlightSimState saved_state{};
        RouteVideoRecording active_rec;
        GameFrontend::ReplayAction last_action = GameFrontend::ReplayAction::None;
    } route_play;
    const auto update_environment = [&] {
        const double weather_start = profiler.cpu_start();
        const bool walking = frontend.walking();
        const auto &walker = frontend.walker();
        const auto &aircraft = ac_hud.state();
        const double lat = walking ? walker.lat : aircraft.lat_deg;
        const double lon = walking ? walker.lon : aircraft.lon_deg;
        const double cam_ground = walking ? walker.alt - 1.68 : aircraft.ground_m;
        if ((frame_idx % 180) == 0) reg_atmo.sync_region(lat, lon);
        reg_atmo.update_field(sky_clouds.atmo().sun_dir, sim_time, false, wx_advance);
        const WeatherSnapshot *bakedw = reg_atmo.has_weather() ? &reg_atmo.weather() : nullptr;
        sky_clouds.update_solar_and_weather(lat, lon, cam_ground, tod_slot, sim_time,
                                            regime_fog_mult(), wx_advance, bakedw, season_override,
                                            std::max(0.0, PlatformWindow::now_sec() - cycle_clock_origin));
        streamer.set_sun_direction(sky_clouds.atmo().sun_dir);
        if (dot(sky_clouds.atmo().sun_dir, last_stream_sun) < 0.996f) {
            last_stream_sun = sky_clouds.atmo().sun_dir;
            streamer.invalidate_shadows();
        }
        profiler.cpu_end(FlightProfiler::Weather, weather_start);
    };

    auto render_one_frame = [&](bool draw_hud, int max_new_tree_cells = 1, const std::string &save_png = "", float fov_deg = 50.0f, bool replay_hud = false) {
        frame_idx++;
        profiler.start_frame(frame_idx, quality);
        double prepare_start = profiler.cpu_start();
        const FlightSimState &st = ac_hud.state();
        const GraphicsSettings &render_settings = ac_hud.graphics_settings();
        // Proactive closed-loop thermal + 60 FPS frame-time guard (<80 C ceiling on laptop APUs):
        // smoothly reduces internal resolution, worker burst, cloud rays, and frame ceiling under heat.
        double wall_now = PlatformWindow::now_sec();
        if (!promo_only && topdown_dir.empty() && airport_qa_dir.empty() && (wall_now - last_thermal_check_t) >= 0.5) {
            last_thermal_check_t = wall_now;
            const double temp_c = FlightProfiler::peak_package_temp_c();
            const double avg_ms = profiler.enabled() ? profiler.frame_avg_ms() : cur_avg_ms;
            if (temp_c >= 76.5) {
                thermal_pixel_cap = 40;
                streamer.set_thermal_sleep_ms(16);
                sky_clouds.set_thermal_sparse_clouds(true);
                thermal_fps_cap = 30.0;
            } else if (temp_c >= 73.5) {
                thermal_pixel_cap = std::max(44, thermal_pixel_cap - 6);
                streamer.set_thermal_sleep_ms(6);
                sky_clouds.set_thermal_sparse_clouds(true);
                thermal_fps_cap = 40.0;
            } else if (temp_c >= 69.5 || (avg_ms > 0.1 && avg_ms >= 17.5)) {
                thermal_pixel_cap = std::max(50, thermal_pixel_cap - 4);
                streamer.set_thermal_sleep_ms(0);
                sky_clouds.set_thermal_sparse_clouds(temp_c >= 71.0);
                thermal_fps_cap = (temp_c >= 71.0) ? 48.0 : 60.0;
            } else if ((temp_c <= 0.0 || temp_c <= 64.0) && (avg_ms <= 0.1 || avg_ms <= 14.5)) {
                thermal_pixel_cap = std::min(85, thermal_pixel_cap + 3);
                streamer.set_thermal_sleep_ms(0);
                sky_clouds.set_thermal_sparse_clouds(false);
                thermal_fps_cap = 60.0;
            }
        }
        const int eff_pixel_pct = promo_only ? render_settings.pixel_percent
            : ((topdown_dir.empty() && airport_qa_dir.empty())
                ? std::min(render_settings.pixel_percent, thermal_pixel_cap)
                : render_settings.pixel_percent);
        const bool walking = frontend.walking() && !route_play.active;
        const auto &walker = frontend.walker();
        const double lat = walking ? walker.lat : st.lat_deg;
        const double lon = walking ? walker.lon : st.lon_deg;
        const double cam_e = (lon - pack.anchor_lon()) * pack.anchor_m_lon();
        const double cam_n = (lat - pack.anchor_lat()) * pack.anchor_m_lat();
        const double cam_alt = walking ? walker.alt : st.alt_msl_m;
        const double cam_ground = walking ? walker.alt - 1.68 : st.ground_m;
        const double cam_agl = std::max(0.0, cam_alt - cam_ground);
        pack.set_online_camera(lat, lon);

        sky_clouds.resize_if_needed(output_w, output_h, quality,
                                    eff_pixel_pct, render_settings.aa_mode, render_settings.weather_detail);
        profiler.cpu_end(FlightProfiler::Prepare, prepare_start);
        const LocalAirSample &air_now = ac_hud.last_air();
        const Vec2 local_wind_en{static_cast<float>(air_now.wind_east_mps), static_cast<float>(air_now.wind_north_mps)};
        if ((frame_idx % 90 == 0) && pack.consume_online_arrived()) {
            streamer.invalidate_close_pages();
        }
        streamer.begin_frame(frame_idx);
        // Frame-time-budgeted upload cap; drains PBO staging bursts quickly without long frame stalls.
        const int max_up = streamer.has_async_transition() ? 10 : ((quality <= 1) ? 5 : 6);
        const double up_budget_ms = streamer.has_async_transition() ? 3.0 : ((quality == 0) ? 1.4 : (quality == 1 ? 1.8 : 2.2));
        profiler.begin(FlightProfiler::Upload);
        streamer.pump_gpu_uploads(static_cast<float>(sim_time), max_up, up_budget_ms);
        profiler.end(FlightProfiler::Upload);
        prepare_start = profiler.cpu_start();

        // Camera basis: Heading 0 = North (-Z), Heading 90 = East (+X)
        Mat4 ac_rot = Mat4::rotate_yxz(
            static_cast<float>(-st.heading_deg * DEG2RAD_D),
            static_cast<float>(st.pitch_deg * DEG2RAD_D),
            static_cast<float>(-st.roll_deg * DEG2RAD_D)
        );
        const Vec2 shake_rot = (!walking && st.view_mode == 0 && !st.paused) ? ac_hud.cockpit_shake_rot() : Vec2{0.0f, 0.0f};
        const Vec3 look_feel = (!walking && st.view_mode == 0 && !st.paused && render_settings.head_motion_mode > 0)
            ? ac_hud.cockpit_look_feel() : Vec3{0.0f, 0.0f, 0.0f};
        Mat4 look_rot = Mat4::rotate_yxz(
            (st.look_yaw_deg + look_feel.x) * DEG2RAD_F,
            (st.look_pitch_deg + shake_rot.x + look_feel.y) * DEG2RAD_F,
            (shake_rot.y + look_feel.z) * DEG2RAD_F
        );
        Mat4 cam_rot = ac_rot * look_rot;
        Vec3 cam_off{0.0f, 0.0f, 0.0f};
        if (walking) {
            cam_rot = Mat4::rotate_yxz(float(-walker.heading * DEG2RAD_D), float(walker.pitch * DEG2RAD_D), 0);
            fov_deg = 46.0f; // Natural human-eye perspective (~74 deg horizontal on 16:9, no wide-angle fisheye!)
        } else if (st.view_mode == 0) {
            if (std::abs(fov_deg - 50.0f) < 0.05f) {
                const float spd_breath = (!st.paused && render_settings.head_motion_mode > 0)
                    ? static_cast<float>(std::clamp((st.speed_mps - 25.0) / 45.0, 0.0, 1.0)) * 1.4f
                    : 0.0f;
                fov_deg = static_cast<float>(render_settings.cockpit_fov_deg) + spd_breath;
            }
            const Vec3 shk = st.paused ? Vec3{0.0f, 0.0f, 0.0f} : ac_hud.cockpit_shake_pos();
            Vec4 co = ac_rot.mul({st.seat_lat_m + shk.x, COCKPIT_EYE_HEIGHT_M + st.seat_height_m + shk.y, -st.seat_fore_m + shk.z, 0.0f});
            cam_off = {co.x, co.y, co.z};
        } else if (st.view_mode == 1) {
            Vec4 co = cam_rot.mul({0.0f, 1.65f, st.chase_dist_m, 0.0f});
            cam_off = {co.x, co.y, co.z};
        } else if (st.view_mode == 2 && topdown_dir.empty()) {
            Vec4 co = ac_rot.mul({st.free_cam_off_m.x, st.free_cam_off_m.y, st.free_cam_off_m.z, 0.0f});
            cam_off = {co.x, co.y, co.z};
        }
        Mat4 cam_world = Mat4::translate(cam_off) * cam_rot;
        Mat4 view = cam_world.inverse();
        const float aspect = static_cast<float>(output_w) / output_h;
        Mat4 proj = Mat4::perspective_reverse_z(fov_deg * DEG2RAD_F, aspect, walking ? 0.075f : (st.view_mode == 1 ? 0.25f : 0.12f));
        const Mat4 native_cockpit_vp = proj * view;
        // Cover the internal scene pixel footprint over eight frames for TAA modes.
        if (render_settings.aa_mode > 0 && sky_clouds.scene_w() > 0 && sky_clouds.scene_h() > 0) {
            auto halton = [](uint64_t n, uint32_t base) {
                float f = 1.0f, value = 0.0f;
                while (n > 0) {
                    f /= static_cast<float>(base);
                    value += f * static_cast<float>(n % base);
                    n /= base;
                }
                return value;
            };
            uint64_t sample = ((frame_idx - 1) % 8) + 1;
            float jx = (halton(sample, 2) - 0.5f) * 2.0f / static_cast<float>(sky_clouds.scene_w());
            float jy = (halton(sample, 3) - 0.5f) * 2.0f / static_cast<float>(sky_clouds.scene_h());
            proj.m[8] -= jx;
            proj.m[9] -= jy;
        }
        Mat4 vp = proj * view;
        Mat4 inv_vp = vp.inverse();
        Frustum frustum = Frustum::from_vp_reverse_z(vp);

        profiler.cpu_end(FlightProfiler::Prepare, prepare_start);

        // 1. Begin Reverse-Z Scene Pass + Rayleigh/Mie Sky
        const double t_p0 = PlatformWindow::now_sec();
        profiler.begin(FlightProfiler::Sky);
        sky_clouds.begin_scene_pass(inv_vp);
        profiler.end(FlightProfiler::Sky);
        const double t_p1 = PlatformWindow::now_sec();

        // 2. Smooth Continuous CDLOD Quadtree Terrain + 16-Pattern Array + Cliffs + IBL Reflections + Peak Fog + RT Night!
        profiler.begin(FlightProfiler::Terrain);
        TerrainGrassZone grass_zone{};
        double nearest_runway_m = 1200.0;
        auto consider_grass_runway = [&](const AirportRunway &rw) {
            const double ax = (rw.lon_a - pack.anchor_lon()) * pack.anchor_m_lon();
            const double an = (rw.lat_a - pack.anchor_lat()) * pack.anchor_m_lat();
            const double bx = (rw.lon_b - pack.anchor_lon()) * pack.anchor_m_lon();
            const double bn = (rw.lat_b - pack.anchor_lat()) * pack.anchor_m_lat();
            const double de = bx - ax, dn = bn - an;
            const double len = std::hypot(de, dn);
            if (len < 100.0) return;
            const double axis_e = de / len, axis_n = dn / len;
            const double t = std::clamp(((cam_e - ax) * axis_e + (cam_n - an) * axis_n) / len, 0.0, 1.0);
            const double dist = std::hypot(cam_e - (ax + de * t), cam_n - (an + dn * t));
            if (dist >= nearest_runway_m) return;
            nearest_runway_m = dist;
            grass_zone.center_en = {static_cast<float>((ax + bx) * 0.5), static_cast<float>((an + bn) * 0.5)};
            grass_zone.axis_en = {static_cast<float>(axis_e), static_cast<float>(axis_n)};
            grass_zone.half_length_m = static_cast<float>(len * 0.5 + 70.0);
            grass_zone.runway_half_width_m = static_cast<float>(rw.width_m * 0.5);
            grass_zone.outer_half_width_m = static_cast<float>(std::max(45.0, rw.width_m * 0.5 + 78.0));
            std::string surface = rw.surface;
            for (char &ch : surface) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            grass_zone.grass_runway = surface.find("grass") != std::string::npos || surface.find("turf") != std::string::npos;
        };
        for (const AirportInfo &airport : pack.airports()) {
            if (airport.runways.empty()) {
                consider_grass_runway({airport.rwy_lat_a, airport.rwy_lon_a, airport.rwy_elev_a,
                                       airport.rwy_lat_b, airport.rwy_lon_b, airport.rwy_elev_b,
                                       airport.rwy_width_m, airport.surface});
            } else {
                for (const AirportRunway &rw : airport.runways) consider_grass_runway(rw);
            }
        }
        if (nearest_runway_m >= 1200.0) grass_zone = {};
        const Vec4 terrain_wx_env{
            ac_hud.ground_wetness(),
            ac_hud.last_air().snow_factor,
            ac_hud.last_air().lightning_flash,
            std::max(reg_atmo.weather().storm_int, sky_clouds.atmo().precip)
        };
        terrain.update_and_render(
            streamer, vp, frustum,
            cam_e, cam_n, cam_alt, cam_agl,
            walking ? 0 : st.vel_east_mps, walking ? 0 : st.vel_north_mps,
            sky_clouds.atmo().sun_dir,
            sky_clouds.atmo().haze_cool, sky_clouds.atmo().haze_warm,
            sky_clouds.atmo().haze_density, sky_clouds.atmo().cloud_cov,
            static_cast<float>(sim_time), local_wind_en, sky_clouds.refl_env_tex(), quality,
            reg_atmo.weather().season, reg_atmo.weather().snowline_m,
            render_settings.terrain_lod_percent, grass_zone, terrain_wx_env
        );
        profiler.end(FlightProfiler::Terrain);
        const double t_p2 = PlatformWindow::now_sec();

        // 3. Static world geometry (settlements, hangars, roads & volumetric airfield trees) BEFORE cloud depth capture
        // so high-altitude airfields & city towers properly occlude clouds behind them, while moving aircraft/ships
        // draw after the copy so moving silhouettes never leave holes in cloud temporal history.
        profiler.begin(FlightProfiler::World);
        if (!cockpit_qa) {
            objects.update_and_render(
                vp, frustum, cam_e, cam_n, cam_alt,
                sky_clouds.atmo(), static_cast<float>(sim_time), local_wind_en, sky_clouds.refl_env_tex(), quality, max_new_tree_cells
            );
            if (topdown_dir.empty()) {
                vegetation.update_around(pack, lat, lon, static_cast<float>(cam_agl));
                vegetation.draw(
                    vp, Vec3{static_cast<float>(cam_e), static_cast<float>(cam_alt), static_cast<float>(cam_n)},
                    sky_clouds.atmo(), static_cast<float>(cam_agl), static_cast<float>(sim_time),
                    local_wind_en, reg_atmo.weather().season, reg_atmo.weather().snowline_m
                );
            }
        }
        profiler.end(FlightProfiler::World);
        const double t_p3 = PlatformWindow::now_sec();

        profiler.begin(FlightProfiler::CloudDepth);
        sky_clouds.capture_cloud_occlusion_depth();
        profiler.end(FlightProfiler::CloudDepth);
        const double t_p4 = PlatformWindow::now_sec();

        // 4. Living AI aircraft/maritime traffic + C172S interior/exterior GLBs (after CloudDepth so moving silhouettes never ghost in cloud history).
        profiler.begin(FlightProfiler::Aircraft);
        if (!cockpit_qa) {
            living_traffic.update_and_render(vp, cam_e, cam_n, cam_alt, sky_clouds.atmo(), static_cast<float>(sim_time), quality);
        }
        const bool native_cockpit = !walking && st.view_mode == 0 && render_settings.cockpit_native;
        if (walking) {
            const auto &p = frontend.parking_spot();
            const Vec3 offset{float((p.lon - lon) * pack.anchor_m_lon()), float(p.alt - cam_alt),
                              float((lat - p.lat) * pack.anchor_m_lat())};
            ac_hud.sync_walkaround_damage(frontend.care());
            ac_hud.render_parked_aircraft(vp, offset, float(p.heading), sky_clouds.atmo(), sky_clouds.refl_env_tex());
        } else if (!native_cockpit)
            ac_hud.render_aircraft_3d(vp, cam_off, sky_clouds.atmo(), sky_clouds.refl_env_tex(), static_cast<float>(sim_time));
        // 4b. Lightweight 3D exterior precipitation streaks (rain/snow past cockpit; 0 cost when dry!)
        if (topdown_dir.empty()) {
            ac_hud.render_precip_streaks(
                vp, Vec3{static_cast<float>(cam_e), static_cast<float>(cam_alt), static_cast<float>(-cam_n)},
                Vec3{cam_rot.m[0], cam_rot.m[1], cam_rot.m[2]},
                Vec3{cam_rot.m[4], cam_rot.m[5], cam_rot.m[6]},
                sky_clouds.atmo(), static_cast<float>(sim_time), walking);
        }
        profiler.end(FlightProfiler::Aircraft);
        const double t_p5 = PlatformWindow::now_sec();

        // 5. Quarter-Res Temporally Reprojected Physics-Coupled 3D Volumetric Clouds + Valley Fog + Rain Shafts + AgX/LUT Post
        profiler.begin(FlightProfiler::CloudsPost);
        const float valley_fog = (reg_atmo.regime() == WindRegime::KATABATIC_DAWN) ? 0.92f
            : ((reg_atmo.regime() == WindRegime::WINTER_ICING || reg_atmo.regime() == WindRegime::CONVECTIVE_STORM) ? 0.34f
            : (air_now.relative_humidity > 0.86 ? static_cast<float>((air_now.relative_humidity - 0.86) * 4.0) : 0.0f));
        const Vec4 cloud_wx_fx{valley_fog, air_now.snow_factor, air_now.lightning_flash, reg_atmo.weather().storm_int};
        sky_clouds.render_clouds_and_post(
            vp, inv_vp, Vec3{cam_rot.m[8], cam_rot.m[9], cam_rot.m[10]}, cam_e, cam_n, cam_alt,
            terrain.cloud_noise_tex(), terrain.cloud_vol_tex(), terrain.cloud_detail_tex(), terrain.lut_tex(),
            reg_atmo.field_tex(), reg_atmo.terrain_field_tex(), reg_atmo.domain_bounds(),
            static_cast<float>(sim_time), static_cast<int>(frame_idx), cloud_wx_fx
        );
        profiler.end(FlightProfiler::CloudsPost);

        // The cockpit bypasses scene downscaling and TAA jitter. It is rendered
        // at output resolution over the reconstructed terrain/sky, with a small
        // reverse-Z depth attachment and the same AgX/Spain LUT transform.
        if (native_cockpit) {
            profiler.begin(FlightProfiler::NativeCockpit);
            sky_clouds.begin_native_cockpit_pass();
            ac_hud.render_aircraft_3d(native_cockpit_vp, cam_off, sky_clouds.atmo(),
                                      sky_clouds.refl_env_tex(), static_cast<float>(sim_time),
                                      true, terrain.lut_tex(), sky_clouds.world_hdr_tex());
            glDepthMask(GL_TRUE);
            glDisable(GL_DEPTH_TEST);
            profiler.end(FlightProfiler::NativeCockpit);
        }

        // 6. Crisp Aviation HUD & Live Streaming + Mesoscale Wind Profiler
        profiler.begin(FlightProfiler::Hud);
        const StreamStats stream = draw_hud || profiler.enabled() ? streamer.stats() : StreamStats{};
        if (draw_hud && walking) frontend.draw_walk(ac_hud, win, &reg_atmo, &sky_clouds.atmo(), sim_time);
        else if (draw_hud) {
            const bool debug_active = profiler.overlay() || weather_debug_level >= 0 || physics_debug;
            const DebugOverlayView debug{profiler.overlay() ? &profiler : nullptr, weather_debug_level, physics_debug,
                                         vp, ac_rot, cam_e, cam_n};
            ac_hud.render_hud(
                output_w, output_h, sky_clouds.atmo(), reg_atmo,
                stream, terrain.last_stats(),
                quality, objects.total_buildings(),
                cur_fps, cur_avg_ms, cur_p95_ms,
                (reg_atmo.has_weather() && !reg_atmo.manual_weather_override()) ? &reg_atmo.weather() : nullptr,
                reg_atmo.regime_name(), reg_atmo.has_library() ? reg_atmo.library().provenance() : nullptr,
                show_diagnostics, debug_active ? &debug : nullptr
            );
            frontend.draw_navigation(ac_hud, win);
        } else if (replay_hud && route_play.active) {
            route_play.last_action = frontend.draw_replay_bar(
                ac_hud, win, route_play.active_rec.title,
                route_play.cur_t, route_play.dur_t,
                route_play.playing, route_play.speed, ac_hud.state().view_mode,
                &route_play.seek_t);
        }
        profiler.end(FlightProfiler::Hud);
        const double t_p6 = PlatformWindow::now_sec();
        if ((t_p6 - t_p0) * 1000.0 > 45.0) {
            std::printf("[SLOW-FRAME F%llu] total=%.1fms (sky=%.1f terr=%.1f world=%.1f cdepth=%.1f air+tr=%.1f cld+ckpt+hud=%.1f)\n",
                        static_cast<unsigned long long>(frame_idx), (t_p6 - t_p0) * 1000.0,
                        (t_p1 - t_p0) * 1000.0, (t_p2 - t_p1) * 1000.0, (t_p3 - t_p2) * 1000.0,
                        (t_p4 - t_p3) * 1000.0, (t_p5 - t_p4) * 1000.0, (t_p6 - t_p5) * 1000.0);
        }
        if (!save_png.empty()) {
            if (!PlatformWindow::save_framebuffer_png(save_png, output_w, output_h))
                std::fprintf(stderr, "[CAPTURE] Could not save %s\n", save_png.c_str());
        }
        const double present_start = profiler.cpu_start();
        const PixelRect viewport = win.presentation_rect(output_w, output_h);
        sky_clouds.present(viewport.x, viewport.y, viewport.w, viewport.h, win.width(), win.height());
        win.swap_buffers();
        profiler.cpu_end(FlightProfiler::Present, present_start);
        if (profiler.enabled()) {
            profiler.end_frame(stream, reg_atmo.work_stats(), pack, terrain.last_stats().max_active_lod,
                               sky_clouds.scene_w(), sky_clouds.scene_h());
        }
        if (!first_presented) {
            profiler.startup_stage("first_present");
            first_presented = true;
        }
    };

    // Captures can intentionally advance the world clock without accepting player input.
    const auto capture_frame = [&](double elapsed, bool draw_hud, int tree_budget = 1,
                                   const std::string &png = "", float fov = 50.0f) {
        sim_time += elapsed;
        update_environment();
        render_one_frame(draw_hud, tree_budget, png, fov);
    };

    if (quick_profile) {
        tod_slot = (weather_arg == "fog") ? 0 : 2;
        const auto route = [&](const char *name, double lat, double lon, double agl, double heading) {
            streamer.clear_pending_requests();
            ac_hud.reset_flight(lat, lon, pack.sample_elevation(lat, lon) + agl, heading, 100.0, false);
            sky_clouds.reset_temporal_history();
            profiler.set_scene(std::string(name) + "_warmup");
            for (int i = 0; i < 30 && !win.should_close(); ++i) {
                win.poll_events();
                step_flight(1.0 / 60.0);
                capture_frame(1.0 / 60.0, true);
            }
            profiler.set_scene(name);
            for (int i = 0; i < 240 && !win.should_close(); ++i) {
                double f0 = PlatformWindow::now_sec();
                win.poll_events();
                step_flight(1.0 / 60.0);
                capture_frame(1.0 / 60.0, true);
                double spare = (1.0 / 60.0) - (PlatformWindow::now_sec() - f0);
                if (spare > 0.001) std::this_thread::sleep_for(std::chrono::duration<double>(spare));
            }
            if (!win.should_close()) {
                profiler.set_scene(std::string(name) + "_capture");
                capture_frame(1.0 / 60.0, debug_overlays, 1, profile_dir + "/" + name + ".png");
            }
        };
        if (req_region == "es_catalonia_coast") {
            auto reanchor_route = [&](const char *name, double lat, double lon, double agl, double heading) {
                transition_world(lat, lon);
                route(name, lat, lon, agl, heading);
            };
            reanchor_route("barcelona_downtown", 41.3680, 2.1550, 290.0, 38.0);
            // Also grab an external chase shot over Eixample / Barcelona skyline for visual inspection.
            if (!win.should_close()) {
                ac_hud.state().view_mode = 1;
                ac_hud.state().look_yaw_deg = -18.0f;
                ac_hud.state().look_pitch_deg = -14.0f;
                sky_clouds.reset_temporal_history();
                for (int f = 0; f < 4; ++f) {
                    win.poll_events();
                    capture_frame(1.0 / 60.0, false, 1, (f == 3) ? (profile_dir + "/barcelona_chase.png") : "");
                }
                ac_hud.state().view_mode = 0;
                ac_hud.state().look_yaw_deg = 0.0f;
                ac_hud.state().look_pitch_deg = 0.0f;
            }
            reanchor_route("valles_sabadell", 41.5220, 2.1050, 250.0, 175.0);
            reanchor_route("girona_city", 41.9680, 2.8120, 240.0, 52.0);
        } else {
            route("pyrenees", 42.53131, 0.47676, 120.0, 160.0);
            route("zaragoza", 41.6420, -0.8920, 260.0, 28.0);
        }
        if (debug_overlays && !win.should_close()) {
            profiler.set_scene("debug_chase");
            ac_hud.state().view_mode = 1;
            ac_hud.state().look_yaw_deg = -32.0f;
            ac_hud.state().look_pitch_deg = -8.0f;
            weather_debug_level = 0;
            sky_clouds.reset_temporal_history();
            capture_frame(1.0 / 60.0, true, 1, profile_dir + "/debug_chase.png");
        }
        profiler.finish();
        std::printf("[PROFILE] Capture complete -> %s\n", profile_dir.c_str());
        return 0;
    }

    // Autonomous All-Region Visual + Real JSBSim Assisted Takeoff/Departure & Landing Verification
    if (!region_test_dir.empty()) {
        std::filesystem::create_directories(region_test_dir);
        pack.set_online_provider(OnlineOrthoProvider::OFF);
        tod_slot = 2; // Crisp morning/midday VFR light
        wx_advance = 0.28f;
        ac_hud.state().menu_open = false;
        ac_hud.state().paused = false;
        ac_hud.state().parking_brake = false;
        ac_hud.systems().reset_to_running_default();
        ac_hud.systems().set_flaps_target_deg(0.0);

        auto wait_tiles = [&](int frames) {
            sky_clouds.reset_temporal_history();
            for (int f = 0; f < frames && !win.should_close(); ++f) {
                double f0 = PlatformWindow::now_sec();
                win.poll_events();
                capture_frame(1.0 / 60.0, false, 64);
                streamer.flush_pending_sync(16, 0.25);
                double sp = (1.0 / 60.0) - (PlatformWindow::now_sec() - f0);
                if (sp > 0.001) std::this_thread::sleep_for(std::chrono::duration<double>(sp));
            }
        };

        // 1. REAL TAKEOFF & DEPARTURE from Castejon de Sos (ES-0205 RWY 16 Grass Strip, open valley departure)
        if (const AirportInfo *es = pack.find_airport("ES-0205")) {
            const double dn = (es->rwy_lat_a - es->rwy_lat_b) * 111132.0;
            const double de = (es->rwy_lon_a - es->rwy_lon_b) * 111320.0 * std::cos(es->rwy_lat_b * DEG2RAD_D);
            const double rwy_hdg = std::fmod(std::atan2(de, dn) * RAD2DEG_D + 360.0, 360.0);
            const double start_lat = es->rwy_lat_b + (es->rwy_lat_a - es->rwy_lat_b) * 0.08;
            const double start_lon = es->rwy_lon_b + (es->rwy_lon_a - es->rwy_lon_b) * 0.08;
            ac_hud.state().view_mode = 0;
            ac_hud.state().look_yaw_deg = 0.0f; ac_hud.state().look_pitch_deg = -2.0f;
            ac_hud.reset_flight(start_lat, start_lon, es->rwy_elev_b + 1.0, rwy_hdg, 0.0, true);
            ac_hud.state().paused = false; ac_hud.state().parking_brake = false;
            wait_tiles(16);
            capture_frame(1.0 / 60.0, true, 1, region_test_dir + "/01_castejon_grass_lineup.png");

            double liftoff_ias = 0.0, roll_dist_m = 0.0, max_hdg_dev = 0.0, max_climb_fpm = 0.0;
            bool lifted = false;
            bool captured_climb = false;
            constexpr double dt = 1.0 / 60.0;
            for (int step = 0; step < 34 * 60 && !win.should_close(); ++step) {
                sim_time += dt;
                const auto &fs = ac_hud.jsb_state();
                // Rotate at 53 kt and hold Vy climb attitude (~+7.5 deg) -> steady 650-850 fpm departure!
                double pitch_cmd = (fs.on_ground && fs.ias_kt < 53.0) ? 0.0
                                   : ((fs.ias_kt < 66.0) ? 0.56 : 0.68);
                ac_hud.step_autonomous_assist(dt, 0.0, pitch_cmd, 1.0, 0.0, sky_clouds.atmo(), reg_atmo, sim_time);
                const auto &ns = ac_hud.jsb_state();
                const double dev = std::abs(std::remainder(ns.heading_deg - rwy_hdg, 360.0));
                if (ns.on_ground && dev > max_hdg_dev) max_hdg_dev = dev;
                if (!lifted && !ns.on_ground && ns.agl_m > 1.5) {
                    lifted = true;
                    liftoff_ias = ns.ias_kt;
                    roll_dist_m = nspain::haversine_m(start_lat, start_lon, ns.lat_deg, ns.lon_deg);
                }
                if (lifted) max_climb_fpm = std::max(max_climb_fpm, ns.vsi_mps * 196.85);
                if (step % 2 == 0) {
                    double f0 = PlatformWindow::now_sec();
                    win.poll_events();
                    update_environment();
                    render_one_frame(true, 4);
                    double sp = (1.0 / 60.0) - (PlatformWindow::now_sec() - f0);
                    if (sp > 0.001) std::this_thread::sleep_for(std::chrono::duration<double>(sp));
                }
                if (lifted && !captured_climb && ns.agl_m > 65.0) {
                    captured_climb = true;
                    update_environment();
                    render_one_frame(true, 1, region_test_dir + "/02_castejon_departure_climb.png");
                    break;
                }
            }
            std::printf("[FLIGHT-DEMO TAKEOFF ES-0205] lifted=%d roll=%.0fm liftoff=%.1fkt max_hdg_dev=%.1fdeg max_climb=%.0ffpm final_agl=%.1fm\n",
                        lifted ? 1 : 0, roll_dist_m, liftoff_ias, max_hdg_dev, max_climb_fpm, ac_hud.jsb_state().agl_m);
        }

        // 2. ALL-REGION VISUAL INSPECTION TOUR across Northern Spain (Aragon Pack)
        auto tour_snap = [&](const char *fname, double lat, double lon, double agl, double hdg) {
            const double d = nspain::haversine_m(lat, lon, pack.anchor_lat(), pack.anchor_lon());
            if (d <= 55000.0 || !transition_world(lat, lon)) streamer.clear_pending_requests();
            ac_hud.state().view_mode = 0; ac_hud.state().look_yaw_deg = 0.0f; ac_hud.state().look_pitch_deg = -4.0f;
            ac_hud.reset_flight(lat, lon, pack.sample_elevation(lat, lon) + agl, hdg, 100.0, false);
            ac_hud.state().paused = true;
            wait_tiles(16);
            capture_frame(1.0 / 60.0, true, 1, region_test_dir + "/" + fname);
            std::printf("[FLIGHT-DEMO REGION] Captured %s (%.4f, %.4f @ %.0fm AGL)\n", fname, lat, lon, agl);
        };
        tour_snap("03_mediano_reservoir.png", 42.3120,  0.1940, 180.0, 15.0);
        tour_snap("04_huesca_lehc.png",       42.0810, -0.3234, 155.0, 310.0);
        tour_snap("05_zaragoza_city.png",     41.6420, -0.8920, 240.0, 28.0);

        // 3. REAL APPROACH, FLARE & LANDING onto Santa Cilia (LECI paved runway)
        if (const AirportInfo *leci = pack.find_airport("LECI")) {
            if (nspain::haversine_m(leci->lat, leci->lon, pack.anchor_lat(), pack.anchor_lon()) <= 45000.0 ||
                !transition_world(leci->lat, leci->lon)) streamer.clear_pending_requests();
            const double dn = (leci->rwy_lat_b - leci->rwy_lat_a) * 111132.0;
            const double de = (leci->rwy_lon_b - leci->rwy_lon_a) * 111320.0 * std::cos(leci->rwy_lat_a * DEG2RAD_D);
            const double rwy_hdg = std::fmod(std::atan2(de, dn) * RAD2DEG_D + 360.0, 360.0);
            const double rwy_rad = rwy_hdg * DEG2RAD_D;
            const double init_dist = 920.0;
            const double init_lat = leci->rwy_lat_a - std::cos(rwy_rad) * init_dist / 111132.0;
            const double init_lon = leci->rwy_lon_a - std::sin(rwy_rad) * init_dist / (111320.0 * std::cos(leci->rwy_lat_a * DEG2RAD_D));
            ac_hud.state().view_mode = 0; ac_hud.state().look_yaw_deg = 0.0f; ac_hud.state().look_pitch_deg = -2.0f;
            ac_hud.systems().set_flaps_immediate_deg(20.0);
            ac_hud.reset_flight(init_lat, init_lon, leci->rwy_elev_a + 64.0, rwy_hdg, 58.0, false);
            ac_hud.state().paused = false; ac_hud.state().parking_brake = false;
            wait_tiles(12);

            bool captured_final = false, touched = false;
            double td_fpm = 0.0, td_ias = 0.0, td_g = 1.0, td_lat = 0.0, td_lon = 0.0, rollout_m = 0.0;
            double prev_vsi = 0.0;
            constexpr double dt = 1.0 / 60.0;
            for (int step = 0; step < 52 * 60 && !win.should_close(); ++step) {
                sim_time += dt;
                const auto &fs = ac_hud.jsb_state();
                const double d_thr = nspain::haversine_m(fs.lat_deg, fs.lon_deg, leci->rwy_lat_a, leci->rwy_lon_a);
                const double rel_n = (fs.lat_deg - leci->rwy_lat_a) * 111132.0;
                const double rel_e = (fs.lon_deg - leci->rwy_lon_a) * 111320.0 * std::cos(leci->rwy_lat_a * DEG2RAD_D);
                const double along = rel_e * std::sin(rwy_rad) + rel_n * std::cos(rwy_rad);
                const double xtrack = rel_e * std::cos(rwy_rad) - rel_n * std::sin(rwy_rad);
                double roll_cmd = 0.0, pitch_cmd = 0.0, thr_cmd = 0.10, brk_cmd = 0.0;
                if (!touched) {
                    const double aim_hdg = rwy_hdg - std::atan2(xtrack, 240.0) * RAD2DEG_D;
                    const double h_err = std::remainder(aim_hdg - fs.heading_deg, 360.0);
                    roll_cmd = std::clamp(h_err / 24.0, -0.55, 0.55);
                    const double rwy_frac = std::clamp(along / 846.0, 0.0, 1.0);
                    const double rwy_msl_here = leci->rwy_elev_a * (1.0 - rwy_frac) + leci->rwy_elev_b * rwy_frac + 0.35;
                    const double h_rwy = fs.alt_msl_m - rwy_msl_here;
                    if (along < 40.0 || h_rwy > 5.5) {
                        // Reference glideslope to runway MSL (not pre-threshold valley DEM!), aiming 220 m past threshold!
                        const double target_h = std::max(1.5, (220.0 - along) * 0.058);
                        const double h_err_m = target_h - h_rwy;
                        const double tgt_vsi = std::clamp(-2.1 + h_err_m * 0.16, -3.8, -0.3);
                        pitch_cmd = std::clamp((tgt_vsi - fs.vsi_mps) * 0.24 - 0.12, -0.50, 0.28);
                        thr_cmd = (along > 30.0 && h_rwy < 10.0) ? 0.0 : std::clamp(0.18 + (56.0 - fs.ias_kt) * 0.032, 0.0, 0.52);
                    } else {
                        // Over asphalt: idle round-out & gentle flare (~120 fpm) right onto the runway surface!
                        thr_cmd = 0.0;
                        const double tgt_vsi = (h_rwy > 2.4) ? -1.0 : -0.45;
                        pitch_cmd = std::clamp((tgt_vsi - fs.vsi_mps) * 0.28 + 0.06, -0.30, 0.42);
                    }
                    prev_vsi = fs.vsi_mps;
                } else {
                    thr_cmd = 0.0;
                    pitch_cmd = 0.0;
                    brk_cmd = 0.72;
                    roll_cmd = 0.0;
                }
                ac_hud.step_autonomous_assist(dt, roll_cmd, pitch_cmd, thr_cmd, brk_cmd, sky_clouds.atmo(), reg_atmo, sim_time);
                const auto &ns = ac_hud.jsb_state();
                if (!captured_final && !touched && d_thr < 380.0) {
                    captured_final = true;
                    update_environment();
                    render_one_frame(true, 1, region_test_dir + "/06_leci_short_final.png");
                }
                if (!touched && ns.on_ground) {
                    touched = true;
                    td_fpm = std::abs(std::min(0.0, prev_vsi)) * 196.85;
                    td_ias = ns.ias_kt;
                    td_g = ns.g_load;
                    td_lat = ns.lat_deg; td_lon = ns.lon_deg;
                    update_environment();
                    render_one_frame(true, 1, region_test_dir + "/07_leci_touchdown.png");
                }
                if (step % 2 == 0) {
                    double f0 = PlatformWindow::now_sec();
                    win.poll_events();
                    update_environment();
                    render_one_frame(true, 4);
                    double sp = (1.0 / 60.0) - (PlatformWindow::now_sec() - f0);
                    if (sp > 0.001) std::this_thread::sleep_for(std::chrono::duration<double>(sp));
                }
                if (touched && ns.ground_speed_mps < 2.5) {
                    rollout_m = nspain::haversine_m(td_lat, td_lon, ns.lat_deg, ns.lon_deg);
                    break;
                }
            }
            std::printf("[FLIGHT-DEMO LANDING LECI] touched=%d td_fpm=%.0ffpm td_ias=%.1fkt td_g=%.2fG rollout=%.0fm final_spd=%.1fkt\n",
                        touched ? 1 : 0, td_fpm, td_ias, td_g, rollout_m, ac_hud.jsb_state().ground_speed_mps * 1.94384);
        }
        std::printf("[FLIGHT-DEMO] COMPLETE -> %s\n", region_test_dir.c_str());
        return 0;
    }

    // Continuous Recorded Crossing Flight Replay (--replay-flight <trace>) with Boundary Profiling & Persistence Check
    if (!replay_flight_path.empty()) {
        FlightReplayTrace trace;
        if (!load_flight_replay(replay_flight_path, trace)) {
            std::fprintf(stderr, "[REPLAY] Failed to load trace: %s\n", replay_flight_path.c_str());
            return 1;
        }
        const AirportInfo *dep = pack.find_airport(trace.start_airport);
        if (!dep) {
            std::fprintf(stderr, "[REPLAY] Missing departure airport %s\n", trace.start_airport.c_str());
            return 1;
        }
        tod_slot = 2;
        wx_advance = trace.wx_advance;
        reg_atmo.set_solver_seed(20261125, false);
        reg_atmo.set_manual_weather_override(true);
        reg_atmo.set_regime(static_cast<WindRegime>(trace.weather_regime));
        reg_atmo.set_season(trace.season);
        transition_world(dep->lat, dep->lon);

        ScopedTempDir replay_tmp("opensky-gpu-replay-XXXXXX");
        if (replay_tmp.dir.empty()) return 1;
        const auto profile_path = replay_tmp.dir / "fleet.ini";
        AircraftSession replay_session;
        if (!prepare_replay_session(pack, trace, profile_path, replay_session, ac_hud, 15.0)) {
            std::fprintf(stderr, "[REPLAY] Failed to prepare replay session at %s\n", trace.start_airport.c_str());
            return 1;
        }
        const double start_fuel =
            replay_session.profile().care_.left_fuel_gal + replay_session.profile().care_.right_fuel_gal;
        sim_time = 0.0;

        std::ofstream cross_csv;
        if (!trace_dir.empty()) {
            cross_csv.open(std::filesystem::path(trace_dir) / "crossing_frames.csv");
            cross_csv << "frame,sim_t,wall_ms,transition_ms,render_ms,lat_deg,lon_deg,agl_m,region_id,"
                         "anchor_drift_m,resident,pending,max_active_lod,res_l7,res_l8,res_l9,gpu_storage_mb\n";
        }

        BoundaryMetrics boundary{};
        if (const auto *r0 = pack.find_region_at(dep->lat, dep->lon)) boundary.start_region = r0->region_id;
        else boundary.start_region = "es_aragon_pyrenees";

        const double run_t0 = PlatformWindow::now_sec();
        std::vector<double> all_wall_ms, win_wall_ms;
        all_wall_ms.reserve(trace.steps.size());
        double max_all_trans_ms = 0.0, max_win_trans_ms = 0.0;
        int gt33_all = 0, gt33_win = 0;
        bool settled_sharp = false;

        profiler.set_scene("crossing_replay");
        const double target_frame_s = 1.0 / std::clamp(win.refresh_hz(), 30.0, 60.0);

        for (size_t i = 0; i < trace.steps.size() && !win.should_close(); ++i) {
            const ReplayStep &rs = trace.steps[i];
            const double loop_t0 = PlatformWindow::now_sec();
            win.poll_events();

            sim_time += rs.dt;
            update_environment();
            const double phys_t0 = profiler.cpu_start();
            apply_replay_step(ac_hud, replay_session, rs, sky_clouds.atmo(), reg_atmo, sim_time);
            profiler.cpu_end(FlightProfiler::InputPhysics, phys_t0);

            const auto &flight = ac_hud.jsb_state();
            const auto *cur_reg = pack.find_region_at(flight.lat_deg, flight.lon_deg);
            const std::string reg_name = cur_reg ? cur_reg->region_id : "outside";
            const int cur_lod_before = terrain.last_stats().max_active_lod;
            if (!boundary.crossed_region && cur_reg && cur_reg->region_id != boundary.start_region) {
                boundary.crossed_region = true;
                boundary.crossed_to_region = cur_reg->region_id;
                boundary.cross_sim_time_s = sim_time;
                boundary.cross_wall_ms = (PlatformWindow::now_sec() - run_t0) * 1000.0;
                boundary.pre_cross_max_lod = std::max(7, cur_lod_before);
                std::printf("[BOUNDARY-CROSS] step=%zu sim_t=%.2fs -> %s at (%.4f, %.4f) pre_lod=%d\n",
                            i, sim_time, boundary.crossed_to_region.c_str(),
                            flight.lat_deg, flight.lon_deg, boundary.pre_cross_max_lod);
            }

            const double tr_t0 = PlatformWindow::now_sec();
            const bool did_commit = step_flight_world_transition(
                flight.lat_deg, flight.lon_deg, flight.vel_east_mps, flight.vel_north_mps);
            const double tr_ms = (PlatformWindow::now_sec() - tr_t0) * 1000.0;
            if (did_commit && boundary.crossed_region && boundary.commit_wall_ms < 0.0 &&
                pack.find_region_at(pack.anchor_lat(), pack.anchor_lon()) == cur_reg) {
                boundary.commit_wall_ms = (PlatformWindow::now_sec() - run_t0) * 1000.0;
            }

            const bool pace_realtime =
                (sim_time < 20.0) || (flight.lon_deg >= 0.610 && flight.lon_deg <= 0.720) ||
                (trace.route.empty() ? false
                    : nspain::haversine_m(flight.lat_deg, flight.lon_deg, trace.route.back().lat, trace.route.back().lon) < 3500.0);
            const uint64_t step_mask = (thermal_fps_cap <= 35.0) ? 15ULL : ((thermal_fps_cap <= 50.0) ? 3ULL : 1ULL);
            const bool render_step = pace_realtime || (i & step_mask) == 0 || did_commit || async_cross.active;

            double rend_ms = 0.0;
            double wall_ms = 0.0;
            if (render_step) {
                const double r_t0 = PlatformWindow::now_sec();
                render_one_frame(true, 1);
                rend_ms = (PlatformWindow::now_sec() - r_t0) * 1000.0;
                wall_ms = (PlatformWindow::now_sec() - loop_t0) * 1000.0;
            } else {
                wall_ms = (PlatformWindow::now_sec() - loop_t0) * 1000.0;
            }

            const StreamStats ss = streamer.stats();
            const CdlodDrawStats ts = terrain.last_stats();
            const double drift_m = nspain::haversine_m(flight.lat_deg, flight.lon_deg, pack.anchor_lat(), pack.anchor_lon());

            if (boundary.crossed_region && !settled_sharp && boundary.commit_wall_ms >= 0.0 &&
                ts.max_active_lod >= std::min(7, boundary.pre_cross_max_lod) && ss.queued_jobs <= 4) {
                settled_sharp = true;
                boundary.sharp_terrain_wall_ms = (PlatformWindow::now_sec() - run_t0) * 1000.0;
                boundary.time_to_sharp_since_cross_ms = boundary.sharp_terrain_wall_ms - boundary.cross_wall_ms;
                boundary.time_to_sharp_since_commit_ms = boundary.sharp_terrain_wall_ms - boundary.commit_wall_ms;
                std::printf("[BOUNDARY-SHARP] since_cross=%.1fms since_commit=%.1fms max_lod=%d res_L7/8/9=%d/%d/%d\n",
                            boundary.time_to_sharp_since_cross_ms, boundary.time_to_sharp_since_commit_ms,
                            ts.max_active_lod, ss.resident_by_lod[7], ss.resident_by_lod[8], ss.resident_by_lod[9]);
            }

            if (render_step) {
                all_wall_ms.push_back(wall_ms);
                max_all_trans_ms = std::max(max_all_trans_ms, tr_ms);
                if (wall_ms > 33.333) ++gt33_all;
            }
            const bool in_boundary_win =
                boundary.crossed_region ? (sim_time >= boundary.cross_sim_time_s - 30.0 && sim_time <= boundary.cross_sim_time_s + 60.0)
                                        : (flight.lon_deg >= 0.56);
            if (in_boundary_win && render_step) {
                win_wall_ms.push_back(wall_ms);
                max_win_trans_ms = std::max(max_win_trans_ms, tr_ms);
                if (wall_ms > 33.333) ++gt33_win;
            }

            if (render_step && cross_csv.is_open()) {
                cross_csv << frame_idx << ',' << sim_time << ',' << wall_ms << ',' << tr_ms << ',' << rend_ms << ','
                          << flight.lat_deg << ',' << flight.lon_deg << ',' << flight.agl_m << ',' << reg_name << ','
                          << drift_m << ',' << ss.resident_pages << ',' << ss.queued_jobs << ','
                          << ts.max_active_lod << ',' << ss.resident_by_lod[7] << ','
                          << ss.resident_by_lod[8] << ',' << ss.resident_by_lod[9] << ','
                          << (static_cast<double>(ss.gpu_page_bytes) / (1024.0 * 1024.0)) << '\n';
            }

            const double pace_budget = pace_realtime
                ? std::max(target_frame_s, 1.0 / thermal_fps_cap)
                : ((render_step && thermal_fps_cap < 55.0) ? std::min(0.016, 1.0 / thermal_fps_cap) : 0.0);
            const double spare = pace_budget - (PlatformWindow::now_sec() - loop_t0);
            if (spare > 0.0005) std::this_thread::sleep_for(std::chrono::duration<double>(spare));
        }

        auto print_pct = [&](const char *label, std::vector<double> vals, double max_tr, int gt33) {
            if (vals.empty()) return;
            std::sort(vals.begin(), vals.end());
            const auto pct = [&](double p) {
                size_t idx = static_cast<size_t>(std::ceil(p * vals.size())) - 1;
                return vals[std::min(idx, vals.size() - 1)];
            };
            const double sum = std::accumulate(vals.begin(), vals.end(), 0.0);
            const double mean = sum / vals.size();
            std::printf("[BOUNDARY-SUMMARY %s] frames=%zu mean=%.2fms (%.1f FPS) p50=%.2f p95=%.2f p99=%.2f max=%.2f max_trans=%.2f >33ms=%d\n",
                        label, vals.size(), mean, 1000.0 / std::max(0.1, mean),
                        pct(0.50), pct(0.95), pct(0.99), vals.back(), max_tr, gt33);
        };
        print_pct("FULL", all_wall_ms, max_all_trans_ms, gt33_all);
        print_pct("WINDOW", win_wall_ms, max_win_trans_ms, gt33_win);
        std::printf("[BOUNDARY-METRICS] crossed=%d sharp_cross_ms=%.1f sharp_commit_ms=%.1f\n",
                    boundary.crossed_region ? 1 : 0, boundary.time_to_sharp_since_cross_ms,
                    boundary.time_to_sharp_since_commit_ms);

        cancel_async_crossing();
        const bool persist_ok = verify_replay_persistence(
            pack, replay_session, ac_hud, profile_path, trace, start_fuel, boundary.crossed_region);
        audio.shutdown();
        profiler.finish();
        return persist_ok ? 0 : 1;
    }

    // Calibrated North-Up Nadir Top-Down Satellite Comparison Mode (--topdown <dir>)
    if (!topdown_dir.empty()) {
        std::filesystem::create_directories(topdown_dir);
        quality = 2; // HIGH detail for satellite QA comparison
        tod_slot = 2;
        wx_advance = 0.22f;
        reg_atmo.set_regime(WindRegime::CIERZO_NW); // Crisp clear air for clean top-down fidelity measurement!

        struct TopDownSite { const char *name; const char *label; double lat, lon, agl_m; };
        static const TopDownSite kSites[] = {
            {"zaragoza_pilar",   "Zaragoza Basilica del Pilar & Ebro", 41.6566, -0.8785,  680.0},
            {"zaragoza_center",  "Zaragoza Urban Grid & Avenues",      41.6490, -0.8860,  720.0},
            {"huesca_catedral",  "Huesca Old Town & Catedral",         42.1404, -0.4086,  660.0},
            {"castejon_es0205",  "Castejon de Sos (ES-0205) Valley",   42.5210,  0.4830,  900.0},
            {"embalse_mediano",  "Embalse de Mediano & Torre",         42.3175,  0.2037, 1100.0},
            {"santa_cilia_leci", "Santa Cilia Aerodrome (LECI)",       42.5692, -0.7280,  950.0},
        };

        const float td_fov = 40.0f;
        const float aspect = static_cast<float>(output_w) / output_h;
        std::FILE *mf = std::fopen((topdown_dir + "/meta.json").c_str(), "wb");
        if (mf) std::fprintf(mf, "{\n  \"sites\": [\n");

        const int nsites = static_cast<int>(sizeof(kSites) / sizeof(kSites[0]));
        for (int i = 0; i < nsites; i++) {
            const auto &site = kSites[i];
            FlightSimState &s = ac_hud.state();
            s.paused = true;
            s.slew_fast = false;
            s.view_mode = 2; // Free external camera (no cockpit/aircraft blocking nadir!)
            double gnd = pack.sample_elevation(site.lat, site.lon);
            ac_hud.reset_flight(site.lat, site.lon, gnd + site.agl_m, 0.0, 0.0, false);
            s.pitch_deg = -89.95; // Exact North-Up straight down nadir!
            s.roll_deg = 0.0;
            s.look_yaw_deg = 0.0f;
            s.look_pitch_deg = 0.0f;
            reg_atmo.update_field(sky_clouds.atmo().sun_dir, sim_time, true);
            step_flight(1.0 / 60.0);
            sky_clouds.reset_temporal_history();
            for (int w = 0; w < 12; w++) {
                win.poll_events();
                capture_frame(1.0 / 60.0, false, 64, "", td_fov);
                streamer.flush_pending_sync(16, 0.25);
            }
            std::string png_name = std::string(site.name) + ".png";
            std::string png_path = topdown_dir + "/" + png_name;
            for (int f = 0; f < 4; f++) {
                win.poll_events();
                capture_frame(1.0 / 60.0, false, 64, (f == 3) ? png_path : "", td_fov);
            }
            double half_n_m = site.agl_m * std::tan(0.5 * td_fov * DEG2RAD_D);
            double half_e_m = half_n_m * aspect;
            double dlat = half_n_m / pack.anchor_m_lat();
            double dlon = half_e_m / pack.anchor_m_lon();
            if (mf) {
                std::fprintf(mf,
                    "    {\"name\":\"%s\",\"label\":\"%s\",\"image\":\"%s\",\"lat\":%.6f,\"lon\":%.6f,"
                    "\"ground_m\":%.2f,\"agl_m\":%.2f,\"fov_deg\":%.2f,\"width\":%d,\"height\":%d,"
                    "\"min_lat\":%.7f,\"max_lat\":%.7f,\"min_lon\":%.7f,\"max_lon\":%.7f,"
                    "\"span_e_m\":%.2f,\"span_n_m\":%.2f}%s\n",
                    site.name, site.label, png_name.c_str(), site.lat, site.lon,
                    gnd, site.agl_m, td_fov, output_w, output_h,
                    site.lat - dlat, site.lat + dlat, site.lon - dlon, site.lon + dlon,
                    half_e_m * 2.0, half_n_m * 2.0, (i + 1 < nsites) ? "," : "");
            }
            std::printf("[TOPDOWN] %-18s | gnd=%6.1fm agl=%6.1fm | span=%.0fx%.0fm | cars=%d | canopy=material-pass\n",
                        site.name, gnd, site.agl_m, half_e_m * 2.0, half_n_m * 2.0, objects.active_cars());
        }
        if (mf) {
            std::fprintf(mf, "  ]\n}\n");
            std::fclose(mf);
        }
        profiler.finish();
        std::printf("[TOPDOWN] COMPLETE -> %s/meta.json\n", topdown_dir.c_str());
        return 0;
    }

    // Ground-Level Airport Scenery & Cockpit Inspection Mode (--airport-qa <dir>, separate from --autotest)
    if (!airport_qa_dir.empty()) {
        std::filesystem::create_directories(airport_qa_dir);
        quality = 2;
        tod_slot = 2;
        wx_advance = 0.22f;
        reg_atmo.set_regime(WindRegime::CIERZO_NW);

        auto capture_airport_ground = [&](const char *ap_id, const char *tag) {
            const AirportInfo *ap = pack.find_airport(ap_id);
            if (!ap) return;
            transition_world(ap->lat, ap->lon);
            frontend.open_airport(*ap);
            vegetation.prepare(pack, *ap);
            sky_clouds.reset_temporal_history();
            for (int w = 0; w < 14; ++w) {
                win.poll_events();
                capture_frame(1.0 / 60.0, false, 64, "", 58.0f);
                streamer.flush_pending_sync(16, 0.25);
            }
            for (int f = 0; f < 4; ++f) {
                win.poll_events();
                capture_frame(1.0 / 60.0, false, 64, (f == 3) ? (airport_qa_dir + "/" + tag + "_walk.png") : "", 58.0f);
            }
            // Also capture cockpit view from the aircraft parked beside the terrain-clear runway!
            frontend.board(ac_hud);
            ac_hud.state().mouse_yoke_enabled = true;
            sky_clouds.reset_temporal_history();
            for (int w = 0; w < 10; ++w) {
                win.poll_events();
                capture_frame(1.0 / 60.0, true, 64, "", 50.0f);
                streamer.flush_pending_sync(16, 0.25);
            }
            for (int f = 0; f < 4; ++f) {
                win.poll_events();
                capture_frame(1.0 / 60.0, true, 64, (f == 3) ? (airport_qa_dir + "/" + tag + "_cockpit.png") : "", 50.0f);
            }
            // Low chase view down the departure runway to inspect surface, markers, fences, buildings, and trees!
            ac_hud.state().view_mode = 1;
            ac_hud.state().look_yaw_deg = 22.0f;
            ac_hud.state().look_pitch_deg = -10.0f;
            sky_clouds.reset_temporal_history();
            for (int f = 0; f < 4; ++f) {
                win.poll_events();
                capture_frame(1.0 / 60.0, false, 64, (f == 3) ? (airport_qa_dir + "/" + tag + "_runway.png") : "", 52.0f);
            }
            const auto ts = terrain.last_stats();
            const auto ss = streamer.stats();
            std::printf("[AIRPORT-QA] Captured %s (%s, srf=%s, hdg=%.0f) | terrain nodes=%d maxL=%d resident=%d gnd=%.1fm\n",
                        ap->id.c_str(), ap->name.c_str(), ap->surface.c_str(), ac_hud.state().heading_deg,
                        ts.visible_nodes, ts.max_active_lod, ss.resident_pages, pack.sample_elevation(ap->lat, ap->lon));
        };
        if (!req_airport.empty()) {
            capture_airport_ground(req_airport.c_str(), req_airport.c_str());
        } else {
            capture_airport_ground("ES-0205", "castejon_es0205");
            capture_airport_ground("LECI", "santa_cilia_leci");
            capture_airport_ground("LEHC", "huesca_lehc");
        }
        std::printf("[AIRPORT-QA] COMPLETE -> %s\n", airport_qa_dir.c_str());
        return 0;
    }

    // Automated Visual + Speed-Aware Async Streaming p95 Benchmark Suite (--autotest <dir>)
    if (!autotest_dir.empty()) {
        std::filesystem::create_directories(autotest_dir);
        tod_slot = 2;
        wx_advance = 0.28f;

        if (!bench_only && !cloud_qa && !cockpit_qa && !promo_only) {
        // Export whole-corridor 50km Pyrenees 10-TOD Shadow & Valley Sky-AO Map for direct visual inspection!
        {
            int sw = pack.corridor_dem_w(), sh = pack.corridor_dem_h();
            const auto &gi = pack.corridor_gi_u32();
            if (static_cast<int>(gi.size()) == sw * sh) {
                std::ofstream pf(autotest_dir + "/corridor_shadows_10tod.ppm", std::ios::binary);
                pf << "P6\n" << (sw * 5) << " " << sh << "\n255\n";
                std::vector<uint8_t> row(static_cast<size_t>(sw) * 5 * 3);
                static constexpr float kShow[4] = {0.0f, 2.0f, 5.0f, 9.0f};
                for (int y = sh - 1; y >= 0; y--) { // North-up!
                    for (int x = 0; x < sw; x++) {
                        uint32_t b = gi[static_cast<size_t>(y) * sw + x];
                        for (int c = 0; c < 4; c++) {
                            uint8_t v = static_cast<uint8_t>(std::clamp(unpack_tod10_vis(b, kShow[c]) * 255.0f + 0.5f, 0.0f, 255.0f));
                            size_t o = (static_cast<size_t>(c) * sw + x) * 3;
                            row[o] = row[o + 1] = row[o + 2] = v;
                        }
                        uint8_t ao = static_cast<uint8_t>((b >> 20u) & 0xFFu);
                        size_t o = (static_cast<size_t>(4) * sw + x) * 3;
                        row[o] = row[o + 1] = row[o + 2] = ao;
                    }
                    pf.write(reinterpret_cast<const char *>(row.data()), static_cast<std::streamsize>(row.size()));
                }
            }
        }

        }

        if (!bench_only) {
        auto snap = [&](const std::string &fname, double lat, double lon, double agl, double hdg, double pitch, int vmode,
                        double time_advance = 0.0, float look_yaw = 0.0f, float look_pitch = 0.0f,
                        double roll = 0.0, double ail = 0.0, bool draw_hud = true) {
            FlightSimState &s = ac_hud.state();
            profiler.set_scene(fname);
            s.paused = true;
            s.slew_fast = false;
            s.view_mode = vmode;
            double gnd = pack.sample_elevation(lat, lon);
            if (promo_only) transition_world(lat, lon);
            ac_hud.reset_flight(lat, lon, gnd + agl, hdg, 100.0, false);
            // Ordinary QA shots share one region anchor. Promo shots move
            // across large areas, so transition_world reanchors and cancels
            // stale work while preserving the target site's prewarm requests.
            if (!promo_only) streamer.clear_pending_requests();
            s.pitch_deg = pitch;
            s.roll_deg = roll;
            s.aileron = ail;
            s.rudder = -ail * 0.45;
            s.look_yaw_deg = look_yaw;
            s.look_pitch_deg = look_pitch;
            sim_time += time_advance;
            reg_atmo.update_field(sky_clouds.atmo().sun_dir, sim_time, true);
            step_flight(1.0 / 60.0);
            s.roll_deg = roll;
            s.aileron = ail;
            s.rudder = -ail * 0.45;
            s.look_yaw_deg = look_yaw;
            s.look_pitch_deg = look_pitch;
            sky_clouds.reset_temporal_history();
            // Drain all 9 CDLOD split passes (L2 -> L9), TOD shadow refreshes & local tree cells cleanly for steady-state visual inspection!
            const int warm_frames = cockpit_qa ? 5 : (promo_only ? 20 : 14);
            for (int w = 0; w < warm_frames; w++) {
                win.poll_events();
                step_flight(1.0 / 60.0);
                capture_frame(1.0 / 60.0, draw_hud, 128);
                if (cockpit_qa) streamer.flush_pending_sync(4, 0.12);
                else if (promo_only) streamer.flush_pending_sync(8, 0.05);
                else streamer.flush_pending_sync(24, 0.25);
            }
            std::string p = autotest_dir + "/" + fname;
            for (int f = 0; f < 10; f++) {
                win.poll_events();
                step_flight(1.0 / 60.0);
                capture_frame(1.0 / 60.0, draw_hud, 128, (f == 9) ? p : "");
            }
            StreamStats ss = streamer.stats();
            CdlodDrawStats cd = terrain.last_stats();
            std::printf("[AUTOTEST] %s | gnd=%.1fm | CDLOD=%d(L%d) nodes7-9=%d/%d/%d | cache=%d/%d pages7-9=%d/%d/%d jobs=%d worker=%.0fms | canopy=material-pass | cars=%d\n",
                        fname.c_str(), gnd, cd.visible_nodes, cd.max_active_lod,
                        cd.visible_by_lod[7], cd.visible_by_lod[8], cd.visible_by_lod[9], ss.resident_pages, ss.max_pages,
                        ss.resident_by_lod[7], ss.resident_by_lod[8], ss.resident_by_lod[9], ss.queued_jobs, ss.avg_worker_ms,
                        objects.active_cars());
        };

        if (season_qa) {
            static constexpr const char *kSeasonNames[4] = {"winter", "spring", "summer", "autumn"};
            for (int s = 0; s < 4 && !win.should_close(); ++s) {
                season_override = s;
                reg_atmo.set_season(s);
                wx_advance = 0.28f;
                tod_slot = 2;
                sky_clouds.reset_temporal_history();
                // Same Pyrenean forest approach and alpine horizon in each
                // season; only the baked regional season and terrain response change.
                snap(std::string("season_") + kSeasonNames[s] + "_canopy.png",
                     42.54388, 0.45465, 30.0, 315.0, -6.0, 2, 0.0, 0.0f, 0.0f, 0.0, 0.0, false);
                snap(std::string("season_") + kSeasonNames[s] + "_alpine.png",
                     42.53131, 0.47676, 1850.0, 8.0, -5.0, 2, 0.0, 0.0f, 0.0f, 0.0, 0.0, false);
            }
            profiler.finish();
            std::printf("[SEASON-QA] Four-season canopy and alpine captures complete -> %s\n", autotest_dir.c_str());
            return 0;
        }

        if (cockpit_qa) {
            snap("cockpit_forward.png", 42.53131, 0.47676, 110.0, 160.0, -3.0, 0, 0.0, 0.0f, -7.0f, 0.0, -0.25, false);
            snap("cockpit_panel.png", 42.53131, 0.47676, 110.0, 160.0, -3.0, 0, 0.0, -13.0f, -27.0f, 0.0, -0.25, false);
            snap("cockpit_side.png", 42.53131, 0.47676, 110.0, 160.0, 0.0, 0, 0.0, 82.0f, -16.0f, 0.0, 0.0, false);
            snap("cockpit_aft_left.png", 42.53131, 0.47676, 110.0, 160.0, 0.0, 0, 0.0, 155.0f, -12.0f, 0.0, 0.0, false);
            snap("cockpit_aft_right.png", 42.53131, 0.47676, 110.0, 160.0, 0.0, 0, 0.0, -155.0f, -12.0f, 0.0, 0.0, false);
            // Compare the same forward pane in a local convective shower. Rain
            // coverage is driven from the regional sample, not a shader-only toggle.
            reg_atmo.set_regime(WindRegime::CONVECTIVE_STORM);
            wx_advance = 0.94f;
            sim_time += 45.0;
            reg_atmo.update_field(sky_clouds.atmo().sun_dir, sim_time, true, wx_advance);
            snap("cockpit_rain.png", 42.53131, 0.47676, 110.0, 160.0, -3.0, 0, 45.0,
                 0.0f, -7.0f, 0.0, -0.25, false);
            snap("cockpit_side_rain.png", 42.53131, 0.47676, 110.0, 160.0, 0.0, 0, 0.0,
                 82.0f, -16.0f, 0.0, 0.0, false);
            std::printf("[COCKPIT-QA] Local forward-pane rain sample: %.2f mm/h\n",
                        ac_hud.last_air().precipitation_mm_hr);
            reg_atmo.set_regime(WindRegime::PYRENEAN_THERMAL);
            wx_advance = 0.28f;
            profiler.finish();
            std::printf("[COCKPIT-QA] Cockpit view captures complete -> %s\n", autotest_dir.c_str());
            return 0;
        }

        if (promo_only) {
            if (req_region == "es_catalonia_coast") {
                snap("promo_cockpit_barcelona_beaches.png", 41.3640, 2.1990, 900.0, 305.0, -2.0, 0,
                     0.0, 0.0f, -3.0f, 0.0, 0.0, false);
                snap("promo_chase_barcelona_beaches.png", 41.3640, 2.1990, 900.0, 305.0, -2.0, 1,
                     0.0, 0.0f, -7.0f, 0.0, 0.0, false);
                snap("promo_cockpit_pals_beach.png", 41.9929, 3.2011, 500.0, 180.0, -2.0, 0,
                     0.0, 0.0f, -3.0f, 0.0, 0.0, false);
                snap("promo_chase_pals_beach.png", 41.9929, 3.2011, 500.0, 180.0, -2.0, 1,
                     0.0, 0.0f, -7.0f, 0.0, 0.0, false);
            } else if (req_region == "es_catalonia_pyrenees") {
                snap("promo_cockpit_catalan_pyrenees.png", 42.3387, 1.4092, 950.0, 350.0, -2.0, 0,
                     0.0, 0.0f, -3.0f, 0.0, 0.0, false);
                snap("promo_chase_catalan_pyrenees.png", 42.3387, 1.4092, 950.0, 350.0, -2.0, 1,
                     0.0, 0.0f, -7.0f, 0.0, 0.0, false);
            } else {
                snap("promo_cockpit_mediano.png", 42.31750, 0.20370, 600.0, 18.0, -3.0, 0,
                     0.0, 0.0f, -4.0f, 0.0, 0.0, false);
                snap("promo_chase_mediano.png", 42.31750, 0.20370, 600.0, 18.0, -3.0, 1,
                     0.0, 0.0f, -7.0f, 0.0, 0.0, false);
                snap("promo_cockpit_castejon.png", 42.53131, 0.47676, 350.0, 315.0, -3.0, 0,
                     0.0, 0.0f, -4.0f, 0.0, 0.0, false);
                snap("promo_chase_castejon.png", 42.53131, 0.47676, 350.0, 315.0, -3.0, 1,
                     0.0, 0.0f, -7.0f, 0.0, 0.0, false);
            }
            profiler.finish();
            std::printf("[PROMO-QA] Scenic cockpit and chase captures complete for %s -> %s\n",
                        req_region.empty() ? "Aragon" : req_region.c_str(), autotest_dir.c_str());
            return 0;
        }

        if (!cloud_qa) {
        // 1. Aircraft Showcase & Northern Spain Visual Verification Shots!
        snap("shot_cockpit_3d_panel.png",    42.53131,  0.47676, 110.0, 160.0, -4.5, 0, 0.0,   0.0f, -11.0f, -14.0, -0.55, false);
        snap("shot_cockpit_aft.png",          42.53131,  0.47676, 110.0, 160.0,   0.0, 0, 0.0, 155.0f,    0.0f,   0.0,  0.00, false);
        snap("shot_cockpit_wing_view.png",   42.53131,  0.47676, 130.0, 160.0, -3.5, 0, 0.0,  62.0f,  -6.0f, -10.0, -0.30, false);
        snap("shot_aircraft_front34.png",    42.53131,  0.47676, 150.0, 160.0, -3.5, 1, 0.0, 144.0f,  -7.0f,  14.0,  0.65, false);
        snap("shot_aircraft_rear34.png",     42.53131,  0.47676, 140.0, 160.0, -4.0, 1, 0.0, -28.0f,  -8.0f, -16.0, -0.60, false);
        snap("shot_ES-0205.png",             42.53131,  0.47676,  95.0, 160.0, -5.5, 0);
        snap("shot_closeup_es0205.png",      42.52380,  0.48120,  22.0, 158.0, -9.0, 2);
        snap("shot_lidar_forest_close.png",  42.54388,  0.45465,  30.0, 315.0,  -6.0, 2);
        snap("shot_cockpit_final_26m.png",   42.52657,  0.47909,  40.0, 160.0, -6.0, 0);
        snap("shot_chase_c172s.png",         42.53131,  0.47676, 130.0, 160.0, -4.0, 1);
        snap("shot_water_mediano.png",       42.31200,  0.19400, 185.0,  18.0, -9.0, 2);
        snap("shot_clouds_pyrenees.png",     42.31200,  0.19400, 520.0,  82.0,  6.0, 2);
        snap("shot_clouds_pyrenees_t45.png", 42.31200,  0.19400, 520.0,  82.0,  6.0, 2, 45.0);
        snap("shot_city_zaragoza.png",       41.65250, -0.88150, 165.0,  26.0, -16.0, 2);
        snap("shot_closeup_zaragoza_55m.png",41.65380, -0.88060,  58.0,  22.0, -14.0, 2);
        snap("shot_city_huesca.png",         42.13550, -0.41100, 165.0,  20.0, -15.0, 2);
        snap("shot_closeup_village.png",     42.51120,  0.48920,  75.0,  18.0, -16.0, 2);
        snap("shot_LECI.png",                42.56920, -0.74299, 114.0,  90.0, -5.5, 0);
        reg_atmo.set_regime(WindRegime::KATABATIC_DAWN);
        snap("shot_fog_scattering.png",      42.53600,  0.46800, 210.0, 118.0, -3.0, 2);
        reg_atmo.set_regime(WindRegime::PYRENEAN_THERMAL);

        // 1b. Pre-Baked 10-Time-of-Day Showcase (Dawn TOD0 -> Noon TOD5 -> Sunset TOD9!)
        tod_slot = 0;
        snap("shot_tod0_dawn.png",           42.51800,  0.48600, 180.0,  22.0, -10.0, 2);
        tod_slot = 5;
        snap("shot_tod5_noon.png",           41.65250, -0.88150, 165.0,  26.0, -16.0, 2);
        tod_slot = 9;
        snap("shot_tod9_sunset.png",         42.53600,  0.46800, 220.0, 265.0,  -4.0, 2);

        // 1c. SHARP Pre-Baked Ray-Traced Night Local Lights (Cities, Small Towns & Rural Roads!)
        tod_slot = 10;
        snap("shot_night_zaragoza.png",      41.65250, -0.88150, 165.0,  26.0, -18.0, 2);
        snap("shot_night_village_road.png",  42.51120,  0.48920,  85.0,  18.0, -18.0, 2);

        }

        // 1d. Flying THROUGH Volumetric Clouds & Weather Advancement Sequence (Approach -> Inside -> Breakout -> Rain Front!)
        tod_slot = 2;
        wx_advance = 0.28f;
        snap("shot_clouds_mediano.png",     42.31200,  0.19400,  470.0,  48.0,  +11.0, 2, 0.0, 0, 0, 0, 0, false);
        wx_advance = 0.37f;
        snap("shot_cloud_crest.png",       42.34000,  0.22000, 1810.0,  92.0,  -3.0, 2, 8.0, 0, 0, 0, 0, false);
        wx_advance = 0.48f;
        snap("shot_cloud_1_approach.png",    42.34000,  0.22000,  440.0,  32.0,  +7.0, 0);
        wx_advance = 0.72f;
        snap("shot_cloud_2_inside.png",      42.34800,  0.22600, 1820.0,  32.0,  +2.0, 0, 25.0);
        wx_advance = 0.62f;
        snap("shot_cloud_3_breakout.png",    42.36000,  0.23500, 3250.0,  32.0,  -7.0, 1, 50.0);
        wx_advance = 0.94f;
        reg_atmo.set_regime(WindRegime::CONVECTIVE_STORM);
        snap("shot_cloud_4_overcast_rain.png",42.53131, 0.47676,  140.0, 160.0,  -4.0, 0, 75.0);
        snap("shot_rain_mediano.png",      42.31200,  0.19400,  245.0,  48.0,  -4.0, 2, 0.0, 0, 0, 0, 0, false);
        reg_atmo.set_regime(WindRegime::PYRENEAN_THERMAL);
        wx_advance = 0.28f;
        tod_slot = 2;

        if (cloud_qa) {
            std::printf("[CLOUD-QA] COMPLETE.\n");
            return 0;
        }
        // 1e. Distance-tiered IGN PNOA online close-area ortho streaming (Z20@200m .. Z14@20km).
        pack.set_online_provider(OnlineOrthoProvider::PNOA);
        pack.prefetch_online_box(42.52380, 0.48120, 200.0, 20);
        pack.prefetch_online_box(42.52380, 0.48120, 500.0, 19);
        streamer.invalidate_close_pages(true);
        snap("shot_online_pnoa_z20.png", 42.52380, 0.48120, 22.0, 158.0, -9.0, 2);
        pack.set_online_provider(init_online);
        streamer.invalidate_close_pages(true);
        }

        // 2. Live Flight & HIGH-SPEED 350 KT Async Streaming p95 Benchmarks!
        auto bench = [&](const char *tag, double lat, double lon, double agl, double hdg, double pitch, bool fast_350kt) {
            for (int q : {2, 1, 0}) {
                quality = q;
                profiler.set_scene(std::string(tag) + " warmup");
                FlightSimState &s = ac_hud.state();
                s.view_mode = 0;
                s.paused = false;
                s.slew_fast = fast_350kt;
                double gnd = pack.sample_elevation(lat, lon);
                ac_hud.reset_flight(lat, lon, gnd + agl, hdg, 100.0, false);
                s.pitch_deg = pitch;
                sky_clouds.reset_temporal_history();
                for (int w = 0; w < 10; w++) {
                    win.poll_events();
                    capture_frame(1.0 / 60.0, true, 128);
                    streamer.flush_pending_sync(20, 0.20);
                }
                for (int w = 0; w < 6; w++) {
                    win.poll_events();
                    step_flight(1.0 / 60.0);
                    capture_frame(1.0 / 60.0, true, 128);
                }
                streamer.flush_pending_sync(20, 0.20);
                profiler.set_scene(tag);
                constexpr int N = 120;
                std::vector<double> ms;
                ms.reserve(N);
                double max_up_ms = 0.0;
                int up_cnt = 0;
                double t_start = PlatformWindow::now_sec();
                double prev = t_start;
                for (int f = 0; f < N; f++) {
                    win.poll_events();
                    step_flight(1.0 / 60.0);
                    capture_frame(1.0 / 60.0, true);
                    double now = PlatformWindow::now_sec();
                    ms.push_back((now - prev) * 1000.0);
                    prev = now;
                    StreamStats ss = streamer.stats();
                    max_up_ms = std::max(max_up_ms, ss.last_upload_ms);
                    up_cnt += ss.uploads_this_frame;
                }
                double elapsed = prev - t_start;
                std::sort(ms.begin(), ms.end());
                double p95 = ms[static_cast<size_t>(N * 0.95)];
                double fps = static_cast<double>(N) / std::max(0.001, elapsed);
                static const char *kQN[4] = {"LOW iGPU", "MEDIUM", "HIGH", "ULTRA"};
                std::printf("[BENCH] %-24s %-8s: %5.1f fps (avg=%5.2f ms, p95=%5.2f ms | PBO uploads=%d max=%.2fms)\n",
                            tag, kQN[q], fps, 1000.0 / fps, p95, up_cnt, max_up_ms);
            }
        };

        bench("PYRENEES 100kt(ES-0205)", 42.5313,  0.4768, 120.0, 160.0, -4.0, false);
        bench("ZARAGOZA 100kt(79k GPU)", 41.6420, -0.8920, 260.0,  28.0, -4.0, false);
        bench("MEDIANO  100kt(WATER)",   42.3120,  0.1940, 185.0,  12.0, -4.0, false);
        bench("STRESS   350kt(STREAM)",  42.2000,  0.1000, 420.0,  15.0, -2.0, true);
        profiler.finish();
        std::printf("[AUTOTEST] COMPLETE.\n");
        return 0;
    }

    auto enter_region = [&](const AirportInfo &airport) {
        char title[128];
        std::snprintf(title, sizeof(title), "Loading %s - %s", airport.id.c_str(), airport.name.c_str());
        const auto *from = pack.find_region_at(pack.anchor_lat(), pack.anchor_lon());
        const auto *to = pack.find_region_by_id(airport.region_id);
        const double distance = nspain::haversine_m(airport.lat, airport.lon, pack.anchor_lat(), pack.anchor_lon());
        if (from != to || distance > 50000.0) {
            transition_world(airport.lat, airport.lon, title);
        } else {
            show_loading(title, "Clearing queued pages & syncing local ramp...", 0.55f);
            streamer.clear_pending_requests();
        }
        show_loading(title, "Preparing airfield ramp & vegetation...", 0.90f);
        vegetation.prepare(pack, airport);
        reg_atmo.sync_region(airport.lat, airport.lon);
        sky_clouds.reset_temporal_history();
    };

    std::vector<uint64_t> airfield_pages;
    auto prepare_airfield = [&](const AirportInfo &ap) {
        airfield_pages.clear();
        std::unordered_set<uint64_t> unique;
        auto cover = [&](double lat, double lon) {
            constexpr double span = AsyncTerrainStreamer::WORLD_SPAN_M;
            const double x = (lon - pack.anchor_lon()) * pack.anchor_m_lon() / span + .5;
            const double y = (lat - pack.anchor_lat()) * pack.anchor_m_lat() / span + .5;
            // CDLOD splits four siblings together. Queue the whole path up front.
            for (int lod = 1; lod <= AsyncTerrainStreamer::MAX_LOD; ++lod) {
                int width = 1 << lod;
                int qx = std::clamp(int(x * width), 0, width - 1) & ~1;
                int qy = std::clamp(int(y * width), 0, width - 1) & ~1;
                for (int dy = 0; dy < 2; ++dy) for (int dx = 0; dx < 2; ++dx) {
                    QuadKey key{uint8_t(lod), uint16_t(qx + dx), uint16_t(qy + dy)};
                    if (unique.insert(key.pack()).second) airfield_pages.push_back(key.pack());
                }
            }
        };
        const auto &spot = frontend.parking_spot();
        for (double east : {-180.0, 0.0, 180.0})
            for (double north : {-180.0, 0.0, 180.0})
                cover(spot.lat + north / pack.anchor_m_lat(), spot.lon + east / pack.anchor_m_lon());
        cover(ap.rwy_lat_a, ap.rwy_lon_a);
        cover(ap.rwy_lat_b, ap.rwy_lon_b);
        pack.set_online_camera(spot.lat, spot.lon);
    };

    // Interactive Main Flight Loop
    bool prev_audio_grounded = true;
    uint32_t audio_td_seq = 0;
    double prev_walk_lat = 0.0, prev_walk_lon = 0.0;
    bool prev_walk_valid = false;
    auto push_audio_state = [&](double dt_sec, bool menu_or_paused) {
        if (!audio.is_active()) return;
        AudioState as{};
        as.paused = menu_or_paused;
        as.walking = frontend.walking();
        as.master_volume = std::clamp(ac_hud.graphics_settings().audio_volume_percent * 0.01f, 0.0f, 1.0f);
        const auto &st = ac_hud.state();
        const auto &jsb = ac_hud.jsb_state();
        const auto &sys = ac_hud.systems();
        const auto &air = ac_hud.last_air();
        as.in_cockpit = !as.walking && st.view_mode == 0;
        as.engine_running = sys.is_engine_running() && jsb.rpm > 120.0;
        as.starter_cranking = sys.is_starter_cranking();
        as.sputtering = sys.is_sputtering() || sys.is_fuel_starved();
        as.rpm = static_cast<float>(jsb.rpm);
        as.throttle = static_cast<float>(st.throttle);
        as.ias_kt = static_cast<float>(jsb.ias_kt);
        as.ground_speed_mps = static_cast<float>(jsb.ground_speed_mps);
        as.on_ground = jsb.on_ground;
        as.stall_warn = jsb.stall_warn || jsb.stalled;
        as.flaps_moving = sys.is_flaps_moving();
        as.avionics_powered = sys.is_avionics_bus_powered(0) || sys.is_avionics_bus_powered(1);
        as.wind_speed_mps = static_cast<float>(std::hypot(air.wind_east_mps, air.wind_north_mps));
        as.turb_rms = static_cast<float>(air.turb_rms);
        as.precip_mm_hr = static_cast<float>(air.precipitation_mm_hr);
        as.snow_factor = air.snow_factor;
        as.lightning_flash = air.lightning_flash;
        as.brake_amount = (win.key_down(KEY_X) || st.parking_brake) ? 0.8f : 0.0f;
        if (const AirportInfo *ap = pack.find_nearest_airport(st.lat_deg, st.lon_deg)) {
            as.ground_roughness = (ap->surface.find("grass") != std::string::npos ||
                                   ap->surface.find("turf") != std::string::npos) ? 0.24f : 0.06f;
        }
        if (!as.walking && !menu_or_paused && !prev_audio_grounded && jsb.on_ground) {
            ++audio_td_seq;
            as.touchdown_sink_mps = static_cast<float>(std::max(0.4, -jsb.vsi_mps));
        }
        prev_audio_grounded = jsb.on_ground;
        as.touchdown_seq = audio_td_seq;

        if (as.walking) {
            const auto &w = frontend.walker();
            const auto &p = frontend.parking_spot();
            const double de = (p.lon - w.lon) * pack.anchor_m_lon();
            const double dn = (p.lat - w.lat) * pack.anchor_m_lat();
            as.aircraft_dist_m = static_cast<float>(std::hypot(de, dn));
            const double hdg_r = w.heading * DEG2RAD_D;
            const double right_e = std::cos(hdg_r), right_n = -std::sin(hdg_r);
            const double denom = std::max(1.0, std::hypot(de, dn));
            as.aircraft_pan = static_cast<float>(std::clamp((de * right_e + dn * right_n) / denom, -1.0, 1.0));
            if (prev_walk_valid && dt_sec > 1e-4) {
                const double we = (w.lon - prev_walk_lon) * pack.anchor_m_lon();
                const double wn = (w.lat - prev_walk_lat) * pack.anchor_m_lat();
                as.walk_speed_mps = static_cast<float>(std::hypot(we, wn) / dt_sec);
            }
            prev_walk_lat = w.lat;
            prev_walk_lon = w.lon;
            prev_walk_valid = true;
        } else {
            prev_walk_valid = false;
            as.aircraft_dist_m = (st.view_mode == 0) ? 0.0f : (st.view_mode == 1 ? st.chase_dist_m : length(st.free_cam_off_m));
        }
        audio.update_state(as);
    };

    const std::filesystem::path route_video_path = profile_file.empty()
        ? std::filesystem::path{} : (profile_file.parent_path() / "last_flight.nsroute");
    RouteVideoRecording recorded_route;
    double last_record_sim_t = -1.0;
    {
        RouteVideoRecording probe;
        if (!route_video_path.empty() && load_route_video(route_video_path, probe) && probe.frames.size() >= 2) {
            frontend.set_replay_available(true);
        }
    }
    auto exit_route_replay = [&]() {
        if (!route_play.active) return;
        route_play.active = false;
        ac_hud.state() = route_play.saved_state;
        ac_hud.state().menu_open = route_play.saved_menu_open;
        sim_time = route_play.saved_sim_time;
        if (nspain::haversine_m(ac_hud.state().lat_deg, ac_hud.state().lon_deg, pack.anchor_lat(), pack.anchor_lon()) > 35000.0) {
            transition_world(ac_hud.state().lat_deg, ac_hud.state().lon_deg);
        }
        sky_clouds.reset_temporal_history();
    };

    SimulationClock clock;
    while (!win.should_close()) {
        double now = PlatformWindow::now_sec();
        const double frame_sec = now - last_t;
        double dt = std::clamp(frame_sec, 0.001, 0.05);
        last_t = now;

        frame_hist[hist_pos] = frame_sec * 1000.0;
        hist_pos = (hist_pos + 1) % static_cast<int>(frame_hist.size());
        std::vector<double> tmp = frame_hist;
        std::sort(tmp.begin(), tmp.end());
        cur_avg_ms = std::accumulate(tmp.begin(), tmp.end(), 0.0) / tmp.size();
        cur_p95_ms = tmp[static_cast<size_t>(tmp.size() * 0.95)];
        cur_fps = 1000.0 / std::max(0.1, cur_avg_ms);

        // Never warp pointer on X11/XWayland (XWarpPointer without grab causes runaway delta drift on Wayland);
        // mouse yoke uses zero-warp cursor-anchored deflection relative to the last center/click position.
        win.set_relative_mouse(false);
        win.poll_events();
        const auto request = frontend.take_request();
        switch (request) {
            case GameFrontend::Request::Walk:
                if (const auto *airport = frontend.parked_airport()) {
                    enter_region(*airport);
                    const float wx_est = std::clamp((sky_clouds.atmo().cloud_cov - 0.16f) / 0.78f, 0.0f, 1.0f);
                    const LocalAirSample park_wx = reg_atmo.sample_at(airport->lat, airport->lon, airport->elev_m + 2.0,
                                                                      sky_clouds.atmo().sun_dir, sim_time, wx_est);
                    frontend.open_airport(*airport, &park_wx);
                    prepare_airfield(*airport);
                }
                break;
            case GameFrontend::Request::Board: {
                const float wx_est = std::clamp((sky_clouds.atmo().cloud_cov - 0.16f) / 0.78f, 0.0f, 1.0f);
                const auto &ps = frontend.parking_spot();
                const LocalAirSample park_wx = reg_atmo.sample_at(ps.lat, ps.lon, ps.alt,
                                                                  sky_clouds.atmo().sun_dir, sim_time, wx_est);
                frontend.board(ac_hud, park_wx.oat_c);
                recorded_route = {};
                if (const auto *ap = frontend.parked_airport()) recorded_route.title = ap->id + " Flight";
                else recorded_route.title = "OpenSky Flight";
                last_record_sim_t = -1.0;
                sky_clouds.reset_temporal_history();
                break;
            }
            case GameFrontend::Request::Park:
                if (frontend.can_park(ac_hud)) {
                    if (const auto *airport = pack.find_nearest_airport(ac_hud.state().lat_deg, ac_hud.state().lon_deg)) {
                        if (recorded_route.frames.size() >= 10 && !route_video_path.empty()) {
                            if (const auto *dep = frontend.parked_airport()) {
                                recorded_route.title = dep->id + " -> " + airport->id;
                            }
                            if (save_route_video(route_video_path, recorded_route)) frontend.set_replay_available(true);
                        }
                        enter_region(*airport);
                        frontend.park(ac_hud, *airport);
                        prepare_airfield(*airport);
                    }
                }
                break;
            case GameFrontend::Request::Editor:
                std::system("bash ./run_builder.sh >/dev/null 2>&1 &");
                break;
            case GameFrontend::Request::WatchReplay: {
                RouteVideoRecording loaded;
                if (recorded_route.frames.size() >= 10) loaded = recorded_route;
                else if (!route_video_path.empty()) load_route_video(route_video_path, loaded);
                if (loaded.frames.size() >= 2) {
                    route_play.saved_state = ac_hud.state();
                    route_play.saved_sim_time = sim_time;
                    route_play.saved_walking = frontend.walking();
                    route_play.saved_menu_open = ac_hud.state().menu_open;
                    route_play.active_rec = std::move(loaded);
                    route_play.first_t = route_play.active_rec.frames.front().sim_t;
                    route_play.dur_t = route_play.active_rec.duration_sec();
                    route_play.cur_t = 0.0;
                    route_play.playing = true;
                    route_play.speed = 1.0f;
                    route_play.seek_t = -1.0;
                    route_play.last_action = GameFrontend::ReplayAction::None;
                    route_play.active = true;
                    ac_hud.state().menu_open = false;
                    ac_hud.state().paused = true;
                    ac_hud.state().view_mode = 1; // Cinematic chase orbit view
                    ac_hud.state().look_yaw_deg = 22.0f;
                    ac_hud.state().look_pitch_deg = -10.0f;
                    transition_world(route_play.active_rec.frames.front().lat_deg,
                                     route_play.active_rec.frames.front().lon_deg);
                    sky_clouds.reset_temporal_history();
                }
                break;
            }
            case GameFrontend::Request::Quit: win.request_close(); break;
            case GameFrontend::Request::None: break;
        }
        if (win.should_close()) break;

        if (route_play.active) {
            if (win.key_pressed(KEY_ESC) || route_play.last_action == GameFrontend::ReplayAction::Exit) {
                exit_route_replay();
                continue;
            }
            if (win.key_pressed(KEY_SPACE) || route_play.last_action == GameFrontend::ReplayAction::TogglePlay) {
                if (route_play.cur_t >= route_play.dur_t - 0.05 && !route_play.playing) route_play.cur_t = 0.0;
                route_play.playing = !route_play.playing;
            }
            if (win.key_pressed(KEY_1) || route_play.last_action == GameFrontend::ReplayAction::Speed05) route_play.speed = 0.5f;
            if (win.key_pressed(KEY_2) || route_play.last_action == GameFrontend::ReplayAction::Speed1) route_play.speed = 1.0f;
            if (win.key_pressed(KEY_3) || route_play.last_action == GameFrontend::ReplayAction::Speed2) route_play.speed = 2.0f;
            if (win.key_pressed(KEY_4) || route_play.last_action == GameFrontend::ReplayAction::Speed4) route_play.speed = 4.0f;
            if (win.key_pressed(KEY_LEFT) || route_play.last_action == GameFrontend::ReplayAction::Back10) {
                route_play.cur_t = std::max(0.0, route_play.cur_t - 10.0);
            }
            if (win.key_pressed(KEY_RIGHT) || route_play.last_action == GameFrontend::ReplayAction::Fwd10) {
                route_play.cur_t = std::min(route_play.dur_t, route_play.cur_t + 10.0);
            }
            if (route_play.seek_t >= 0.0) {
                route_play.cur_t = std::clamp(route_play.seek_t, 0.0, route_play.dur_t);
                route_play.seek_t = -1.0;
            }
            if (win.key_pressed(KEY_C) || route_play.last_action == GameFrontend::ReplayAction::SwitchView) {
                ac_hud.state().view_mode = (ac_hud.state().view_mode == 1) ? 0 : 1;
            }
            if (win.key_pressed(KEY_R)) {
                ac_hud.state().look_yaw_deg = 0.0f;
                ac_hud.state().look_pitch_deg = 0.0f;
            }
            if (win.mouse_right_down()) {
                ac_hud.state().look_yaw_deg = std::clamp(ac_hud.state().look_yaw_deg - win.mouse_dx() * 0.22f, -175.0f, 175.0f);
                ac_hud.state().look_pitch_deg = std::clamp(ac_hud.state().look_pitch_deg - win.mouse_dy() * 0.18f, -75.0f, 75.0f);
            }
            route_play.last_action = GameFrontend::ReplayAction::None;
            if (route_play.playing) {
                route_play.cur_t += dt * static_cast<double>(route_play.speed);
                if (route_play.cur_t >= route_play.dur_t) {
                    route_play.cur_t = route_play.dur_t;
                    route_play.playing = false;
                }
            }
            const double sample_t = route_play.first_t + route_play.cur_t;
            const RouteVideoFrame rf = sample_route_video(route_play.active_rec, sample_t);
            const RouteVideoFrame rf2 = sample_route_video(route_play.active_rec,
                                                           std::min(route_play.first_t + route_play.dur_t, sample_t + 0.5));
            const double vel_dt = std::max(0.05, rf2.sim_t - rf.sim_t);
            double r_mlat = 111132.0, r_mlon = 82600.0;
            nspain::meters_per_degree(rf.lat_deg, r_mlat, r_mlon);
            FlightSimState &rs = ac_hud.state();
            rs.lat_deg = rf.lat_deg;
            rs.lon_deg = rf.lon_deg;
            rs.alt_msl_m = rf.alt_msl_m;
            rs.ground_m = pack.sample_elevation(rf.lat_deg, rf.lon_deg);
            rs.heading_deg = rf.heading_deg;
            rs.pitch_deg = rf.pitch_deg;
            rs.roll_deg = rf.roll_deg;
            rs.throttle = rf.throttle;
            rs.aileron = rf.aileron;
            rs.elevator = rf.elevator;
            rs.rudder = rf.rudder;
            rs.speed_mps = rf.ias_kt * 0.514444;
            rs.vel_east_mps = (rf2.lon_deg - rf.lon_deg) * r_mlon / vel_dt;
            rs.vel_north_mps = (rf2.lat_deg - rf.lat_deg) * r_mlat / vel_dt;
            rs.visual_rpm_override = rf.rpm;
            rs.visual_flaps_deg = rf.flaps_deg;
            sim_time = sample_t;
            update_environment();
            step_flight_world_transition(rs.lat_deg, rs.lon_deg, rs.vel_east_mps, rs.vel_north_mps);
            render_one_frame(false, 1, "", 50.0f, true);
            push_audio_state(dt, true);
            const double eff_period = std::max(frame_period, 1.0 / thermal_fps_cap);
            const double spare = eff_period - (PlatformWindow::now_sec() - now);
            if (spare > 0.0005) std::this_thread::sleep_for(std::chrono::duration<double>(spare));
            continue;
        }

        if (frontend.walking() && win.key_pressed(KEY_ESC) && !airfield_pages.empty()) airfield_pages.clear();
        if (frontend.walking() && airfield_pages.empty()) frontend.step_walk(win, dt);
        if (frontend.walking() && !airfield_pages.empty()) {
            streamer.begin_frame(++frame_idx);
            streamer.pump_gpu_uploads(-10.0f, 2, 0.8);
            bool ready = true;
            for (uint64_t key : airfield_pages) {
                if (streamer.find_resident(key, float(sim_time)) >= 0) continue;
                streamer.request_page(key, 20000.0f - QuadKey::unpack(key).lod * 500.0f);
                ready = false;
            }
            if (ready) airfield_pages.clear();
        }
        if (ac_hud.state().menu_open && (!frontend.walking() || !airfield_pages.empty())) {
            clock.pause();
            if (now - menu_clock_refresh >= 0.5) {
                const FlightSimState &menu_state = ac_hud.state();
                const bool menu_walking = frontend.walking();
                const double menu_lat = menu_walking ? frontend.walker().lat : menu_state.lat_deg;
                const double menu_lon = menu_walking ? frontend.walker().lon : menu_state.lon_deg;
                const double menu_ground = menu_walking ? frontend.walker().alt - 1.68 : menu_state.ground_m;
                const WeatherSnapshot *menu_weather = reg_atmo.has_weather() ? &reg_atmo.weather() : nullptr;
                const double cycle_elapsed = std::max(0.0, now - cycle_clock_origin);
                sky_clouds.update_solar_and_weather(menu_lat, menu_lon, menu_ground, tod_slot, sim_time,
                    regime_fog_mult(), wx_advance, menu_weather, season_override, cycle_elapsed);
                reg_atmo.update_field(sky_clouds.atmo().sun_dir, sim_time, false, wx_advance);
                menu_clock_refresh = now;
            }
            frontend.draw_menu(ac_hud, sky_clouds, win, !airfield_pages.empty(), tod_slot, &reg_atmo, sim_time);
            quality = ac_hud.graphics_settings().quality_preset;
            const auto viewport = win.presentation_rect(output_w, output_h);
            sky_clouds.present(viewport.x, viewport.y, viewport.w, viewport.h, win.width(), win.height());
            win.swap_buffers();
            const auto provider = ac_hud.graphics_settings().terrain_streaming ? OnlineOrthoProvider::PNOA : OnlineOrthoProvider::OFF;
            if (pack.online_provider() != provider) {
                pack.set_online_provider(provider);
                streamer.invalidate_close_pages(true);
            }
            const double menu_period = std::max(1.0 / 45.0, 1.0 / thermal_fps_cap);
            const double spare = menu_period - (PlatformWindow::now_sec() - now);
            push_audio_state(dt, true);
            if (spare > 0.0005) std::this_thread::sleep_for(std::chrono::duration<double>(spare));
            continue;
        }
        if (frontend.walking()) {
            if (win.key_pressed(KEY_U)) audio.set_muted(!audio.is_muted());
            sim_time += dt;
            update_environment();
            render_one_frame(true);
            push_audio_state(dt, false);
            const double walk_period = std::max(frame_period, 1.0 / thermal_fps_cap);
            const double spare = walk_period - (PlatformWindow::now_sec() - now);
            if (spare > 0.0005) std::this_thread::sleep_for(std::chrono::duration<double>(spare));
            continue;
        }

        if (win.key_pressed(KEY_T)) frontend.toggle_tutorial();
        if (frontend.active_flight() && !ac_hud.state().menu_open && win.key_pressed(KEY_TAB))
            frontend.toggle_kneeboard();
        auto set_quality_preset = [&](int q) {
            quality = std::clamp(q, 0, 3);
            auto s = ac_hud.graphics_settings();
            s.quality_preset = quality;
            ac_hud.set_graphics_settings(s);
            save_graphics_settings(s);
        };
        if (win.key_pressed(KEY_F1)) set_quality_preset(0);
        if (win.key_pressed(KEY_F2)) set_quality_preset(1);
        if (win.key_pressed(KEY_F3)) set_quality_preset(2);
        if (win.key_pressed(KEY_F4)) set_quality_preset(3);
        if (win.key_pressed(KEY_F5)) profiler.set_overlay(!profiler.overlay());
        if (win.key_pressed(KEY_F6)) weather_debug_level = (weather_debug_level + 2) % (nspain::NSWX_LEVELS + 1) - 1;
        if (win.key_pressed(KEY_F7)) physics_debug = !physics_debug;
        if (win.key_pressed(KEY_N)) {
            season_override = (season_override < 0) ? 0 : (season_override < 3 ? season_override + 1 : -1);
            reg_atmo.set_season(season_override);
            const char *season_name = (season_override < 0)
                ? "AUTO UTC" : WeatherLibrary::season_name(season_override);
            std::printf("[SEASON] Preview set to %s\n", season_name);
        }
        if (win.key_pressed(KEY_L)) {
            tod_slot = (tod_slot < 0) ? 2 : -1;
            graphics_settings.time_of_day = tod_slot;
            save_graphics_settings(graphics_settings);
        }
        if (win.key_pressed(KEY_P)) {
            tod_slot = (tod_slot < 0 || tod_slot >= 10) ? 0 : (tod_slot + 1);
            graphics_settings.time_of_day = tod_slot;
            save_graphics_settings(graphics_settings);
        }
        if (win.key_pressed(KEY_B)) {
            wx_advance = (wx_advance > 0.85f) ? 0.05f : std::min(0.95f, wx_advance + 0.30f);
            reg_atmo.set_manual_weather_override(true); // Manual wx cycles temporarily take over from live feed.
        }
        if (win.key_pressed(KEY_O)) { // Toggle distance-tiered PNOA online ortho streaming.
            auto settings = ac_hud.graphics_settings();
            settings.terrain_streaming = !settings.terrain_streaming;
            ac_hud.set_graphics_settings(settings);
            save_graphics_settings(settings);
            pack.set_online_provider(settings.terrain_streaming ? OnlineOrthoProvider::PNOA : OnlineOrthoProvider::OFF);
            streamer.invalidate_close_pages(true);
        }
        if (win.key_pressed(KEY_M)) {
            if (!reg_atmo.manual_weather_override()) {
                reg_atmo.set_manual_weather_override(true);
                reg_atmo.set_regime(WindRegime::PYRENEAN_THERMAL);
            } else if (static_cast<int>(reg_atmo.regime()) + 1 >= static_cast<int>(WindRegime::COUNT)) {
                reg_atmo.set_manual_weather_override(false);
                reg_atmo.set_regime(WindRegime::PYRENEAN_THERMAL);
            } else {
                reg_atmo.cycle_regime();
            }
            if (reg_atmo.regime() == WindRegime::CONVECTIVE_STORM) wx_advance = 0.88f;
            else if (reg_atmo.regime() == WindRegime::WINTER_ICING) wx_advance = 0.78f;
            std::printf("[WX] Active weather regime: %s%s\n", reg_atmo.regime_name(),
                        reg_atmo.manual_weather_override() ? "" : " (Live/Auto)");
        }
        if (win.key_pressed(KEY_H)) show_diagnostics = !show_diagnostics;
        if (win.key_pressed(KEY_U)) {
            audio.set_muted(!audio.is_muted());
            std::printf("[AUDIO] %s\n", audio.is_muted() ? "Muted" : "Unmuted");
        }
        const double simulated = clock.consume(frame_sec);
        sim_time += simulated;
        update_environment();
        step_flight(simulated);
        frontend.step_flight(ac_hud, simulated);
        if (!ac_hud.state().menu_open && !ac_hud.state().paused && simulated > 0.0 &&
            (last_record_sim_t < 0.0 || sim_time - last_record_sim_t >= 0.20) &&
            recorded_route.frames.size() < 72000) {
            const auto &fs_rec = ac_hud.jsb_state();
            const auto &st_rec = ac_hud.state();
            recorded_route.frames.push_back({
                sim_time, fs_rec.lat_deg, fs_rec.lon_deg, fs_rec.alt_msl_m,
                fs_rec.heading_deg, fs_rec.pitch_deg, fs_rec.roll_deg,
                fs_rec.ias_kt, fs_rec.rpm, ac_hud.systems().get_flaps_actual_deg(),
                st_rec.throttle, st_rec.aileron, st_rec.elevator, st_rec.rudder
            });
            last_record_sim_t = sim_time;
            if (recorded_route.frames.size() >= 10) frontend.set_replay_available(true);
        }
        // Asynchronously stage & swap local terrain anchor on long cross-country legs with NO main-thread freeze.
        if (step_flight_world_transition(ac_hud.state().lat_deg, ac_hud.state().lon_deg,
                                         ac_hud.state().vel_east_mps, ac_hud.state().vel_north_mps)) {
            last_t = PlatformWindow::now_sec();
        }
        const bool show_flight_overlays = frontend.walking() || route_play.active ||
            ac_hud.graphics_settings().show_flight_hud || frontend.tutorial_open();
        render_one_frame(show_flight_overlays);
        push_audio_state(dt, ac_hud.state().paused || ac_hud.state().menu_open);
        // Closed-loop frame pacing caps GPU/CPU load so long flights remain cool (<80 C).
        const double eff_period = std::max(frame_period, 1.0 / thermal_fps_cap);
        const double spare = eff_period - (PlatformWindow::now_sec() - now);
        if (spare > 0.0005) std::this_thread::sleep_for(std::chrono::duration<double>(spare));
    }

    cancel_async_crossing();
    if (recorded_route.frames.size() >= 10 && !route_video_path.empty()) {
        save_route_video(route_video_path, recorded_route);
    }
    audio.shutdown();
    frontend.save_profile();
    profiler.finish();
    return 0;
}
