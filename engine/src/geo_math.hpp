#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace nspain {

constexpr double PI = 3.14159265358979323846;
constexpr double DEG2RAD = PI / 180.0;
constexpr double RAD2DEG = 180.0 / PI;

// WGS84 ellipsoid constants
constexpr double WGS84_A = 6378137.0;
constexpr double WGS84_F = 1.0 / 298.257223563;
constexpr double WGS84_E2 = WGS84_F * (2.0 - WGS84_F);

struct Vec3d {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

struct EnuBasis {
    Vec3d east;
    Vec3d north;
    Vec3d up;
};

inline Vec3d geodetic_to_ecef(double lat_deg, double lon_deg, double h_m) {
    const double phi = lat_deg * DEG2RAD;
    const double lam = lon_deg * DEG2RAD;
    const double sp = std::sin(phi);
    const double cp = std::cos(phi);
    const double N = WGS84_A / std::sqrt(1.0 - WGS84_E2 * sp * sp);
    return {
        (N + h_m) * cp * std::cos(lam),
        (N + h_m) * cp * std::sin(lam),
        (N * (1.0 - WGS84_E2) + h_m) * sp,
    };
}

inline EnuBasis enu_basis(double lat_deg, double lon_deg) {
    const double phi = lat_deg * DEG2RAD;
    const double lam = lon_deg * DEG2RAD;
    const double sp = std::sin(phi), cp = std::cos(phi);
    const double sl = std::sin(lam), cl = std::cos(lam);
    return {
        {-sl, cl, 0.0},
        {-sp * cl, -sp * sl, cp},
        {cp * cl, cp * sl, sp},
    };
}

inline double dot3(const Vec3d &a, const Vec3d &b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

// Convert ECEF position to renderer Y-up local ENU (X=East, Y=Up, Z=-North).
inline Vec3d ecef_to_local(const Vec3d &ecef, const Vec3d &origin, const EnuBasis &b) {
    const Vec3d d{ecef.x - origin.x, ecef.y - origin.y, ecef.z - origin.z};
    return {dot3(d, b.east), dot3(d, b.up), -dot3(d, b.north)};
}

inline void meters_per_degree(double lat_deg, double &m_lat, double &m_lon) {
    const double p = lat_deg * DEG2RAD;
    m_lat = 111132.92 - 559.82 * std::cos(2.0 * p) + 1.175 * std::cos(4.0 * p);
    m_lon = 111412.84 * std::cos(p) - 93.5 * std::cos(3.0 * p);
}

inline double haversine_m(double lat1, double lon1, double lat2, double lon2) {
    const double dlat = (lat2 - lat1) * DEG2RAD;
    const double dlon = (lon2 - lon1) * DEG2RAD;
    const double s1 = std::sin(dlat * 0.5);
    const double s2 = std::sin(dlon * 0.5);
    const double a = s1 * s1 + std::cos(lat1 * DEG2RAD) * std::cos(lat2 * DEG2RAD) * s2 * s2;
    return 2.0 * WGS84_A * std::asin(std::min(1.0, std::sqrt(a)));
}

struct MercatorTilePos {
    int32_t tx = 0;
    int32_t ty = 0;
    double fx = 0.0; // [0, 1) within tile
    double fy = 0.0; // [0, 1) within tile
};

inline MercatorTilePos latlon_to_tile(double lat_deg, double lon_deg, int zoom) {
    const double lat_clamped = std::clamp(lat_deg, -85.0511, 85.0511);
    const double lat_rad = lat_clamped * DEG2RAD;
    const double n = static_cast<double>(1u << zoom);
    const double xt = (lon_deg + 180.0) / 360.0 * n;
    const double yt = (1.0 - std::asinh(std::tan(lat_rad)) / PI) * 0.5 * n;
    const int32_t lim = static_cast<int32_t>(n) - 1;
    int32_t tx = std::clamp(static_cast<int32_t>(std::floor(xt)), 0, lim);
    int32_t ty = std::clamp(static_cast<int32_t>(std::floor(yt)), 0, lim);
    return {tx, ty, std::clamp(xt - tx, 0.0, 1.0), std::clamp(yt - ty, 0.0, 1.0)};
}

} // namespace nspain
