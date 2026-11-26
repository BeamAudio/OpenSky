#include "../src/c172_systems.hpp"
#include "../src/flight_model_jsb.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <iostream>

using namespace nspain;

namespace {

void configure(FlightModel &flight) {
    flight.set_ground_provider([](double, double) { return FlightGround{500.0, 0.82, 0.03, true}; });
    FlightEnvironment weather{};
    weather.terrain_elev_m = 500.0;
    weather.air_temp_c = 10.0;
    weather.relative_humidity = 0.62;
    weather.qnh_hpa = 1013.25;
    flight.set_environment(weather);
    FlightEngineInputs engine{};
    engine.running = true;
    flight.set_engine(engine);
    FlightControls controls{};
    controls.throttle = 0.72;
    controls.mixture = 0.90;
    flight.set_controls(controls);
    assert(flight.load("game/aircraft/c172s"));
}

void test_trimmed_flight_and_air_response() {
    FlightModel flight;
    configure(flight);
    assert(flight.initialize(42.0810, -0.3234, 1500.0, 90.0, 100.0, false));

    const FlightState initial = flight.state();
    assert(std::isfinite(initial.ias_kt) && initial.ias_kt > 80.0 && initial.ias_kt < 125.0);
    assert(initial.weight_lb > 2100.0 && initial.weight_lb < 2500.0);
    assert(initial.rpm > 1400.0 && initial.rpm <= 2800.0);
    assert(initial.air_density_kg_m3 > 0.7 && initial.air_density_kg_m3 < 1.4);

    for (int i = 0; i < 120; ++i) assert(flight.advance(1.0 / 60.0));
    const FlightState cruise = flight.state();
    assert(std::isfinite(cruise.lat_deg) && std::isfinite(cruise.lon_deg));
    assert(std::isfinite(cruise.alt_msl_m) && std::isfinite(cruise.roll_deg));
    assert(cruise.ground_speed_mps > 25.0 && cruise.ground_speed_mps < 80.0);
    assert(cruise.lat_deg > 42.07 && cruise.lat_deg < 42.10);
    assert(cruise.lon_deg > -0.33 && cruise.lon_deg < -0.30);

    FlightControls controls = flight.controls();
    controls.aileron = 0.18;
    flight.set_controls(controls);
    for (int i = 0; i < 60; ++i) assert(flight.advance(1.0 / 60.0));
    assert(std::isfinite(flight.state().p_rps));
    assert(std::abs(flight.state().roll_deg - cruise.roll_deg) > 0.05);

    std::cout << "[PASS] Upstream JSBSim C172S trim, 120 Hz substeps, geodetic propagation and control response\n";
    std::cout << "       IAS " << cruise.ias_kt << " kt | altitude " << cruise.alt_msl_m
              << " m | roll " << cruise.roll_deg << " deg | RPM " << cruise.rpm << "\n";
}

void test_dem_contact_and_brakes() {
    FlightModel flight;
    configure(flight);
    FlightEngineInputs engine{};
    engine.running = false;
    engine.magnetos = 0;
    flight.set_engine(engine);
    FlightControls controls{};
    controls.throttle = 0.0;
    controls.brake_left = controls.brake_right = 1.0;
    flight.set_controls(controls);
    assert(flight.initialize(42.0810, -0.3234, 501.30, 90.0, 25.0, true));
    for (int i = 0; i < 120; ++i) assert(flight.advance(1.0 / 60.0));

    const FlightState &state = flight.state();
    const int wheels_down = static_cast<int>(state.wheel_wow[0]) + static_cast<int>(state.wheel_wow[1]) +
                            static_cast<int>(state.wheel_wow[2]);
    assert(state.on_ground && wheels_down >= 2);
    assert(state.ground_speed_mps < 25.0 * 0.514444);
    for (double component : state.wheel_offset_m[1]) assert(std::isfinite(component));

    std::cout << "[PASS] Terrain callback, wheel contact and differential brake path\n";
}

void test_grass_takeoff_roll_and_liftoff() {
    FlightModel flight;
    flight.set_ground_provider([](double, double) { return FlightGround{900.0, 0.62, 0.18, true}; });
    FlightEnvironment weather{};
    weather.terrain_elev_m = 900.0;
    weather.air_temp_c = 15.0;
    weather.relative_humidity = 0.55;
    weather.qnh_hpa = 1013.25;
    flight.set_environment(weather);
    FlightEngineInputs engine{};
    engine.running = true;
    flight.set_engine(engine);
    FlightControls controls{};
    controls.throttle = 0.0;
    controls.mixture = 1.0;
    controls.brake_left = controls.brake_right = 1.0;
    flight.set_controls(controls);
    assert(flight.load("game/aircraft/c172s"));
    assert(flight.initialize(42.5225, 0.4811, 901.30, 340.0, 0.0, true));

    // Sit at idle briefly on grass with parking brake, then release brake and apply full throttle.
    for (int i = 0; i < 180; ++i) assert(flight.advance(1.0 / 60.0));
    assert(flight.state().rpm > 550.0);

    controls.brake_left = controls.brake_right = 0.0;
    controls.throttle = 1.0;
    flight.set_controls(controls);
    bool lifted_off = false;
    for (int i = 0; i < 1500; ++i) {
        if (flight.state().ias_kt >= 55.0) {
            controls.elevator = 0.30;
            flight.set_controls(controls);
        }
        assert(flight.advance(1.0 / 60.0));
        if (!flight.state().on_ground && flight.state().agl_m > 5.0 && flight.state().vsi_mps > 1.0) {
            lifted_off = true;
            break;
        }
    }
    assert(lifted_off);
    std::cout << "[PASS] Grass airfield (900m MSL) idle -> full-throttle takeoff roll & liftoff ("
              << flight.state().ias_kt << " kt, AGL " << flight.state().agl_m << " m)\n";
}

void test_ownership_care_and_cold_start_systems() {
    C172Systems sys;
    sys.reset_cold_and_dark();
    sys.set_pitot_cover(true);
    assert(!sys.is_engine_running());
    assert(sys.is_pitot_blocked());

    // Dead battery cannot crank starter
    sys.set_master_battery(true);
    sys.set_battery_soc(0.0);
    sys.set_magnetos(4);
    C172EnvStep step{};
    step.dt = 0.25;
    step.throttle01 = 0.1;
    step.mixture01 = 1.0;
    step.rpm = 0.0;
    step.ias_kt = 0.0;
    sys.update(step);
    assert(!sys.is_starter_cranking() && !sys.is_engine_running());

    // Charged battery + prime + crank starts engine and spring-returns magnetos to BOTH
    sys.set_battery_soc(0.95);
    sys.actuate_primer();
    sys.actuate_primer();
    sys.set_magnetos(4);
    for (int i = 0; i < 6; ++i) sys.update(step);
    assert(sys.is_engine_running());
    assert(sys.get_magnetos() == 3);

    // Cold-engine full-power abuse accelerates cylinder wear
    sys.set_engine_running_state(true, 4.0);
    sys.set_oil_state(7.4, 0.0);
    sys.set_wear_state(0.0, 0.0, 0.0);
    step.throttle01 = 0.95;
    step.rpm = 2400.0;
    step.ias_kt = 60.0;
    step.ambient_temp_c = 4.0;
    // Cool oil down by resetting cold-and-dark then starting
    sys.reset_cold_and_dark();
    sys.set_master_battery(true);
    sys.set_master_alternator(true);
    sys.set_battery_soc(0.95);
    sys.actuate_primer();
    sys.actuate_primer();
    sys.set_magnetos(4);
    for (int i = 0; i < 6; ++i) sys.update(step);
    assert(sys.is_engine_running() && sys.get_oil_temp_f() < 110.0);
    for (int i = 0; i < 20; ++i) sys.update(step);
    assert(sys.get_cylinder_wear() > 0.002);

    // Spark-plug fouling causes larger single-magneto run-up power drop than clean plugs
    sys.reset_to_running_default();
    sys.set_magnetos(1); // Right mag only
    step.throttle01 = 0.55;
    step.rpm = 1700.0;
    sys.set_wear_state(0.0, 0.0, 0.0);
    sys.update(step);
    const double clean_mag_pwr = sys.get_engine_power_mult();
    sys.set_wear_state(0.85, 0.0, 0.0);
    sys.update(step);
    const double fouled_mag_pwr = sys.get_engine_power_mult();
    assert(fouled_mag_pwr < clean_mag_pwr - 0.15);

    // Sump water causes periodic sputter dip
    sys.reset_to_running_default();
    sys.set_sump_water(0.8);
    bool saw_sputter = false;
    for (int i = 0; i < 24; ++i) {
        sys.update(step);
        if (sys.is_sputtering()) saw_sputter = true;
    }
    assert(saw_sputter);

    // Low oil degrades pressure and eventually causes oil starvation
    sys.reset_to_running_default();
    sys.set_oil_state(2.0, 12.0);
    sys.update(step);
    assert(sys.has_oil_starvation_failure());

    // 60 s of humid Pyrenean ground warm-up & taxi must NEVER choke takeoff power via carb ice or rapid plug fouling
    sys.reset_to_running_default();
    C172EnvStep taxi{};
    taxi.dt = 1.0; taxi.throttle01 = 0.14; taxi.mixture01 = 1.0; taxi.rpm = 950.0; taxi.ias_kt = 8.0;
    taxi.ambient_temp_c = 10.0; taxi.dewpoint_c = 8.5; taxi.cloud_cover_pct = 85.0;
    for (int i = 0; i < 60; ++i) sys.update(taxi);
    assert(sys.get_carb_ice_level() < 0.01 && sys.get_engine_power_mult() > 0.95);

    // Northern Spain VOR-DME reception & circuit-breaker gating
    sys.reset_to_running_default();
    sys.tune_nav_standby(1, 116.40); // STG Santiago VOR
    sys.swap_nav(1);
    sys.set_nav_obs_deg(1, 360);
    sys.update_navigation(42.850, -8.425, 1200.0); // ~4.6 NM south of STG
    assert(sys.get_nav_ident(1) == "STG" && sys.get_nav_to_from(1) == 1 && sys.get_nav_dme_nm(1) > 2.0);
    sys.set_breaker_pulled(CB_AVIONICS_BUS_1, true);
    sys.update_navigation(42.850, -8.425, 1200.0);
    assert(sys.get_nav_ident(1) == "---" && sys.get_nav_to_from(1) == 0);
    sys.set_breaker_pulled(CB_AVIONICS_BUS_1, false);

    // Flaps freeze immediately when CB_FLAPS is pulled
    sys.set_flaps_target_deg(20.0);
    C172EnvStep flap_step{};
    flap_step.dt = 0.5;
    sys.update(flap_step);
    const double mid_flaps = sys.get_flaps_actual_deg();
    assert(mid_flaps > 1.0 && mid_flaps < 20.0);
    sys.set_breaker_pulled(CB_FLAPS, true);
    sys.set_flaps_target_deg(30.0);
    sys.update(flap_step);
    assert(std::abs(sys.get_flaps_actual_deg() - mid_flaps) < 1e-6);

    std::cout << "[PASS] C172S ownership care: cold-and-dark start, pitot cover, cold wear, mag-drop fouling, sump sputter, low oil, VORs & breakers\n";
}

} // namespace

int main() {
    std::cout << "JSBSim upstream " << FlightModel::version() << " flight integration checks\n";
    test_trimmed_flight_and_air_response();
    test_dem_contact_and_brakes();
    test_grass_takeoff_roll_and_liftoff();
    test_ownership_care_and_cold_start_systems();
    return 0;
}
