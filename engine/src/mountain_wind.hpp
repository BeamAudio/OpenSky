#pragma once

#include "geo_math.hpp"

#include <algorithm>
#include <cmath>

namespace nspain {

struct MountainWindInput {
    double lat_deg = 42.50;
    double lon_deg = 0.50;
    double alt_msl_m = 1500.0;
    double terrain_elev_m = 1200.0;
    double dz_de = 0.0; // dh / dEast (dimensionless slope)
    double dz_dn = 0.0; // dh / dNorth (dimensionless slope)

    // Live Open-Meteo weather inputs
    double wind_speed_kt = 12.0;
    double wind_dir_deg = 270.0; // Meteorological FROM direction (deg true)
    double gust_kt = 18.0;
    double temp_c = 15.0;
    double sim_time_s = 0.0;
};

struct MountainWindOutput {
    // Local ENU wind components (m/s)
    double wind_east_mps = 0.0;
    double wind_north_mps = 0.0;
    double wind_up_mps = 0.0;

    // Local NED wind components for JSBSim (m/s: +North, +East, +Down)
    double wind_down_mps = 0.0;

    // Diagnostic decomposition
    double ridge_lift_mps = 0.0;
    double lee_rotor_up_mps = 0.0;
    double gust_surge_mps = 0.0;
    bool in_lee_rotor = false;

    // Body angular turbulence excitations (rad/s)
    double p_turb_rps = 0.0;
    double q_turb_rps = 0.0;
    double r_turb_rps = 0.0;
};

class MountainWind {
public:
    static MountainWindOutput solve(const MountainWindInput &in);

    // 4-neighbor finite-difference DEM gradient helper for any elevation sampler `double(double lat, double lon)`
    template <typename ElevSampler>
    static void sample_dem_gradient(
        ElevSampler &&sample_elev_fn,
        double lat_deg,
        double lon_deg,
        double step_m,
        double &out_center_elev_m,
        double &out_dz_de,
        double &out_dz_dn
    ) {
        double m_lat = 111132.0, m_lon = 82000.0;
        meters_per_degree(lat_deg, m_lat, m_lon);
        const double h_step = std::max(10.0, step_m);
        const double dlat = h_step / std::max(1000.0, m_lat);
        const double dlon = h_step / std::max(1000.0, m_lon);

        out_center_elev_m = sample_elev_fn(lat_deg, lon_deg);
        const double h_e_plus = sample_elev_fn(lat_deg, lon_deg + dlon);
        const double h_e_minus = sample_elev_fn(lat_deg, lon_deg - dlon);
        const double h_n_plus = sample_elev_fn(lat_deg + dlat, lon_deg);
        const double h_n_minus = sample_elev_fn(lat_deg - dlat, lon_deg);

        out_dz_de = (h_e_plus - h_e_minus) / (2.0 * h_step);
        out_dz_dn = (h_n_plus - h_n_minus) / (2.0 * h_step);
    }
};

} // namespace nspain
