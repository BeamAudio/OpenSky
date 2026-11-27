#include "living_traffic.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <queue>
#include <sstream>

namespace nsim {

namespace {

std::string read_text_file(const std::string &path) {
    std::ifstream f(path);
    if (!f.is_open()) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

GLuint compile_shader(GLenum type, const std::string &src, const std::string &label) {
    GLuint s = glCreateShader(type);
    const char *p = src.c_str();
    glShaderSource(s, 1, &p, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        std::fprintf(stderr, "[LIVING-TRAFFIC] Shader error (%s):\n%s\n", label.c_str(), log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

uint32_t next_rng(uint32_t &state) {
    state = state * 1664525u + 1013904223u;
    return state;
}

float rand01(uint32_t &state) {
    return static_cast<float>((next_rng(state) >> 8) & 0xFFFFu) * (1.0f / 65535.0f);
}

float wrap_angle(float a) {
    while (a >  3.14159265f) a -= 6.2831853f;
    while (a < -3.14159265f) a += 6.2831853f;
    return a;
}

// Compute bearing in radians: 0 = North, +PI/2 = East (matches living_traffic.vert yaw!).
float bearing_rad(double lat0, double lon0, double lat1, double lon1) {
    double mlat, mlon;
    nspain::meters_per_degree(0.5 * (lat0 + lat1), mlat, mlon);
    double de = (lon1 - lon0) * mlon;
    double dn = (lat1 - lat0) * mlat;
    return static_cast<float>(std::atan2(de, dn));
}

struct MeshBuilder {
    std::vector<float> &v;
    std::vector<uint32_t> &idx;

    void add_box(float cx, float cy, float cz, float sx, float sy, float sz, float part) {
        const float hx = sx * 0.5f, hy = sy * 0.5f, hz = sz * 0.5f;
        const uint32_t base = static_cast<uint32_t>(v.size() / 7);
        auto push_quad = [&](Vec3 a, Vec3 b, Vec3 c, Vec3 d, Vec3 n) {
            uint32_t i0 = static_cast<uint32_t>(v.size() / 7);
            auto pv = [&](Vec3 p) { v.insert(v.end(), {p.x, p.y, p.z, n.x, n.y, n.z, part}); };
            pv(a); pv(b); pv(c); pv(d);
            idx.insert(idx.end(), {i0, i0 + 1, i0 + 2, i0, i0 + 2, i0 + 3});
        };
        (void)base;
        // +Y top, -Y bottom, +X right, -X left, -Z nose/bow, +Z tail/stern
        push_quad({cx - hx, cy + hy, cz - hz}, {cx + hx, cy + hy, cz - hz}, {cx + hx, cy + hy, cz + hz}, {cx - hx, cy + hy, cz + hz}, {0, 1, 0});
        push_quad({cx - hx, cy - hy, cz + hz}, {cx + hx, cy - hy, cz + hz}, {cx + hx, cy - hy, cz - hz}, {cx - hx, cy - hy, cz - hz}, {0, -1, 0});
        push_quad({cx + hx, cy - hy, cz - hz}, {cx + hx, cy - hy, cz + hz}, {cx + hx, cy + hy, cz + hz}, {cx + hx, cy + hy, cz - hz}, {1, 0, 0});
        push_quad({cx - hx, cy - hy, cz + hz}, {cx - hx, cy - hy, cz - hz}, {cx - hx, cy + hy, cz - hz}, {cx - hx, cy + hy, cz + hz}, {-1, 0, 0});
        push_quad({cx - hx, cy - hy, cz - hz}, {cx + hx, cy - hy, cz - hz}, {cx + hx, cy + hy, cz - hz}, {cx - hx, cy + hy, cz - hz}, {0, 0, -1});
        push_quad({cx + hx, cy - hy, cz + hz}, {cx - hx, cy - hy, cz + hz}, {cx - hx, cy + hy, cz + hz}, {cx + hx, cy + hy, cz + hz}, {0, 0, 1});
    }

    // Tapered wedge pointing toward -Z (aircraft nose or ship bow)
    void add_bow(float cx, float cy, float cz_aft, float cz_tip, float sx, float sy, float part) {
        const float hx = sx * 0.5f, hy = sy * 0.5f;
        Vec3 tl{cx - hx, cy + hy, cz_aft}, tr{cx + hx, cy + hy, cz_aft};
        Vec3 bl{cx - hx, cy - hy, cz_aft}, br{cx + hx, cy - hy, cz_aft};
        Vec3 tip_t{cx, cy + hy * 0.65f, cz_tip}, tip_b{cx, cy - hy * 0.35f, cz_tip};
        auto tri = [&](Vec3 a, Vec3 b, Vec3 c) {
            Vec3 n = normalize(cross(b - a, c - a));
            uint32_t i0 = static_cast<uint32_t>(v.size() / 7);
            auto pv = [&](Vec3 p) { v.insert(v.end(), {p.x, p.y, p.z, n.x, n.y, n.z, part}); };
            pv(a); pv(b); pv(c);
            idx.insert(idx.end(), {i0, i0 + 1, i0 + 2});
        };
        tri(tl, tr, tip_t);
        tri(br, bl, tip_b);
        tri(tr, br, tip_b); tri(tr, tip_b, tip_t);
        tri(bl, tl, tip_t); tri(bl, tip_t, tip_b);
    }

    // Closed 6-face swept wing pair (left + right, -Z forward, zero backface-culled gaps!)
    void add_swept_wings(float cy, float cz_root, float span, float chord_root, float chord_tip, float sweep_z, float thick, float part) {
        const float hs = span * 0.5f;
        const float ht = thick * 0.5f;
        auto push_quad = [&](Vec3 a, Vec3 b, Vec3 c, Vec3 d) {
            Vec3 n = normalize(cross(b - a, c - a));
            uint32_t i0 = static_cast<uint32_t>(v.size() / 7);
            auto pv = [&](Vec3 p) { v.insert(v.end(), {p.x, p.y, p.z, n.x, n.y, n.z, part}); };
            pv(a); pv(b); pv(c); pv(d);
            idx.insert(idx.end(), {i0, i0 + 1, i0 + 2, i0, i0 + 2, i0 + 3});
        };
        for (float side : {-1.0f, 1.0f}) {
            Vec3 r0t{0.0f, cy + ht, cz_root - chord_root * 0.5f};
            Vec3 r1t{0.0f, cy + ht, cz_root + chord_root * 0.5f};
            Vec3 t0t{side * hs, cy + 0.35f + ht * 0.6f, cz_root + sweep_z - chord_tip * 0.5f};
            Vec3 t1t{side * hs, cy + 0.35f + ht * 0.6f, cz_root + sweep_z + chord_tip * 0.5f};
            Vec3 r0b{0.0f, cy - ht, cz_root - chord_root * 0.5f};
            Vec3 r1b{0.0f, cy - ht, cz_root + chord_root * 0.5f};
            Vec3 t0b{side * hs, cy + 0.35f - ht * 0.6f, cz_root + sweep_z - chord_tip * 0.5f};
            Vec3 t1b{side * hs, cy + 0.35f - ht * 0.6f, cz_root + sweep_z + chord_tip * 0.5f};
            if (side > 0.0f) {
                push_quad(r0t, t0t, t1t, r1t); // Top
                push_quad(r0b, r1b, t1b, t0b); // Bottom
                push_quad(r0b, t0b, t0t, r0t); // Leading edge
                push_quad(r1t, t1t, t1b, r1b); // Trailing edge
                push_quad(t0b, t1b, t1t, t0t); // Wingtip cap
            } else {
                push_quad(r0t, r1t, t1t, t0t); // Top
                push_quad(r0b, t0b, t1b, r1b); // Bottom
                push_quad(r0t, t0t, t0b, r0b); // Leading edge
                push_quad(r1b, t1b, t1t, r1t); // Trailing edge
                push_quad(t0t, t1t, t1b, t0b); // Wingtip cap
            }
        }
    }
};

} // namespace

LivingTraffic::~LivingTraffic() { shutdown(); }

void LivingTraffic::shutdown() {
    if (prog_) { glDeleteProgram(prog_); prog_ = 0; }
    if (vao_)  { glDeleteVertexArrays(1, &vao_); vao_ = 0; }
    if (vbo_)  { glDeleteBuffers(1, &vbo_); vbo_ = 0; }
    if (ebo_)  { glDeleteBuffers(1, &ebo_); ebo_ = 0; }
    if (ssbo_) { glDeleteBuffers(1, &ssbo_); ssbo_ = 0; }
}

bool LivingTraffic::init(const PackReader *pack, const std::string &shader_root) {
    pack_ = pack;
    if (!pack_) return false;

    std::string vs = read_text_file(shader_root + "/living_traffic.vert");
    std::string fs = read_text_file(shader_root + "/living_traffic.frag");
    if (vs.empty() || fs.empty()) return false;
    GLuint vsh = compile_shader(GL_VERTEX_SHADER, vs, "living_traffic.vert");
    GLuint fsh = compile_shader(GL_FRAGMENT_SHADER, fs, "living_traffic.frag");
    if (!vsh || !fsh) {
        if (vsh) glDeleteShader(vsh);
        if (fsh) glDeleteShader(fsh);
        return false;
    }
    prog_ = glCreateProgram();
    glAttachShader(prog_, vsh);
    glAttachShader(prog_, fsh);
    glLinkProgram(prog_);
    glDeleteShader(vsh);
    glDeleteShader(fsh);
    GLint ok = 0;
    glGetProgramiv(prog_, GL_LINK_STATUS, &ok);
    if (!ok) {
        glDeleteProgram(prog_);
        prog_ = 0;
        return false;
    }
    u_vp_   = glGetUniformLocation(prog_, "u_vp");
    u_cam_  = glGetUniformLocation(prog_, "u_cam_enu");
    u_base_ = glGetUniformLocation(prog_, "u_base_instance");
    u_sun_  = glGetUniformLocation(prog_, "u_sun_dir");
    u_cool_ = glGetUniformLocation(prog_, "u_haze_cool");
    u_warm_ = glGetUniformLocation(prog_, "u_haze_warm");
    u_hd_   = glGetUniformLocation(prog_, "u_haze_density");
    u_time_ = glGetUniformLocation(prog_, "u_time");

    build_meshes();
    init_air_fleet();
    init_sea_graph_and_fleet();
    prev_t_ = -1.0f;
    std::printf("[LIVING-TRAFFIC] Active: %zu aerial flights across %zu airports | %zu maritime vessels across %zu ports\n",
                air_agents_.size(), pack_->airports().size(), ship_agents_.size(), active_ports_.size());
    return true;
}

void LivingTraffic::on_reanchor() {
    ramp_center_lat_ = 1e9;
    ramp_center_lon_ = 1e9;
}

void LivingTraffic::build_meshes() {
    std::vector<float> v;
    std::vector<uint32_t> idx;
    MeshBuilder mb{v, idx};

    auto begin_mesh = [&](MeshKind k) {
        auto &r = ranges_[static_cast<int>(k)];
        r.first_index = static_cast<uint32_t>(idx.size());
        r.base_vertex = 0;
    };
    auto end_mesh = [&](MeshKind k) {
        auto &r = ranges_[static_cast<int>(k)];
        r.index_count = static_cast<uint32_t>(idx.size()) - r.first_index;
    };

    // 0. GA High-Wing Prop Aircraft (~11 m wingspan, closed solid 3D boxes so wings never backface-cull, tires at Y=0!)
    begin_mesh(MeshKind::GA);
    mb.add_box(0.0f, 1.38f,  0.10f, 1.24f, 1.16f, 4.6f, 0.0f);   // Fuselage cabin
    mb.add_bow(0.0f, 1.34f, -2.20f, -3.55f, 1.10f, 0.96f, 0.0f); // Cowling nose
    mb.add_box(0.0f, 1.38f,  3.20f, 0.62f, 0.70f, 3.0f, 0.0f);   // Tail boom
    mb.add_box(0.0f, 1.48f,  0.40f, 1.28f, 0.24f, 4.5f, 1.0f);   // Accent cheatline
    mb.add_box(0.0f, 1.62f, -0.70f, 1.18f, 0.56f, 1.5f, 2.0f);   // Cabin windshield/windows
    mb.add_box(0.0f, 2.08f, -0.25f, 10.8f, 0.20f, 1.44f, 0.0f);  // Solid closed high wing
    mb.add_box(-2.4f, 1.52f, -0.25f, 0.10f, 1.08f, 0.14f, 0.0f); // Left wing strut
    mb.add_box( 2.4f, 1.52f, -0.25f, 0.10f, 1.08f, 0.14f, 0.0f); // Right wing strut
    mb.add_box(0.0f, 2.48f,  4.15f, 0.14f, 1.65f, 1.25f, 1.0f);  // Vertical fin
    mb.add_box(0.0f, 1.68f,  4.35f, 3.45f, 0.14f, 0.86f, 0.0f);  // Horizontal tail
    mb.add_box(-1.05f, 0.42f, 0.20f, 0.22f, 0.84f, 0.36f, 2.0f); // Left gear
    mb.add_box( 1.05f, 0.42f, 0.20f, 0.22f, 0.84f, 0.36f, 2.0f); // Right gear
    mb.add_box( 0.00f, 0.38f,-2.30f, 0.20f, 0.76f, 0.32f, 2.0f); // Nose gear
    // Compact nav / beacon / strobe / landing lights flush on wingtips & tail
    mb.add_box(-5.42f, 2.08f, -0.32f, 0.12f, 0.10f, 0.14f, 3.0f); // Port red nav
    mb.add_box( 5.42f, 2.08f, -0.32f, 0.12f, 0.10f, 0.14f, 4.0f); // Starboard green nav
    mb.add_box( 0.00f, 3.34f,  4.25f, 0.14f, 0.14f, 0.14f, 3.0f); // Tail beacon
    mb.add_box(-5.42f, 2.08f, -0.10f, 0.10f, 0.10f, 0.12f, 5.0f); // Left strobe
    mb.add_box( 5.42f, 2.08f, -0.10f, 0.10f, 0.10f, 0.12f, 5.0f); // Right strobe
    mb.add_box( 0.00f, 1.20f, -3.55f, 0.22f, 0.18f, 0.14f, 6.0f); // Nose landing light
    end_mesh(MeshKind::GA);

    // 1. Twinjet Airliner (~35 m wingspan, A320/B737 scale, landing gear grounded at Y=0)
    begin_mesh(MeshKind::Airliner);
    mb.add_box(0.0f, 3.6f,  0.0f, 3.9f, 4.0f, 29.0f, 0.0f);
    mb.add_bow(0.0f, 3.4f, -14.5f, -20.0f, 3.7f, 3.6f, 0.0f);
    mb.add_bow(0.0f, 3.8f,  14.5f,  19.5f, 3.2f, 3.0f, 0.0f);
    mb.add_box(0.0f, 4.0f, -15.6f, 3.2f, 1.0f, 2.2f, 2.0f); // Cockpit windscreen
    mb.add_swept_wings(2.6f, -1.5f, 35.8f, 5.8f, 1.7f, 6.5f, 0.62f, 0.0f);
    mb.add_box(-5.8f, 1.6f, -3.2f, 2.1f, 2.1f, 4.4f, 1.0f); // Left turbofan
    mb.add_box( 5.8f, 1.6f, -3.2f, 2.1f, 2.1f, 4.4f, 1.0f); // Right turbofan
    mb.add_box(0.0f, 8.2f,  15.6f, 0.55f, 7.6f, 5.4f, 1.0f); // Swept airline tail
    mb.add_swept_wings(4.1f, 15.8f, 12.6f, 2.8f, 1.2f, 2.6f, 0.36f, 0.0f); // Horizontal stab
    // Grounded tricycle landing gear struts + bogeys
    mb.add_box(-3.6f, 1.1f,  0.6f, 0.55f, 2.2f, 0.95f, 2.0f);
    mb.add_box( 3.6f, 1.1f,  0.6f, 0.55f, 2.2f, 0.95f, 2.0f);
    mb.add_box( 0.0f, 1.0f,-13.8f, 0.45f, 2.0f, 0.75f, 2.0f);
    // Crisp flush anti-collision & nav lights
    mb.add_box(-17.9f, 3.0f,  4.8f, 0.28f, 0.24f, 0.30f, 3.0f); // Port red
    mb.add_box( 17.9f, 3.0f,  4.8f, 0.28f, 0.24f, 0.30f, 4.0f); // Starboard green
    mb.add_box(  0.0f, 5.7f,  0.0f, 0.32f, 0.28f, 0.32f, 3.0f); // Dorsal beacon
    mb.add_box(-17.9f, 3.0f,  5.3f, 0.28f, 0.24f, 0.30f, 5.0f); // Left strobe
    mb.add_box( 17.9f, 3.0f,  5.3f, 0.28f, 0.24f, 0.30f, 5.0f); // Right strobe
    mb.add_box(  0.0f, 2.2f, -18.5f, 0.65f, 0.50f, 0.40f, 6.0f); // Nose landing light
    end_mesh(MeshKind::Airliner);

    // 2. Container / Cargo Ship (~175 m long, sits at water surface Y = 0)
    begin_mesh(MeshKind::Cargo);
    mb.add_box(0.0f,  4.2f,   4.0f, 26.0f,  9.5f, 134.0f, 0.0f); // Main hull
    mb.add_bow(0.0f,  4.8f, -63.0f, -92.0f, 26.0f, 10.2f, 0.0f); // Pointed bow
    mb.add_box(0.0f, 13.0f, -12.0f, 22.0f,  8.5f,  88.0f, 1.0f); // Stacked cargo containers
    mb.add_box(0.0f, 18.0f,  50.0f, 22.0f, 18.0f,  16.0f, 0.0f); // Aft bridge tower
    mb.add_box(0.0f, 23.5f,  43.0f, 20.0f,  3.0f,   4.0f, 2.0f); // Bridge windows
    mb.add_box(0.0f, 27.5f,  54.0f,  5.5f,  6.5f,   7.5f, 1.0f); // Funnel
    mb.add_box(-12.5f, 25.0f, 45.0f, 1.6f, 1.6f, 1.6f, 3.0f);    // Port red nav
    mb.add_box( 12.5f, 25.0f, 45.0f, 1.6f, 1.6f, 1.6f, 4.0f);    // Starboard green nav
    mb.add_box(  0.0f, 31.5f, 50.0f, 1.8f, 1.8f, 1.8f, 5.0f);    // Masthead white
    mb.add_box(  0.0f, 16.0f, 41.0f, 18.0f, 2.0f, 1.5f, 6.0f);   // Deck work lights
    // Animated sea wake ribbon extending aft (+Z)
    mb.add_box(0.0f, 0.35f, 132.0f, 32.0f, 0.20f, 140.0f, 7.0f);
    end_mesh(MeshKind::Cargo);

    // 3. Mediterranean Ferry (~140 m long, white/blue multi-deck Ro-Pax)
    begin_mesh(MeshKind::Ferry);
    mb.add_box(0.0f,  4.5f,   2.0f, 23.0f,  9.5f, 108.0f, 0.0f); // Lower hull
    mb.add_bow(0.0f,  5.0f, -52.0f, -74.0f, 23.0f, 10.0f, 0.0f); // Bow
    mb.add_box(0.0f, 13.5f,   0.0f, 21.0f,  9.0f,  94.0f, 1.0f); // Passenger decks
    mb.add_box(0.0f, 15.2f, -38.0f, 19.0f,  3.2f,  12.0f, 2.0f); // Forward bridge glass
    mb.add_box(0.0f, 12.8f,   0.0f, 21.4f,  2.2f,  76.0f, 6.0f); // Illuminated passenger cabin window band
    mb.add_box(0.0f, 20.5f,  24.0f,  6.0f,  6.5f,  10.0f, 0.0f); // Funnel
    mb.add_box(-11.0f, 18.5f, -36.0f, 1.4f, 1.4f, 1.4f, 3.0f);
    mb.add_box( 11.0f, 18.5f, -36.0f, 1.4f, 1.4f, 1.4f, 4.0f);
    mb.add_box(  0.0f, 24.5f, -32.0f, 1.6f, 1.6f, 1.6f, 5.0f);
    mb.add_box(0.0f, 0.35f, 108.0f, 28.0f, 0.20f, 120.0f, 7.0f);
    end_mesh(MeshKind::Ferry);

    // 4. Coastal Yacht / Fishing Boat (~18 m long)
    begin_mesh(MeshKind::Boat);
    mb.add_box(0.0f, 1.1f,  0.5f, 4.8f, 2.2f, 13.0f, 0.0f);
    mb.add_bow(0.0f, 1.2f, -6.0f, -10.5f, 4.8f, 2.3f, 0.0f);
    mb.add_box(0.0f, 2.8f,  0.5f, 3.4f, 1.7f,  5.6f, 1.0f);
    mb.add_box(0.0f, 3.0f, -2.0f, 3.0f, 0.9f,  1.4f, 2.0f);
    mb.add_box(0.0f, 5.8f,  0.8f, 0.35f, 4.6f, 0.35f, 0.0f);
    mb.add_box(0.0f, 8.2f,  0.8f, 0.55f, 0.55f, 0.55f, 5.0f);
    mb.add_box(0.0f, 0.20f, 16.0f, 6.2f, 0.14f, 22.0f, 7.0f);
    end_mesh(MeshKind::Boat);

    glCreateVertexArrays(1, &vao_);
    glCreateBuffers(1, &vbo_);
    glCreateBuffers(1, &ebo_);
    glCreateBuffers(1, &ssbo_);
    glNamedBufferStorage(vbo_, static_cast<GLsizeiptr>(v.size() * sizeof(float)), v.data(), 0);
    glNamedBufferStorage(ebo_, static_cast<GLsizeiptr>(idx.size() * sizeof(uint32_t)), idx.data(), 0);
    glNamedBufferData(ssbo_, static_cast<GLsizeiptr>(MAX_ENTITIES * sizeof(EntityGpu)), nullptr, GL_DYNAMIC_DRAW);

    glVertexArrayVertexBuffer(vao_, 0, vbo_, 0, 7 * sizeof(float));
    glVertexArrayElementBuffer(vao_, ebo_);
    glEnableVertexArrayAttrib(vao_, 0); glVertexArrayAttribFormat(vao_, 0, 3, GL_FLOAT, GL_FALSE, 0); glVertexArrayAttribBinding(vao_, 0, 0);
    glEnableVertexArrayAttrib(vao_, 1); glVertexArrayAttribFormat(vao_, 1, 3, GL_FLOAT, GL_FALSE, 12); glVertexArrayAttribBinding(vao_, 1, 0);
    glEnableVertexArrayAttrib(vao_, 2); glVertexArrayAttribFormat(vao_, 2, 1, GL_FLOAT, GL_FALSE, 24); glVertexArrayAttribBinding(vao_, 2, 0);
}

void LivingTraffic::init_air_fleet() {
    air_agents_.clear();
    const auto &aps = pack_->airports();
    if (aps.size() < 2) return;

    const int n_ga = std::min(22, std::max(8, static_cast<int>(aps.size()) / 2));
    const int n_jet = 12;
    air_agents_.reserve(static_cast<size_t>(n_ga + n_jet));
    for (int i = 0; i < n_ga + n_jet; ++i) {
        AirAgent a{};
        a.is_airliner = (i >= n_ga);
        a.rng = static_cast<uint32_t>(i + 1) * 2654435761u ^ 0x51A7F319u;
        a.livery = static_cast<float>(i % 8);
        a.beacon_phase = rand01(a.rng);
        plan_air_flight(a, true);
        air_agents_.push_back(a);
    }
}

void LivingTraffic::plan_air_flight(AirAgent &a, bool stagger_phase) {
    const auto &aps = pack_->airports();
    if (aps.size() < 2) return;

    // Collect suitable airports (airliners use major/medium paved runways >= 1,800 m; GA uses all fields).
    std::vector<int> pool;
    pool.reserve(aps.size());
    for (int i = 0; i < static_cast<int>(aps.size()); ++i) {
        const auto &ap = aps[i];
        double rlen = nspain::haversine_m(ap.rwy_lat_a, ap.rwy_lon_a, ap.rwy_lat_b, ap.rwy_lon_b);
        if (a.is_airliner) {
            if (ap.kind == "large_airport" || ap.kind == "medium_airport" || rlen >= 1800.0) pool.push_back(i);
        } else {
            pool.push_back(i);
        }
    }
    if (pool.size() < 2) {
        pool.clear();
        for (int i = 0; i < static_cast<int>(aps.size()); ++i) pool.push_back(i);
    }

    int orig = (a.dest_ap >= 0 && std::find(pool.begin(), pool.end(), a.dest_ap) != pool.end())
        ? a.dest_ap
        : pool[next_rng(a.rng) % pool.size()];
    int dest = orig;
    for (int tries = 0; tries < 16; ++tries) {
        int cand = pool[next_rng(a.rng) % pool.size()];
        if (cand == orig) continue;
        double d_km = nspain::haversine_m(aps[orig].lat, aps[orig].lon, aps[cand].lat, aps[cand].lon) * 0.001;
        if (a.is_airliner ? (d_km >= 35.0) : (d_km >= 12.0 && d_km <= 220.0)) { dest = cand; break; }
        dest = cand;
    }
    if (dest == orig) dest = pool[(orig + 1) % pool.size()];

    a.orig_ap = orig;
    a.dest_ap = dest;
    const auto &ap0 = aps[orig];
    const auto &ap1 = aps[dest];

    a.rwy_a_lat = ap0.rwy_lat_a; a.rwy_a_lon = ap0.rwy_lon_a; a.rwy_a_elev = pack_->sample_elevation(ap0.rwy_lat_a, ap0.rwy_lon_a);
    a.rwy_b_lat = ap0.rwy_lat_b; a.rwy_b_lon = ap0.rwy_lon_b; a.rwy_b_elev = pack_->sample_elevation(ap0.rwy_lat_b, ap0.rwy_lon_b);
    a.dest_a_lat = ap1.rwy_lat_a; a.dest_a_lon = ap1.rwy_lon_a; a.dest_a_elev = pack_->sample_elevation(ap1.rwy_lat_a, ap1.rwy_lon_a);
    a.dest_b_lat = ap1.rwy_lat_b; a.dest_b_lon = ap1.rwy_lon_b; a.dest_b_elev = pack_->sample_elevation(ap1.rwy_lat_b, ap1.rwy_lon_b);

    // 4.5 km final approach fix aligned strictly with destination runway extended centerline.
    double mlat1, mlon1;
    nspain::meters_per_degree(a.dest_a_lat, mlat1, mlon1);
    double rde = (a.dest_b_lon - a.dest_a_lon) * mlon1;
    double rdn = (a.dest_b_lat - a.dest_a_lat) * mlat1;
    double rlen = std::max(100.0, std::hypot(rde, rdn));
    double app_dist = a.is_airliner ? 7500.0 : 4200.0;
    a.app_lat = a.dest_a_lat - (rdn / rlen) * app_dist / mlat1;
    a.app_lon = a.dest_a_lon - (rde / rlen) * app_dist / mlon1;

    // Sample terrain along route to guarantee safe VFR/IFR terrain clearance.
    double max_gnd = std::max(a.rwy_a_elev, a.dest_a_elev);
    for (int s = 1; s <= 7; ++s) {
        double t = static_cast<double>(s) / 8.0;
        double sl = ap0.lat + (ap1.lat - ap0.lat) * t;
        double slo = ap0.lon + (ap1.lon - ap0.lon) * t;
        max_gnd = std::max(max_gnd, pack_->sample_elevation(sl, slo));
    }
    if (a.is_airliner) {
        a.cruise_alt_m = std::max(max_gnd + 1800.0, 3200.0 + rand01(a.rng) * 4200.0);
        a.cruise_spd_mps = 145.0 + rand01(a.rng) * 55.0; // ~280-390 kt TAS
    } else {
        a.cruise_alt_m = std::max(max_gnd + 550.0, std::max(a.rwy_a_elev, a.dest_a_elev) + 650.0) + rand01(a.rng) * 550.0;
        a.cruise_spd_mps = 52.0 + rand01(a.rng) * 16.0; // ~100-132 kt TAS
    }

    // Verified ramp stand (reserve stand #0 for player; give airliners >= 60 m clearance from GA ramp!).
    const auto stands = pack_->compute_parking_stands(ap0, 6);
    size_t pick = 0;
    if (stands.size() > 1) {
        pick = 1 + (next_rng(a.rng) % (stands.size() - 1));
        if (a.is_airliner) pick = stands.size() - 1;
    }
    const ParkingStand &ps = stands[pick];
    a.park_lat = ps.lat; a.park_lon = ps.lon; a.park_elev = ps.elev_m; a.park_hdg_deg = ps.heading_deg;

    if (!stagger_phase) {
        a.phase = AirPhase::Parked;
        a.lat = a.park_lat; a.lon = a.park_lon; a.alt_m = a.park_elev;
        a.heading_rad = static_cast<float>(a.park_hdg_deg * DEG2RAD_D);
        a.pitch_rad = 0.0f; a.roll_rad = 0.0f; a.speed_mps = 0.0f;
        a.phase_timer = 15.0f + rand01(a.rng) * 35.0f;
        return;
    }

    // Stagger initial fleet so airports, departures, cruise corridors & finals are all alive immediately.
    float u = rand01(a.rng);
    if (u < 0.14f) {
        a.phase = AirPhase::Parked;
        a.lat = a.park_lat; a.lon = a.park_lon; a.alt_m = a.park_elev;
        a.heading_rad = static_cast<float>(a.park_hdg_deg * DEG2RAD_D);
        a.speed_mps = 0.0f;
        a.phase_timer = 4.0f + rand01(a.rng) * 20.0f;
    } else if (u < 0.28f) {
        a.phase = AirPhase::Takeoff;
        a.lat = a.rwy_a_lat; a.lon = a.rwy_a_lon; a.alt_m = a.rwy_a_elev;
        a.heading_rad = bearing_rad(a.rwy_a_lat, a.rwy_a_lon, a.rwy_b_lat, a.rwy_b_lon);
        a.speed_mps = 8.0f;
        a.phase_timer = 0.0f;
    } else if (u < 0.84f) {
        a.phase = AirPhase::Cruise;
        double frac = 0.08 + 0.82 * rand01(a.rng);
        a.lat = a.rwy_b_lat + (a.app_lat - a.rwy_b_lat) * frac;
        a.lon = a.rwy_b_lon + (a.app_lon - a.rwy_b_lon) * frac;
        a.alt_m = a.cruise_alt_m;
        a.heading_rad = bearing_rad(a.lat, a.lon, a.app_lat, a.app_lon);
        a.speed_mps = static_cast<float>(a.cruise_spd_mps);
    } else {
        a.phase = AirPhase::Approach;
        double frac = 0.15 + 0.65 * rand01(a.rng);
        a.lat = a.app_lat + (a.dest_a_lat - a.app_lat) * frac;
        a.lon = a.app_lon + (a.dest_a_lon - a.app_lon) * frac;
        double dist_rem = nspain::haversine_m(a.lat, a.lon, a.dest_a_lat, a.dest_a_lon);
        a.alt_m = a.dest_a_elev + dist_rem * 0.055; // ~3.1 deg glideslope
        a.heading_rad = bearing_rad(a.lat, a.lon, a.dest_a_lat, a.dest_a_lon);
        a.speed_mps = a.is_airliner ? 74.0f : 36.0f;
    }
    a.pitch_rad = 0.0f;
    a.roll_rad = 0.0f;
}

void LivingTraffic::step_air_agent(AirAgent &a, float dt) {
    auto advance_geo = [&](double spd_mps, float hdg_rad, float step_dt) {
        double mlat, mlon;
        nspain::meters_per_degree(a.lat, mlat, mlon);
        a.lat += (std::cos(hdg_rad) * spd_mps * step_dt) / mlat;
        a.lon += (std::sin(hdg_rad) * spd_mps * step_dt) / mlon;
    };

    auto steer_toward = [&](double tlat, double tlon, float max_rate_rad, float step_dt) {
        float want = bearing_rad(a.lat, a.lon, tlat, tlon);
        float err = wrap_angle(want - a.heading_rad);
        float turn = std::clamp(err, -max_rate_rad * step_dt, max_rate_rad * step_dt);
        a.heading_rad = wrap_angle(a.heading_rad + turn);
        float yaw_rate = (step_dt > 1e-4f) ? (turn / step_dt) : 0.0f;
        // Coordinated bank angle: roll = atan(v * omega / g)
        float want_roll = std::clamp(std::atan((a.speed_mps * yaw_rate) / 9.81f), -0.44f, 0.44f);
        a.roll_rad += (want_roll - a.roll_rad) * std::min(1.0f, step_dt * 3.5f);
    };

    const float vr = a.is_airliner ? 72.0f : 31.0f;
    const float climb_spd = a.is_airliner ? 115.0f : 44.0f;
    const float climb_vs = a.is_airliner ? 11.5f : 4.2f;

    switch (a.phase) {
    case AirPhase::Parked:
        a.phase_timer -= dt;
        a.speed_mps = 0.0f;
        a.pitch_rad = 0.0f;
        a.roll_rad = 0.0f;
        if (a.phase_timer <= 0.0f) {
            a.phase = AirPhase::Takeoff;
            a.lat = a.rwy_a_lat; a.lon = a.rwy_a_lon; a.alt_m = a.rwy_a_elev;
            a.heading_rad = bearing_rad(a.rwy_a_lat, a.rwy_a_lon, a.rwy_b_lat, a.rwy_b_lon);
            a.speed_mps = 2.0f;
        }
        break;
    case AirPhase::Takeoff: {
        a.speed_mps += (a.is_airliner ? 3.2f : 2.6f) * dt;
        advance_geo(a.speed_mps, a.heading_rad, dt);
        a.alt_m = pack_->sample_elevation(a.lat, a.lon);
        a.roll_rad = 0.0f;
        a.pitch_rad = (a.speed_mps > vr * 0.85f) ? 0.14f : 0.0f;
        if (a.speed_mps >= vr) {
            a.phase = AirPhase::Climb;
            a.phase_timer = 0.0f;
        }
        break;
    }
    case AirPhase::Climb: {
        a.phase_timer += dt;
        a.speed_mps += std::clamp(climb_spd - a.speed_mps, -4.0f * dt, 4.0f * dt);
        if (a.phase_timer > 6.0f) steer_toward(a.app_lat, a.app_lon, a.is_airliner ? 0.045f : 0.09f, dt);
        advance_geo(a.speed_mps, a.heading_rad, dt);
        a.alt_m += climb_vs * dt;
        a.pitch_rad += (0.12f - a.pitch_rad) * std::min(1.0f, dt * 3.0f);
        if (a.alt_m >= a.cruise_alt_m) {
            a.alt_m = a.cruise_alt_m;
            a.phase = AirPhase::Cruise;
        }
        break;
    }
    case AirPhase::Cruise: {
        a.speed_mps += std::clamp(static_cast<float>(a.cruise_spd_mps) - a.speed_mps, -5.0f * dt, 5.0f * dt);
        steer_toward(a.app_lat, a.app_lon, a.is_airliner ? 0.045f : 0.085f, dt);
        advance_geo(a.speed_mps, a.heading_rad, dt);
        double d_app = nspain::haversine_m(a.lat, a.lon, a.app_lat, a.app_lon);
        double top_of_desc = a.is_airliner ? 22000.0 : 8500.0;
        double target_alt = a.cruise_alt_m;
        if (d_app < top_of_desc) {
            double app_alt = a.dest_a_elev + (a.is_airliner ? 420.0 : 240.0);
            double frac = std::clamp(d_app / top_of_desc, 0.0, 1.0);
            target_alt = app_alt + (a.cruise_alt_m - app_alt) * frac;
        }
        double gnd = pack_->sample_elevation(a.lat, a.lon);
        target_alt = std::max(target_alt, gnd + 240.0);
        double dz = std::clamp(target_alt - a.alt_m, -(a.is_airliner ? 9.5 : 4.5) * dt, (a.is_airliner ? 8.0 : 4.0) * dt);
        a.alt_m += dz;
        float want_pitch = static_cast<float>(std::atan2(dz / std::max(0.001f, dt), std::max(20.0f, a.speed_mps)));
        a.pitch_rad += (want_pitch - a.pitch_rad) * std::min(1.0f, dt * 3.0f);
        if (d_app < (a.is_airliner ? 1200.0 : 650.0)) a.phase = AirPhase::Approach;
        break;
    }
    case AirPhase::Approach: {
        float app_spd = a.is_airliner ? 72.0f : 35.0f;
        a.speed_mps += std::clamp(app_spd - a.speed_mps, -5.0f * dt, 4.0f * dt);
        steer_toward(a.dest_a_lat, a.dest_a_lon, a.is_airliner ? 0.055f : 0.11f, dt);
        advance_geo(a.speed_mps, a.heading_rad, dt);
        double d_thr = nspain::haversine_m(a.lat, a.lon, a.dest_a_lat, a.dest_a_lon);
        double gs_alt = a.dest_a_elev + d_thr * 0.055;
        double dz = std::clamp(gs_alt - a.alt_m, -8.5 * dt, 3.0 * dt);
        a.alt_m = std::max(a.dest_a_elev, a.alt_m + dz);
        a.pitch_rad += (-0.045f - a.pitch_rad) * std::min(1.0f, dt * 3.0f);
        if (d_thr < 160.0) {
            a.phase = AirPhase::Rollout;
            a.heading_rad = bearing_rad(a.dest_a_lat, a.dest_a_lon, a.dest_b_lat, a.dest_b_lon);
            a.alt_m = a.dest_a_elev;
        }
        break;
    }
    case AirPhase::Rollout: {
        a.speed_mps = std::max(0.0f, a.speed_mps - (a.is_airliner ? 3.4f : 2.8f) * dt);
        advance_geo(a.speed_mps, a.heading_rad, dt);
        a.alt_m = pack_->sample_elevation(a.lat, a.lon);
        a.pitch_rad *= std::max(0.0f, 1.0f - dt * 5.0f);
        a.roll_rad *= std::max(0.0f, 1.0f - dt * 5.0f);
        if (a.speed_mps <= 2.5f) {
            // Arrived cleanly at destination airport -> dispatch a brand-new individual flight!
            plan_air_flight(a, false);
        }
        break;
    }
    }
}

void LivingTraffic::init_sea_graph_and_fleet() {
    sea_nodes_.clear();
    sea_adj_.clear();
    active_ports_.clear();
    deep_ports_.clear();
    ship_agents_.clear();

    // Real Catalan Mediterranean ports, coastal fairways & offshore Balearic/Marseille/Valencia shipping lanes!
    static const SeaNode kNodes[] = {
        // 0..10: Real ports & harbor approaches (strictly over open port entrance water)
        {"Port de Barcelona",       41.3180, 2.1850, true,  true }, // 0
        {"Port Olimpic Barcelona",  41.3760, 2.2120, true,  false}, // 1
        {"Badalona Marina",         41.4280, 2.2620, true,  false}, // 2
        {"Mataro Port",             41.5220, 2.4620, true,  false}, // 3
        {"Blanes Port",             41.6620, 2.8120, true,  false}, // 4
        {"Palamos Commercial Port", 41.8340, 3.1380, true,  true }, // 5
        {"Roses / Empuriabrava",    42.2420, 3.1750, true,  false}, // 6
        {"Vilanova i la Geltru",    41.2020, 1.7450, true,  false}, // 7
        {"Port de Tarragona",       41.0780, 1.2350, true,  true }, // 8
        {"Cambrils Marina",         41.0480, 1.0720, true,  false}, // 9
        {"Sant Carles / Ebro",      40.5950, 0.6500, true,  false}, // 10
        // 11..21: Coastal shipping fairways & offshore Mediterranean lanes
        {"BCN Sea Buoy",            41.2850, 2.2450, false, true }, // 11
        {"BCN North Fairway",       41.4050, 2.3450, false, true }, // 12
        {"Maresme Fairway",         41.5450, 2.6650, false, true }, // 13
        {"Costa Brava South",       41.7200, 3.0400, false, true }, // 14
        {"Costa Brava North",       41.9800, 3.2600, false, true }, // 15
        {"Garraf Fairway",          41.1650, 1.8800, false, true }, // 16
        {"Tarragona Sea Buoy",      41.0200, 1.3200, false, true }, // 17
        {"Ebro Offshore Fairway",   40.6800, 0.9600, false, true }, // 18
        {"Offshore Mallorca Lane",  40.9600, 2.3800, true,  true }, // 19 (Balearic corridor portal)
        {"Offshore Marseille Lane", 41.8200, 3.3100, true,  true }, // 20 (NE Mediterranean portal)
        {"Offshore Valencia Lane",  40.5600, 1.1800, true,  true }, // 21 (SW Mediterranean portal)
    };
    static const std::pair<int, int> kEdges[] = {
        {0, 11}, {1, 11}, {1, 12}, {2, 12}, {3, 12}, {3, 13}, {4, 13}, {4, 14},
        {5, 14}, {5, 15}, {6, 15}, {7, 16}, {8, 17}, {9, 17}, {10, 18},
        {11, 12}, {12, 13}, {13, 14}, {14, 15}, {11, 16}, {16, 17}, {17, 18},
        {11, 19}, {17, 19}, {14, 20}, {15, 20}, {19, 20}, {17, 21}, {18, 21}, {19, 21},
        {0, 1}, {1, 2}, {2, 3}, {7, 11}, {8, 9}
    };

    const int N = static_cast<int>(sizeof(kNodes) / sizeof(kNodes[0]));
    sea_nodes_.assign(kNodes, kNodes + N);
    sea_adj_.assign(N, {});

    auto node_in_water = [&](int idx) -> bool {
        const auto &nd = sea_nodes_[idx];
        if (nd.lat < pack_->lat_min() - 0.05 || nd.lat > pack_->lat_max() + 0.05 ||
            nd.lon < pack_->lon_min() - 0.05 || nd.lon > pack_->lon_max() + 0.05) return false;
        // Verify node is actually sea water (<= 1.5 m MSL) so ships never sail onto land.
        return pack_->sample_elevation(nd.lat, nd.lon) <= 1.5;
    };

    std::vector<bool> ok(N, false);
    for (int i = 0; i < N; ++i) {
        ok[i] = node_in_water(i);
        if (ok[i] && sea_nodes_[i].is_port) {
            active_ports_.push_back(i);
            if (sea_nodes_[i].deep_water) deep_ports_.push_back(i);
        }
    }
    if (active_ports_.size() < 2) return;

    for (auto [u, v] : kEdges) {
        if (!ok[u] || !ok[v]) continue;
        // Verify edge midpoint is also open water (never cut across capes/headlands!).
        double mlat = 0.5 * (sea_nodes_[u].lat + sea_nodes_[v].lat);
        double mlon = 0.5 * (sea_nodes_[u].lon + sea_nodes_[v].lon);
        if (pack_->sample_elevation(mlat, mlon) > 2.0) continue;
        float dist = static_cast<float>(nspain::haversine_m(sea_nodes_[u].lat, sea_nodes_[u].lon, sea_nodes_[v].lat, sea_nodes_[v].lon));
        sea_adj_[u].push_back({v, dist});
        sea_adj_[v].push_back({u, dist});
    }

    // Spawn 30 individual vessels (8 container/cargo ships, 6 ferries, 16 coastal yachts/fishing boats).
    const int n_ships = 30;
    ship_agents_.reserve(n_ships);
    for (int i = 0; i < n_ships; ++i) {
        ShipAgent s{};
        s.rng = static_cast<uint32_t>(i + 101) * 2246822519u ^ 0x3C6EF372u;
        if (i < 8 && deep_ports_.size() >= 2) {
            s.kind = MeshKind::Cargo;
            s.cruise_spd_mps = 7.2f + rand01(s.rng) * 2.1f; // ~14-18 kt
            s.scale = 0.85f + rand01(s.rng) * 0.35f;
            s.lane_offset_m = 220.0f;
            s.livery = static_cast<float>(2 + (i % 5));
        } else if (i < 14 && deep_ports_.size() >= 2) {
            s.kind = MeshKind::Ferry;
            s.cruise_spd_mps = 10.2f + rand01(s.rng) * 1.8f; // ~20-23 kt
            s.scale = 0.90f + rand01(s.rng) * 0.22f;
            s.lane_offset_m = 180.0f;
            s.livery = (i % 2 == 0) ? 4.0f : 7.0f;
        } else {
            s.kind = MeshKind::Boat;
            s.cruise_spd_mps = 4.2f + rand01(s.rng) * 2.8f; // ~8-14 kt
            s.scale = 0.85f + rand01(s.rng) * 0.45f;
            s.lane_offset_m = 90.0f;
            s.livery = static_cast<float>(i % 8);
        }
        s.beacon_phase = rand01(s.rng);
        if (plan_ship_route(s, -1, true)) {
            ship_agents_.push_back(std::move(s));
        }
    }
}

bool LivingTraffic::plan_ship_route(ShipAgent &s, int forced_orig, bool stagger_progress) {
    const auto &ports = (s.kind == MeshKind::Boat || deep_ports_.size() < 2) ? active_ports_ : deep_ports_;
    if (ports.size() < 2) return false;

    const int N = static_cast<int>(sea_nodes_.size());
    int orig = (forced_orig >= 0) ? forced_orig : ports[next_rng(s.rng) % ports.size()];
    int dest = orig;
    for (int tries = 0; tries < 12; ++tries) {
        int cand = ports[next_rng(s.rng) % ports.size()];
        if (cand != orig) { dest = cand; break; }
    }
    if (dest == orig) return false;

    // Dijkstra shortest path over the real maritime shipping-lane graph.
    std::vector<float> dist(N, 1e30f);
    std::vector<int> prev(N, -1);
    using QN = std::pair<float, int>;
    std::priority_queue<QN, std::vector<QN>, std::greater<QN>> pq;
    dist[orig] = 0.0f;
    pq.push({0.0f, orig});
    while (!pq.empty()) {
        auto [d, u] = pq.top();
        pq.pop();
        if (d > dist[u]) continue;
        if (u == dest) break;
        for (auto [v, w] : sea_adj_[u]) {
            // Slight random tie-break so vessels use varied lanes cleanly.
            if (d + w < dist[v]) {
                dist[v] = d + w;
                prev[v] = u;
                pq.push({dist[v], v});
            }
        }
    }
    if (prev[dest] < 0) return false;

    s.route.clear();
    for (int cur = dest; cur >= 0; cur = prev[cur]) s.route.push_back(cur);
    std::reverse(s.route.begin(), s.route.end());
    if (s.route.size() < 2) return false;

    s.step = 0;
    s.dwell_sec = 0.0f;
    if (stagger_progress) {
        s.step = static_cast<int>(next_rng(s.rng) % (s.route.size() - 1));
        float frac = rand01(s.rng);
        const auto &a = sea_nodes_[s.route[s.step]];
        const auto &b = sea_nodes_[s.route[s.step + 1]];
        s.lat = a.lat + (b.lat - a.lat) * frac;
        s.lon = a.lon + (b.lon - a.lon) * frac;
        s.heading_rad = bearing_rad(a.lat, a.lon, b.lat, b.lon);
        s.speed_mps = s.cruise_spd_mps;
    } else {
        const auto &a = sea_nodes_[s.route[0]];
        const auto &b = sea_nodes_[s.route[1]];
        s.lat = a.lat; s.lon = a.lon;
        s.heading_rad = bearing_rad(a.lat, a.lon, b.lat, b.lon);
        s.speed_mps = 1.0f;
    }
    s.water_elev_m = 0.0;
    return true;
}

void LivingTraffic::step_ship_agent(ShipAgent &s, float dt) {
    if (s.route.size() < 2) return;
    if (s.dwell_sec > 0.0f) {
        s.dwell_sec -= dt;
        s.speed_mps = std::max(0.0f, s.speed_mps - 1.5f * dt);
        if (s.dwell_sec <= 0.0f) {
            int cur_port = s.route.back();
            plan_ship_route(s, cur_port, false);
        }
        return;
    }

    int next_idx = std::min(s.step + 1, static_cast<int>(s.route.size()) - 1);
    const auto &tgt = sea_nodes_[s.route[next_idx]];
    double dist_m = nspain::haversine_m(s.lat, s.lon, tgt.lat, tgt.lon);
    const bool final_leg = (next_idx == static_cast<int>(s.route.size()) - 1);

    float want_spd = s.cruise_spd_mps;
    if (final_leg && dist_m < 900.0) want_spd = std::max(1.8f, s.cruise_spd_mps * static_cast<float>(dist_m / 900.0));
    s.speed_mps += std::clamp(want_spd - s.speed_mps, -0.8f * dt, 0.6f * dt);

    float want_hdg = bearing_rad(s.lat, s.lon, tgt.lat, tgt.lon);
    float err = wrap_angle(want_hdg - s.heading_rad);
    float max_turn = (s.kind == MeshKind::Boat) ? 0.22f : 0.07f;
    s.heading_rad = wrap_angle(s.heading_rad + std::clamp(err, -max_turn * dt, max_turn * dt));

    double mlat, mlon;
    nspain::meters_per_degree(s.lat, mlat, mlon);
    s.lat += (std::cos(s.heading_rad) * s.speed_mps * dt) / mlat;
    s.lon += (std::sin(s.heading_rad) * s.speed_mps * dt) / mlon;

    if (dist_m < (s.kind == MeshKind::Boat ? 140.0 : 280.0)) {
        if (final_leg) {
            // Reached destination port -> dock briefly, then dispatch to a new port!
            s.dwell_sec = 20.0f + rand01(s.rng) * 35.0f;
        } else {
            s.step++;
        }
    }
}

void LivingTraffic::refresh_nearby_parked_ga(double cam_lat, double cam_lon) {
    if (nspain::haversine_m(cam_lat, cam_lon, ramp_center_lat_, ramp_center_lon_) < 6000.0) return;
    ramp_center_lat_ = cam_lat;
    ramp_center_lon_ = cam_lon;
    ramp_statics_.clear();
    for (const auto &ap : pack_->airports()) {
        if (nspain::haversine_m(cam_lat, cam_lon, ap.lat, ap.lon) > 22000.0) continue;
        const auto stands = pack_->compute_parking_stands(ap, 5);
        // Reserve stands[0] strictly for the player; populate stands[1..4] with parked ramp GA aircraft!
        for (size_t i = 1; i < stands.size(); ++i) {
            uint32_t h = static_cast<uint32_t>(i * 2654435761u) ^ static_cast<uint32_t>(std::abs(ap.lat * 10000.0));
            ramp_statics_.push_back({
                stands[i].lat, stands[i].lon, stands[i].elev_m,
                static_cast<float>(stands[i].heading_deg * DEG2RAD_D),
                static_cast<float>(h % 8u)
            });
        }
    }
}

void LivingTraffic::update_and_render(
    const Mat4 &vp,
    double cam_east_m, double cam_north_m, double cam_alt_m,
    const AtmosphereState &atmo, float time_sec, int quality
) {
    if (!pack_ || !prog_ || !vao_) return;
    const float dt = (prev_t_ >= 0.0f) ? std::clamp(time_sec - prev_t_, 0.0f, 0.12f) : 0.016f;
    prev_t_ = time_sec;

    const double cam_lat = pack_->anchor_lat() + cam_north_m / pack_->anchor_m_lat();
    const double cam_lon = pack_->anchor_lon() + cam_east_m / pack_->anchor_m_lon();
    refresh_nearby_parked_ga(cam_lat, cam_lon);

    for (auto &b : gpu_batches_) b.clear();

    const double max_air_d = (quality == 0) ? 20000.0 : (quality >= 4 ? 56000.0 : 42000.0);
    const double max_sea_d = (quality == 0) ? 16000.0 : (quality >= 4 ? 48000.0 : 34000.0);

    int drawn_air = 0, drawn_sea = 0;
    for (auto &a : air_agents_) {
        step_air_agent(a, dt);
        double e = (a.lon - pack_->anchor_lon()) * pack_->anchor_m_lon();
        double n = (a.lat - pack_->anchor_lat()) * pack_->anchor_m_lat();
        if (std::hypot(e - cam_east_m, n - cam_north_m) > max_air_d) continue;
        EntityGpu g{};
        g.pos_yaw[0] = static_cast<float>(e);
        g.pos_yaw[1] = static_cast<float>(a.alt_m);
        g.pos_yaw[2] = static_cast<float>(n);
        g.pos_yaw[3] = a.heading_rad;
        g.pr_scale[0] = a.pitch_rad;
        g.pr_scale[1] = a.roll_rad;
        g.pr_scale[2] = 1.0f;
        g.pr_scale[3] = a.livery;
        g.lights[0] = a.beacon_phase;
        g.lights[1] = (a.phase == AirPhase::Parked) ? 0.0f : 1.0f;
        g.lights[2] = (a.phase == AirPhase::Takeoff || a.phase == AirPhase::Approach || a.phase == AirPhase::Rollout) ? 1.0f : 0.0f;
        g.lights[3] = 0.0f;
        gpu_batches_[static_cast<int>(a.is_airliner ? MeshKind::Airliner : MeshKind::GA)].push_back(g);
        ++drawn_air;
    }

    for (const auto &ps : ramp_statics_) {
        double e = (ps.lon - pack_->anchor_lon()) * pack_->anchor_m_lon();
        double n = (ps.lat - pack_->anchor_lat()) * pack_->anchor_m_lat();
        if (std::hypot(e - cam_east_m, n - cam_north_m) > 16000.0) continue;
        EntityGpu g{};
        g.pos_yaw[0] = static_cast<float>(e);
        g.pos_yaw[1] = static_cast<float>(ps.elev_m);
        g.pos_yaw[2] = static_cast<float>(n);
        g.pos_yaw[3] = ps.heading_rad;
        g.pr_scale[0] = 0.0f; g.pr_scale[1] = 0.0f; g.pr_scale[2] = 1.0f; g.pr_scale[3] = ps.livery;
        g.lights[0] = 0.0f; g.lights[1] = 0.0f; g.lights[2] = 0.0f; g.lights[3] = 0.0f;
        gpu_batches_[static_cast<int>(MeshKind::GA)].push_back(g);
        ++drawn_air;
    }

    for (auto &s : ship_agents_) {
        step_ship_agent(s, dt);
        double e = (s.lon - pack_->anchor_lon()) * pack_->anchor_m_lon();
        double n = (s.lat - pack_->anchor_lat()) * pack_->anchor_m_lat();
        // Apply right-hand maritime traffic separation perpendicular to heading.
        double oe = std::cos(s.heading_rad) * s.lane_offset_m;
        double on = -std::sin(s.heading_rad) * s.lane_offset_m;
        e += oe; n += on;
        if (std::hypot(e - cam_east_m, n - cam_north_m) > max_sea_d) continue;
        EntityGpu g{};
        g.pos_yaw[0] = static_cast<float>(e);
        g.pos_yaw[1] = static_cast<float>(s.water_elev_m);
        g.pos_yaw[2] = static_cast<float>(n);
        g.pos_yaw[3] = s.heading_rad;
        g.pr_scale[0] = 0.0f;
        g.pr_scale[1] = 0.015f * std::sin(time_sec * 0.9f + s.beacon_phase * 6.28f); // Subtle ocean swell roll
        g.pr_scale[2] = s.scale;
        g.pr_scale[3] = s.livery;
        g.lights[0] = s.beacon_phase;
        g.lights[1] = 0.0f; // Steady maritime nav/masthead lights (no aircraft strobe)
        g.lights[2] = 1.0f; // Illuminated bridge/deck lights at dusk/night
        g.lights[3] = std::clamp(s.speed_mps / std::max(1.0f, s.cruise_spd_mps), 0.0f, 1.0f);
        gpu_batches_[static_cast<int>(s.kind)].push_back(g);
        ++drawn_sea;
    }

    active_aircraft_ = drawn_air;
    active_ships_ = drawn_sea;

    gpu_flat_.clear();
    int offsets[static_cast<int>(MeshKind::Count)]{};
    int counts[static_cast<int>(MeshKind::Count)]{};
    for (int k = 0; k < static_cast<int>(MeshKind::Count); ++k) {
        offsets[k] = static_cast<int>(gpu_flat_.size());
        int room = MAX_ENTITIES - static_cast<int>(gpu_flat_.size());
        int take = std::min(room, static_cast<int>(gpu_batches_[k].size()));
        counts[k] = take;
        if (take > 0) gpu_flat_.insert(gpu_flat_.end(), gpu_batches_[k].begin(), gpu_batches_[k].begin() + take);
    }
    if (gpu_flat_.empty()) return;

    glNamedBufferSubData(ssbo_, 0, static_cast<GLsizeiptr>(gpu_flat_.size() * sizeof(EntityGpu)), gpu_flat_.data());
    glUseProgram(prog_);
    glUniformMatrix4fv(u_vp_, 1, GL_FALSE, vp.m);
    glUniform3f(u_cam_, static_cast<float>(cam_east_m), static_cast<float>(cam_alt_m), static_cast<float>(cam_north_m));
    glUniform3f(u_sun_, atmo.sun_dir.x, atmo.sun_dir.y, atmo.sun_dir.z);
    glUniform3f(u_cool_, atmo.haze_cool.x, atmo.haze_cool.y, atmo.haze_cool.z);
    glUniform3f(u_warm_, atmo.haze_warm.x, atmo.haze_warm.y, atmo.haze_warm.z);
    glUniform1f(u_hd_, atmo.haze_density);
    glUniform1f(u_time_, time_sec);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, ssbo_);
    glBindVertexArray(vao_);
    for (int k = 0; k < static_cast<int>(MeshKind::Count); ++k) {
        if (counts[k] <= 0 || ranges_[k].index_count == 0) continue;
        glUniform1i(u_base_, offsets[k]);
        glDrawElementsInstanced(
            GL_TRIANGLES,
            static_cast<GLsizei>(ranges_[k].index_count),
            GL_UNSIGNED_INT,
            reinterpret_cast<const void *>(static_cast<uintptr_t>(ranges_[k].first_index) * sizeof(uint32_t)),
            static_cast<GLsizei>(counts[k])
        );
    }
    glBindVertexArray(0);
}

LivingTraffic::NearestAircraftSample LivingTraffic::nearest_aircraft(
    double listener_lat_deg, double listener_lon_deg, double listener_alt_m
) const {
    NearestAircraftSample best{};
    if (!pack_) return best;
    const double m_lon = pack_->anchor_m_lon();
    const double m_lat = pack_->anchor_m_lat();
    float best_d2 = 3000.0f * 3000.0f;
    for (const auto &a : air_agents_) {
        if (a.phase == AirPhase::Parked || a.speed_mps < 2.0f) continue;
        const float de = static_cast<float>((a.lon - listener_lon_deg) * m_lon);
        const float dn = static_cast<float>((a.lat - listener_lat_deg) * m_lat);
        const float du = static_cast<float>(a.alt_m - listener_alt_m);
        const float d2 = de * de + dn * dn + du * du;
        if (d2 >= best_d2) continue;
        best_d2 = d2;
        const float ch = std::cos(a.heading_rad);
        const float sh = std::sin(a.heading_rad);
        const float cp = std::cos(a.pitch_rad);
        const float sp = std::sin(a.pitch_rad);
        best.valid = true;
        best.dist_m = std::sqrt(d2);
        best.rel_east_m = de;
        best.rel_up_m = du;
        best.rel_north_m = dn;
        best.vel_east_mps = a.speed_mps * sh * cp;
        best.vel_north_mps = a.speed_mps * ch * cp;
        best.vel_up_mps = a.speed_mps * sp;
        best.is_airliner = a.is_airliner;
        const float spd01 = std::clamp(a.speed_mps / (a.is_airliner ? 120.0f : 55.0f), 0.15f, 1.0f);
        best.rpm_hint = a.is_airliner ? (1400.0f + 1300.0f * spd01) : (1200.0f + 1350.0f * spd01);
    }
    return best;
}

bool LivingTraffic::run_selftest(const PackReader &pack) {
    // 1. Verify parking stands across every loaded airport.
    int paved_airports = 0;
    for (const auto &ap : pack.airports()) {
        auto stands = pack.compute_parking_stands(ap, 4);
        if (stands.empty()) {
            std::fprintf(stderr, "[SELFTEST FAIL] Airport %s returned 0 parking stands\n", ap.id.c_str());
            return false;
        }
        if (!std::isfinite(stands[0].lat) || !std::isfinite(stands[0].lon) || !std::isfinite(stands[0].elev_m)) {
            std::fprintf(stderr, "[SELFTEST FAIL] Airport %s non-finite stand\n", ap.id.c_str());
            return false;
        }
        if (stands[0].paved_apron) ++paved_airports;
    }

    // 2. Verify headless aerial + maritime individual route generation & stepping.
    LivingTraffic lt;
    lt.pack_ = &pack;
    lt.init_air_fleet();
    lt.init_sea_graph_and_fleet();
    if (lt.air_agents_.size() < 12) {
        std::fprintf(stderr, "[SELFTEST FAIL] Too few aerial flights: %zu\n", lt.air_agents_.size());
        return false;
    }
    for (auto &a : lt.air_agents_) {
        if (a.orig_ap == a.dest_ap || !std::isfinite(a.lat) || !std::isfinite(a.alt_m)) {
            std::fprintf(stderr, "[SELFTEST FAIL] Invalid aerial flight plan orig=%d dest=%d\n", a.orig_ap, a.dest_ap);
            return false;
        }
        lt.step_air_agent(a, 0.5f);
    }
    for (auto &s : lt.ship_agents_) {
        if (s.route.size() < 2 || s.route.front() == s.route.back()) {
            std::fprintf(stderr, "[SELFTEST FAIL] Invalid maritime route size=%zu\n", s.route.size());
            return false;
        }
        lt.step_ship_agent(s, 1.0f);
        if (pack.sample_elevation(s.lat, s.lon) > 2.5) {
            std::fprintf(stderr, "[SELFTEST FAIL] Ship sailed onto land (elev=%.1fm)\n", pack.sample_elevation(s.lat, s.lon));
            return false;
        }
    }
    std::printf("[LIVING-TRAFFIC SELFTEST OK] %zu airports (%d paved aprons) | %zu air flights | %zu sea vessels (%zu ports)\n",
                pack.airports().size(), paved_airports, lt.air_agents_.size(), lt.ship_agents_.size(), lt.active_ports_.size());
    return true;
}

} // namespace nsim
