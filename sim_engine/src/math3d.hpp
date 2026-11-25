#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace nsim {

constexpr double PI_D = 3.14159265358979323846;
constexpr float PI_F = 3.14159265358979323846f;
constexpr double DEG2RAD_D = PI_D / 180.0;
constexpr double RAD2DEG_D = 180.0 / PI_D;
constexpr float DEG2RAD_F = PI_F / 180.0f;

struct Vec2 {
    float x = 0.0f, y = 0.0f;
    Vec2() = default;
    Vec2(float x_, float y_) : x(x_), y(y_) {}
    Vec2 operator+(const Vec2 &o) const { return {x + o.x, y + o.y}; }
    Vec2 operator-(const Vec2 &o) const { return {x - o.x, y - o.y}; }
    Vec2 operator*(float s) const { return {x * s, y * s}; }
};

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    Vec3() = default;
    Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
    Vec3 operator+(const Vec3 &o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3 &o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator-() const { return {-x, -y, -z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    Vec3 operator/(float s) const { return {x / s, y / s, z / s}; }
};

struct Vec4 {
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 0.0f;
    Vec4() = default;
    Vec4(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
};

struct DVec3 {
    double x = 0.0, y = 0.0, z = 0.0;
    DVec3() = default;
    DVec3(double x_, double y_, double z_) : x(x_), y(y_), z(z_) {}
    DVec3 operator+(const DVec3 &o) const { return {x + o.x, y + o.y, z + o.z}; }
    DVec3 operator-(const DVec3 &o) const { return {x - o.x, y - o.y, z - o.z}; }
    DVec3 operator*(double s) const { return {x * s, y * s, z * s}; }
    Vec3 to_f32() const { return {static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)}; }
};

inline float dot(const Vec3 &a, const Vec3 &b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(const Vec3 &a, const Vec3 &b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float length(const Vec3 &v) { return std::sqrt(dot(v, v)); }
inline Vec3 normalize(const Vec3 &v) {
    float l = length(v);
    return (l > 1e-7f) ? (v / l) : Vec3(0.0f, 1.0f, 0.0f);
}
inline Vec3 mix(const Vec3 &a, const Vec3 &b, float t) { return a * (1.0f - t) + b * t; }

// Column-major 4x4 matrix matching OpenGL GLSL layout (m[col * 4 + row])
struct Mat4 {
    float m[16] = {};

    static Mat4 identity() {
        Mat4 r{};
        r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
        return r;
    }

    static Mat4 translate(const Vec3 &t) {
        Mat4 r = identity();
        r.m[12] = t.x;
        r.m[13] = t.y;
        r.m[14] = t.z;
        return r;
    }

    static Mat4 scale(const Vec3 &s) {
        Mat4 r{};
        r.m[0] = s.x;
        r.m[5] = s.y;
        r.m[10] = s.z;
        r.m[15] = 1.0f;
        return r;
    }

    static Mat4 rotate_yxz(float yaw_rad, float pitch_rad, float roll_rad) {
        float cy = std::cos(yaw_rad), sy = std::sin(yaw_rad);
        float cp = std::cos(pitch_rad), sp = std::sin(pitch_rad);
        float cr = std::cos(roll_rad), sr = std::sin(roll_rad);
        // R = Ry(yaw) * Rx(pitch) * Rz(roll)
        Mat4 r = identity();
        r.m[0] = cy * cr + sy * sp * sr;
        r.m[1] = cp * sr;
        r.m[2] = -sy * cr + cy * sp * sr;
        r.m[4] = -cy * sr + sy * sp * cr;
        r.m[5] = cp * cr;
        r.m[6] = sy * sr + cy * sp * cr;
        r.m[8] = sy * cp;
        r.m[9] = -sp;
        r.m[10] = cy * cp;
        return r;
    }

    static Mat4 look_at(const Vec3 &eye, const Vec3 &target, const Vec3 &up) {
        Vec3 f = normalize(target - eye);
        Vec3 s = normalize(cross(f, up));
        Vec3 u = cross(s, f);
        Mat4 r = identity();
        r.m[0] = s.x;  r.m[4] = s.y;  r.m[8]  = s.z;  r.m[12] = -dot(s, eye);
        r.m[1] = u.x;  r.m[5] = u.y;  r.m[9]  = u.z;  r.m[13] = -dot(u, eye);
        r.m[2] = -f.x; r.m[6] = -f.y; r.m[10] = -f.z; r.m[14] = dot(f, eye);
        return r;
    }

    // Reverse-Z infinite far plane projection (paired with glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE)):
    // Maps z = -near -> 1.0 and z = -infinity -> 0.0 -> sub-mm precision from 0.15m cockpit to 250km Pyrenees!
    static Mat4 perspective_reverse_z(float fovy_rad, float aspect, float z_near) {
        float f = 1.0f / std::tan(fovy_rad * 0.5f);
        Mat4 r{};
        r.m[0] = f / std::max(0.01f, aspect);
        r.m[5] = f;
        r.m[10] = 0.0f;
        r.m[11] = -1.0f;
        r.m[14] = z_near;
        r.m[15] = 0.0f;
        return r;
    }

    static Mat4 ortho(float l, float r_, float b, float t, float n, float f) {
        Mat4 res{};
        res.m[0] = 2.0f / (r_ - l);
        res.m[5] = 2.0f / (t - b);
        res.m[10] = -1.0f / (f - n);
        res.m[12] = -(r_ + l) / (r_ - l);
        res.m[13] = -(t + b) / (t - b);
        res.m[14] = -n / (f - n);
        res.m[15] = 1.0f;
        return res;
    }

    Mat4 operator*(const Mat4 &b) const {
        Mat4 res{};
        for (int c = 0; c < 4; c++) {
            for (int r = 0; r < 4; r++) {
                float s = 0.0f;
                for (int k = 0; k < 4; k++) {
                    s += m[k * 4 + r] * b.m[c * 4 + k];
                }
                res.m[c * 4 + r] = s;
            }
        }
        return res;
    }

    Vec4 mul(const Vec4 &v) const {
        return {
            m[0] * v.x + m[4] * v.y + m[8]  * v.z + m[12] * v.w,
            m[1] * v.x + m[5] * v.y + m[9]  * v.z + m[13] * v.w,
            m[2] * v.x + m[6] * v.y + m[10] * v.z + m[14] * v.w,
            m[3] * v.x + m[7] * v.y + m[11] * v.z + m[15] * v.w
        };
    }

    // Fast inverse of rigid-body or general perspective/view matrix via Gauss-Jordan
    Mat4 inverse() const {
        double a[4][8]{};
        for (int r = 0; r < 4; r++) {
            for (int c = 0; c < 4; c++) a[r][c] = m[c * 4 + r];
            a[r][4 + r] = 1.0;
        }
        for (int i = 0; i < 4; i++) {
            int piv = i;
            for (int r = i + 1; r < 4; r++) {
                if (std::abs(a[r][i]) > std::abs(a[piv][i])) piv = r;
            }
            if (piv != i) {
                for (int c = 0; c < 8; c++) std::swap(a[i][c], a[piv][c]);
            }
            double diag = a[i][i];
            if (std::abs(diag) < 1e-12) return identity();
            double inv_d = 1.0 / diag;
            for (int c = 0; c < 8; c++) a[i][c] *= inv_d;
            for (int r = 0; r < 4; r++) {
                if (r == i) continue;
                double fac = a[r][i];
                for (int c = 0; c < 8; c++) a[r][c] -= fac * a[i][c];
            }
        }
        Mat4 res{};
        for (int r = 0; r < 4; r++) {
            for (int c = 0; c < 4; c++) res.m[c * 4 + r] = static_cast<float>(a[r][4 + c]);
        }
        return res;
    }
};

// 5-plane infinite Reverse-Z view frustum (Left, Right, Bottom, Top, Near) for exact AABB culling
struct Frustum {
    Vec4 planes[5]; // ax + by + cz + d >= 0 is inside

    static Frustum from_vp_reverse_z(const Mat4 &vp) {
        auto row = [&](int r) -> Vec4 {
            return {vp.m[r], vp.m[4 + r], vp.m[8 + r], vp.m[12 + r]};
        };
        Vec4 r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);
        auto make_p = [](Vec4 p) -> Vec4 {
            float l = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
            return (l > 1e-7f) ? Vec4{p.x / l, p.y / l, p.z / l, p.w / l} : p;
        };
        Frustum fr{};
        fr.planes[0] = make_p({r3.x + r0.x, r3.y + r0.y, r3.z + r0.z, r3.w + r0.w}); // Left
        fr.planes[1] = make_p({r3.x - r0.x, r3.y - r0.y, r3.z - r0.z, r3.w - r0.w}); // Right
        fr.planes[2] = make_p({r3.x + r1.x, r3.y + r1.y, r3.z + r1.z, r3.w + r1.w}); // Bottom
        fr.planes[3] = make_p({r3.x - r1.x, r3.y - r1.y, r3.z - r1.z, r3.w - r1.w}); // Top
        fr.planes[4] = make_p({r3.x - r2.x, r3.y - r2.y, r3.z - r2.z, r3.w - r2.w}); // Near (Reverse-Z: z <= w)
        return fr;
    }

    bool intersects_aabb(const Vec3 &bmin, const Vec3 &bmax) const {
        for (int i = 0; i < 5; i++) {
            const Vec4 &p = planes[i];
            float px = (p.x >= 0.0f) ? bmax.x : bmin.x;
            float py = (p.y >= 0.0f) ? bmax.y : bmin.y;
            float pz = (p.z >= 0.0f) ? bmax.z : bmin.z;
            if (p.x * px + p.y * py + p.z * pz + p.w < 0.0f) return false;
        }
        return true;
    }
};

// Shared 10-Time-of-Day Iberian Solar Trajectory & 32-bit GI Packing (10 x 2-bit sun vis + 8-bit Sky AO + 4-bit aux)
static constexpr float kTodAzDeg[10] = {78.0f, 96.0f, 114.0f, 134.0f, 156.0f, 180.0f, 204.0f, 226.0f, 248.0f, 274.0f};
static constexpr float kTodElDeg[10] = { 7.0f, 13.0f,  19.0f,  30.0f,  42.0f,  48.0f,  42.0f,  30.0f,  20.0f,   8.0f};
static constexpr const char *kTodLabels[10] = {
    "07:15 DAWN", "08:30 EARLY AM", "09:45 FAIR VFR", "11:00 LATE AM", "12:15 PRE-NOON",
    "13:30 NOON", "14:45 EARLY PM", "16:00 MID PM",   "17:15 GOLDEN",  "18:30 SUNSET"
};

inline Vec3 tod_slot_sun_dir(int slot) {
    int s = std::clamp(slot, 0, 9);
    float az = kTodAzDeg[s] * DEG2RAD_F;
    float el = kTodElDeg[s] * DEG2RAD_F;
    return normalize(Vec3(std::sin(az) * std::cos(el), std::sin(el), -std::cos(az) * std::cos(el)));
}

inline float sun_dir_to_phase10(const Vec3 &sun_dir) {
    // OpenGL world: +X=East, +Y=Up, -Z=North => azimuth CW from North = atan2(x, -z)
    float az = std::atan2(sun_dir.x, -sun_dir.z) * (180.0f / PI_F);
    if (az < 0.0f) az += 360.0f;
    if (az <= kTodAzDeg[0]) return 0.0f;
    if (az >= kTodAzDeg[9]) return 9.0f;
    for (int i = 0; i < 9; i++) {
        if (az <= kTodAzDeg[i + 1]) {
            return static_cast<float>(i) + (az - kTodAzDeg[i]) / std::max(1e-3f, kTodAzDeg[i + 1] - kTodAzDeg[i]);
        }
    }
    return 9.0f;
}

inline float unpack_tod10_vis(uint32_t bits, float phase10) {
    float p = std::clamp(phase10, 0.0f, 9.0f);
    int k0 = std::clamp(static_cast<int>(std::floor(p)), 0, 9);
    int k1 = std::min(9, k0 + 1);
    float f = p - static_cast<float>(k0);
    float s0 = static_cast<float>((bits >> (k0 * 2)) & 3u) * (1.0f / 3.0f);
    float s1 = static_cast<float>((bits >> (k1 * 2)) & 3u) * (1.0f / 3.0f);
    return s0 + (s1 - s0) * f;
}

inline float unpack_tod10_ao(uint32_t bits) {
    return static_cast<float>((bits >> 20u) & 0xFFu) * (1.0f / 255.0f);
}

inline float unpack_tod10_night(uint32_t bits) {
    return static_cast<float>((bits >> 28u) & 0xFu) * (1.0f / 15.0f);
}

inline uint32_t pack_tod10_u32(const float vis[10], float sky_ao, float night_light = 0.0f) {
    uint32_t ao = static_cast<uint32_t>(std::clamp(std::lround(sky_ao * 255.0f), 0L, 255L));
    uint32_t nl = static_cast<uint32_t>(std::clamp(std::lround(night_light * 15.0f), 0L, 15L));
    uint32_t out = (ao << 20u) | (nl << 28u);
    for (int k = 0; k < 10; k++) {
        uint32_t q = static_cast<uint32_t>(std::clamp(std::lround(vis[k] * 3.0f), 0L, 3L));
        out |= (q << (k * 2));
    }
    return out;
}

inline uint32_t combine_bake_u32(uint32_t a, uint32_t b) {
    uint32_t ao_a = (a >> 20u) & 0xFFu;
    uint32_t ao_b = (b >> 20u) & 0xFFu;
    uint32_t ao = (ao_a * ao_b + 127u) / 255u;
    uint32_t nl = std::max((a >> 28u) & 0xFu, (b >> 28u) & 0xFu);
    uint32_t out = (ao << 20u) | (nl << 28u);
    for (int k = 0; k < 10; k++) {
        uint32_t shift = static_cast<uint32_t>(k * 2);
        uint32_t qa = (a >> shift) & 3u;
        uint32_t qb = (b >> shift) & 3u;
        out |= (std::min(qa, qb) << shift);
    }
    return out;
}

} // namespace nsim
