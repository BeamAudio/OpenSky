#pragma once

namespace nsim {

struct AircraftCareSnapshot {
    double left_fuel_gal = 21.0;
    double right_fuel_gal = 21.0;
    double oil_quarts = 7.4;
    double oil_hours = 0.0;
    double battery_soc = 0.95;
    double spark_fouling = 0.0;
    double shock_cooling = 0.0;
    double cylinder_wear = 0.0;
    double airframe_damage = 0.0;
    double tire_wear = 0.0;
    double brake_wear = 0.0;
    double gear_alignment = 0.0;
    double prop_nicks = 0.0;
    double windshield_dirt = 0.0;
    double windshield_crack = 0.0;
    double dmg_nose = 0.0;
    double dmg_wing_l = 0.0;
    double dmg_wing_r = 0.0;
    double dmg_tail = 0.0;
    double sump_water = 0.0;
    bool chocks_on = true;
    bool pitot_cover_on = true;
    bool tie_downs_on = true;
    bool master_on = false;
    bool avionics_on = false;
    int magnetos = 0;
    int fuel_selector = 2;
    double mixture = 0.0;
    double flaps_deg = 0.0;
    double carb_heat = 0.0;
    bool engine_running = false;
};

} // namespace nsim
