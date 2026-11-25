#include "../src/geo_math.hpp"
#include "../../protocol/pack_format.h"

#include <cassert>
#include <cmath>
#include <iostream>

int main() {
    using namespace nspain;

    // 1. Struct sizes must match protocol/pack_format.py bit-for-bit.
    static_assert(sizeof(PackHeader) == 128);
    static_assert(sizeof(PackTileEntry) == 32);
    static_assert(sizeof(HeightTileHeader) == 8);

    // 2. Key packing preserves strict ordering by (layer, z, x, y).
    uint64_t k1 = make_tile_key(LAYER_ELEVATION, 13, 4087, 3024);
    uint64_t k2 = make_tile_key(LAYER_ELEVATION, 13, 4087, 3025);
    uint64_t k3 = make_tile_key(LAYER_IMAGERY, 10, 0, 0);
    assert(k1 < k2 && k2 < k3);

    // 3. WGS84 floating-origin ENU accuracy across Zaragoza -> Huesca (~75 km).
    const double z_lat = 41.6662, z_lon = -1.0416, z_h = 263.0;
    const double h_lat = 42.0810, h_lon = -0.3233;
    const double d = haversine_m(z_lat, z_lon, h_lat, h_lon);
    assert(d > 74000.0 && d < 76500.0);

    const Vec3d orig = geodetic_to_ecef(z_lat, z_lon, z_h);
    const EnuBasis b = enu_basis(z_lat, z_lon);
    const Vec3d loc_self = ecef_to_local(orig, orig, b);
    assert(std::abs(loc_self.x) < 1e-6 && std::abs(loc_self.y) < 1e-6 && std::abs(loc_self.z) < 1e-6);

    // 1 km North should map to (x~0, z~-1000) in renderer Y-up ENU.
    double m_lat = 0.0, m_lon = 0.0;
    meters_per_degree(z_lat, m_lat, m_lon);
    const Vec3d north1k = geodetic_to_ecef(z_lat + 1000.0 / m_lat, z_lon, z_h);
    const Vec3d loc_n = ecef_to_local(north1k, orig, b);
    assert(std::abs(loc_n.x) < 0.5);
    assert(std::abs(loc_n.z + 1000.0) < 1.5);
    // Spherical Earth drop at 1 km is ~0.078 m
    assert(loc_n.y < -0.05 && loc_n.y > -0.12);

    // 4. Web Mercator tile coordinates for Zaragoza LEZG at z15 (matches Python deg2tile).
    const MercatorTilePos tp = latlon_to_tile(z_lat, z_lon, 15);
    assert(tp.tx == 16289 && tp.ty == 12204);

    std::cout << "C++ geo_math & pack_format tests PASSED (d_LEZG_LEHC=" << d << " m)\n";
    return 0;
}
