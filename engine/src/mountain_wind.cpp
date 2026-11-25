#include "mountain_wind.hpp"

namespace nspain {

namespace {
constexpr double KT_TO_MPS = 0.514444444;

double dryden_band_wave(double t, double spatial_phase, double freq1, double freq2, double freq3) {
    return 0.52 * std::sin(t * freq1 + spatial_phase)
        + 0.33 * std::cos(t * freq2 - spatial_phase * 1.37)
        + 0.15 * std::sin(t * freq3 + spatial_phase * 2.41);
}
} // namespace

MountainWindOutput MountainWind::solve(const MountainWindInput &in) {
    MountainWindOutput out{};

    const double h_agl = std::max(0.0, in.alt_msl_m - in.terrain_elev_m);
    const double base_mps = std::max(0.0, in.wind_speed_kt) * KT_TO_MPS;
    const double gust_delta_mps = std::max(0.0, in.gust_kt - in.wind_speed_kt) * KT_TO_MPS;

    // Meteorological wind direction is FROM; propagation direction is wind_dir_deg + 180 deg
    const double to_rad = (in.wind_dir_deg + 180.0) * DEG2RAD;
    const double dir_east = std::sin(to_rad);
    const double dir_north = std::cos(to_rad);

    // Spatial phase from geographic position so waves/rotors are anchored to mountain topography
    const double x_east_m = in.lon_deg * 82600.0;
    const double y_north_m = in.lat_deg * 111132.0;
    const double downwind_m = x_east_m * dir_east + y_north_m * dir_north;
    const double crosswind_m = x_east_m * dir_north - y_north_m * dir_east;
    const double spatial_phase = downwind_m * 0.0045 + crosswind_m * 0.0021;

    // 1. Continuous Dryden-style Gust & Mechanical/Thermal Turbulence
    const double gust_wave = dryden_band_wave(in.sim_time_s, spatial_phase, 1.35, 2.85, 5.40);
    // Normalize gust surge so gusts add positive kinetic energy peaks
    const double gust_env = 0.5 + 0.5 * gust_wave;
    out.gust_surge_mps = gust_delta_mps * gust_env;

    // Wind shear boundary layer near ground + speedup over high ridges
    const double boundary_factor = std::clamp(0.72 + 0.28 * std::pow(std::min(h_agl, 300.0) / 300.0, 0.22), 0.72, 1.15);
    const double horiz_speed_mps = (base_mps + out.gust_surge_mps) * boundary_factor;

    const double ve = dir_east * horiz_speed_mps;
    const double vn = dir_north * horiz_speed_mps;

    // 2. Orographic Slope Deflection (Windward Ridge Lift & Leeward Downdraft)
    // Dot product of horizontal wind vector V_w = (ve, vn) with terrain gradient (dz_de, dz_dn)
    const double v_dot_grad = ve * in.dz_de + vn * in.dz_dn;
    const double orographic_decay = std::exp(-h_agl / 650.0);
    // Clamp vertical deflection slope ratio to realistic mountain airflow limits
    const double slope_mag = std::hypot(in.dz_de, in.dz_dn);
    const double slope_atten = 1.0 / (1.0 + 0.45 * slope_mag * slope_mag);
    out.ridge_lift_mps = v_dot_grad * orographic_decay * slope_atten;

    // 3. Lee Rotor & Mountain Wave Solver
    // Triggered when wind flows across steep lee slopes (|grad| > 0.18 and v_dot_grad < 0) within 450 m AGL
    if (slope_mag > 0.18 && v_dot_grad < -0.15 && h_agl < 450.0) {
        out.in_lee_rotor = true;
        const double lee_factor = std::clamp((slope_mag - 0.18) / 0.35, 0.0, 1.5)
            * (1.0 - h_agl / 450.0)
            * std::clamp(horiz_speed_mps / 10.0, 0.2, 2.0);

        // Coherent lee-wave sinusoidal vertical oscillation + rolling rotor vorticity
        const double rotor_phase = downwind_m * 0.018 + in.sim_time_s * 2.4;
        out.lee_rotor_up_mps = -1.65 * lee_factor + 2.80 * lee_factor * std::sin(rotor_phase);

        out.p_turb_rps += 0.22 * lee_factor * std::cos(rotor_phase * 1.15);
        out.q_turb_rps += 0.16 * lee_factor * std::sin(rotor_phase * 0.95);
        out.r_turb_rps += 0.12 * lee_factor * std::sin(rotor_phase * 1.35);
    }

    // 4. Thermal & Low-Altitude Mechanical Turbulence
    const double thermal_strength = std::clamp((in.temp_c - 8.0) / 24.0, 0.0, 1.0) * std::exp(-h_agl / 1400.0);
    const double turb_amp = (0.015 * base_mps + 0.055 * gust_delta_mps + 0.04 * thermal_strength)
        * (0.45 + 0.55 * std::exp(-h_agl / 850.0));

    const double turb_w = dryden_band_wave(in.sim_time_s + 3.7, spatial_phase * 1.3, 1.9, 3.7, 6.8) * (0.35 * gust_delta_mps + 0.45 * thermal_strength);
    out.p_turb_rps += turb_amp * dryden_band_wave(in.sim_time_s + 1.1, spatial_phase, 2.1, 4.3, 7.9);
    out.q_turb_rps += turb_amp * 0.75 * dryden_band_wave(in.sim_time_s + 2.3, spatial_phase, 1.8, 3.9, 6.4);
    out.r_turb_rps += turb_amp * 0.60 * dryden_band_wave(in.sim_time_s + 4.9, spatial_phase, 1.5, 3.2, 5.7);

    out.wind_east_mps = ve;
    out.wind_north_mps = vn;
    out.wind_up_mps = std::clamp(out.ridge_lift_mps + out.lee_rotor_up_mps + turb_w, -16.0, 16.0);
    out.wind_down_mps = -out.wind_up_mps;

    return out;
}

} // namespace nspain
