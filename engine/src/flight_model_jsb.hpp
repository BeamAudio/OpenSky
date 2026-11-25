#pragma once

#include <array>
#include <functional>
#include <memory>
#include <string>

namespace nspain {

struct FlightControls {
    double aileron = 0, elevator = 0, rudder = 0, elevator_trim = 0;
    double throttle = 0.62, mixture = 1.0, flaps = 0;
    double brake_left = 0, brake_right = 0;
};

struct FlightEnvironment {
    double air_temp_c = 15, qnh_hpa = 1013.25;
    double relative_humidity = 0.50;
    double terrain_elev_m = 0;
    double wind_north_mps = 0, wind_east_mps = 0, wind_down_mps = 0;
    double p_turb_rps = 0, q_turb_rps = 0, r_turb_rps = 0;
    double airframe_ice_factor = 0, airframe_ice_kg = 0, rain_drag_factor = 0;
};

struct FlightGround {
    double elev_m = 0.0;
    double friction = 0.82;
    double roughness = 0.03;
    bool solid = true;
};

struct FlightEngineInputs {
    double left_fuel_gal = 21, right_fuel_gal = 21;
    int magnetos = 3;
    double power_multiplier = 1.0;
    bool starter = false, fuel_starved = false, running = true;
};

struct FlightState {
    double lat_deg = 0, lon_deg = 0, alt_msl_m = 0, agl_m = 0;
    double heading_deg = 0, pitch_deg = 0, roll_deg = 0;
    double vel_north_mps = 0, vel_east_mps = 0, ground_speed_mps = 0, vsi_mps = 0;
    double air_north_mps = 0, air_east_mps = 0, air_up_mps = 0;
    double ias_kt = 0, tas_kt = 0;
    double p_rps = 0, q_rps = 0, r_rps = 0;
    double alpha_deg = 0, beta_deg = 0, cl = 0, cd = 0;
    double g_load = 1, air_density_kg_m3 = 1.225, density_alt_ft = 0;
    double rpm = 0, thrust_n = 0, shaft_hp = 0, fuel_flow_gph = 0, weight_lb = 0;
    bool on_ground = false, stalled = false, stall_warn = false, engine_running = false;
    std::array<bool, 3> wheel_wow{};
    std::array<double, 3> wheel_comp_m{};
    // Body coordinates with X forward, Y right and Z down.
    std::array<std::array<double, 3>, 3> wheel_offset_m{};
};

class FlightModel {
public:
    using GroundProvider = std::function<FlightGround(double lat_deg, double lon_deg)>;
    static constexpr double step_sec = 1.0 / 120.0;

    FlightModel();
    ~FlightModel();
    bool load(const std::string &aircraft_dir);
    void set_ground_provider(GroundProvider provider);
    void set_controls(const FlightControls &controls) { controls_ = controls; }
    void set_environment(const FlightEnvironment &env) { env_ = env; }
    void set_engine(const FlightEngineInputs &engine) { engine_ = engine; }
    bool initialize(double lat_deg, double lon_deg, double altitude_m, double heading_deg, double cas_kt, bool on_ground);
    bool advance(double dt);
    void apply_impact_impulse(double push_east_m, double push_up_m, double push_north_m,
                              double vel_east_mps, double vel_up_mps, double vel_north_mps,
                              double p_rps, double q_rps, double r_rps);
    const FlightControls &controls() const { return controls_; }
    const FlightState &state() const { return state_; }
    static const std::string &version();

private:
    class Impl;
    void apply_controls();
    void apply_environment(double altitude_m);
    void apply_engine();
    void read_state();

    // The executive's ground callback holds references to these inputs.
    GroundProvider ground_;
    FlightControls controls_;
    FlightEnvironment env_;
    FlightEngineInputs engine_;
    FlightState state_;
    double accumulator_ = 0;
    double neutral_aileron_ = 0, neutral_rudder_ = 0;
    std::unique_ptr<Impl> impl_;
};

} // namespace nspain
