#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace nspain {

enum CircuitBreakerId : int {
    CB_ALT_FIELD = 0,
    CB_AVIONICS_BUS_1 = 1,
    CB_AVIONICS_BUS_2 = 2,
    CB_FUEL_PUMP = 3,
    CB_FLAPS = 4,
    CB_INST_LIGHTS = 5,
    CB_BCN_LIGHT = 6,
    CB_LAND_LIGHT = 7,
    CB_NAV_LIGHT = 8,
    CB_STRB_LIGHT = 9,
    CB_PITOT_HEAT = 10,
    CB_TURN_COORD = 11,
    CB_COUNT = 12
};

enum FuelSelectorPos : int {
    FUEL_SEL_OFF = 0,
    FUEL_SEL_LEFT = 1,
    FUEL_SEL_BOTH = 2,
    FUEL_SEL_RIGHT = 3
};

enum XpdrMode : int {
    XPDR_OFF = 0,
    XPDR_STBY = 1,
    XPDR_ON = 2,
    XPDR_ALT = 3
};

struct C172EnvStep {
    double dt = 1.0 / 60.0;
    double throttle01 = 0.70;
    double mixture01 = 0.85;
    double rpm = 2300.0;
    double ias_kt = 100.0;
    double fuel_flow_demand_gph = 8.4;
    double ambient_temp_c = 12.0;
    double dewpoint_c = 4.0;
    double cloud_cover_pct = 30.0;
    double precip_mm = 0.0;
};

struct PistonSystemsConfig {
    double left_tank_capacity_gal = 26.5;
    double right_tank_capacity_gal = 26.5;
    double oil_capacity_quarts = 8.0;
    double max_flaps_deg = 30.0;
    double bus_voltage = 28.0;
};

class PistonAircraftSystems {
public:
    PistonAircraftSystems();
    ~PistonAircraftSystems() = default;

    void configure(const PistonSystemsConfig &config);
    const PistonSystemsConfig &configuration() const { return config_; }

    void reset_to_running_default();
    void reset_cold_and_dark();
    void update(const C172EnvStep &in);
    void update_navigation(double lat_deg, double lon_deg, double alt_msl_m);

    // GDScript-friendly update overload
    void step_systems(
        double dt,
        double throttle01,
        double mixture01,
        double rpm,
        double ias_kt,
        double fuel_flow_demand_gph,
        double ambient_temp_c,
        double dewpoint_c,
        double cloud_cover_pct,
        double precip_mm
    );

    // Circuit breakers (12 individual breakers: false=pushed in/closed, true=pulled/tripped/open)
    bool is_breaker_pulled(int cb_id) const;
    void set_breaker_pulled(int cb_id, bool pulled);
    bool toggle_breaker(int cb_id);
    void trip_breaker(int cb_id);
    bool is_circuit_powered(int cb_id) const;
    static const char *get_breaker_name(int cb_id);

    // Electrical switches
    void set_master_battery(bool on) { master_battery_ = on; }
    bool get_master_battery() const { return master_battery_; }

    void set_master_alternator(bool on) { master_alternator_ = on; }
    bool get_master_alternator() const { return master_alternator_; }

    void set_avionics_master(bool on) { avionics_master_ = on; }
    bool get_avionics_master() const { return avionics_master_; }

    void set_fuel_pump_switch(bool on) { fuel_pump_switch_ = on; }
    bool get_fuel_pump_switch() const { return fuel_pump_switch_; }
    bool is_fuel_pump_active() const;

    void set_pitot_heat_switch(bool on) { pitot_heat_switch_ = on; }
    bool get_pitot_heat_switch() const { return pitot_heat_switch_; }
    bool is_pitot_heat_active() const;

    void set_carb_heat(double v) { carb_heat_ = (v < 0.0) ? 0.0 : ((v > 1.0) ? 1.0 : v); }
    double get_carb_heat() const { return carb_heat_; }

    void set_magnetos(int pos) { magnetos_ = (pos < 0) ? 0 : ((pos > 4) ? 4 : pos); }
    int get_magnetos() const { return magnetos_; }

    void set_light_switch(int light_idx, bool on);
    bool get_light_switch(int light_idx) const;
    bool is_light_powered(int light_idx) const;

    // Altimeter Kollsman barometric pressure setting (hPa)
    void set_baro_hpa(double hpa);
    double get_baro_hpa() const { return baro_hpa_; }

    // 24V Electric Starter Motor state
    bool is_starter_cranking() const { return starter_cranking_; }
    bool is_flaps_moving() const { return flaps_moving_; }

    // Flaps (motor driven via CB_FLAPS)
    void set_flaps_target_deg(double deg);
    double get_flaps_target_deg() const { return flaps_target_deg_; }
    double get_flaps_actual_deg() const { return flaps_actual_deg_; }
    double get_flaps01() const { return config_.max_flaps_deg > 0.0 ? flaps_actual_deg_ / config_.max_flaps_deg : 0.0; }

    // Fuel system
    void set_fuel_selector(int pos);
    int get_fuel_selector() const { return fuel_selector_; }
    void cycle_fuel_selector();
    void set_fuel_shutoff_pulled(bool pulled) { fuel_shutoff_pulled_ = pulled; }
    bool get_fuel_shutoff_pulled() const { return fuel_shutoff_pulled_; }
    void set_tank_gallons(double left_gal, double right_gal);
    double get_left_tank_gal() const { return fuel_left_gal_; }
    double get_right_tank_gal() const { return fuel_right_gal_; }
    double get_total_fuel_lbs() const { return (fuel_left_gal_ + fuel_right_gal_) * 6.0; }
    void actuate_primer();
    int get_primer_strokes() const { return primer_strokes_; }
    bool is_fuel_starved() const { return fuel_starved_; }

    // Electrical outputs
    double get_bus_volts() const { return bus_volts_; }
    double get_ammeter_amps() const { return ammeter_amps_; }
    double get_load_amps() const { return load_amps_; }
    double get_battery_soc() const { return battery_soc_; }
    bool get_low_volts_annunciator() const { return low_volts_annunciator_; }

    // Icing, thermodynamics, wear & failure outputs
    double get_carb_ice_level() const { return carb_ice_level_; }
    double get_airframe_ice_kg() const { return airframe_ice_kg_; }
    bool is_pitot_blocked() const { return pitot_blocked_ || pitot_cover_on_; }
    bool is_pitot_cover_on() const { return pitot_cover_on_; }
    void set_pitot_cover(bool on) { pitot_cover_on_ = on; }
    double get_cht_f() const { return cht_f_; }
    double get_egt_f() const { return egt_f_; }
    double get_oil_temp_f() const { return oil_temp_f_; }
    double get_oil_press_psi() const { return oil_press_psi_; }
    double get_oil_quarts() const { return oil_quarts_; }
    double get_oil_hours() const { return oil_hours_; }
    double get_vacuum_inhg() const { return vacuum_inhg_; }
    double get_fuel_flow_gph() const { return actual_fuel_flow_gph_; }
    double get_spark_fouling() const { return spark_fouling_; }
    double get_shock_cooling_wear() const { return shock_cooling_wear_; }
    double get_cylinder_wear() const { return cylinder_wear_; }
    double get_sump_water() const { return sump_water_; }
    bool is_sputtering() const { return sump_sputter_active_; }
    bool has_overtemp_failure() const { return overtemp_failure_; }
    bool has_oil_starvation_failure() const { return oil_starvation_failure_; }
    bool is_engine_running() const { return engine_running_; }
    double get_engine_power_mult() const { return engine_power_mult_; }

    // Ownership persistence & maintenance hooks
    void set_battery_soc(double soc);
    void set_oil_state(double quarts, double hours);
    void set_wear_state(double spark_fouling, double shock_cooling, double cylinder_wear);
    void set_sump_water(double water);
    void set_engine_running_state(bool running, double ambient_temp_c = 15.0);
    void set_flaps_immediate_deg(double deg);
    void clear_failures() { overtemp_failure_ = false; oil_starvation_failure_ = false; }

    // Avionics stack (Dual COM1/COM2, Dual NAV1/NAV2, Mode C/S Transponder)
    void swap_com(int idx);
    void tune_com_standby(int idx, double freq_mhz);
    void step_com_standby(int idx, double delta_mhz);
    double get_com_active(int idx) const;
    double get_com_standby(int idx) const;

    void swap_nav(int idx);
    void tune_nav_standby(int idx, double freq_mhz);
    void step_nav_standby(int idx, double delta_mhz);
    double get_nav_active(int idx) const;
    double get_nav_standby(int idx) const;
    void set_nav_obs_deg(int idx, int deg);
    int get_nav_obs_deg(int idx) const;
    double get_nav_cdi(int idx) const;
    int get_nav_to_from(int idx) const;
    double get_nav_dme_nm(int idx) const;
    std::string get_nav_ident(int idx) const;

    void set_xpdr_mode(int mode);
    int get_xpdr_mode() const { return xpdr_mode_; }
    void cycle_xpdr_mode();
    void set_xpdr_squawk(int octal_code);
    int get_xpdr_squawk() const { return xpdr_squawk_; }
    void set_xpdr_vfr_7000() { xpdr_squawk_ = 7000; }
    void trigger_xpdr_ident() { xpdr_ident_timer_s_ = 18.0; }
    bool is_xpdr_ident_active() const { return xpdr_ident_timer_s_ > 0.0; }

    bool is_avionics_bus_powered(int bus_idx) const;

private:
    // Switches
    bool master_battery_ = true;
    bool master_alternator_ = true;
    bool avionics_master_ = true;
    bool fuel_pump_switch_ = false;
    bool pitot_heat_switch_ = false;
    double carb_heat_ = 0.0;
    int magnetos_ = 3; // 0=OFF, 1=R, 2=L, 3=BOTH, 4=START

    // Lights: 0=BCN, 1=LAND, 2=NAV, 3=STRB, 4=INST
    std::array<bool, 5> light_switches_{true, false, true, true, true};

    // 12 Circuit Breakers (false = pushed in / closed; true = pulled / open)
    std::array<bool, CB_COUNT> breakers_pulled_{};

    // Flaps
    double flaps_target_deg_ = 0.0;
    double flaps_actual_deg_ = 0.0;

    // Electrical bus
    double battery_soc_ = 0.96; // 14 Ah 24V battery state of charge [0..1]
    double bus_volts_ = 28.0;
    double ammeter_amps_ = 2.0;
    double load_amps_ = 10.5;
    bool low_volts_annunciator_ = false;

    PistonSystemsConfig config_{};
    // Package-defined two-tank fuel, oil and flap limits; defaults match the factory C172S.
    double fuel_left_gal_ = 21.0;
    double fuel_right_gal_ = 21.0;
    int fuel_selector_ = FUEL_SEL_BOTH;
    bool fuel_shutoff_pulled_ = false;
    int primer_strokes_ = 0;
    bool fuel_starved_ = false;
    double actual_fuel_flow_gph_ = 8.2;

    // Icing & Pitot
    double carb_ice_level_ = 0.0;
    double airframe_ice_kg_ = 0.0;
    bool pitot_blocked_ = false;
    bool pitot_cover_on_ = false;

    // Engine thermodynamics & health
    double cht_f_ = 365.0;
    double egt_f_ = 1340.0;
    double oil_temp_f_ = 185.0;
    double oil_press_psi_ = 74.0;
    double oil_quarts_ = 7.4;   // Flat-4 360: 8 qt max, 6 qt min normal
    double oil_hours_ = 0.0;    // Hours since last oil & filter change
    double vacuum_inhg_ = 5.0;
    double spark_fouling_ = 0.0;
    double shock_cooling_wear_ = 0.0;
    double cylinder_wear_ = 0.0;
    double sump_water_ = 0.0;   // Un-drained condensation/rainwater in fuel sumps
    double sump_phase_s_ = 0.0;
    bool sump_sputter_active_ = false;
    bool overtemp_failure_ = false;
    bool oil_starvation_failure_ = false;
    bool engine_running_ = true;
    bool starter_cranking_ = false;
    bool flaps_moving_ = false;
    double crank_timer_s_ = 0.0;
    double engine_power_mult_ = 1.0;
    double baro_hpa_ = 1013.25;

    // Avionics stack
    std::array<double, 2> com_active_mhz_{123.500, 121.500};
    std::array<double, 2> com_stby_mhz_{118.100, 122.800};
    std::array<double, 2> nav_active_mhz_{113.00, 114.50};
    std::array<double, 2> nav_stby_mhz_{109.50, 115.80};
    std::array<int, 2> nav_obs_deg_{120, 360};
    std::array<double, 2> nav_cdi_{0.0, 0.0};
    std::array<int, 2> nav_to_from_{0, 0}; // -1=FROM, 0=OFF/NO SIGNAL, +1=TO
    std::array<double, 2> nav_dme_nm_{0.0, 0.0};
    std::array<std::string, 2> nav_ident_{"---", "---"};
    int xpdr_mode_ = XPDR_ALT;
    int xpdr_squawk_ = 7000;
    double xpdr_ident_timer_s_ = 0.0;
};

// Compatibility name retained for the built-in C172 implementation and old callers.
using C172Systems = PistonAircraftSystems;

} // namespace nspain
