#include "c172_systems.hpp"

#include <algorithm>
#include <cmath>

namespace nspain {

namespace {
constexpr const char *BREAKER_NAMES[CB_COUNT] = {
    "ALT_FIELD",
    "AVIONICS_BUS_1",
    "AVIONICS_BUS_2",
    "FUEL_PUMP",
    "FLAPS",
    "INST_LIGHTS",
    "BCN_LIGHT",
    "LAND_LIGHT",
    "NAV_LIGHT",
    "STRB_LIGHT",
    "PITOT_HEAT",
    "TURN_COORD"
};
} // namespace

C172Systems::C172Systems() {
    reset_to_running_default();
}

void C172Systems::reset_to_running_default() {
    master_battery_ = true;
    master_alternator_ = true;
    avionics_master_ = true;
    fuel_pump_switch_ = false;
    pitot_heat_switch_ = false;
    carb_heat_ = 0.0;
    magnetos_ = 3; // BOTH
    light_switches_ = {true, false, true, true, true};
    breakers_pulled_.fill(false);

    flaps_target_deg_ = 0.0;
    flaps_actual_deg_ = 0.0;

    battery_soc_ = 0.96;
    bus_volts_ = 28.0;
    ammeter_amps_ = 2.0;
    load_amps_ = 10.5;
    low_volts_annunciator_ = false;

    fuel_left_gal_ = 21.0;
    fuel_right_gal_ = 21.0;
    fuel_selector_ = FUEL_SEL_BOTH;
    fuel_shutoff_pulled_ = false;
    primer_strokes_ = 0;
    fuel_starved_ = false;
    actual_fuel_flow_gph_ = 8.2;

    carb_ice_level_ = 0.0;
    airframe_ice_kg_ = 0.0;
    pitot_blocked_ = false;
    pitot_cover_on_ = false;

    cht_f_ = 365.0;
    egt_f_ = 1340.0;
    oil_temp_f_ = 185.0;
    oil_press_psi_ = 74.0;
    oil_quarts_ = 7.4;
    oil_hours_ = 0.0;
    vacuum_inhg_ = 5.0;
    spark_fouling_ = 0.0;
    shock_cooling_wear_ = 0.0;
    cylinder_wear_ = 0.0;
    sump_water_ = 0.0;
    sump_phase_s_ = 0.0;
    sump_sputter_active_ = false;
    overtemp_failure_ = false;
    oil_starvation_failure_ = false;
    engine_running_ = true;
    starter_cranking_ = false;
    flaps_moving_ = false;
    crank_timer_s_ = 0.0;
    engine_power_mult_ = 1.0;
    baro_hpa_ = 1013.25;

    com_active_mhz_ = {123.500, 121.500};
    com_stby_mhz_ = {118.100, 122.800};
    nav_active_mhz_ = {113.00, 114.50};
    nav_stby_mhz_ = {109.50, 115.80};
    nav_obs_deg_ = {120, 360};
    nav_cdi_ = {0.0, 0.0};
    nav_to_from_ = {0, 0};
    nav_dme_nm_ = {0.0, 0.0};
    nav_ident_ = {"---", "---"};
    xpdr_mode_ = XPDR_ALT;
    xpdr_squawk_ = 7000;
    xpdr_ident_timer_s_ = 0.0;
}

void C172Systems::reset_cold_and_dark() {
    reset_to_running_default();
    master_battery_ = false;
    master_alternator_ = false;
    avionics_master_ = false;
    fuel_pump_switch_ = false;
    pitot_heat_switch_ = false;
    carb_heat_ = 0.0;
    magnetos_ = 0; // OFF
    light_switches_.fill(false);
    bus_volts_ = 0.0;
    ammeter_amps_ = 0.0;
    load_amps_ = 0.0;
    low_volts_annunciator_ = true;
    engine_running_ = false;
    starter_cranking_ = false;
    crank_timer_s_ = 0.0;
    engine_power_mult_ = 0.0;
    actual_fuel_flow_gph_ = 0.0;
    cht_f_ = 62.0;
    egt_f_ = 62.0;
    oil_temp_f_ = 62.0;
    oil_press_psi_ = 0.0;
    vacuum_inhg_ = 0.0;
    xpdr_mode_ = XPDR_STBY;
}

const char *C172Systems::get_breaker_name(int cb_id) {
    if (cb_id < 0 || cb_id >= CB_COUNT) {
        return "UNKNOWN";
    }
    return BREAKER_NAMES[cb_id];
}

bool C172Systems::is_breaker_pulled(int cb_id) const {
    if (cb_id < 0 || cb_id >= CB_COUNT) {
        return false;
    }
    return breakers_pulled_[cb_id];
}

void C172Systems::set_breaker_pulled(int cb_id, bool pulled) {
    if (cb_id >= 0 && cb_id < CB_COUNT) {
        breakers_pulled_[cb_id] = pulled;
    }
}

bool C172Systems::toggle_breaker(int cb_id) {
    if (cb_id >= 0 && cb_id < CB_COUNT) {
        breakers_pulled_[cb_id] = !breakers_pulled_[cb_id];
        return breakers_pulled_[cb_id];
    }
    return false;
}

void C172Systems::trip_breaker(int cb_id) {
    set_breaker_pulled(cb_id, true);
}

bool C172Systems::is_circuit_powered(int cb_id) const {
    if (cb_id < 0 || cb_id >= CB_COUNT) {
        return false;
    }
    if (bus_volts_ < 16.0 || breakers_pulled_[cb_id]) {
        return false;
    }
    if (cb_id == CB_AVIONICS_BUS_1 || cb_id == CB_AVIONICS_BUS_2) {
        return avionics_master_;
    }
    return true;
}

bool C172Systems::is_avionics_bus_powered(int bus_idx) const {
    if (bus_idx == 1) {
        return is_circuit_powered(CB_AVIONICS_BUS_1);
    }
    if (bus_idx == 2) {
        return is_circuit_powered(CB_AVIONICS_BUS_2);
    }
    return false;
}

bool C172Systems::is_fuel_pump_active() const {
    return fuel_pump_switch_ && is_circuit_powered(CB_FUEL_PUMP);
}

bool C172Systems::is_pitot_heat_active() const {
    return pitot_heat_switch_ && is_circuit_powered(CB_PITOT_HEAT);
}

void C172Systems::set_light_switch(int light_idx, bool on) {
    if (light_idx >= 0 && light_idx < 5) {
        light_switches_[light_idx] = on;
    }
}

bool C172Systems::get_light_switch(int light_idx) const {
    if (light_idx >= 0 && light_idx < 5) {
        return light_switches_[light_idx];
    }
    return false;
}

bool C172Systems::is_light_powered(int light_idx) const {
    static constexpr int LIGHT_CBS[5] = {CB_BCN_LIGHT, CB_LAND_LIGHT, CB_NAV_LIGHT, CB_STRB_LIGHT, CB_INST_LIGHTS};
    if (light_idx < 0 || light_idx >= 5) {
        return false;
    }
    return light_switches_[light_idx] && is_circuit_powered(LIGHT_CBS[light_idx]);
}

void C172Systems::set_baro_hpa(double hpa) {
    baro_hpa_ = std::clamp(hpa, 940.0, 1055.0);
}

void C172Systems::set_flaps_target_deg(double deg) {
    // Snap or clamp to standard C172S detents (0, 10, 20, 30 deg)
    flaps_target_deg_ = std::clamp(deg, 0.0, 30.0);
}

void C172Systems::set_fuel_selector(int pos) {
    fuel_selector_ = std::clamp(pos, 0, 3);
}

void C172Systems::cycle_fuel_selector() {
    // Cycle LEFT(1) -> BOTH(2) -> RIGHT(3) -> OFF(0) -> LEFT(1)
    fuel_selector_ = (fuel_selector_ + 1) % 4;
}

void C172Systems::set_tank_gallons(double left_gal, double right_gal) {
    fuel_left_gal_ = std::clamp(left_gal, 0.0, MAX_TANK_GAL);
    fuel_right_gal_ = std::clamp(right_gal, 0.0, MAX_TANK_GAL);
}

void C172Systems::actuate_primer() {
    primer_strokes_ = std::min(6, primer_strokes_ + 1);
}

void C172Systems::set_battery_soc(double soc) {
    battery_soc_ = std::clamp(soc, 0.0, 1.0);
}

void C172Systems::set_oil_state(double quarts, double hours) {
    oil_quarts_ = std::clamp(quarts, 0.0, 8.0);
    oil_hours_ = std::max(0.0, hours);
}

void C172Systems::set_wear_state(double spark_fouling, double shock_cooling, double cylinder_wear) {
    spark_fouling_ = std::clamp(spark_fouling, 0.0, 1.0);
    shock_cooling_wear_ = std::clamp(shock_cooling, 0.0, 1.0);
    cylinder_wear_ = std::clamp(cylinder_wear, 0.0, 1.0);
}

void C172Systems::set_sump_water(double water) {
    sump_water_ = std::clamp(water, 0.0, 1.0);
}

void C172Systems::set_flaps_immediate_deg(double deg) {
    const double clamped = std::clamp(deg, 0.0, 30.0);
    flaps_target_deg_ = clamped;
    flaps_actual_deg_ = clamped;
}

void C172Systems::set_engine_running_state(bool running, double ambient_temp_c) {
    engine_running_ = running;
    starter_cranking_ = false;
    crank_timer_s_ = 0.0;
    const double amb_f = ambient_temp_c * 1.8 + 32.0;
    if (running) {
        cht_f_ = std::max(amb_f + 120.0, 330.0);
        egt_f_ = 1220.0;
        oil_temp_f_ = std::max(amb_f + 70.0, 165.0);
        oil_press_psi_ = 72.0;
        vacuum_inhg_ = 4.9;
        engine_power_mult_ = 1.0;
    } else {
        cht_f_ = amb_f;
        egt_f_ = amb_f;
        oil_temp_f_ = amb_f;
        oil_press_psi_ = 0.0;
        vacuum_inhg_ = 0.0;
        engine_power_mult_ = 0.0;
        actual_fuel_flow_gph_ = 0.0;
    }
}

void C172Systems::swap_com(int idx) {
    const int i = std::clamp(idx - 1, 0, 1);
    if (!is_avionics_bus_powered(i + 1)) {
        return;
    }
    std::swap(com_active_mhz_[i], com_stby_mhz_[i]);
}

void C172Systems::tune_com_standby(int idx, double freq_mhz) {
    const int i = std::clamp(idx - 1, 0, 1);
    com_stby_mhz_[i] = std::clamp(freq_mhz, 118.000, 136.975);
}

void C172Systems::step_com_standby(int idx, double delta_mhz) {
    const int i = std::clamp(idx - 1, 0, 1);
    if (!is_avionics_bus_powered(i + 1)) {
        return;
    }
    double f = com_stby_mhz_[i] + delta_mhz;
    if (f > 136.975) f = 118.000;
    if (f < 118.000) f = 136.975;
    com_stby_mhz_[i] = std::round(f * 40.0) / 40.0; // 25 kHz spacing
}

double C172Systems::get_com_active(int idx) const {
    return com_active_mhz_[std::clamp(idx - 1, 0, 1)];
}

double C172Systems::get_com_standby(int idx) const {
    return com_stby_mhz_[std::clamp(idx - 1, 0, 1)];
}

void C172Systems::swap_nav(int idx) {
    const int i = std::clamp(idx - 1, 0, 1);
    if (!is_avionics_bus_powered(i + 1)) {
        return;
    }
    std::swap(nav_active_mhz_[i], nav_stby_mhz_[i]);
}

void C172Systems::tune_nav_standby(int idx, double freq_mhz) {
    const int i = std::clamp(idx - 1, 0, 1);
    nav_stby_mhz_[i] = std::clamp(freq_mhz, 108.00, 117.95);
}

void C172Systems::step_nav_standby(int idx, double delta_mhz) {
    const int i = std::clamp(idx - 1, 0, 1);
    if (!is_avionics_bus_powered(i + 1)) {
        return;
    }
    double f = nav_stby_mhz_[i] + delta_mhz;
    if (f > 117.95) f = 108.00;
    if (f < 108.00) f = 117.95;
    nav_stby_mhz_[i] = std::round(f * 20.0) / 20.0; // 50 kHz spacing
}

double C172Systems::get_nav_active(int idx) const {
    return nav_active_mhz_[std::clamp(idx - 1, 0, 1)];
}

double C172Systems::get_nav_standby(int idx) const {
    return nav_stby_mhz_[std::clamp(idx - 1, 0, 1)];
}

void C172Systems::set_nav_obs_deg(int idx, int deg) {
    const int i = std::clamp(idx - 1, 0, 1);
    nav_obs_deg_[i] = ((deg % 360) + 360) % 360;
}

int C172Systems::get_nav_obs_deg(int idx) const {
    return nav_obs_deg_[std::clamp(idx - 1, 0, 1)];
}

double C172Systems::get_nav_cdi(int idx) const {
    return nav_cdi_[std::clamp(idx - 1, 0, 1)];
}

int C172Systems::get_nav_to_from(int idx) const {
    return nav_to_from_[std::clamp(idx - 1, 0, 1)];
}

double C172Systems::get_nav_dme_nm(int idx) const {
    return nav_dme_nm_[std::clamp(idx - 1, 0, 1)];
}

std::string C172Systems::get_nav_ident(int idx) const {
    return nav_ident_[std::clamp(idx - 1, 0, 1)];
}

void C172Systems::update_navigation(double lat_deg, double lon_deg, double alt_msl_m) {
    struct VorStation {
        const char *ident;
        double freq_mhz;
        double lat_deg;
        double lon_deg;
        double elev_m;
    };
    // Authentic Northern Spain / Aragón & Pyrenees VOR-DME navaids
    static constexpr VorStation VORS[] = {
        {"ZAR", 113.00, 41.6581, -1.0314, 265.0}, // Zaragoza VOR-DME
        {"HUE", 114.50, 42.0811, -0.3233, 538.0}, // Huesca-Pirineos VOR
        {"PPN", 112.30, 42.7344, -1.7022, 460.0}, // Pamplona VOR-DME
        {"GRA", 115.80, 42.2010,  0.3420, 620.0}, // Graus / Central Pyrenees VOR
        {"LLE", 113.60, 41.7290,  0.5360, 350.0}, // Lleida-Alguaire VOR
    };
    constexpr double DEG2RAD = 3.14159265358979323846 / 180.0;
    constexpr double RAD2DEG = 180.0 / 3.14159265358979323846;

    const double alt_ft = std::max(100.0, alt_msl_m * 3.28084);
    const double max_range_nm = std::min(130.0, 1.23 * std::sqrt(alt_ft) + 42.0);
    const double cos_lat = std::cos(lat_deg * DEG2RAD);

    for (int i = 0; i < 2; ++i) {
        nav_cdi_[i] = 0.0;
        nav_to_from_[i] = 0;
        nav_dme_nm_[i] = 0.0;
        nav_ident_[i] = "---";
        if (!is_avionics_bus_powered(i + 1)) {
            continue;
        }
        for (const auto &v : VORS) {
            if (std::abs(nav_active_mhz_[i] - v.freq_mhz) > 0.03) {
                continue;
            }
            const double dn_m = (v.lat_deg - lat_deg) * 111132.0;
            const double de_m = (v.lon_deg - lon_deg) * 111320.0 * cos_lat;
            const double horiz_m = std::hypot(de_m, dn_m);
            const double slant_nm = std::hypot(horiz_m, alt_msl_m - v.elev_m) / 1852.0;
            if (slant_nm > max_range_nm) {
                continue;
            }
            double brg_to = std::atan2(de_m, dn_m) * RAD2DEG;
            if (brg_to < 0.0) brg_to += 360.0;
            const double obs = static_cast<double>(nav_obs_deg_[i]);
            const double diff = std::remainder(brg_to - obs, 360.0);
            if (std::abs(diff) <= 90.0) {
                nav_to_from_[i] = 1; // TO
                nav_cdi_[i] = std::clamp(diff / 10.0, -1.0, 1.0);
            } else {
                nav_to_from_[i] = -1; // FROM
                const double back_diff = std::remainder((brg_to + 180.0) - obs, 360.0);
                nav_cdi_[i] = std::clamp(-back_diff / 10.0, -1.0, 1.0);
            }
            nav_dme_nm_[i] = slant_nm;
            nav_ident_[i] = v.ident;
            break;
        }
    }
}

void C172Systems::set_xpdr_mode(int mode) {
    xpdr_mode_ = std::clamp(mode, 0, 3);
}

void C172Systems::cycle_xpdr_mode() {
    if (!is_avionics_bus_powered(1)) {
        return;
    }
    xpdr_mode_ = (xpdr_mode_ + 1) % 4;
}

void C172Systems::set_xpdr_squawk(int octal_code) {
    // Clamp each digit to octal [0..7]
    int d0 = (octal_code / 1000) % 10;
    int d1 = (octal_code / 100) % 10;
    int d2 = (octal_code / 10) % 10;
    int d3 = octal_code % 10;
    d0 = std::clamp(d0, 0, 7);
    d1 = std::clamp(d1, 0, 7);
    d2 = std::clamp(d2, 0, 7);
    d3 = std::clamp(d3, 0, 7);
    xpdr_squawk_ = d0 * 1000 + d1 * 100 + d2 * 10 + d3;
}

void C172Systems::step_systems(
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
) {
    C172EnvStep s{};
    s.dt = dt;
    s.throttle01 = throttle01;
    s.mixture01 = mixture01;
    s.rpm = rpm;
    s.ias_kt = ias_kt;
    s.fuel_flow_demand_gph = fuel_flow_demand_gph;
    s.ambient_temp_c = ambient_temp_c;
    s.dewpoint_c = dewpoint_c;
    s.cloud_cover_pct = cloud_cover_pct;
    s.precip_mm = precip_mm;
    update(s);
}

void C172Systems::update(const C172EnvStep &in) {
    const double dt = std::clamp(in.dt, 0.0, 1.0);
    if (dt <= 0.0) {
        return;
    }

    // 1. Fuel System & Starvation Logic
    bool has_fuel_feed = false;
    if (!fuel_shutoff_pulled_ && fuel_selector_ != FUEL_SEL_OFF) {
        if (fuel_selector_ == FUEL_SEL_LEFT && fuel_left_gal_ > 0.01) {
            has_fuel_feed = true;
        } else if (fuel_selector_ == FUEL_SEL_RIGHT && fuel_right_gal_ > 0.01) {
            has_fuel_feed = true;
        } else if (fuel_selector_ == FUEL_SEL_BOTH && (fuel_left_gal_ > 0.01 || fuel_right_gal_ > 0.01)) {
            has_fuel_feed = true;
        }
    }

    fuel_starved_ = (!has_fuel_feed) || (in.mixture01 < 0.08);
    const bool ignition_on = (magnetos_ >= 1);
    const bool fatal_engine_failure = overtemp_failure_ || oil_starvation_failure_ || (carb_ice_level_ >= 0.98);

    // Pre-check 24V battery bus availability for electric starter motor cranking
    const bool bat_avail = master_battery_ && (battery_soc_ > 0.01);
    const double pre_bat_v = bat_avail ? (20.5 + 3.9 * battery_soc_) : 0.0;
    starter_cranking_ = (magnetos_ == 4) && (!engine_running_) && (pre_bat_v >= 14.0);

    if (engine_running_) {
        engine_running_ = ignition_on && (!fuel_starved_) && (!fatal_engine_failure);
        crank_timer_s_ = 0.0;
    } else if (ignition_on && (!fuel_starved_) && (!fatal_engine_failure)) {
        if (starter_cranking_) {
            crank_timer_s_ += dt;
            const double req_crank_s = (primer_strokes_ >= 2 || is_fuel_pump_active()) ? 0.85 : 1.40;
            if (crank_timer_s_ >= req_crank_s && in.mixture01 >= 0.20) {
                engine_running_ = true;
                starter_cranking_ = false;
                crank_timer_s_ = 0.0;
                primer_strokes_ = 0;
                magnetos_ = 3; // Spring-return ignition key from START to BOTH
            }
        } else if (in.ias_kt >= 55.0) {
            // In-flight airspeed windmilling restart when fuel & magnetos restored
            engine_running_ = true;
            crank_timer_s_ = 0.0;
        } else {
            crank_timer_s_ = std::max(0.0, crank_timer_s_ - dt * 0.5);
        }
    } else {
        crank_timer_s_ = std::max(0.0, crank_timer_s_ - dt * 0.5);
    }

    if (engine_running_) {
        actual_fuel_flow_gph_ = std::max(1.2, in.fuel_flow_demand_gph);
        const double gal_used = actual_fuel_flow_gph_ * (dt / 3600.0);
        if (fuel_selector_ == FUEL_SEL_LEFT) {
            fuel_left_gal_ = std::max(0.0, fuel_left_gal_ - gal_used);
        } else if (fuel_selector_ == FUEL_SEL_RIGHT) {
            fuel_right_gal_ = std::max(0.0, fuel_right_gal_ - gal_used);
        } else if (fuel_selector_ == FUEL_SEL_BOTH) {
            if (fuel_left_gal_ > 0.01 && fuel_right_gal_ > 0.01) {
                fuel_left_gal_ = std::max(0.0, fuel_left_gal_ - 0.5 * gal_used);
                fuel_right_gal_ = std::max(0.0, fuel_right_gal_ - 0.5 * gal_used);
            } else if (fuel_left_gal_ > 0.01) {
                fuel_left_gal_ = std::max(0.0, fuel_left_gal_ - gal_used);
            } else {
                fuel_right_gal_ = std::max(0.0, fuel_right_gal_ - gal_used);
            }
        }
    } else {
        actual_fuel_flow_gph_ = 0.0;
    }

    // 2. Electrical Bus, 24V 14Ah Battery, 28V 60A Alternator & 12 Circuit Breakers
    const bool alt_online = master_alternator_
        && (!breakers_pulled_[CB_ALT_FIELD])
        && engine_running_
        && (in.rpm >= 720.0);

    // Pre-update bus voltage availability to evaluate powered circuits
    if (alt_online) {
        bus_volts_ = 28.0;
    } else if (master_battery_ && battery_soc_ > 0.01) {
        bus_volts_ = 20.5 + 3.9 * battery_soc_;
    } else {
        bus_volts_ = 0.0;
    }

    // Compute active electrical loads gated by their individual circuit breakers
    double load_a = (bus_volts_ > 16.0) ? 1.8 : 0.0; // Base annunciator/contactor draw
    if (starter_cranking_) load_a += 65.0; // 24V high-torque starter draw (sags bus voltage realistically!)
    if (is_circuit_powered(CB_AVIONICS_BUS_1)) load_a += 3.6;
    if (is_circuit_powered(CB_AVIONICS_BUS_2)) load_a += 3.0;
    if (is_fuel_pump_active()) load_a += 4.5;
    if (is_pitot_heat_active()) load_a += 9.5;
    if (is_circuit_powered(CB_TURN_COORD)) load_a += 0.9;
    if (light_switches_[0] && is_circuit_powered(CB_BCN_LIGHT)) load_a += 2.5;
    if (light_switches_[1] && is_circuit_powered(CB_LAND_LIGHT)) load_a += 7.5;
    if (light_switches_[2] && is_circuit_powered(CB_NAV_LIGHT)) load_a += 2.0;
    if (light_switches_[3] && is_circuit_powered(CB_STRB_LIGHT)) load_a += 3.2;
    if (light_switches_[4] && is_circuit_powered(CB_INST_LIGHTS)) load_a += 1.2;

    // Electric flap motor (10 deg/s travel ONLY when CB_FLAPS is powered!)
    flaps_moving_ = false;
    if (is_circuit_powered(CB_FLAPS) && std::abs(flaps_target_deg_ - flaps_actual_deg_) > 0.01) {
        flaps_moving_ = true;
        load_a += 8.0;
        const double max_step = 10.0 * dt;
        const double diff = flaps_target_deg_ - flaps_actual_deg_;
        flaps_actual_deg_ += std::clamp(diff, -max_step, max_step);
    }

    load_amps_ = load_a;

    if (alt_online) {
        const double charge_a = master_battery_ ? std::clamp((1.0 - battery_soc_) * 16.0 + 1.2, 0.4, 18.0) : 0.0;
        ammeter_amps_ = charge_a;
        bus_volts_ = 28.0 - std::max(0.0, load_amps_ + charge_a - 55.0) * 0.15;
        if (master_battery_) {
            battery_soc_ = std::clamp(battery_soc_ + (charge_a * (dt / 3600.0)) / 14.0, 0.0, 1.0);
        }
    } else if (master_battery_ && battery_soc_ > 0.01) {
        ammeter_amps_ = -load_amps_;
        bus_volts_ = std::max(14.0, (20.5 + 3.9 * battery_soc_) - load_amps_ * 0.045);
        battery_soc_ = std::clamp(battery_soc_ - (load_amps_ * (dt / 3600.0)) / 14.0, 0.0, 1.0);
    } else {
        ammeter_amps_ = 0.0;
        bus_volts_ = 0.0;
    }

    low_volts_annunciator_ = (bus_volts_ < 24.5);

    // 3. Carburetor / Induction Icing, Structural Airframe Icing & Pitot Heat
    const double spread_c = std::max(0.0, in.ambient_temp_c - in.dewpoint_c);
    const bool carb_ice_temp_band = (in.ambient_temp_c >= -5.0 && in.ambient_temp_c <= 20.0);
    const bool moist_air = (spread_c <= 10.0) || (in.cloud_cover_pct > 65.0) || (in.precip_mm > 0.01);

    if (carb_heat_ > 0.25) {
        // Applying carb heat melts induction ice rapidly
        carb_ice_level_ = std::max(0.0, carb_ice_level_ - 0.095 * carb_heat_ * dt);
    } else if (engine_running_ && carb_ice_temp_band && moist_air && in.throttle01 < 0.60 && in.ias_kt > 35.0) {
        // Induction icing builds gradually during low-power flight in moist air, never choking a ground taxi/takeoff.
        const double thr_factor = (0.60 - in.throttle01) / 0.60;
        const double humidity_factor = std::clamp(1.0 - spread_c / 12.0, 0.25, 1.0);
        carb_ice_level_ = std::min(1.0, carb_ice_level_ + 0.006 * thr_factor * humidity_factor * dt);
    } else {
        const double clear_rate = (in.throttle01 > 0.80) ? 0.022 : 0.006;
        carb_ice_level_ = std::max(0.0, carb_ice_level_ - clear_rate * dt);
    }

    // Structural airframe icing & pitot blockage in freezing visible moisture
    const bool freezing_moisture = (in.ambient_temp_c < 0.0)
        && ((in.cloud_cover_pct > 65.0) || (in.precip_mm > 0.05) || (spread_c < 2.5));
    if (freezing_moisture) {
        airframe_ice_kg_ = std::min(45.0, airframe_ice_kg_ + 0.18 * dt);
        if (!is_pitot_heat_active()) {
            pitot_blocked_ = true;
        } else {
            pitot_blocked_ = false;
        }
    } else {
        if (in.ambient_temp_c > 2.0) {
            airframe_ice_kg_ = std::max(0.0, airframe_ice_kg_ - 0.25 * dt);
        }
        if (is_pitot_heat_active() || in.ambient_temp_c > 2.5) {
            pitot_blocked_ = false;
        }
    }

    // 4. Engine Thermodynamics (CHT, EGT, Oil Temp/Press, Vacuum, Spark Fouling, Shock Cooling)
    const double amb_f = in.ambient_temp_c * 1.8 + 32.0;
    if (engine_running_) {
        // EGT peaks at peak-EGT mixture (~0.78)
        const double mix_delta = in.mixture01 - 0.78;
        const double egt_target_f = 1060.0 + 380.0 * in.throttle01 - 1650.0 * (mix_delta * mix_delta);
        egt_f_ += (std::max(amb_f, egt_target_f) - egt_f_) * std::min(1.0, dt * 0.65);

        // CHT rises with power and leaned mixture, cooled by airspeed airflow (+ full-rich fuel cooling)
        const double lean_heat = std::max(0.0, (0.82 - in.mixture01) * 140.0);
        const double rich_cool = std::max(0.0, (in.mixture01 - 0.85) / 0.15) * 28.0;
        // The propeller also supplies cooling air during the ground roll. A
        // stationary running engine is not equivalent to zero airflow.
        const double propwash = 0.62 + 0.28 * std::clamp(in.rpm / 2400.0, 0.0, 1.0);
        const double air_cool = std::clamp(std::max(in.ias_kt / 88.0, propwash), 0.35, 1.38);
        const double cht_target_f = amb_f + (255.0 + 115.0 * in.throttle01 - rich_cool + lean_heat) / std::sqrt(air_cool);
        const double prev_cht_f = cht_f_;
        // Cylinder temperature changes over minutes, not a few seconds of
        // takeoff power on the ground.
        cht_f_ += (cht_target_f - cht_f_) * std::min(1.0, dt * 0.008);

        // Shock cooling check: rapid CHT drop > 55 degF/min when hot
        const double cht_rate_f_per_min = ((cht_f_ - prev_cht_f) / dt) * 60.0;
        if (prev_cht_f > 380.0 && cht_rate_f_per_min < -55.0) {
            shock_cooling_wear_ = std::min(1.0, shock_cooling_wear_ + 0.015 * dt);
            cylinder_wear_ = std::min(1.0, cylinder_wear_ + 0.008 * dt);
        }
        // Cold-engine full-power abuse accelerates cylinder wear until oil warms past 110 degF
        if (oil_temp_f_ < 110.0 && in.throttle01 > 0.72) {
            cylinder_wear_ = std::min(1.0, cylinder_wear_ + 0.0045 * (in.throttle01 - 0.72) * dt);
        }
        if (cht_f_ > 455.0) {
            cylinder_wear_ = std::min(1.0, cylinder_wear_ + 0.004 * dt);
        }

        // Oil burn & aging (IO-360 uses ~0.12 qt/hr normally, faster when run hot/hard)
        oil_hours_ += dt / 3600.0;
        const double hot_burn = (oil_temp_f_ > 220.0 || cht_f_ > 435.0) ? 2.2 : 1.0;
        oil_quarts_ = std::max(0.0, oil_quarts_ - (0.12 * hot_burn) * (dt / 3600.0));

        // Oil temperature & oil pressure reflect actual oil quantity, oil age, and cylinder wear
        const double low_oil_heat = std::max(0.0, 5.5 - oil_quarts_) * 16.0;
        const double dirty_oil_heat = std::clamp((oil_hours_ - 10.0) / 15.0, 0.0, 1.0) * 14.0;
        const double oil_target_f = amb_f + 115.0 + 55.0 * in.throttle01 + 0.12 * (cht_f_ - 350.0)
                                    + low_oil_heat + dirty_oil_heat;
        oil_temp_f_ += (oil_target_f - oil_temp_f_) * std::min(1.0, dt * 0.04);

        const double rpm_norm = std::clamp(in.rpm / 2400.0, 0.25, 1.15);
        const double temp_viscosity = std::clamp(1.18 - (oil_temp_f_ - 180.0) * 0.0035, 0.55, 1.35);
        const double qty_factor = std::clamp((oil_quarts_ - 2.0) / 3.5, 0.08, 1.0);
        const double age_factor = 1.0 - 0.10 * std::clamp((oil_hours_ - 10.0) / 20.0, 0.0, 1.0)
                                      - 0.10 * cylinder_wear_;
        oil_press_psi_ = std::clamp(72.0 * rpm_norm * temp_viscosity * qty_factor * age_factor, 0.0, 110.0);

        // Vacuum suction pump driven by engine RPM (4.8 - 5.2 inHg in cruise)
        vacuum_inhg_ = std::clamp(1.8 + 3.4 * std::clamp((in.rpm - 700.0) / 1600.0, 0.0, 1.05), 0.0, 5.4);

        // Spark-plug lead fouling accumulates gradually over prolonged rich ground idling (~15-20 min)
        if (in.rpm < 1050.0 && in.mixture01 > 0.82) {
            spark_fouling_ = std::min(1.0, spark_fouling_ + 0.0008 * dt);
        } else if (in.rpm > 1900.0 && in.mixture01 < 0.82) {
            spark_fouling_ = std::max(0.0, spark_fouling_ - 0.045 * dt);
        }

        // Un-drained fuel sump water causes periodic power sputters until cleared
        sump_sputter_active_ = false;
        if (sump_water_ > 0.12) {
            sump_phase_s_ += dt;
            const double cyc = std::fmod(sump_phase_s_, 4.2);
            if (cyc < 0.55 * sump_water_) sump_sputter_active_ = true;
            sump_water_ = std::max(0.0, sump_water_ - (0.002 + 0.006 * in.throttle01) * dt);
        }

        if (cht_f_ > 505.0 || oil_temp_f_ > 248.0) {
            overtemp_failure_ = true;
        }
        if ((oil_press_psi_ < 20.0 && in.rpm > 1200.0) || oil_quarts_ < 2.2) {
            oil_starvation_failure_ = true;
        }
    } else {
        egt_f_ += (amb_f - egt_f_) * std::min(1.0, dt * 0.35);
        cht_f_ += (amb_f - cht_f_) * std::min(1.0, dt * 0.05);
        oil_temp_f_ += (amb_f - oil_temp_f_) * std::min(1.0, dt * 0.03);
        oil_press_psi_ += (0.0 - oil_press_psi_) * std::min(1.0, dt * 1.5);
        vacuum_inhg_ += (0.0 - vacuum_inhg_) * std::min(1.0, dt * 1.8);
        sump_sputter_active_ = false;
    }

    // Net engine power multiplier combining carb ice, carb heat, spark fouling, single-mag check, cylinder wear & sump sputter
    const double ice_choke = 1.0 - 0.78 * carb_ice_level_;
    const double carb_heat_loss = 1.0 - 0.045 * carb_heat_;
    const double fouling_loss = 1.0 - 0.22 * spark_fouling_;
    const double mag_factor = (magnetos_ == 1 || magnetos_ == 2) ? (0.95 - 0.12 * spark_fouling_) : 1.0;
    const double cyl_factor = 1.0 - 0.24 * cylinder_wear_;
    const double sputter_factor = sump_sputter_active_ ? 0.42 : 1.0;
    engine_power_mult_ = engine_running_
        ? std::clamp(ice_choke * carb_heat_loss * fouling_loss * mag_factor * cyl_factor * sputter_factor, 0.0, 1.0)
        : 0.0;

    if (xpdr_ident_timer_s_ > 0.0) {
        xpdr_ident_timer_s_ = std::max(0.0, xpdr_ident_timer_s_ - dt);
    }
}

} // namespace nspain
