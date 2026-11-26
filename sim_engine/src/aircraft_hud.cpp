#include "aircraft_hud.hpp"
#include "../third_party/stb_image.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace nsim {

namespace {
double dewpoint_from_temperature_rh(double temp_c, double relative_humidity) {
    constexpr double a = 17.625, b = 243.04;
    const double rh = std::clamp(relative_humidity, 0.01, 1.0);
    const double gamma = std::log(rh) + a * temp_c / (b + temp_c);
    return b * gamma / (a - gamma);
}

} // namespace

AircraftAndHud::~AircraftAndHud() {
    shutdown();
}

std::string AircraftAndHud::visual_node_name(const GlbMeshData &mesh, const char *fallback) const {
    const bool cockpit = &mesh == &ckpt_mesh_;
    const std::string key = std::string("node.") + (cockpit ? "cockpit_" : "exterior_") + fallback;
    if (const auto it = aircraft_.nodes.find(key.substr(5)); it != aircraft_.nodes.end()) return it->second;
    return fallback;
}

bool AircraftAndHud::init(const PackReader *pack, const std::string &asset_root, const AircraftPackage &aircraft,
                          const std::string &shader_root) {
    pack_ = pack;
    aircraft_ = aircraft;
    ac_prog_  = compile_program_files(shader_root + "/aircraft.vert", shader_root + "/aircraft.frag");
    hud_prog_ = compile_program_files(shader_root + "/hud.vert", shader_root + "/hud.frag");
    precip_prog_ = compile_program_files(shader_root + "/precip.vert", shader_root + "/precip.frag");
    if (!ac_prog_ || !hud_prog_) return false;
    u_ac_mvp_   = glGetUniformLocation(ac_prog_, "u_mvp");
    u_ac_model_ = glGetUniformLocation(ac_prog_, "u_model");
    u_ac_node_  = glGetUniformLocation(ac_prog_, "u_ac_node");
    u_ac_sun_   = glGetUniformLocation(ac_prog_, "u_sun_dir");
    u_ac_cool_  = glGetUniformLocation(ac_prog_, "u_haze_cool");
    u_ac_warm_  = glGetUniformLocation(ac_prog_, "u_haze_warm");
    u_ac_tint_  = glGetUniformLocation(ac_prog_, "u_base_tint");
    u_ac_pbr_   = glGetUniformLocation(ac_prog_, "u_pbr_factors");
    u_ac_em_    = glGetUniformLocation(ac_prog_, "u_emissive_col");
    u_ac_int_   = glGetUniformLocation(ac_prog_, "u_is_interior");
    u_ac_alpha_ = glGetUniformLocation(ac_prog_, "u_alpha_mul");
    u_ac_livery_ = glGetUniformLocation(ac_prog_, "u_livery");
    u_ac_unlit_ = glGetUniformLocation(ac_prog_, "u_unlit");
    u_ac_cabin_glass_ = glGetUniformLocation(ac_prog_, "u_cabin_glass");
    u_ac_glass_weather_ = glGetUniformLocation(ac_prog_, "u_glass_weather");
    u_ac_glass_fx_ = glGetUniformLocation(ac_prog_, "u_glass_fx");
    u_ac_plane_wx_ = glGetUniformLocation(ac_prog_, "u_plane_wx");
    u_ac_dmg_zones_ = glGetUniformLocation(ac_prog_, "u_dmg_zones");
    u_ac_dmg_extra_ = glGetUniformLocation(ac_prog_, "u_dmg_extra");
    u_ac_baked_reflection_ = glGetUniformLocation(ac_prog_, "u_baked_reflection");
    u_ac_detail_ = glGetUniformLocation(ac_prog_, "u_aircraft_detail");
    u_ac_native_output_ = glGetUniformLocation(ac_prog_, "u_native_output");
    u_ac_output_exposure_ = glGetUniformLocation(ac_prog_, "u_output_exposure");
    u_ac_output_lut_ = glGetUniformLocation(ac_prog_, "u_output_lut");
    u_hud_scr_  = glGetUniformLocation(hud_prog_, "u_screen_size");
    if (precip_prog_) {
        u_pr_vp_    = glGetUniformLocation(precip_prog_, "u_vp");
        u_pr_cam_   = glGetUniformLocation(precip_prog_, "u_cam_world");
        u_pr_vel_   = glGetUniformLocation(precip_prog_, "u_rel_vel");
        u_pr_right_ = glGetUniformLocation(precip_prog_, "u_cam_right");
        u_pr_up_    = glGetUniformLocation(precip_prog_, "u_cam_up");
        u_pr_time_  = glGetUniformLocation(precip_prog_, "u_time");
        u_pr_snow_  = glGetUniformLocation(precip_prog_, "u_snow");
        u_pr_int_   = glGetUniformLocation(precip_prog_, "u_intensity");
        u_pr_col_   = glGetUniformLocation(precip_prog_, "u_color");
        glCreateVertexArrays(1, &precip_vao_);
    }

    uint8_t wpix[4] = {255, 255, 255, 255};
    glCreateTextures(GL_TEXTURE_2D, 1, &white_tex_);
    glTextureStorage2D(white_tex_, 1, GL_RGBA8, 1, 1);
    glTextureSubImage2D(white_tex_, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, wpix);
    glTextureParameteri(white_tex_, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTextureParameteri(white_tex_, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    // Two physically simulated weather LUT volumes hold four precipitation
    // regimes each. Runtime glass shading selects/interpolates a 3D slice; no
    // particle simulation or per-frame texture upload runs in the game.
    auto load_rain_lut = [&](const std::string &path) -> GLuint {
        int rw = 0, rh = 0, rc = 0;
        unsigned char *rpix = stbi_load(path.c_str(), &rw, &rh, &rc, 4);
        if (rpix && rw == 512 && rh == 512 * 4) {
            GLuint tex = 0;
            glCreateTextures(GL_TEXTURE_3D, 1, &tex);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTextureStorage3D(tex, 1, GL_RGBA8, rw, rh / 4, 4);
            glTextureSubImage3D(tex, 0, 0, 0, 0, rw, rh / 4, 4,
                                GL_RGBA, GL_UNSIGNED_BYTE, rpix);
            glTextureParameteri(tex, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTextureParameteri(tex, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTextureParameteri(tex, GL_TEXTURE_WRAP_S, GL_REPEAT);
            glTextureParameteri(tex, GL_TEXTURE_WRAP_T, GL_REPEAT);
            glTextureParameteri(tex, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
            stbi_image_free(rpix);
            return tex;
        } else {
            if (rpix) stbi_image_free(rpix);
            std::fprintf(stderr, "[AIRCRAFT] Warning: invalid rain LUT volume %s (%dx%d; expected 512x2048)\n",
                         path.c_str(), rw, rh);
            return 0;
        }
    };
    std::filesystem::path rain_front, rain_side;
    std::string asset_error;
    if (!resolve_aircraft_asset(aircraft_, aircraft_.rain_front, asset_root, rain_front, asset_error) ||
        !resolve_aircraft_asset(aircraft_, aircraft_.rain_side, asset_root, rain_side, asset_error)) {
        std::fprintf(stderr, "[AIRCRAFT] Invalid optional package texture: %s\n", asset_error.c_str());
        return false;
    }
    if (!rain_front.empty()) windshield_rain_front_tex_ = load_rain_lut(rain_front.string());
    if (!rain_side.empty()) windshield_rain_side_tex_ = load_rain_lut(rain_side.string());
    if (windshield_rain_front_tex_ && windshield_rain_side_tex_)
        std::printf("[AIRCRAFT] 512x512x4 physically simulated rain LUTs loaded (front + side glass).\n");

    if (!init_physics(pack, aircraft.flight_model_dir.string())) return false;
    opentrack_.start_listening(4242);

    std::filesystem::path exterior_path, cockpit_path;
    if (!resolve_aircraft_asset(aircraft_, aircraft_.exterior_model, asset_root, exterior_path, asset_error) ||
        !WorldObjects::load_colored_glb(exterior_path.string(), ext_mesh_)) {
        std::fprintf(stderr, "[AIRCRAFT] Unable to load aircraft exterior (%s): %s\n",
                     aircraft_.id.c_str(), asset_error.c_str());
        return false;
    }
    if (!aircraft_.cockpit_model.empty() &&
        (!resolve_aircraft_asset(aircraft_, aircraft_.cockpit_model, asset_root, cockpit_path, asset_error) ||
         !WorldObjects::load_colored_glb(cockpit_path.string(), ckpt_mesh_))) {
        std::fprintf(stderr, "[AIRCRAFT] Unable to load aircraft cockpit (%s): %s\n",
                     aircraft_.id.c_str(), asset_error.c_str());
        return false;
    }
    const std::string windshield_node = visual_node_name(ckpt_mesh_, "Windshield");
    if (auto wi = ckpt_mesh_.node_index.find(windshield_node); wi != ckpt_mesh_.node_index.end()) {
        size_t count = 0;
        float material_alpha = 1.0f;
        for (const auto &sm : ckpt_mesh_.submeshes) if (sm.node_idx == wi->second) {
            ++count;
            material_alpha = sm.base_color.w;
        }
        std::printf("[AIRCRAFT] Cockpit windshield node %d: %zu draw part(s), GLTF alpha %.3f, rain %s.\n",
                    wi->second, count, material_alpha,
                    windshield_rain_front_tex_ && windshield_rain_side_tex_ ? "ready" : "unavailable");
    } else {
        if (!aircraft_.cockpit_model.empty())
            std::fprintf(stderr, "[AIRCRAFT] Optional windshield node '%s' is missing; cockpit rain effects are disabled.\n",
                         windshield_node.c_str());
    }

    // Load crisp HiDPI TrueType monospace font atlas (1024x576, 16x6 cells, white glyph + dark outline drop-shadow!)
    {
        int fw = 0, fh = 0, fch = 0;
        std::string fpath = asset_root + "/pbr/hud_font.png";
        unsigned char *fpix = stbi_load(fpath.c_str(), &fw, &fh, &fch, 4);
        if (fpix && fw > 0 && fh > 0) {
            int mips = 1;
            for (int m = std::max(fw, fh); m > 1; m >>= 1) mips++;
            glCreateTextures(GL_TEXTURE_2D, 1, &hud_font_tex_);
            glTextureStorage2D(hud_font_tex_, mips, GL_RGBA8, fw, fh);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTextureSubImage2D(hud_font_tex_, 0, 0, 0, fw, fh, GL_RGBA, GL_UNSIGNED_BYTE, fpix);
            glGenerateTextureMipmap(hud_font_tex_);
            glTextureParameteri(hud_font_tex_, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            glTextureParameteri(hud_font_tex_, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTextureParameteri(hud_font_tex_, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTextureParameteri(hud_font_tex_, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            stbi_image_free(fpix);
        } else {
            if (fpix) stbi_image_free(fpix);
            std::fprintf(stderr, "[HUD] Warning: failed to load %s\n", fpath.c_str());
        }
    }

    glCreateVertexArrays(1, &hud_vao_);
    glCreateBuffers(1, &hud_vbo_);
    hud_vbo_cap_ = 32768;
    glNamedBufferData(hud_vbo_, static_cast<GLsizeiptr>(hud_vbo_cap_ * sizeof(HudVert)), nullptr, GL_STREAM_DRAW);
    glVertexArrayVertexBuffer(hud_vao_, 0, hud_vbo_, 0, sizeof(HudVert));
    glEnableVertexArrayAttrib(hud_vao_, 0); glVertexArrayAttribFormat(hud_vao_, 0, 2, GL_FLOAT, GL_FALSE, 0); glVertexArrayAttribBinding(hud_vao_, 0, 0);
    glEnableVertexArrayAttrib(hud_vao_, 1); glVertexArrayAttribFormat(hud_vao_, 1, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float)); glVertexArrayAttribBinding(hud_vao_, 1, 0);
    glEnableVertexArrayAttrib(hud_vao_, 2); glVertexArrayAttribFormat(hud_vao_, 2, 4, GL_FLOAT, GL_FALSE, 4 * sizeof(float)); glVertexArrayAttribBinding(hud_vao_, 2, 0);
    hud_verts_.reserve(8192);
    return true;
}

bool AircraftAndHud::init_physics(const PackReader *pack, const std::string &aircraft_dir) {
    if (!pack) return false;
    pack_ = pack;
    fdm_.set_ground_provider([this](double lat, double lon) {
        nspain::FlightGround ground{};
        double runway_elev = 0.0, friction = ground.friction, roughness = ground.roughness;
        std::string runway_id;
        if (pack_ && pack_->query_runway(lat, lon, runway_elev, friction, roughness, runway_id)) {
            ground.elev_m = runway_elev + 0.35;
            ground.friction = friction;
            ground.roughness = roughness;
            ground.solid = true;
        } else if (pack_) {
            ground.elev_m = pack_->sample_elevation(lat, lon);
            ground.friction = 0.58;
            ground.roughness = 0.36;
            ground.solid = pack_->sample_surface(lat, lon).z <= 0.35f;
        }
        // Weather-coupled surface grip: rain lowers braking friction (wet runway), freezing rain/snow turns slick!
        if (ground_wetness_ > 0.02) {
            const bool icy = (last_air_.oat_c < 0.5 && ground_wetness_ > 0.15);
            const double grip_mult = icy ? std::lerp(1.0, 0.38, ground_wetness_) : std::lerp(1.0, 0.62, ground_wetness_);
            ground.friction = std::max(0.20, ground.friction * grip_mult);
        }
        return ground;
    });
    if (!fdm_.load(aircraft_dir)) {
        std::fprintf(stderr, "[JSBSIM] Aircraft package '%s' failed to load from %s\n",
                     aircraft_.id.c_str(), aircraft_dir.c_str());
        return false;
    }
    std::printf("[JSBSIM] Upstream JSBSim %s | aircraft package %s (%s)\n",
                nspain::FlightModel::version().c_str(), aircraft_.id.c_str(), aircraft_.name.c_str());
    nspain::PistonSystemsConfig systems_config{};
    systems_config.left_tank_capacity_gal = aircraft_.left_tank_capacity_gal;
    systems_config.right_tank_capacity_gal = aircraft_.right_tank_capacity_gal;
    systems_config.oil_capacity_quarts = aircraft_.oil_capacity_quarts;
    systems_config.max_flaps_deg = aircraft_.max_flaps_deg;
    systems_config.bus_voltage = aircraft_.bus_voltage;
    systems_.configure(systems_config);
    systems_.reset_to_running_default();
    reset_flight(st_.lat_deg, st_.lon_deg, st_.alt_msl_m, st_.heading_deg, st_.speed_mps * 1.94384, false);
    return true;
}

void AircraftAndHud::shutdown() {
    opentrack_.stop_listening();
    yoke_.shutdown();
    auto del_g = [](GlbMeshData &m) {
        if (m.vao) { glDeleteVertexArrays(1, &m.vao); m.vao = 0; }
        if (m.vbo) { glDeleteBuffers(1, &m.vbo); m.vbo = 0; }
        if (m.ebo) { glDeleteBuffers(1, &m.ebo); m.ebo = 0; }
        for (GLuint t : m.owned_textures) if (t) glDeleteTextures(1, &t);
        m.owned_textures.clear();
    };
    del_g(ext_mesh_);
    del_g(ckpt_mesh_);
    if (windshield_rain_front_tex_) { glDeleteTextures(1, &windshield_rain_front_tex_); windshield_rain_front_tex_ = 0; }
    if (windshield_rain_side_tex_) { glDeleteTextures(1, &windshield_rain_side_tex_); windshield_rain_side_tex_ = 0; }
    if (white_tex_)    { glDeleteTextures(1, &white_tex_); white_tex_ = 0; }
    if (hud_font_tex_) { glDeleteTextures(1, &hud_font_tex_); hud_font_tex_ = 0; }
    if (ac_prog_)  { glDeleteProgram(ac_prog_); ac_prog_ = 0; }
    if (precip_prog_) { glDeleteProgram(precip_prog_); precip_prog_ = 0; }
    if (precip_vao_)  { glDeleteVertexArrays(1, &precip_vao_); precip_vao_ = 0; }
    if (hud_prog_) { glDeleteProgram(hud_prog_); hud_prog_ = 0; }
    if (hud_vao_)  { glDeleteVertexArrays(1, &hud_vao_); hud_vao_ = 0; }
    if (hud_vbo_)  { glDeleteBuffers(1, &hud_vbo_); hud_vbo_ = 0; hud_vbo_cap_ = 0; }
}

void AircraftAndHud::reset_flight(double lat, double lon, double alt_m, double hdg_deg, double ias_kt, bool on_ground) {
    st_.runway_tracking = false;
    mouse_yoke_roll_ = mouse_yoke_pitch_ = 0.0;
    raw_yoke_x_ = raw_yoke_y_ = 0.0;
    yoke_anchor_valid_ = false;
    kb_bank_cmd_ = kb_pitch_cmd_ = 0.0;
    throttle_hw_active_ = mixture_hw_active_ = false;
    throttle_hw_last_ = mixture_hw_last_ = -1.0;
    st_.ap_master = false;
    st_.ap_hdg_deg = hdg_deg;
    st_.ap_alt_ft = std::round(alt_m * 3.28084 / 100.0) * 100.0;
    ai_gyro_spool_ = systems_.is_engine_running() ? 1.0f : 0.0f;
    prev_ground_coll_ = on_ground;
    prev_vsi_coll_ = 0.0;
    impact_cooldown_s_ = 0.0;
    impact_fx_s_ = 0.0f;
    collision_msg_.clear();
    collision_msg_s_ = 0.0;
    if (!owned_mode_) {
        damage_ = prop_nicks_ = gear_alignment_ = windshield_crack_ = 0.0;
        dmg_nose_ = dmg_wing_l_ = dmg_wing_r_ = dmg_tail_ = 0.0;
    }
    taxi_hold_hdg_deg_ = hdg_deg;
    taxi_hold_valid_ = true;
    st_.lat_deg = lat;
    st_.lon_deg = lon;
    st_.ground_m = pack_->sample_elevation(lat, lon);
    if (on_ground) {
        double mlat, mlon;
        nspain::meters_per_degree(lat, mlat, mlon);
        const double hr = hdg_deg * DEG2RAD_D;
        const double fe = std::sin(hr), fn = std::cos(hr);
        const double re = std::cos(hr), rn = -std::sin(hr);
        const double pts[4][2] = {{0.0, 0.0}, {fe * 2.0, fn * 2.0}, {-fe * 0.6 + re * 1.5, -fn * 0.6 + rn * 1.5},
                                  {-fe * 0.6 - re * 1.5, -fn * 0.6 - rn * 1.5}};
        for (const auto &p : pts) {
            const double plat = lat + p[1] / mlat;
            const double plon = lon + p[0] / mlon;
            st_.ground_m = std::max(st_.ground_m, pack_->sample_elevation(plat, plon));
            double runway_elev, friction, roughness;
            std::string runway_id;
            if (pack_->query_runway(plat, plon, runway_elev, friction, roughness, runway_id))
                st_.ground_m = std::max(st_.ground_m, runway_elev + 0.35);
        }
    }
    st_.alt_msl_m = std::max(st_.ground_m + (on_ground ? 1.42 : 35.0), alt_m);
    st_.heading_deg = std::fmod(hdg_deg + 360.0, 360.0);
    st_.pitch_deg = on_ground ? 0.0 : 3.0;
    st_.roll_deg = 0.0;
    st_.speed_mps = ias_kt * 0.514444;
    double hr = st_.heading_deg * DEG2RAD_D;
    st_.vel_east_mps = std::sin(hr) * st_.speed_mps;
    st_.vel_north_mps = std::cos(hr) * st_.speed_mps;
    st_.vsi_mps = 0.0;
    st_.look_yaw_deg = 0.0f;
    st_.look_pitch_deg = 0.0f;
    st_.throttle = on_ground ? 0.0 : 0.62;
    nspain::FlightEnvironment env{};
    env.terrain_elev_m = st_.ground_m;
    fdm_.set_environment(env);
    nspain::FlightControls controls{};
    controls.aileron = st_.aileron;
    controls.elevator = st_.elevator;
    controls.rudder = st_.rudder;
    controls.elevator_trim = st_.trim;
    controls.throttle = st_.throttle;
    controls.mixture = st_.mixture;
    controls.brake_left = controls.brake_right = st_.parking_brake ? 1.0 : 0.0;
    controls.flaps = systems_.get_flaps01();
    fdm_.set_controls(controls);
    nspain::FlightEngineInputs engine{};
    engine.left_fuel_gal = systems_.get_left_tank_gal();
    engine.right_fuel_gal = systems_.get_right_tank_gal();
    engine.magnetos = systems_.get_magnetos();
    engine.power_multiplier = systems_.get_engine_power_mult() *
                              (1.0 - damage_ * 0.55) * (1.0 - prop_nicks_ * 0.12);
    engine.starter = systems_.is_starter_cranking();
    engine.fuel_starved = systems_.is_fuel_starved();
    engine.running = systems_.is_engine_running();
    fdm_.set_engine(engine);
    if (!fdm_.initialize(st_.lat_deg, st_.lon_deg, st_.alt_msl_m, st_.heading_deg, ias_kt, on_ground)) {
        std::fprintf(stderr, "[JSBSIM] Could not initialize package '%s' at %.5f, %.5f / %.0f m\n",
                     aircraft_.id.c_str(), lat, lon, st_.alt_msl_m);
        return;
    }
    const auto &fs = fdm_.state();
    st_.lat_deg = fs.lat_deg; st_.lon_deg = fs.lon_deg; st_.alt_msl_m = fs.alt_msl_m;
    st_.ground_m = pack_->sample_elevation(fs.lat_deg, fs.lon_deg);
    st_.heading_deg = fs.heading_deg; st_.pitch_deg = fs.pitch_deg; st_.roll_deg = fs.roll_deg;
    st_.speed_mps = fs.ground_speed_mps; st_.vel_east_mps = fs.vel_east_mps;
    st_.vel_north_mps = fs.vel_north_mps; st_.vsi_mps = fs.vsi_mps;
    st_.trim = fdm_.controls().elevator_trim;
}

void AircraftAndHud::teleport_to_airport(const AirportInfo &ap, double approach_dist_m, double agl_m) {
    double dn = (ap.rwy_lat_b - ap.rwy_lat_a) * 111132.0;
    double de = (ap.rwy_lon_b - ap.rwy_lon_a) * 111320.0 * std::cos(ap.rwy_lat_a * DEG2RAD_D);
    double hdg = std::atan2(de, dn);
    double hdg_deg = std::fmod(hdg * RAD2DEG_D + 360.0, 360.0);
    double lat = ap.rwy_lat_a - std::cos(hdg) * approach_dist_m / 111132.0;
    double lon = ap.rwy_lon_a - std::sin(hdg) * approach_dist_m / (111320.0 * std::cos(ap.rwy_lat_a * DEG2RAD_D));
    double gnd = pack_->sample_elevation(lat, lon);
    reset_flight(lat, lon, std::max(gnd + 40.0, ap.rwy_elev_a + agl_m), hdg_deg, 100.0, false);
    st_.pitch_deg = -5.0;
}

FlightAssistOut compute_flight_and_taxi_assist(
    double roll_cmd, double pitch_cmd, double manual_rudder,
    bool manual_rudder_active, double taxi_hold_hdg_deg,
    const nspain::FlightState &fs
) {
    FlightAssistOut out{};
    const auto expo = [](double x, double linear, double dz) {
        const double ax = std::abs(x);
        if (ax <= dz) return 0.0;
        const double u = std::copysign((ax - dz) / (1.0 - dz), x);
        return u * (linear + (1.0 - linear) * u * u);
    };
    const double roll_u = expo(std::clamp(roll_cmd, -1.0, 1.0), 0.45, 0.04);
    const double pitch_u = expo(std::clamp(pitch_cmd, -1.0, 1.0), 0.68, 0.04);

    if (fs.on_ground) {
        const double steer = manual_rudder_active ? std::clamp(manual_rudder, -1.0, 1.0) : roll_u;
        const double spd = std::max(0.0, fs.ground_speed_mps);
        // Speed-sensitive yaw rate: agile tight apron turns at taxi speed, calm authority at takeoff speed.
        const double high_spd = std::clamp((spd - 8.0) / 18.0, 0.0, 1.0);
        const double max_yaw_rps = std::lerp(0.42, 0.11, high_spd);
        if (std::abs(steer) <= 0.05 && !manual_rudder_active && spd > 0.6) {
            // Active heading-hold counters C172S P-factor and crosswind weathervaning automatically.
            const double hdg_err = std::remainder(taxi_hold_hdg_deg - fs.heading_deg, 360.0);
            out.rudder = std::clamp(hdg_err * 0.075 - fs.r_rps * 1.15, -0.85, 0.85);
            out.holding_heading = true;
        } else {
            const double target_r = steer * max_yaw_rps;
            out.rudder = std::clamp((target_r - fs.r_rps) * 2.1 + steer * 0.48, -0.95, 0.95);
            // Gentle differential toe-brake assist for tight low-speed apron/taxiway turns.
            if (spd < 9.0 && std::abs(steer) > 0.35) {
                const double diff_brk = std::clamp((std::abs(steer) - 0.35) / 0.65, 0.0, 1.0) * 0.55;
                if (steer < 0.0) out.brake_left_add = diff_brk;
                else out.brake_right_add = diff_brk;
            }
        }
        // Keep wings level in crosswind on ground; pitch-damped rotation lifts off cleanly at 55 kt without zoom-stalling.
        out.aileron = std::clamp(-fs.roll_deg * 0.05 + roll_u * 0.18, -0.40, 0.40);
        if (pitch_u > 0.0) {
            const double rot_pitch = std::min(11.0, 3.5 + pitch_u * 13.0);
            const double rot_ff = std::clamp((rot_pitch - 3.0) * 0.045, 0.0, 0.36);
            out.elevator = std::clamp(rot_ff + (rot_pitch - fs.pitch_deg) * 0.095 - fs.q_rps * 0.90, -0.15, 0.70);
        } else {
            out.elevator = std::clamp(pitch_u * 0.65, -0.65, 0.0);
        }
        return out;
    }

    out.target_bank_deg = roll_u * 30.0;
    const double turn_pitch_comp = std::min(2.4, std::abs(fs.roll_deg) * 0.065);
    const double vsi_hold_pitch = std::clamp(1.6 + turn_pitch_comp - fs.vsi_mps * 1.45, -6.0, 9.5);
    const double direct_pitch = 1.6 + turn_pitch_comp + pitch_u * (pitch_u >= 0.0 ? 11.8 : 11.5);
    const double blend_t = std::clamp(std::abs(pitch_u) / 0.035, 0.0, 1.0);
    const double smooth_w = blend_t * blend_t * (3.0 - 2.0 * blend_t);
    out.target_pitch_deg = std::lerp(vsi_hold_pitch, direct_pitch, smooth_w);
    const double stall_onset_kt = (fs.cl > 1.25) ? 45.0 : 50.0;
    const double stall_guard = std::max(
        std::clamp((stall_onset_kt - fs.ias_kt) / 10.0, 0.0, 1.0),
        std::clamp((fs.alpha_deg - 13.5) / 3.5, 0.0, 1.0));
    out.target_pitch_deg = std::min(out.target_pitch_deg, 13.5 - stall_guard * 12.0);
    if (fs.ias_kt > 136.0) {
        const double overspd = std::clamp((fs.ias_kt - 136.0) / 18.0, 0.0, 1.0);
        out.target_pitch_deg = std::max(out.target_pitch_deg, overspd * 5.5);
    }

    const double q_scale = std::clamp(92.0 / std::max(46.0, fs.ias_kt), 0.65, 1.45);
    // Static-stability elevator feedforward prevents 3-5 deg nose-down droop right after liftoff or in flare.
    const double pitch_ff = std::clamp((out.target_pitch_deg - 1.5) * 0.022, -0.20, 0.28);
    out.aileron = std::clamp(((out.target_bank_deg - fs.roll_deg) * 0.044 - fs.p_rps * 0.72) * q_scale, -0.85, 0.85);
    out.elevator = std::clamp(pitch_ff + ((out.target_pitch_deg - fs.pitch_deg) * 0.088 - fs.q_rps * 0.92) * q_scale, -0.85, 0.88);
    out.rudder = manual_rudder_active
        ? std::clamp(manual_rudder, -1.0, 1.0)
        : std::clamp(out.aileron * 0.38 - fs.beta_deg * 0.050 - fs.r_rps * 0.36, -0.55, 0.55);
    return out;
}

void AircraftAndHud::apply_autopilot(const nspain::FlightState &fs_cur, double &eff_roll, double &eff_pitch) {
    const bool ap_bus_ok = systems_.get_master_battery() && systems_.get_avionics_master() &&
                           !fs_cur.on_ground && !st_.slew_fast;
    if (!ap_bus_ok || std::abs(eff_roll) > 0.42 || std::abs(eff_pitch) > 0.42) {
        st_.ap_master = false;
        return;
    }
    if (!st_.ap_master) return;
    if (st_.ap_nav_hold && st_.ap_nav_valid) {
        const double err = std::remainder(st_.ap_nav_brg_deg - fs_cur.heading_deg, 360.0);
        eff_roll = std::clamp(err / 32.0, -0.72, 0.72);
    } else if (st_.ap_hdg_hold) {
        const double err = std::remainder(st_.ap_hdg_deg - fs_cur.heading_deg, 360.0);
        eff_roll = std::clamp(err / 32.0, -0.72, 0.72);
    } else {
        eff_roll = 0.0;
    }
    if (st_.ap_alt_hold) {
        const double cur_ft = fs_cur.alt_msl_m * 3.28084;
        const double tgt_vsi = std::clamp((st_.ap_alt_ft - cur_ft) * 0.011, -2.6, 2.6);
        const double vsi_err = tgt_vsi - fs_cur.vsi_mps;
        eff_pitch = std::clamp(vsi_err * 0.22, -0.58, 0.58);
    } else {
        eff_pitch = 0.0;
    }
}

void AircraftAndHud::step_autonomous_assist(double dt, double roll_cmd, double pitch_cmd, double throttle_tgt, double brake_cmd,
                                            const AtmosphereState &atmo, const RegionalAtmosphere &reg_atmo, double sim_time_sec) {
    const nspain::FlightState &fs = fdm_.state();
    if (!taxi_hold_valid_) { taxi_hold_hdg_deg_ = fs.heading_deg; taxi_hold_valid_ = true; }
    double eff_roll = std::clamp(roll_cmd, -1.0, 1.0);
    double eff_pitch = std::clamp(pitch_cmd, -1.0, 1.0);
    apply_autopilot(fs, eff_roll, eff_pitch);
    const FlightAssistOut assist = compute_flight_and_taxi_assist(
        eff_roll, eff_pitch, 0.0, false, taxi_hold_hdg_deg_, fs);
    if (fs.on_ground && !assist.holding_heading) taxi_hold_hdg_deg_ = fs.heading_deg;
    st_.aileron  += (assist.aileron  - st_.aileron)  * std::min(1.0, dt * 11.0);
    st_.elevator += (assist.elevator - st_.elevator) * std::min(1.0, dt * 11.0);
    st_.rudder   += (assist.rudder   - st_.rudder)   * std::min(1.0, dt * 12.0);
    st_.throttle += (std::clamp(throttle_tgt, 0.0, 1.0) - st_.throttle) * std::min(1.0, dt * 3.5);
    if (!fs.on_ground && (std::abs(pitch_cmd) < 0.14 || st_.ap_master))
        st_.trim = std::clamp(st_.trim + assist.elevator * dt * 0.14, -0.55, 0.55);

    advance_aircraft(dt, atmo, reg_atmo, sim_time_sec, st_.rudder,
                     std::clamp(brake_cmd + assist.brake_left_add, 0.0, 1.0),
                     std::clamp(brake_cmd + assist.brake_right_add, 0.0, 1.0));
}

void AircraftAndHud::poll_yoke() {
    nspain::YokeConfig ycfg{};
    ycfg.deadzone = static_cast<double>(graphics_settings_.yoke_deadzone_pct) * 0.01;
    ycfg.curve = static_cast<double>(graphics_settings_.yoke_curve_pct) * 0.01;
    ycfg.invert_roll = graphics_settings_.yoke_invert_roll;
    ycfg.invert_pitch = graphics_settings_.yoke_invert_pitch;
    ycfg.invert_throttle = graphics_settings_.yoke_invert_throttle;
    ycfg.direct_control = graphics_settings_.yoke_direct;
    yoke_.poll(ycfg);
}

void AircraftAndHud::step_inputs_and_physics(const PlatformWindow &win, double dt, const AtmosphereState &atmo, const RegionalAtmosphere &reg_atmo, double sim_time_sec) {
    opentrack_.poll(dt);
    poll_yoke();
    const auto &yk = yoke_.state();
    const bool pause = win.key_pressed(KEY_ESC) || win.key_pressed(KEY_SPACE);
    if (!st_.menu_open && pause) {
        st_.menu_open = st_.paused = true;
        return;
    }
    if (st_.menu_open) return;

    if (win.key_pressed(KEY_Y)) {
        st_.mouse_yoke_enabled = !st_.mouse_yoke_enabled;
        mouse_yoke_roll_ = mouse_yoke_pitch_ = 0.0;
        raw_yoke_x_ = raw_yoke_y_ = 0.0;
        kb_bank_cmd_ = kb_pitch_cmd_ = 0.0;
        anchor_mx_ = static_cast<double>(win.mouse_x());
        anchor_my_ = static_cast<double>(win.mouse_y());
        yoke_anchor_valid_ = true;
    }
    const bool mouse_yoke = st_.mouse_yoke_enabled && st_.view_mode != 2;
    if (win.mouse_right_down() || (win.mouse_left_down() && !mouse_yoke)) {
        st_.look_yaw_deg   = std::clamp(st_.look_yaw_deg   - win.mouse_dx() * 0.22f, -165.0f, 165.0f);
        st_.look_pitch_deg = std::clamp(st_.look_pitch_deg - win.mouse_dy() * 0.18f, -75.0f,  75.0f);
    }
    if (win.key_pressed(KEY_R) || yk.recenter_pressed) {
        st_.look_yaw_deg = 0.0f;
        st_.look_pitch_deg = 0.0f;
        st_.free_cam_off_m = {0.0f, 3.0f, 12.0f};
        mouse_yoke_roll_ = mouse_yoke_pitch_ = 0.0;
        raw_yoke_x_ = raw_yoke_y_ = 0.0;
        kb_bank_cmd_ = kb_pitch_cmd_ = 0.0;
        anchor_mx_ = static_cast<double>(win.mouse_x());
        anchor_my_ = static_cast<double>(win.mouse_y());
        yoke_anchor_valid_ = true;
        taxi_hold_hdg_deg_ = fdm_.state().heading_deg;
        taxi_hold_valid_ = true;
    }
    if (std::abs(yk.hat_x) > 0.2 || std::abs(yk.hat_y) > 0.2) {
        st_.look_yaw_deg = std::clamp(st_.look_yaw_deg - static_cast<float>(yk.hat_x * dt * 110.0), -165.0f, 165.0f);
        st_.look_pitch_deg = std::clamp(st_.look_pitch_deg - static_cast<float>(yk.hat_y * dt * 85.0), -75.0f, 75.0f);
    }
    if (win.key_pressed(KEY_C)) st_.view_mode = (st_.view_mode + 1) % 3;
    if (st_.view_mode == 0) {
        st_.seat_height_m = std::clamp(st_.seat_height_m + float(dt) * 0.18f *
            (int(win.key_down(KEY_PAGEUP)) - int(win.key_down(KEY_PAGEDOWN))), -0.22f, 0.22f);
        st_.seat_fore_m = std::clamp(st_.seat_fore_m + float(dt) * 0.18f *
            (int(win.key_down(KEY_HOME)) - int(win.key_down(KEY_END))), -0.25f, 0.25f);
        st_.seat_lat_m = std::clamp(st_.seat_lat_m + float(dt) * 0.18f *
            (int(win.key_down(KEY_RIGHT)) - int(win.key_down(KEY_LEFT))), -0.24f, 0.24f);
        if (win.key_pressed(KEY_BACKSPACE)) st_.seat_height_m = st_.seat_fore_m = st_.seat_lat_m = 0.0f;
    } else if (st_.view_mode == 1) {
        st_.chase_dist_m = std::clamp(st_.chase_dist_m + float(dt) * 9.0f *
            (int(win.key_down(KEY_PAGEDOWN)) - int(win.key_down(KEY_PAGEUP))), 5.5f, 26.0f);
        st_.look_yaw_deg = std::clamp(st_.look_yaw_deg + float(dt) * 75.0f *
            (int(win.key_down(KEY_LEFT)) - int(win.key_down(KEY_RIGHT))), -165.0f, 165.0f);
        if (win.key_pressed(KEY_BACKSPACE)) { st_.chase_dist_m = 10.6f; st_.look_yaw_deg = st_.look_pitch_deg = 0.0f; }
    } else if (st_.view_mode == 2) {
        const float cam_spd = win.key_down(KEY_SHIFT) ? 28.0f : 11.0f;
        const float yaw_r = st_.look_yaw_deg * DEG2RAD_F;
        const float fwd_in = float(int(win.key_down(KEY_HOME)) - int(win.key_down(KEY_END)));
        const float lat_in = float(int(win.key_down(KEY_RIGHT)) - int(win.key_down(KEY_LEFT)));
        const float vrt_in = float(int(win.key_down(KEY_PAGEUP)) - int(win.key_down(KEY_PAGEDOWN)));
        st_.free_cam_off_m.x = std::clamp(st_.free_cam_off_m.x + (-std::sin(yaw_r) * fwd_in + std::cos(yaw_r) * lat_in) * cam_spd * float(dt), -120.0f, 120.0f);
        st_.free_cam_off_m.z = std::clamp(st_.free_cam_off_m.z + (-std::cos(yaw_r) * fwd_in - std::sin(yaw_r) * lat_in) * cam_spd * float(dt), -120.0f, 120.0f);
        st_.free_cam_off_m.y = std::clamp(st_.free_cam_off_m.y + vrt_in * cam_spd * float(dt), -15.0f, 80.0f);
        if (win.key_pressed(KEY_BACKSPACE)) { st_.free_cam_off_m = {0.0f, 3.0f, 12.0f}; st_.look_yaw_deg = st_.look_pitch_deg = 0.0f; }
    }
    if (win.key_pressed(KEY_K)) quick_start_or_shutdown();
    if (owned_mode_) {
        if (win.key_pressed(KEY_1)) {
            const bool next = !systems_.get_master_battery();
            systems_.set_master_battery(next);
            systems_.set_master_alternator(next);
            systems_.set_light_switch(0, next); // Beacon with master
        }
        if (win.key_pressed(KEY_2)) systems_.set_avionics_master(!systems_.get_avionics_master());
        if (win.key_pressed(KEY_3)) systems_.set_magnetos((systems_.get_magnetos() + 1) % 4);
        if (win.key_down(KEY_4)) {
            if (!systems_.is_engine_running()) {
                systems_.set_magnetos(4);
                starter_held_ = true;
            }
        } else if (starter_held_) {
            if (systems_.get_magnetos() == 4) systems_.set_magnetos(3);
            starter_held_ = false;
        }
        if (win.key_down(KEY_5)) { st_.mixture = std::clamp(st_.mixture - dt * 0.55, 0.0, 1.0); mixture_hw_active_ = false; }
        if (win.key_down(KEY_6)) { st_.mixture = std::clamp(st_.mixture + dt * 0.55, 0.0, 1.0); mixture_hw_active_ = false; }
        if (yk.has_mixture) {
            if (mixture_hw_last_ < 0.0) mixture_hw_last_ = yk.mixture;
            else if (std::abs(yk.mixture - mixture_hw_last_) > 0.015) mixture_hw_active_ = true;
            if (mixture_hw_active_) { st_.mixture = yk.mixture; mixture_hw_last_ = yk.mixture; }
        }
        if (win.key_pressed(KEY_7)) systems_.set_carb_heat(systems_.get_carb_heat() > 0.5 ? 0.0 : 1.0);
        if (win.key_pressed(KEY_8)) {
            if (!systems_.is_engine_running()) systems_.actuate_primer();
            else systems_.set_pitot_heat_switch(!systems_.get_pitot_heat_switch());
        }
    }
    if (win.key_pressed(KEY_F) || yk.flaps_down_pressed) systems_.set_flaps_target_deg(std::min(aircraft_.max_flaps_deg, systems_.get_flaps_target_deg() + 10.0));
    if (win.key_pressed(KEY_V) || yk.flaps_up_pressed) systems_.set_flaps_target_deg(std::max(0.0, systems_.get_flaps_target_deg() - 10.0));
    if (yk.parking_brake_pressed && !chocks_on_ && !tie_downs_on_) st_.parking_brake = !st_.parking_brake;
    if (yk.ap_disc_pressed) st_.ap_master = false;
    if (std::abs(yk.trim_rate) > 0.1) st_.trim = std::clamp(st_.trim + yk.trim_rate * dt * 0.28, -0.55, 0.55);
    // Cursor-Anchored Trackpad/Mouse Yoke (zero XWarpPointer -> zero XWayland runaway!):
    // Anchors seamlessly around current cursor with compact trackpad travel, magnetic center detent,
    // sliding boundary clamp, and Left-Click / R instant level+recenter.
    const nspain::FlightState &fs_cur = fdm_.state();
    if (!taxi_hold_valid_) {
        taxi_hold_hdg_deg_ = fs_cur.heading_deg;
        taxi_hold_valid_ = true;
    }
    const bool rmb_down = win.mouse_right_down();
    const bool moving_yoke = mouse_yoke && !rmb_down;
    const double cur_mx = static_cast<double>(win.mouse_x());
    const double cur_my = static_cast<double>(win.mouse_y());
    if (moving_yoke) {
        const auto prect = win.presentation_rect(GAME_OUTPUT_W, GAME_OUTPUT_H);
        const double rad = std::clamp(std::min<double>(prect.w, prect.h) * 0.17, 96.0, 155.0);
        if (!yoke_anchor_valid_) {
            anchor_mx_ = cur_mx;
            anchor_my_ = cur_my;
            raw_yoke_x_ = raw_yoke_y_ = 0.0;
            yoke_anchor_valid_ = true;
        } else if (was_rmb_down_) {
            anchor_mx_ = cur_mx - raw_yoke_x_ * rad;
            anchor_my_ = cur_my - raw_yoke_y_ * rad;
        }
        if (win.mouse_left_clicked()) {
            anchor_mx_ = cur_mx;
            anchor_my_ = cur_my;
            raw_yoke_x_ = raw_yoke_y_ = 0.0;
            mouse_yoke_roll_ = mouse_yoke_pitch_ = 0.0;
            kb_bank_cmd_ = kb_pitch_cmd_ = 0.0;
            taxi_hold_hdg_deg_ = fs_cur.heading_deg;
        }
        double dx = cur_mx - anchor_mx_;
        double dy = cur_my - anchor_my_;
        if (std::abs(dx) > rad) {
            dx = std::copysign(rad, dx);
            anchor_mx_ = cur_mx - dx;
        }
        if (std::abs(dy) > rad) {
            dy = std::copysign(rad, dy);
            anchor_my_ = cur_my - dy;
        }
        raw_yoke_x_ = std::clamp(dx / rad, -1.0, 1.0);
        raw_yoke_y_ = std::clamp(dy / rad, -1.0, 1.0);
        auto shape_axis = [](double u) {
            const double a = std::abs(u);
            if (a <= 0.09) return 0.0;
            const double t = (a - 0.09) / 0.91;
            return std::copysign(t * (0.45 + 0.55 * t), u);
        };
        mouse_yoke_roll_ = shape_axis(raw_yoke_x_);
        mouse_yoke_pitch_ = shape_axis(raw_yoke_y_);
    } else if (!mouse_yoke) {
        yoke_anchor_valid_ = false;
        raw_yoke_x_ = raw_yoke_y_ = 0.0;
        mouse_yoke_roll_ = 0.0;
        mouse_yoke_pitch_ = 0.0;
    }
    was_rmb_down_ = rmb_down;

    // Keyboard A/D & Up/Down smoothly drive the SAME attitude/taxi assist so keyboard flying is effortless too!
    const double key_roll_in = (win.key_down(KEY_D) ? 1.0 : 0.0) - (win.key_down(KEY_A) ? 1.0 : 0.0);
    const double key_pitch_in = (win.key_down(KEY_DOWN) ? 1.0 : 0.0) - (win.key_down(KEY_UP) ? 1.0 : 0.0);
    if (key_roll_in != 0.0) kb_bank_cmd_ = std::clamp(kb_bank_cmd_ + key_roll_in * dt * 1.35, -1.0, 1.0);
    else kb_bank_cmd_ *= std::exp(-dt * (fs_cur.on_ground ? 4.5 : 2.2));
    if (key_pitch_in != 0.0) kb_pitch_cmd_ = std::clamp(kb_pitch_cmd_ + key_pitch_in * dt * 1.15, -1.0, 1.0);
    else if ((fs_cur.on_ground && fs_cur.ias_kt < 38.0) || std::abs(kb_pitch_cmd_) < 0.09) kb_pitch_cmd_ *= std::exp(-dt * 3.5);

    const bool key_rud = win.key_down(KEY_E) || win.key_down(KEY_Q);
    const double kb_rud = (win.key_down(KEY_E) ? 0.65 : 0.0) - (win.key_down(KEY_Q) ? 0.65 : 0.0);
    const bool manual_rud = key_rud || yk.has_rudder;
    const double man_rud = key_rud ? kb_rud : (yk.has_rudder ? yk.rudder : 0.0);
    const bool hw_yoke_active = yk.has_yoke && (!mouse_yoke || std::abs(yk.roll) > 0.02 || std::abs(yk.pitch) > 0.02);
    const double roll_cmd = std::clamp((hw_yoke_active ? yk.roll : (mouse_yoke ? mouse_yoke_roll_ : 0.0)) + kb_bank_cmd_, -1.0, 1.0);
    const double pitch_cmd = std::clamp((hw_yoke_active ? yk.pitch : (mouse_yoke ? mouse_yoke_pitch_ : 0.0)) + kb_pitch_cmd_, -1.0, 1.0);
    if (fs_cur.on_ground && (std::abs(roll_cmd) > 0.05 || manual_rud)) {
        taxi_hold_hdg_deg_ = fs_cur.heading_deg;
    }

    double ail_tgt = roll_cmd;
    double elv_tgt = pitch_cmd;
    double rud_tgt = man_rud;
    double assist_brk_l = 0.0, assist_brk_r = 0.0;
    // Cockpit Autopilot (requires electrical + avionics bus & airborne; strong pilot stick overrides/disconnects).
    if (!st_.slew_fast) {
        double eff_roll = roll_cmd, eff_pitch = pitch_cmd;
        apply_autopilot(fs_cur, eff_roll, eff_pitch);
        if (hw_yoke_active && graphics_settings_.yoke_direct && !st_.ap_master) {
            // Direct physical yoke authority: bypasses bank/VSI assist envelope for true manual flight feel.
            ail_tgt = eff_roll;
            elv_tgt = eff_pitch;
            if (manual_rud) {
                rud_tgt = man_rud;
            } else if (fs_cur.on_ground) {
                const double spd = std::max(0.0, fs_cur.ground_speed_mps);
                if (std::abs(eff_roll) <= 0.04 && spd > 0.6) {
                    const double hdg_err = std::remainder(taxi_hold_hdg_deg_ - fs_cur.heading_deg, 360.0);
                    rud_tgt = std::clamp(hdg_err * 0.065 - fs_cur.r_rps * 1.1, -0.75, 0.75);
                } else {
                    rud_tgt = std::clamp(eff_roll * 0.85, -0.95, 0.95);
                }
            } else {
                rud_tgt = std::clamp(ail_tgt * 0.35 - fs_cur.beta_deg * 0.050 - fs_cur.r_rps * 0.35, -0.55, 0.55);
            }
            if (fs_cur.on_ground && !manual_rud) taxi_hold_hdg_deg_ = fs_cur.heading_deg;
            else if (!fs_cur.on_ground) taxi_hold_hdg_deg_ = fs_cur.heading_deg;
            if (fs_cur.on_ground && (std::abs(eff_roll) > 0.10 || manual_rud)) st_.runway_tracking = false;
        } else {
            const FlightAssistOut assist = compute_flight_and_taxi_assist(
                eff_roll, eff_pitch, man_rud, manual_rud, taxi_hold_hdg_deg_, fs_cur);
            ail_tgt = assist.aileron;
            elv_tgt = assist.elevator;
            rud_tgt = assist.rudder;
            assist_brk_l = assist.brake_left_add;
            assist_brk_r = assist.brake_right_add;
            if (fs_cur.on_ground) {
                if (!assist.holding_heading) taxi_hold_hdg_deg_ = fs_cur.heading_deg;
                if (std::abs(roll_cmd) > 0.10 || manual_rud) st_.runway_tracking = false;
            } else {
                taxi_hold_hdg_deg_ = fs_cur.heading_deg;
                if ((std::abs(pitch_cmd) < 0.14 || st_.ap_master) && !st_.paused)
                    st_.trim = std::clamp(st_.trim + assist.elevator * dt * 0.14, -0.55, 0.55);
            }
        }
    }
    const double ctrl_rate = (hw_yoke_active && graphics_settings_.yoke_direct) ? 28.0 : 11.0;
    st_.aileron  += (ail_tgt - st_.aileron) * std::min(1.0, dt * ctrl_rate);
    st_.elevator += (elv_tgt - st_.elevator) * std::min(1.0, dt * ctrl_rate);
    st_.rudder   += (rud_tgt - st_.rudder) * std::min(1.0, dt * (yk.has_rudder ? 24.0 : 12.0));
    if (win.key_down(KEY_W)) { st_.throttle = std::clamp(st_.throttle + dt * 0.55, 0.0, 1.0); throttle_hw_active_ = false; }
    if (win.key_down(KEY_S)) { st_.throttle = std::clamp(st_.throttle - dt * 0.55, 0.0, 1.0); throttle_hw_active_ = false; }
    if (yk.has_throttle) {
        if (throttle_hw_last_ < 0.0) throttle_hw_last_ = yk.throttle;
        else if (std::abs(yk.throttle - throttle_hw_last_) > 0.015) throttle_hw_active_ = true;
        if (throttle_hw_active_) { st_.throttle = yk.throttle; throttle_hw_last_ = yk.throttle; }
    }
    // Convenience ground brake: holding S at idle smoothly slows the aircraft to a stop!
    const double idle_s_brake = (fs_cur.on_ground && st_.throttle <= 0.02 && win.key_down(KEY_S)) ? 0.65 : 0.0;

    float wx_est = std::clamp((atmo.cloud_cov - 0.16f) / 0.78f, 0.0f, 1.0f);
    if (st_.paused) {
        st_.ground_m = pack_->sample_elevation(st_.lat_deg, st_.lon_deg);
        last_air_ = reg_atmo.sample_at(st_.lat_deg, st_.lon_deg, st_.alt_msl_m, atmo.sun_dir, sim_time_sec, wx_est);
        return;
    }

    if (st_.slew_fast) {
        // 350 kt high-speed streaming & CDLOD smoothness stress mode!
        double spd = win.key_down(KEY_SHIFT) ? 260.0 : 180.0; // 350..505 kt!
        st_.heading_deg = std::fmod(st_.heading_deg + (ail_tgt * 55.0) * dt + 360.0, 360.0);
        st_.pitch_deg = std::clamp(st_.pitch_deg + (elv_tgt * 30.0) * dt, -30.0, 30.0);
        st_.roll_deg = ail_tgt * 22.0;
        double hr = st_.heading_deg * DEG2RAD_D;
        double pr = st_.pitch_deg * DEG2RAD_D;
        st_.vel_east_mps = std::sin(hr) * std::cos(pr) * spd;
        st_.vel_north_mps = std::cos(hr) * std::cos(pr) * spd;
        st_.vsi_mps = std::sin(pr) * spd;
        st_.speed_mps = spd;
        st_.lat_deg += st_.vel_north_mps * dt / pack_->anchor_m_lat();
        st_.lon_deg += st_.vel_east_mps * dt / pack_->anchor_m_lon();
        st_.ground_m = pack_->sample_elevation(st_.lat_deg, st_.lon_deg);
        st_.alt_msl_m = std::max(st_.ground_m + 80.0, st_.alt_msl_m + st_.vsi_mps * dt);
        last_air_ = reg_atmo.sample_at(st_.lat_deg, st_.lon_deg, st_.alt_msl_m, atmo.sun_dir, sim_time_sec, wx_est);
        return;
    }

    const auto &fs = fdm_.state();
    double brake_left = 0.0, brake_right = 0.0, rudder = st_.rudder;
    if (st_.parking_brake && !chocks_on_ && !tie_downs_on_ && (win.key_pressed(KEY_X) || st_.throttle >= .08))
        st_.parking_brake = false;
    const double brake_eff = std::clamp(1.0 - 0.55 * brake_wear_, 0.35, 1.0);
    if (chocks_on_ || tie_downs_on_) {
        brake_left = brake_right = 1.0;
    } else {
        const double brk_btn = (win.key_down(KEY_X) || yk.brake_held) ? brake_eff : 0.0;
        const double brk = st_.parking_brake ? 1.0 : std::max(brk_btn, idle_s_brake * brake_eff);
        brake_left = std::clamp(std::max(brk, yk.brake_left * brake_eff) + assist_brk_l * brake_eff, 0.0, 1.0);
        brake_right = std::clamp(std::max(brk, yk.brake_right * brake_eff) + assist_brk_r * brake_eff, 0.0, 1.0);
    }
    if (fs.on_ground && fs.ground_speed_mps > 2.0 && std::abs(gear_alignment_) > 0.01) {
        rudder = std::clamp(rudder + gear_alignment_ * 0.16, -1.0, 1.0);
    }
    if (st_.runway_tracking) {
        const double heading = departure_heading_ * DEG2RAD_D;
        const double north = (fs.lat_deg - departure_lat_) * pack_->anchor_m_lat();
        const double east = (fs.lon_deg - departure_lon_) * pack_->anchor_m_lon();
        const double cross_track = east * std::cos(heading) - north * std::sin(heading);
        if ((!fs.on_ground && fs.agl_m > 2.0) || std::abs(cross_track) > 15.0 ||
            win.key_down(KEY_Q) || win.key_down(KEY_E) || std::abs(yk.rudder) > 0.12) {
            st_.runway_tracking = false;
        } else if (fs.on_ground && !st_.parking_brake) {
            // Aim ahead on the centreline, damping yaw so the nose wheel does
            // not overcorrect. Pedal input immediately returns full control.
            const double lookahead = 18.0 + fs.ground_speed_mps * 0.85;
            const double target_heading = departure_heading_ - std::atan2(cross_track, lookahead) * RAD2DEG_D;
            const double heading_error = std::remainder(target_heading - fs.heading_deg, 360.0);
            rudder = std::clamp(0.048 * heading_error - 0.20 * fs.r_rps, -0.32, 0.32);
        }
    }
    advance_aircraft(dt, atmo, reg_atmo, sim_time_sec, rudder, brake_left, brake_right);
}

void AircraftAndHud::advance_aircraft(double dt, const AtmosphereState &atmo, const RegionalAtmosphere &reg_atmo,
                                     double sim_time_sec, double rudder, double brake_left, double brake_right) {
    if (dt <= 0) return;
    const float wx_est = std::clamp((atmo.cloud_cov - 0.16f) / 0.78f, 0.0f, 1.0f);
    const nspain::FlightState &fs = fdm_.state();
    last_air_ = reg_atmo.sample_at(fs.lat_deg, fs.lon_deg, fs.alt_msl_m, atmo.sun_dir, sim_time_sec, wx_est,
                                   dt, fs.heading_deg, std::max(14.0, fs.tas_kt * 0.514444));

    nspain::C172EnvStep env_step{};
    env_step.dt = dt;
    env_step.throttle01 = st_.throttle;
    env_step.mixture01 = st_.mixture;
    env_step.rpm = fs.rpm;
    env_step.ias_kt = fs.ias_kt;
    env_step.fuel_flow_demand_gph = fs.fuel_flow_gph;
    env_step.ambient_temp_c = last_air_.oat_c;
    env_step.precip_mm = last_air_.precipitation_mm_hr;
    env_step.dewpoint_c = dewpoint_from_temperature_rh(last_air_.oat_c, last_air_.relative_humidity);
    env_step.cloud_cover_pct = atmo.cloud_cov * 100.0;
    systems_.update(env_step);

    double g_here = pack_->sample_elevation(fs.lat_deg, fs.lon_deg);

    nspain::FlightControls ci{};
    ci.aileron = st_.aileron;
    ci.elevator = st_.elevator;
    ci.rudder = rudder;
    ci.elevator_trim = st_.trim;
    ci.throttle = st_.throttle;
    ci.mixture = st_.mixture;
    ci.flaps = systems_.get_flaps01();
    ci.brake_left = brake_left;
    ci.brake_right = brake_right;
    fdm_.set_controls(ci);

    nspain::FlightEnvironment ei{};
    ei.air_temp_c = last_air_.oat_c;
    ei.relative_humidity = last_air_.relative_humidity;
    ei.qnh_hpa = last_air_.qnh_hpa;
    ei.terrain_elev_m = g_here;
    ei.wind_east_mps = last_air_.wind_east_mps;
    ei.wind_north_mps = last_air_.wind_north_mps;
    ei.wind_down_mps = -last_air_.wind_up_mps; // NED convention: +Down = -Up!
    ei.p_turb_rps = last_air_.p_turb_rps;
    ei.q_turb_rps = last_air_.q_turb_rps;
    ei.r_turb_rps = last_air_.r_turb_rps;
    ei.airframe_ice_factor = std::clamp(static_cast<double>(airframe_ice01_), 0.0, 1.0);
    ei.airframe_ice_kg = std::max(systems_.get_airframe_ice_kg(), static_cast<double>(airframe_ice01_) * 22.0);
    ei.rain_drag_factor = std::clamp(last_air_.precipitation_mm_hr / 40.0, 0.0, 1.0);
    fdm_.set_environment(ei);
    nspain::FlightEngineInputs engine{};
    engine.left_fuel_gal = systems_.get_left_tank_gal();
    engine.right_fuel_gal = systems_.get_right_tank_gal();
    engine.magnetos = systems_.get_magnetos();
    engine.power_multiplier = systems_.get_engine_power_mult() *
                              (1.0 - damage_ * 0.55) * (1.0 - prop_nicks_ * 0.12);
    engine.starter = systems_.is_starter_cranking();
    engine.fuel_starved = systems_.is_fuel_starved();
    engine.running = systems_.is_engine_running();
    fdm_.set_engine(engine);

    if (!fdm_.advance(dt)) {
        std::fprintf(stderr, "[JSBSIM] Flight executive stopped; pausing flight\n");
        st_.paused = st_.menu_open = true;
        return;
    }
    const nspain::FlightState &ns = fdm_.state();
    st_.lat_deg = ns.lat_deg;
    st_.lon_deg = ns.lon_deg;
    st_.alt_msl_m = ns.alt_msl_m;
    st_.ground_m = pack_->sample_elevation(ns.lat_deg, ns.lon_deg);

    // Accumulate real rain-soaked ground wetness (wets runways/terrain quickly in rain, dries slowly in sun).
    if (last_air_.precipitation_mm_hr > 0.15) {
        const double wet_rate = std::clamp(last_air_.precipitation_mm_hr / 10.0, 0.15, 2.0) * 0.065;
        ground_wetness_ = std::min(1.0, ground_wetness_ + wet_rate * dt);
    } else {
        const double dry_rate = 0.012 + std::max(0.0, static_cast<double>(atmo.sun_dir.y)) * 0.018;
        ground_wetness_ = std::max(0.0, ground_wetness_ - dry_rate * dt);
    }

    // Dynamic windscreen frost/rime, cabin misting, and physically-coupled cockpit turbulence/buffet!
    const float ice_rate = last_air_.icing_intensity;
    if (ice_rate > 0.02f) {
        windscreen_frost_ = std::min(1.0f, windscreen_frost_ + ice_rate * static_cast<float>(dt) * 0.085f);
        airframe_ice01_ = std::min(1.0f, std::max(airframe_ice01_ + ice_rate * static_cast<float>(dt) * 0.065f,
                                                  static_cast<float>(systems_.get_airframe_ice_kg() / 22.0)));
    } else if (last_air_.oat_c > 1.5) {
        const float melt = static_cast<float>(std::clamp((last_air_.oat_c - 1.5) * 0.015, 0.01, 0.14) * dt);
        windscreen_frost_ = std::max(0.0f, windscreen_frost_ - melt);
        airframe_ice01_ = std::max(0.0f, airframe_ice01_ - melt * 0.85f);
    }
    const float warm_cabin = std::clamp(static_cast<float>((systems_.get_oil_temp_f() - 105.0) / 55.0), 0.0f, 1.0f);
    const float mist_target = (last_air_.oat_c < 9.5 && last_air_.relative_humidity > 0.74)
        ? std::clamp(static_cast<float>((9.5 - last_air_.oat_c) / 10.0 * (last_air_.relative_humidity - 0.70) * 3.0) * (1.0f - warm_cabin * 0.92f), 0.0f, 0.85f)
        : 0.0f;
    cabin_mist_ += (mist_target - cabin_mist_) * static_cast<float>(std::min(1.0, dt * 0.35));

    const float head_scale = (graphics_settings_.head_motion_mode == 0) ? 0.0f
                           : ((graphics_settings_.head_motion_mode == 1) ? 0.50f : 1.0f);
    const float g_delta = static_cast<float>(std::clamp(ns.g_load - 1.0, -1.8, 2.5));
    const float g_jerk = static_cast<float>(std::clamp((ns.g_load - prev_g_load_) / std::max(0.008, dt), -8.0, 8.0));
    prev_g_load_ = ns.g_load;
    // Crisp touchdown seat jolt when main gear makes runway contact.
    if (!prev_on_ground_pov_ && ns.on_ground && head_scale > 0.01f) {
        const float impact = std::clamp(static_cast<float>(-prev_vsi_pov_) * 0.16f, 0.06f, 0.65f);
        cockpit_head_vel_.y -= impact * head_scale;
    }
    prev_on_ground_pov_ = ns.on_ground;
    prev_vsi_pov_ = ns.vsi_mps;

    const float lat_lean = static_cast<float>(std::clamp(-ns.beta_deg * 0.0018 - ns.p_rps * 0.012, -0.024, 0.024)) * head_scale;
    const float target_y = std::clamp(-g_delta * 0.020f - g_jerk * 0.0018f, -0.042f, 0.042f) * head_scale;
    const float target_z = std::clamp(static_cast<float>((ns.tas_kt - 95.0) * 0.00022), -0.012f, 0.012f) * head_scale;
    // Natural frequency ~11 rad/s, critical damping ratio ~0.92 -> solid human head inertia, no bounce/jello
    const float fdt = static_cast<float>(std::min(dt, 0.04));
    const float k_spring = 115.0f, c_damp = 19.5f;
    cockpit_head_vel_.x += ((lat_lean - cockpit_shake_pos_.x) * k_spring - cockpit_head_vel_.x * c_damp) * fdt;
    cockpit_head_vel_.y += ((target_y - cockpit_shake_pos_.y) * k_spring - cockpit_head_vel_.y * c_damp) * fdt;
    cockpit_head_vel_.z += ((target_z - cockpit_shake_pos_.z) * k_spring - cockpit_head_vel_.z * c_damp) * fdt;
    cockpit_shake_pos_.x += cockpit_head_vel_.x * fdt;
    cockpit_shake_pos_.y += cockpit_head_vel_.y * fdt;
    cockpit_shake_pos_.z += cockpit_head_vel_.z * fdt;

    // Tactile mechanical vibration: stall buffet, runway/grass rollout, starter crank & subtle idle engine thrum.
    const float stall_buffet = (!ns.on_ground && ns.ias_kt > 20.0)
        ? std::clamp(static_cast<float>((ns.alpha_deg - 11.8) / 4.5), 0.0f, 1.0f) * 0.0055f : 0.0f;
    const float ground_rumble = (ns.on_ground && ns.ground_speed_mps > 2.0)
        ? std::clamp(static_cast<float>(ns.ground_speed_mps / 26.0), 0.0f, 1.0f) * 0.0040f : 0.0f;
    const float eng_shudder = (systems_.is_starter_cranking() || systems_.is_sputtering()) ? 0.0042f : 0.0f;
    const float idle_thrum = (systems_.is_engine_running() && ns.rpm > 450.0 && ns.rpm < 1250.0)
        ? (0.0007f * std::sin(static_cast<float>(sim_time_sec) * 48.0f) * head_scale) : 0.0f;
    const float mech_amp = (stall_buffet + ground_rumble + eng_shudder) * head_scale;
    if (mech_amp > 1e-5f) {
        static thread_local std::mt19937 vib_rng{std::random_device{}()};
        std::uniform_real_distribution<float> u11(-1.0f, 1.0f);
        cockpit_shake_pos_.x += u11(vib_rng) * mech_amp * 0.65f;
        cockpit_shake_pos_.y += u11(vib_rng) * mech_amp;
    }
    cockpit_shake_pos_.y += idle_thrum;

    // Dynamic pilot gaze anticipation: smoothly look slightly into coordinated turns + gentle horizon hold!
    const float look_blend = std::min(1.0f, fdt * 4.2f);
    const float want_yaw   = (!ns.on_ground) ? std::clamp(static_cast<float>(-ns.roll_deg * 0.16), -6.5f, 6.5f) * head_scale : 0.0f;
    const float want_pitch = (!ns.on_ground) ? std::clamp(static_cast<float>(ns.pitch_deg * 0.10), -2.2f, 2.2f) * head_scale : 0.0f;
    const float want_roll  = (!ns.on_ground) ? std::clamp(static_cast<float>(ns.roll_deg * 0.075), -2.6f, 2.6f) * head_scale : 0.0f;
    cockpit_look_feel_.x += (want_yaw   - cockpit_look_feel_.x) * look_blend;
    cockpit_look_feel_.y += (want_pitch - cockpit_look_feel_.y) * look_blend;
    cockpit_look_feel_.z += (want_roll  - cockpit_look_feel_.z) * look_blend;
    cockpit_shake_rot_ = {0.0f, 0.0f};
    st_.heading_deg = ns.heading_deg;
    st_.pitch_deg = ns.pitch_deg;
    st_.roll_deg = ns.roll_deg;
    st_.speed_mps = ns.ground_speed_mps;
    st_.vsi_mps = ns.vsi_mps;
    st_.vel_east_mps = ns.vel_east_mps;
    st_.vel_north_mps = ns.vel_north_mps;
    resolve_world_collisions(dt);
}

void AircraftAndHud::resolve_world_collisions(double dt) {
    if (impact_fx_s_ > 0.0f) impact_fx_s_ = std::max(0.0f, impact_fx_s_ - static_cast<float>(dt));
    if (impact_cooldown_s_ > 0.0) impact_cooldown_s_ = std::max(0.0, impact_cooldown_s_ - dt);
    if (collision_msg_s_ > 0.0) collision_msg_s_ = std::max(0.0, collision_msg_s_ - dt);
    if (st_.slew_fast) return;

    const nspain::FlightState &fs = fdm_.state();
    // 1. Detect heavy touchdown / crash-landing shock onto terrain or runway
    if (!prev_ground_coll_ && fs.on_ground && prev_vsi_coll_ < -3.3) {
        const double sev = std::clamp((-prev_vsi_coll_ - 3.3) / 8.5, 0.05, 0.90);
        damage_ = std::min(1.0, damage_ + sev * 0.55);
        gear_alignment_ = std::clamp(gear_alignment_ + (fs.roll_deg >= 0.0 ? 1.0 : -1.0) * sev * 0.45, -1.0, 1.0);
        tire_wear_ = std::min(1.0, tire_wear_ + sev * 0.40);
        if (fs.pitch_deg < -1.5) {
            dmg_nose_ = std::min(1.0, dmg_nose_ + sev * 0.65);
            prop_nicks_ = std::min(1.0, prop_nicks_ + sev * 0.70);
        } else {
            dmg_tail_ = std::min(1.0, dmg_tail_ + sev * 0.35);
        }
        if (sev > 0.25) windshield_crack_ = std::min(1.0, windshield_crack_ + (sev - 0.20) * 0.85);
        impact_fx_s_ = std::max(impact_fx_s_, static_cast<float>(0.6 + sev));
        cockpit_head_vel_.y -= static_cast<float>(sev * 1.4);
        collision_msg_ = (sev > 0.45) ? "HARD IMPACT: GEAR & AIRFRAME DAMAGED" : "HARD LANDING: CHECK GEAR";
        collision_msg_s_ = 5.5;
    }
    prev_ground_coll_ = fs.on_ground;
    prev_vsi_coll_ = fs.vsi_mps;

    if (!pack_ || impact_cooldown_s_ > 0.0) return;

    double mlat = 111132.0, mlon = 82600.0;
    nspain::meters_per_degree(fs.lat_deg, mlat, mlon);
    const Mat4 ac_rot = Mat4::rotate_yxz(
        static_cast<float>(-fs.heading_deg * DEG2RAD_D),
        static_cast<float>(fs.pitch_deg * DEG2RAD_D),
        static_cast<float>(-fs.roll_deg * DEG2RAD_D)
    );
    struct Probe { Vec3 local; double radius; int zone; }; // zone: 0=nose,1=wingL,2=wingR,3=tail,4=cabin/gear
    static const Probe kProbes[5] = {
        {{ 0.00f, -0.05f, -2.05f}, 0.55, 0},
        {{-5.35f,  0.42f, -0.15f}, 0.50, 1},
        {{ 5.35f,  0.42f, -0.15f}, 0.50, 2},
        {{ 0.00f,  0.35f,  4.35f}, 0.50, 3},
        {{ 0.00f, -0.45f,  0.15f}, 0.70, 4},
    };

    PackReader::ObjectCollisionHit best{};
    int best_zone = -1;
    for (const Probe &p : kProbes) {
        const Vec4 w = ac_rot.mul({p.local.x, p.local.y, p.local.z, 0.0f});
        const double plat = fs.lat_deg + (-w.z) / mlat;
        const double plon = fs.lon_deg + w.x / mlon;
        const double palt = fs.alt_msl_m + w.y;
        const PackReader::ObjectCollisionHit hit = pack_->query_object_collision(plat, plon, palt, p.radius);
        if (hit.hit && (!best.hit || hit.kind < best.kind || (hit.kind == best.kind && hit.penetration_m > best.penetration_m))) {
            best = hit;
            best_zone = p.zone;
        }
    }
    if (!best.hit) return;

    const double ve = fs.vel_east_mps, vu = fs.vsi_mps, vn = fs.vel_north_mps;
    const double spd = std::hypot(std::hypot(ve, vn), vu);
    if (best.kind == 3) {
        // Tree crown canopy foliage snag: slow aircraft, chip prop/leading edge, push gently clear
        const double sev = std::clamp(spd / 62.0, 0.05, 0.30);
        damage_ = std::min(1.0, damage_ + sev * 0.45);
        if (best_zone == 0 || best_zone == 4) {
            prop_nicks_ = std::min(1.0, prop_nicks_ + sev * 0.75);
            dmg_nose_ = std::min(1.0, dmg_nose_ + sev * 0.55);
            windshield_crack_ = std::min(1.0, windshield_crack_ + sev * 0.35);
        } else if (best_zone == 1) {
            dmg_wing_l_ = std::min(1.0, dmg_wing_l_ + sev * 0.85);
        } else if (best_zone == 2) {
            dmg_wing_r_ = std::min(1.0, dmg_wing_r_ + sev * 0.85);
        } else {
            dmg_tail_ = std::min(1.0, dmg_tail_ + sev * 0.75);
        }
        const double roll_kick = (best_zone == 1) ? 0.55 : ((best_zone == 2) ? -0.55 : 0.0);
        const double yaw_kick  = (best_zone == 1) ? -0.45 : ((best_zone == 2) ? 0.45 : 0.0);
        fdm_.apply_impact_impulse(
            0.0, best.penetration_m + 0.35, 0.0,
            ve * 0.86, std::max(vu * 0.75 + 1.2, 0.8), vn * 0.86,
            fs.p_rps + roll_kick, fs.q_rps + 0.22, fs.r_rps + yaw_kick
        );
        impact_fx_s_ = std::max(impact_fx_s_, 0.9f);
        impact_cooldown_s_ = 0.25;
        cockpit_head_vel_.y -= 0.45f;
        collision_msg_ = "TREE CANOPY STRIKE! PROP & WING SCRAPED";
        collision_msg_s_ = 4.5;
    } else {
        // Solid impact (building, bridge/wall/landmark, or tree trunk)
        const double v_into = -(ve * best.normal_e + vu * best.normal_up + vn * best.normal_n);
        if (v_into < -1.0 && best.penetration_m < 0.25) return;
        const double imp = std::max(std::max(v_into, spd * 0.45), 1.8);
        const double sev = std::clamp(imp / 24.0, 0.12, 0.96);

        damage_ = std::min(1.0, damage_ + sev * 0.72);
        if (best_zone == 0) {
            dmg_nose_ = std::min(1.0, dmg_nose_ + sev * 1.15);
            prop_nicks_ = std::min(1.0, prop_nicks_ + sev * 1.10);
            windshield_crack_ = std::min(1.0, windshield_crack_ + sev * 0.85);
            if (sev > 0.36 && systems_.is_engine_running()) {
                systems_.set_magnetos(0);
                systems_.set_engine_running_state(false, last_air_.oat_c);
            }
        } else if (best_zone == 1) {
            dmg_wing_l_ = std::min(1.0, dmg_wing_l_ + sev * 1.15);
        } else if (best_zone == 2) {
            dmg_wing_r_ = std::min(1.0, dmg_wing_r_ + sev * 1.15);
        } else if (best_zone == 3) {
            dmg_tail_ = std::min(1.0, dmg_tail_ + sev * 1.15);
        } else {
            dmg_nose_ = std::min(1.0, dmg_nose_ + sev * 0.45);
            gear_alignment_ = std::clamp(gear_alignment_ + (fs.roll_deg >= 0.0 ? 1.0 : -1.0) * sev * 0.65, -1.0, 1.0);
            tire_wear_ = std::min(1.0, tire_wear_ + sev * 0.55);
            windshield_crack_ = std::min(1.0, windshield_crack_ + sev * 0.90);
        }

        const double bounce = (1.0 + 0.34) * std::max(0.0, v_into) + 2.2;
        const double nve = std::clamp(ve * 0.55 + best.normal_e * bounce, -65.0, 65.0);
        const double nvu = std::clamp(vu * 0.55 + best.normal_up * bounce + 0.8, -30.0, 35.0);
        const double nvn = std::clamp(vn * 0.55 + best.normal_n * bounce, -65.0, 65.0);
        const double roll_kick = (best_zone == 1) ? (1.8 * sev) : ((best_zone == 2) ? (-1.8 * sev) : 0.0);
        const double yaw_kick  = (best_zone == 1) ? (-1.9 * sev) : ((best_zone == 2) ? (1.9 * sev) : 0.0);
        const double push = std::clamp(static_cast<double>(best.penetration_m) + 0.32, 0.25, 16.0);
        fdm_.apply_impact_impulse(
            best.normal_e * push, best.normal_up * push + 0.12, best.normal_n * push,
            nve, nvu, nvn,
            std::clamp(fs.p_rps + roll_kick, -3.5, 3.5),
            std::clamp(fs.q_rps + (best_zone == 0 ? 0.6 : -0.4) * sev, -2.5, 2.5),
            std::clamp(fs.r_rps + yaw_kick, -3.0, 3.0)
        );
        impact_fx_s_ = 1.6f;
        impact_cooldown_s_ = 0.22;
        cockpit_head_vel_.y -= static_cast<float>(0.65 + sev * 1.1);
        cockpit_head_vel_.z -= static_cast<float>(0.85 * sev);
        collision_msg_ = (best.kind == 0) ? "COLLISION: BUILDING IMPACT!"
            : ((best.kind == 1) ? "COLLISION: STRUCTURE / WALL IMPACT!" : "COLLISION: TREE TRUNK STRIKE!");
        collision_msg_s_ = 5.5;
    }

    const nspain::FlightState &post = fdm_.state();
    st_.lat_deg = post.lat_deg;
    st_.lon_deg = post.lon_deg;
    st_.alt_msl_m = post.alt_msl_m;
    st_.ground_m = pack_->sample_elevation(post.lat_deg, post.lon_deg);
    st_.heading_deg = post.heading_deg;
    st_.pitch_deg = post.pitch_deg;
    st_.roll_deg = post.roll_deg;
    st_.speed_mps = post.ground_speed_mps;
    st_.vsi_mps = post.vsi_mps;
    st_.vel_east_mps = post.vel_east_mps;
    st_.vel_north_mps = post.vel_north_mps;
}

void AircraftAndHud::draw_glb(
    const GlbMeshData &mesh,
    const Mat4 &vp, const Mat4 &root_world, const Mat4 &root_cam_rel,
    const std::vector<Mat4> &node_anim,
    const std::vector<float> &node_alpha,
    const std::vector<float> &node_emiss,
    const AtmosphereState &atmo, GLuint refl_env_tex, bool is_interior, float sim_time_sec,
    bool output_resolution
) {
    if (!mesh.vao || mesh.submeshes.empty() || mesh.nodes.empty()) return;

    // Resolve hierarchical node world matrices once per frame (reusing scratch buffers -> zero heap allocs!)
    const size_t nn = mesh.nodes.size();
    thread_local std::vector<Mat4> local_m, world_m;
    thread_local std::vector<uint8_t> done;
    local_m.resize(nn);
    world_m.resize(nn);
    done.assign(nn, 0);
    for (size_t i = 0; i < nn; i++) {
        local_m[i] = (i < node_anim.size()) ? (mesh.nodes[i].base_local * node_anim[i]) : mesh.nodes[i].base_local;
    }
    auto eval_node = [&](auto &&self, size_t i) -> const Mat4 & {
        if (done[i]) return world_m[i];
        int p = mesh.nodes[i].parent;
        world_m[i] = (p >= 0 && static_cast<size_t>(p) < nn) ? (self(self, static_cast<size_t>(p)) * local_m[i]) : local_m[i];
        done[i] = 1;
        return world_m[i];
    };
    for (size_t i = 0; i < nn; i++) eval_node(eval_node, i);

    glUseProgram(ac_prog_);
    glUniform3f(u_ac_sun_, atmo.sun_dir.x, atmo.sun_dir.y, atmo.sun_dir.z);
    glUniform3f(u_ac_cool_, atmo.haze_cool.x, atmo.haze_cool.y, atmo.haze_cool.z);
    glUniform3f(u_ac_warm_, atmo.haze_warm.x, atmo.haze_warm.y, atmo.haze_warm.z);
    glUniform1f(u_ac_int_, is_interior ? 1.0f : 0.0f);
    glUniform1i(u_ac_detail_, graphics_settings_.aircraft_detail);
    glUniform1i(u_ac_native_output_, output_resolution ? 1 : 0);
    glUniform1f(u_ac_output_exposure_, atmo.exposure);
    glUniform1i(u_ac_output_lut_, 6);
    glBindVertexArray(mesh.vao);
    const nspain::FlightState &flight_air = fdm_.state();
    // Use JSBSim true airspeed for the rain-flow driver. Ground speed varies
    // with wind and can make the drops slide at the wrong rate in a headwind.
    // Slew mode bypasses JSBSim, so derive its flow from the explicit slew speed.
    const float relative_airspeed = st_.slew_fast
        ? static_cast<float>(std::max(0.0, st_.speed_mps))
        : static_cast<float>(std::max(0.0, flight_air.tas_kt) * 0.514444);

    // Pass 0: opaque submeshes; Pass 1: translucent glass, sun visors & spinning prop blur disc
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_FALSE);
        }
        for (const auto &sm : mesh.submeshes) {
            size_t ni = static_cast<size_t>(std::max(0, sm.node_idx));
            float amul = (ni < node_alpha.size()) ? node_alpha[ni] : 1.0f;
            if (amul < 0.02f) continue;
            bool trans = (sm.base_color.w * amul) < 0.96f;
            if ((pass == 0 && trans) || (pass == 1 && !trans)) continue;

            Mat4 m_world = root_world * world_m[ni];
            Mat4 m_cam   = root_cam_rel * world_m[ni];
            Mat4 mvp     = vp * m_world;
            float emul = (ni < node_emiss.size()) ? node_emiss[ni] : 1.0f;

            glUniformMatrix4fv(u_ac_mvp_,   1, GL_FALSE, mvp.m);
            glUniformMatrix4fv(u_ac_model_, 1, GL_FALSE, m_cam.m);
            if (u_ac_node_ >= 0) glUniformMatrix4fv(u_ac_node_, 1, GL_FALSE, world_m[ni].m);
            const float d_nose = static_cast<float>(std::clamp(std::max(dmg_nose_, prop_nicks_ * 0.45), 0.0, 1.0));
            const float d_wl   = static_cast<float>(std::clamp(dmg_wing_l_, 0.0, 1.0));
            const float d_wr   = static_cast<float>(std::clamp(dmg_wing_r_, 0.0, 1.0));
            const float d_tail = static_cast<float>(std::clamp(dmg_tail_, 0.0, 1.0));
            const float scorch = static_cast<float>(std::clamp(
                dmg_nose_ * 0.65 + (systems_.get_cht_f() > 445.0 ? (systems_.get_cht_f() - 445.0) / 55.0 : 0.0) +
                systems_.get_cylinder_wear() * 0.40 + (systems_.has_overtemp_failure() ? 0.85 : 0.0), 0.0, 1.0));
            if (u_ac_dmg_zones_ >= 0) glUniform4f(u_ac_dmg_zones_, d_nose, d_wl, d_wr, d_tail);
            if (u_ac_dmg_extra_ >= 0) {
                glUniform4f(u_ac_dmg_extra_, static_cast<float>(std::clamp(damage_, 0.0, 1.0)),
                            static_cast<float>(std::clamp(windshield_crack_, 0.0, 1.0)),
                            scorch, is_interior ? 1.0f : 0.0f);
            }
            glBindTextureUnit(0, sm.albedo_tex ? sm.albedo_tex : white_tex_);
            glBindTextureUnit(1, sm.normal_tex ? sm.normal_tex : white_tex_);
            glBindTextureUnit(2, sm.mr_tex     ? sm.mr_tex     : white_tex_);
            glBindTextureUnit(3, refl_env_tex  ? refl_env_tex  : white_tex_);
            glBindTextureUnit(4, windshield_rain_front_tex_ ? windshield_rain_front_tex_ : white_tex_);
            glBindTextureUnit(5, windshield_rain_side_tex_ ? windshield_rain_side_tex_ : white_tex_);
            // Pair electrical bus, avionics master, and starter voltage dip directly to 3D cockpit visuals!
            Vec4 tint = sm.base_color;
            if (is_interior) {
                const bool batt_on = systems_.get_master_battery();
                const bool avn_on = batt_on && systems_.get_avionics_master();
                const float crank_dip = systems_.is_starter_cranking() ? 0.56f : 1.0f;
                if (sm.material_name == "AvionicsStack") {
                    if (!avn_on) {
                        tint.x *= 0.14f; tint.y *= 0.14f; tint.z *= 0.16f;
                        emul = 0.0f;
                    } else {
                        emul *= crank_dip;
                    }
                } else if (sm.material_name == "SteamDials" || sm.material_name == "GaugeNeedle" ||
                           sm.material_name == "RotatingCards") {
                    emul *= batt_on ? crank_dip : 0.0f;
                }
            }
            glUniform4f(u_ac_tint_, tint.x, tint.y, tint.z, tint.w);
            glUniform4f(u_ac_pbr_, sm.metallic, sm.roughness, sm.normal_tex ? 1.0f : 0.0f, sm.mr_tex ? 1.0f : 0.0f);
            glUniform3f(u_ac_em_, sm.emissive_col.x * emul, sm.emissive_col.y * emul, sm.emissive_col.z * emul);
            glUniform1f(u_ac_alpha_, amul);
            glUniform1f(u_ac_livery_, (!is_interior && ni == 0) ? 1.0f : 0.0f);
            glUniform1i(u_ac_unlit_, sm.unlit);
            glUniform1f(u_ac_baked_reflection_,
                (sm.material_name == "C172S baked cabin" ||
                 sm.material_name == "C172S baked PBR instrument panel") ? 1.0f : 0.0f);
            const int glass_mode = is_interior
                ? (mesh.nodes[ni].name == visual_node_name(mesh, "Windshield") ? 2 :
                   mesh.nodes[ni].name == visual_node_name(mesh, "Cabin glazing") ? 1 : 0)
                : 0;
            glUniform1i(u_ac_cabin_glass_, glass_mode);
            const float glass_rain_mm = (last_air_.precipitation_mm_hr >= 0.45) ? static_cast<float>(last_air_.precipitation_mm_hr) : 0.0f;
            glUniform3f(u_ac_glass_weather_, glass_rain_mm, sim_time_sec, relative_airspeed);
            const float defrost_clear = std::clamp(static_cast<float>((systems_.get_oil_temp_f() - 120.0) / 55.0), 0.0f, 1.0f);
            const float wet_sheen = std::clamp(static_cast<float>(last_air_.precipitation_mm_hr / 6.0), 0.0f, 1.0f);
            glUniform4f(u_ac_glass_fx_, windscreen_frost_, cabin_mist_,
                        static_cast<float>(windshield_dirt_), last_air_.snow_factor);
            glUniform4f(u_ac_plane_wx_, airframe_ice01_, last_air_.lightning_flash, defrost_clear, wet_sheen);

            if (sm.double_sided || glass_mode != 0) glDisable(GL_CULL_FACE);
            else glEnable(GL_CULL_FACE);
            glDrawElementsBaseVertex(
                GL_TRIANGLES, static_cast<GLsizei>(sm.index_count), GL_UNSIGNED_INT,
                reinterpret_cast<const void *>(static_cast<uintptr_t>(sm.first_index) * sizeof(uint32_t)),
                sm.base_vertex
            );
        }
        if (pass == 1) {
            glDepthMask(GL_TRUE);
            glDisable(GL_BLEND);
        }
    }
    glEnable(GL_CULL_FACE);
    glBindVertexArray(0);
}

void AircraftAndHud::apply_damage_rig(std::vector<Mat4> &anim, float flaps_rad, float ail, float elv, float trm, float rud) const {
    auto set_node = [this](const GlbMeshData &m, std::vector<Mat4> &dst, const char *name, const Mat4 &xf) {
        const std::string mapped = visual_node_name(m, name);
        auto it = m.node_index.find(mapped);
        if (it != m.node_index.end() && static_cast<size_t>(it->second) < dst.size()) dst[static_cast<size_t>(it->second)] = xf;
    };
    const float pn = static_cast<float>(std::clamp(prop_nicks_, 0.0, 1.0));
    const float ga = static_cast<float>(std::clamp(gear_alignment_, -1.0, 1.0));
    const float wl = static_cast<float>(std::clamp(dmg_wing_l_, 0.0, 1.0));
    const float wr = static_cast<float>(std::clamp(dmg_wing_r_, 0.0, 1.0));
    const float td = static_cast<float>(std::clamp(dmg_tail_, 0.0, 1.0));
    const float gd = static_cast<float>(std::clamp(std::max(std::abs(ga) * 0.8, damage_ * 0.45), 0.0, 1.0));

    // Bent/twisted propeller blades & eccentric damaged hub wobble
    const float wob = (pn > 0.06f && prop_angle_rad_ > 0.0f) ? (pn * 0.045f) : 0.0f;
    set_node(ext_mesh_, anim, "PropHub", Mat4::rotate_yxz(wob, wob * 0.6f, prop_angle_rad_));
    if (pn > 0.05f) {
        set_node(ext_mesh_, anim, "PropBlades",
                 Mat4::rotate_yxz(pn * 0.14f, pn * 0.42f, -pn * 0.22f));
    }
    set_node(ext_mesh_, anim, "Flap_L",    Mat4::rotate_yxz(0.0f, flaps_rad + wl * 0.16f, wl * 0.08f));
    set_node(ext_mesh_, anim, "Flap_R",    Mat4::rotate_yxz(0.0f, flaps_rad + wr * 0.16f, -wr * 0.08f));
    set_node(ext_mesh_, anim, "Aileron_L", Mat4::rotate_yxz(0.0f, +ail * 20.0f * DEG2RAD_F - wl * 0.20f, 0.0f));
    set_node(ext_mesh_, anim, "Aileron_R", Mat4::rotate_yxz(0.0f, -ail * 20.0f * DEG2RAD_F - wr * 0.20f, 0.0f));
    set_node(ext_mesh_, anim, "Elevator",  Mat4::rotate_yxz(0.0f, -elv * 22.0f * DEG2RAD_F + td * 0.14f, td * 0.06f));
    set_node(ext_mesh_, anim, "TrimTab",   Mat4::rotate_yxz(0.0f, +trm * 15.0f * DEG2RAD_F, 0.0f));
    set_node(ext_mesh_, anim, "Rudder",    Mat4::rotate_yxz(+rud * 24.0f * DEG2RAD_F + td * 0.18f, 0.0f, td * 0.05f));
    set_node(ext_mesh_, anim, "NoseGear",
             Mat4::rotate_yxz(-rud * 18.0f * DEG2RAD_F + ga * 0.46f, gd * 0.18f, gd * 0.28f));
}

void AircraftAndHud::render_aircraft_3d(const Mat4 &vp, const Vec3 &cam_off, const AtmosphereState &atmo,
                                        GLuint refl_env_tex, float sim_time_sec,
                                        bool output_resolution, GLuint output_lut_tex,
                                        GLuint backdrop_hdr_tex) {
    if (st_.view_mode == 2) return; // Free cam
    Mat4 ac_rot = Mat4::rotate_yxz(
        static_cast<float>(-st_.heading_deg * DEG2RAD_D),
        static_cast<float>(st_.pitch_deg * DEG2RAD_D),
        static_cast<float>(-st_.roll_deg * DEG2RAD_D)
    );

    const nspain::FlightState &fs = fdm_.state();
    const float rdt = (prev_render_t_ >= 0.0f) ? std::clamp(sim_time_sec - prev_render_t_, 0.0f, 0.08f) : 0.016f;
    prev_render_t_ = sim_time_sec;
    const bool cranking = systems_.is_starter_cranking();
    const bool sputtering = systems_.is_sputtering();
    const bool batt_on = systems_.get_master_battery();
    float rpm = (st_.visual_rpm_override >= 0.0)
        ? static_cast<float>(st_.visual_rpm_override)
        : static_cast<float>(std::max(0.0, fs.rpm));
    // Real continuous propeller spin: frozen when stopped, rhythmic compression-stroke chug when cranking!
    float eff_prop_rpm = rpm;
    if (st_.visual_rpm_override < 0.0 && cranking && eff_prop_rpm < 140.0f) {
        eff_prop_rpm = 115.0f * (0.55f + 0.45f * std::sin(sim_time_sec * 13.0f));
    }
    if (eff_prop_rpm > 5.0f) {
        prop_angle_rad_ = std::fmod(prop_angle_rad_ + rdt * eff_prop_rpm * 0.032f, 2.0f * PI_F);
    }
    // Engine-driven vacuum pump spools the attitude indicator gyro upright when running, tumbles when off!
    const float gyro_target = (rpm > 450.0f) ? 1.0f : 0.0f;
    ai_gyro_spool_ += (gyro_target - ai_gyro_spool_) * std::min(1.0f, rdt * (gyro_target > 0.5f ? 0.55f : 0.25f));
    float blur_t = (eff_prop_rpm < 20.0f) ? 0.0f : std::clamp((eff_prop_rpm - 260.0f) / 850.0f, cranking ? 0.18f : 0.0f, 1.0f);
    if (st_.visual_rpm_override < 0.0 && sputtering) blur_t *= 0.68f + 0.32f * std::sin(sim_time_sec * 19.0f);
    float flaps_rad = static_cast<float>(
        (st_.visual_rpm_override >= 0.0 ? st_.visual_flaps_deg : systems_.get_flaps_actual_deg()) * DEG2RAD_D);
    float ail = static_cast<float>(st_.aileron);
    float elv = static_cast<float>(st_.elevator);
    float rud = static_cast<float>(st_.rudder);
    float trm = static_cast<float>(st_.trim);
    const float idle_vib = (rpm > 120.0f && rpm < 950.0f) ? ((950.0f - rpm) / 830.0f) : (cranking ? 0.95f : 0.0f);

    auto set_node = [this](const GlbMeshData &m, std::vector<Mat4> &anim, const char *name, const Mat4 &xf) {
        const std::string mapped = visual_node_name(m, name);
        auto it = m.node_index.find(mapped);
        if (it != m.node_index.end()) anim[static_cast<size_t>(it->second)] = xf;
    };
    auto set_f = [this](const GlbMeshData &m, std::vector<float> &arr, const char *name, float v) {
        const std::string mapped = visual_node_name(m, name);
        auto it = m.node_index.find(mapped);
        if (it != m.node_index.end()) arr[static_cast<size_t>(it->second)] = v;
    };

    if (st_.view_mode == 0) {
        // Pilot seated eye framing: crisp unobstructed 3D 6-pack + GNS430 + cowling + spinning prop veil!
        // The cockpit model is framed around the pilot's eye; place both it
        // and the camera at the eye height above JSBSim's centre of gravity.
        Mat4 ckpt_root = ac_rot * Mat4::translate({0.0f, static_cast<float>(aircraft_.cockpit_eye_height_m) + 0.034f, -0.120f});
        size_t nn = ckpt_mesh_.nodes.size();
        std::vector<Mat4> anim(nn, Mat4::identity());
        std::vector<float> alpha(nn, 1.0f);
        std::vector<float> emiss(nn, 1.0f);

        // Animated spinning prop motion veil visible through front windshield
        set_node(ckpt_mesh_, anim, "PropBlurDisc", Mat4::rotate_yxz(0.0f, 0.0f, prop_angle_rad_));
        // Keep the spinning propeller subtle through the windshield. An opaque
        // disc here obscures the view and reads like a rectangular reflection.
        set_f(ckpt_mesh_, alpha, "PropBlurDisc", blur_t * 0.035f);

        // Live 3D flight controls: Ram's-horn Yoke (with idle/crank shudder), Pedals + Toe Brakes, Knobs, Fuel Shutoff!
        const float y_vib = idle_vib * 0.0022f * std::sin(sim_time_sec * 37.0f);
        Mat4 yoke_xf = Mat4::translate({y_vib, y_vib * 0.6f, elv * 0.055f}) * Mat4::rotate_yxz(0.0f, 0.0f, -ail * 55.0f * DEG2RAD_F);
        set_node(ckpt_mesh_, anim, "Yoke", yoke_xf);
        set_node(ckpt_mesh_, anim, "YokeCopilot", yoke_xf);
        const float brk_l = static_cast<float>(fdm_.controls().brake_left);
        const float brk_r = static_cast<float>(fdm_.controls().brake_right);
        set_node(ckpt_mesh_, anim, "RudderPedal_L", Mat4::translate({0.0f, 0.0f, +rud * 0.036f}) * Mat4::rotate_yxz(0.0f, -brk_l * 18.0f * DEG2RAD_F, 0.0f));
        set_node(ckpt_mesh_, anim, "RudderPedal_R", Mat4::translate({0.0f, 0.0f, -rud * 0.036f}) * Mat4::rotate_yxz(0.0f, -brk_r * 18.0f * DEG2RAD_F, 0.0f));
        set_node(ckpt_mesh_, anim, "ThrottleKnob", Mat4::translate({0.0f, 0.0f, static_cast<float>((1.0 - st_.throttle) * 0.055)}));
        set_node(ckpt_mesh_, anim, "MixtureKnob",  Mat4::translate({0.0f, 0.0f, static_cast<float>((1.0 - st_.mixture)  * 0.055)}));
        set_node(ckpt_mesh_, anim, "CarbHeatKnob", Mat4::translate({0.0f, 0.0f, static_cast<float>(systems_.get_carb_heat() * 0.042)}));
        set_node(ckpt_mesh_, anim, "ParkingBrake", Mat4::translate({0.0f, 0.0f, st_.parking_brake ? 0.032f : 0.0f}));
        {
            static constexpr float sel_ang[4] = {180.0f, -65.0f, 0.0f, 65.0f}; // OFF, LEFT, BOTH, RIGHT
            const int fsel = std::clamp(systems_.get_fuel_selector(), 0, 3);
            set_node(ckpt_mesh_, anim, "FuelSelector", Mat4::rotate_yxz(sel_ang[fsel] * DEG2RAD_F, 0.0f, 0.0f));
            set_node(ckpt_mesh_, anim, "FuelShutoff",  Mat4::translate({0.0f, 0.0f, fsel == 0 ? 0.028f : 0.0f}));
        }
        set_node(ckpt_mesh_, anim, "TrimWheel",    Mat4::rotate_yxz(0.0f, trm * 2.2f, 0.0f));
        set_node(ckpt_mesh_, anim, "FlapLever",    Mat4::rotate_yxz(0.0f, flaps_rad * 0.9f, 0.0f));
        set_node(ckpt_mesh_, anim, "CompassCard",  Mat4::rotate_yxz(static_cast<float>(st_.heading_deg * DEG2RAD_D), 0.0f, 0.0f));

        // Live 3D Steam Gauge Needles & Rotating Cards (clockwise = -Z rotation in PanelGroup local frame!)
        // If pitot cover was left on or pitot iced over, ASI needle stays pegged at the 40 kt lower stop.
        float ias_kt = systems_.is_pitot_blocked()
            ? 40.0f
            : std::clamp(static_cast<float>(fs.ias_kt), 40.0f, 160.0f);
        float asi_cw = 25.0f + (ias_kt - 40.0f) / 120.0f * 310.0f;
        set_node(ckpt_mesh_, anim, "Needle_ASI", Mat4::rotate_yxz(0.0f, 0.0f, -asi_cw * DEG2RAD_F));

        // Vacuum-driven AI tumbles gently when engine is stopped and erects smoothly as vacuum builds!
        const float ai_roll_deg = std::lerp(16.0f, static_cast<float>(st_.roll_deg), ai_gyro_spool_);
        const float ai_pitch_deg = std::lerp(-8.0f, static_cast<float>(st_.pitch_deg), ai_gyro_spool_);
        float pitch_off = -std::clamp(ai_pitch_deg, -20.0f, 20.0f) * 0.00065f;
        set_node(ckpt_mesh_, anim, "Card_AI",
                 Mat4::rotate_yxz(0.0f, 0.0f, ai_roll_deg * DEG2RAD_F) *
                 Mat4::translate({0.0f, pitch_off, 0.0f}));

        double alt_ft = std::max(0.0, st_.alt_msl_m * 3.28084);
        float alt100_cw  = static_cast<float>(std::fmod(alt_ft, 1000.0)  / 1000.0  * 360.0);
        float alt1000_cw = static_cast<float>(std::fmod(alt_ft, 10000.0) / 10000.0 * 360.0);
        set_node(ckpt_mesh_, anim, "Needle_ALT_100",  Mat4::rotate_yxz(0.0f, 0.0f, -alt100_cw  * DEG2RAD_F));
        set_node(ckpt_mesh_, anim, "Needle_ALT_1000", Mat4::rotate_yxz(0.0f, 0.0f, -alt1000_cw * DEG2RAD_F));

        float tc_bank = batt_on ? std::clamp(static_cast<float>(st_.roll_deg * 0.85), -28.0f, 28.0f) : 0.0f;
        set_node(ckpt_mesh_, anim, "Card_TC",  Mat4::rotate_yxz(0.0f, 0.0f, -tc_bank * DEG2RAD_F));
        set_node(ckpt_mesh_, anim, "Card_HDG", Mat4::rotate_yxz(0.0f, 0.0f, static_cast<float>(st_.heading_deg * DEG2RAD_D)));

        float hfpm = std::clamp(static_cast<float>(st_.vsi_mps * 196.85 / 100.0), -20.0f, 20.0f);
        float vsi_cw = 270.0f + (hfpm / 20.0f) * 165.0f;
        set_node(ckpt_mesh_, anim, "Needle_VSI", Mat4::rotate_yxz(0.0f, 0.0f, -vsi_cw * DEG2RAD_F));

        const float rpm_disp = eff_prop_rpm + (sputtering ? (std::sin(sim_time_sec * 17.0f) * 160.0f) : 0.0f);
        float r100 = std::clamp(rpm_disp / 100.0f, 0.0f, 35.0f);
        float rpm_cw = 230.0f + (r100 / 35.0f) * 260.0f;
        set_node(ckpt_mesh_, anim, "Needle_RPM", Mat4::rotate_yxz(0.0f, 0.0f, -rpm_cw * DEG2RAD_F));

        if (output_resolution && output_lut_tex) glBindTextureUnit(6, output_lut_tex);
        glBindTextureUnit(7, (output_resolution && backdrop_hdr_tex) ? backdrop_hdr_tex : white_tex_);
        Mat4 cockpit_cam_rel = Mat4::translate({-cam_off.x, -cam_off.y, -cam_off.z}) * ckpt_root;
        draw_glb(ckpt_mesh_, vp, ckpt_root, cockpit_cam_rel, anim, alpha, emiss, atmo, refl_env_tex, true,
                 sim_time_sec, output_resolution);
    } else if (st_.view_mode == 1) {
        size_t nn = ext_mesh_.nodes.size();
        std::vector<Mat4> anim(nn, Mat4::identity());
        std::vector<float> alpha(nn, 1.0f);
        std::vector<float> emiss(nn, 1.0f);

        // Live animated exterior control surfaces, chrome spinner + soft fading PropBlades + PropBlurDisc & aviation lights!
        apply_damage_rig(anim, flaps_rad, ail, elv, trm, rud);
        set_f(ext_mesh_, alpha,   "PropBlades",   1.0f - blur_t * 0.78f);
        set_node(ext_mesh_, anim, "PropBlurDisc", Mat4::rotate_yxz(0.0f, 0.0f, prop_angle_rad_ * 1.37f));
        set_f(ext_mesh_, alpha,   "PropBlurDisc", blur_t * 0.035f);

        // Exterior aviation lights coupled 100% to electrical bus & engine state (all dark when Cold & Dark!)
        const float bus_mul = batt_on ? (cranking ? 0.56f : 1.0f) : 0.0f;
        float cyc = std::fmod(sim_time_sec, 1.25f);
        bool strobe_on = (cyc < 0.065f) || (cyc > 0.16f && cyc < 0.225f);
        float bcn_pulse = 0.25f + 2.5f * std::pow(0.5f + 0.5f * std::sin(sim_time_sec * 5.8f), 4.0f);
        const bool bcn_sw = batt_on && systems_.get_light_switch(0);
        const bool running = systems_.is_engine_running();
        set_f(ext_mesh_, emiss, "BeaconRed",    bcn_sw ? (bcn_pulse * bus_mul) : 0.0f);
        set_f(ext_mesh_, emiss, "NavLightRed",  batt_on ? (2.4f * bus_mul) : 0.0f);
        set_f(ext_mesh_, emiss, "NavLightGreen", batt_on ? (2.4f * bus_mul) : 0.0f);
        set_f(ext_mesh_, emiss, "StrobeWhite",  (batt_on && (running || !fs.on_ground)) ? ((strobe_on ? 3.5f : 0.08f) * bus_mul) : 0.0f);
        set_f(ext_mesh_, emiss, "LandingLight", (batt_on && running) ? (4.2f * bus_mul) : 0.0f);

        // Align visual tire bottom (-1.358 m in exterior GLB) with JSBSim static bogey height (~1.20 m).
        Mat4 root_cam_rel = Mat4::translate({-cam_off.x, -cam_off.y + 0.16f, -cam_off.z}) * ac_rot;
        draw_glb(ext_mesh_, vp, ac_rot, root_cam_rel, anim, alpha, emiss, atmo, refl_env_tex, false,
                 sim_time_sec, output_resolution);
    }
}

void AircraftAndHud::render_precip_streaks(const Mat4 &vp, const Vec3 &cam_world_en_alt,
                                           const Vec3 &cam_right, const Vec3 &cam_up,
                                           const AtmosphereState &atmo, float sim_time_sec, bool walking) {
    if (!precip_prog_ || !precip_vao_) return;
    const float dmg_smoke = static_cast<float>(std::clamp(
        std::max({damage_ * 0.9, dmg_nose_, systems_.has_overtemp_failure() ? 1.0 : 0.0}) +
        (impact_fx_s_ > 0.05f ? 0.45f : 0.0f), 0.0, 1.0));
    const float rate = static_cast<float>(std::max(0.0, last_air_.precipitation_mm_hr));
    if (rate < 0.12f && dmg_smoke < 0.18f) return;

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_GEQUAL);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glUseProgram(precip_prog_);
    glUniformMatrix4fv(u_pr_vp_, 1, GL_FALSE, vp.m);
    glUniform3f(u_pr_right_, cam_right.x, cam_right.y, cam_right.z);
    glUniform3f(u_pr_up_, cam_up.x, cam_up.y, cam_up.z);
    glUniform1f(u_pr_time_, sim_time_sec);
    glBindVertexArray(precip_vao_);

    if (rate >= 0.12f) {
        const float intensity = std::clamp(rate / 16.0f, 0.05f, 1.0f);
        Vec3 rel_vel{
            static_cast<float>(last_air_.wind_east_mps - (walking ? 0.0 : st_.vel_east_mps)),
            static_cast<float>(last_air_.wind_up_mps - (walking ? 0.0 : st_.vsi_mps)),
            static_cast<float>(-(last_air_.wind_north_mps - (walking ? 0.0 : st_.vel_north_mps)))
        };
        const float day = std::clamp((atmo.sun_dir.y + 0.10f) / 0.25f, 0.0f, 1.0f);
        const float fl = last_air_.lightning_flash;
        const float sn = last_air_.snow_factor;
        const Vec3 rain_c = {(0.55f * day + 0.14f) + fl * 0.45f, (0.62f * day + 0.16f) + fl * 0.52f, (0.70f * day + 0.20f) + fl * 0.72f};
        const Vec3 snow_c = {(0.84f * day + 0.18f) + fl * 0.35f, (0.88f * day + 0.20f) + fl * 0.40f, (0.94f * day + 0.24f) + fl * 0.50f};
        const Vec3 col = {std::lerp(rain_c.x, snow_c.x, sn), std::lerp(rain_c.y, snow_c.y, sn), std::lerp(rain_c.z, snow_c.z, sn)};
        glUniform3f(u_pr_cam_, cam_world_en_alt.x, cam_world_en_alt.y, cam_world_en_alt.z);
        glUniform3f(u_pr_vel_, rel_vel.x, rel_vel.y, rel_vel.z);
        glUniform1f(u_pr_snow_, sn);
        glUniform1f(u_pr_int_, intensity);
        glUniform3f(u_pr_col_, col.x, col.y, col.z);
        glDrawArrays(GL_TRIANGLES, 0, 240 * 6);
    }

    // Exterior/walkaround engine & impact smoke / sparks plume cleanly trailing nose cowling
    if (dmg_smoke >= 0.18f && (st_.view_mode != 0 || walking)) {
        const double hdg_r = st_.heading_deg * DEG2RAD_D;
        Vec3 cowl_off{static_cast<float>(std::sin(hdg_r) * 1.45), 0.15f, static_cast<float>(-std::cos(hdg_r) * 1.45)};
        if (walking) cowl_off = {0.0f, 0.0f, 0.0f}; // offset unused in walking unless parked spot known; keep plume over nose when flying/chase
        if (!walking) {
            const float dday = std::clamp((atmo.sun_dir.y + 0.08f) / 0.24f, 0.15f, 1.0f);
            const float sparks = std::clamp(impact_fx_s_ * 0.7f, 0.0f, 1.0f);
            Vec3 smoke_col = {
                std::lerp(0.14f, 0.95f, sparks) * dday,
                std::lerp(0.14f, 0.42f, sparks) * dday,
                std::lerp(0.16f, 0.10f, sparks) * dday
            };
            Vec3 plume_vel{
                static_cast<float>(last_air_.wind_east_mps - st_.vel_east_mps) * 0.45f,
                static_cast<float>(1.6 - st_.vsi_mps * 0.45),
                static_cast<float>(-(last_air_.wind_north_mps - st_.vel_north_mps)) * 0.45f
            };
            glUniform3f(u_pr_cam_, cowl_off.x, cowl_off.y, cowl_off.z);
            glUniform3f(u_pr_vel_, plume_vel.x, plume_vel.y, plume_vel.z);
            glUniform1f(u_pr_snow_, 2.0f);
            glUniform1f(u_pr_int_, dmg_smoke);
            glUniform3f(u_pr_col_, smoke_col.x, smoke_col.y, smoke_col.z);
            glDrawArrays(GL_TRIANGLES, 0, 28 * 6);
        }
    }

    glBindVertexArray(0);
    glDepthMask(GL_TRUE);
    glEnable(GL_CULL_FACE);
    glDisable(GL_BLEND);
}

void AircraftAndHud::render_parked_aircraft(const Mat4 &vp, Vec3 offset, float heading,
                                          const AtmosphereState &atmo, GLuint reflection, GLuint lut) {
    const Mat4 model = Mat4::translate(offset) * Mat4::rotate_yxz(-heading * DEG2RAD_F, 0, 0);
    if (lut) glBindTextureUnit(6, lut);
    std::vector<Mat4> anim(ext_mesh_.nodes.size(), Mat4::identity());
    apply_damage_rig(anim, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
    std::vector<float> alpha(ext_mesh_.nodes.size(), 1.0f);
    const auto blur = ext_mesh_.node_index.find(visual_node_name(ext_mesh_, "PropBlurDisc"));
    if (blur != ext_mesh_.node_index.end()) alpha[blur->second] = 0.0f;
    draw_glb(ext_mesh_, vp, model, model, anim, alpha, {}, atmo, reflection, false, 0.0f, lut != 0);
}

void AircraftAndHud::start_owned_flight(double lat, double lon, double alt, double heading,
                                        const AircraftCareSnapshot &care, double ambient_temp_c, bool on_runway) {
    owned_mode_ = true;
    starter_held_ = false;
    if (care.engine_running) systems_.reset_to_running_default();
    else systems_.reset_cold_and_dark();

    systems_.set_tank_gallons(care.left_fuel_gal, care.right_fuel_gal);
    systems_.set_oil_state(care.oil_quarts, care.oil_hours);
    systems_.set_battery_soc(care.battery_soc);
    systems_.set_wear_state(care.spark_fouling, care.shock_cooling, care.cylinder_wear);
    systems_.set_sump_water(care.sump_water);
    systems_.set_pitot_cover(care.pitot_cover_on);
    systems_.set_master_battery(care.master_on);
    systems_.set_master_alternator(care.master_on);
    systems_.set_avionics_master(care.avionics_on);
    systems_.set_light_switch(0, care.master_on);
    systems_.set_magnetos(care.magnetos);
    systems_.set_fuel_selector(care.fuel_selector);
    systems_.set_flaps_immediate_deg(care.flaps_deg);
    systems_.set_carb_heat(care.carb_heat);
    systems_.clear_failures();
    systems_.set_engine_running_state(care.engine_running, ambient_temp_c);

    damage_ = care.airframe_damage;
    tire_wear_ = care.tire_wear;
    brake_wear_ = care.brake_wear;
    gear_alignment_ = care.gear_alignment;
    prop_nicks_ = care.prop_nicks;
    windshield_dirt_ = care.windshield_dirt;
    windshield_crack_ = care.windshield_crack;
    dmg_nose_ = care.dmg_nose;
    dmg_wing_l_ = care.dmg_wing_l;
    dmg_wing_r_ = care.dmg_wing_r;
    dmg_tail_ = care.dmg_tail;
    chocks_on_ = care.chocks_on;
    tie_downs_on_ = care.tie_downs_on;

    st_.aileron = st_.elevator = st_.rudder = st_.trim = 0;
    st_.throttle = 0.0;
    st_.mixture = std::clamp(care.mixture, 0.0, 1.0);
    st_.view_mode = 0;
    st_.parking_brake = true;
    st_.slew_fast = false;
    reset_flight(lat, lon, alt, heading, 0, true);
    departure_lat_ = lat;
    departure_lon_ = lon;
    departure_heading_ = heading;
    st_.runway_tracking = on_runway;
    st_.paused = st_.menu_open = false;
}

void AircraftAndHud::sync_care_snapshot(AircraftCareSnapshot &care) const {
    care.left_fuel_gal = systems_.get_left_tank_gal();
    care.right_fuel_gal = systems_.get_right_tank_gal();
    care.oil_quarts = systems_.get_oil_quarts();
    care.oil_hours = systems_.get_oil_hours();
    care.battery_soc = systems_.get_battery_soc();
    care.spark_fouling = systems_.get_spark_fouling();
    care.shock_cooling = systems_.get_shock_cooling_wear();
    care.cylinder_wear = systems_.get_cylinder_wear();
    care.sump_water = systems_.get_sump_water();
    care.pitot_cover_on = systems_.is_pitot_cover_on();
    care.master_on = systems_.get_master_battery();
    care.avionics_on = systems_.get_avionics_master();
    care.magnetos = std::min(3, systems_.get_magnetos());
    care.fuel_selector = systems_.get_fuel_selector();
    care.mixture = st_.mixture;
    care.flaps_deg = systems_.get_flaps_target_deg();
    care.windshield_crack = std::max(care.windshield_crack, windshield_crack_);
    care.dmg_nose = std::max(care.dmg_nose, dmg_nose_);
    care.dmg_wing_l = std::max(care.dmg_wing_l, dmg_wing_l_);
    care.dmg_wing_r = std::max(care.dmg_wing_r, dmg_wing_r_);
    care.dmg_tail = std::max(care.dmg_tail, dmg_tail_);
    care.airframe_damage = std::max(care.airframe_damage, damage_);
    care.prop_nicks = std::max(care.prop_nicks, prop_nicks_);
    care.tire_wear = std::max(care.tire_wear, tire_wear_);
    if (std::abs(gear_alignment_) > std::abs(care.gear_alignment)) care.gear_alignment = gear_alignment_;
    care.carb_heat = systems_.get_carb_heat();
    care.engine_running = systems_.is_engine_running();
}

void AircraftAndHud::sync_walkaround_damage(const AircraftCareSnapshot &care) {
    damage_ = care.airframe_damage;
    tire_wear_ = care.tire_wear;
    brake_wear_ = care.brake_wear;
    gear_alignment_ = care.gear_alignment;
    prop_nicks_ = care.prop_nicks;
    windshield_dirt_ = care.windshield_dirt;
    windshield_crack_ = care.windshield_crack;
    dmg_nose_ = care.dmg_nose;
    dmg_wing_l_ = care.dmg_wing_l;
    dmg_wing_r_ = care.dmg_wing_r;
    dmg_tail_ = care.dmg_tail;
}

void AircraftAndHud::quick_start_or_shutdown() {
    if (systems_.is_engine_running()) {
        // Clean Cessna shutdown: idle-cutoff -> mags off -> avionics & master off
        st_.throttle = 0.0;
        st_.mixture = 0.0;
        systems_.set_magnetos(0);
        systems_.set_avionics_master(false);
        systems_.set_master_battery(false);
        systems_.set_master_alternator(false);
        systems_.set_light_switch(0, false);
        systems_.set_carb_heat(0.0);
        systems_.set_pitot_heat_switch(false);
        systems_.set_engine_running_state(false, last_air_.oat_c);
        return;
    }
    const double total_fuel = systems_.get_left_tank_gal() + systems_.get_right_tank_gal();
    if (total_fuel < 0.05 || systems_.get_battery_soc() < 0.06 ||
        systems_.get_oil_quarts() < 2.3 || damage_ >= 0.85 ||
        systems_.has_overtemp_failure() || systems_.has_oil_starvation_failure()) {
        return; // Cannot quick-start an unserviceable / dead-battery / dry aircraft!
    }
    systems_.set_master_battery(true);
    systems_.set_master_alternator(true);
    systems_.set_avionics_master(true);
    systems_.set_light_switch(0, true);
    systems_.set_fuel_selector(nspain::FUEL_SEL_BOTH);
    systems_.set_magnetos(3);
    systems_.set_carb_heat(0.0);
    st_.mixture = 1.0;
    st_.throttle = std::max(st_.throttle, 0.04);
    systems_.set_engine_running_state(true, last_air_.oat_c);
}

void AircraftAndHud::hud_rect(float x, float y, float w, float h, float r, float g, float b, float a) {
    HudVert v0{x, y, -1.0f, -1.0f, r, g, b, a}, v1{x + w, y, -1.0f, -1.0f, r, g, b, a};
    HudVert v2{x + w, y + h, -1.0f, -1.0f, r, g, b, a}, v3{x, y + h, -1.0f, -1.0f, r, g, b, a};
    hud_verts_.push_back(v0); hud_verts_.push_back(v1); hud_verts_.push_back(v2);
    hud_verts_.push_back(v0); hud_verts_.push_back(v2); hud_verts_.push_back(v3);
}

void AircraftAndHud::hud_border(float x, float y, float w, float h, float t, float r, float g, float b, float a) {
    hud_rect(x, y, w, t, r, g, b, a);
    hud_rect(x, y + h - t, w, t, r, g, b, a);
    hud_rect(x, y, t, h, r, g, b, a);
    hud_rect(x + w - t, y, t, h, r, g, b, a);
}

void AircraftAndHud::hud_text(float x, float y, float font_h, const char *text, float r, float g, float b, float a) {
    if (!text || !*text) return;
    // Transliterate Spanish UTF-8 accents cleanly into ASCII 32..126 for crisp TrueType monospace atlas
    char ascii[512];
    size_t out_i = 0;
    for (size_t i = 0; text[i] && out_i + 1 < sizeof(ascii); i++) {
        uint8_t c = static_cast<uint8_t>(text[i]);
        if (c == 0xC3 && text[i + 1]) {
            uint8_t n = static_cast<uint8_t>(text[++i]);
            char repl = ' ';
            if (n == 0xA1 || n == 0xA0) repl = 'a';
            else if (n == 0xA9 || n == 0xA8) repl = 'e';
            else if (n == 0xAD || n == 0xAC) repl = 'i';
            else if (n == 0xB3 || n == 0xB2) repl = 'o';
            else if (n == 0xBA || n == 0xBC) repl = 'u';
            else if (n == 0xB1) repl = 'n';
            else if (n == 0x81) repl = 'A';
            else if (n == 0x89) repl = 'E';
            else if (n == 0x8D) repl = 'I';
            else if (n == 0x93) repl = 'O';
            else if (n == 0x9A) repl = 'U';
            else if (n == 0x91) repl = 'N';
            ascii[out_i++] = repl;
        } else if (c >= 32 && c < 127) {
            ascii[out_i++] = static_cast<char>(c);
        }
    }
    ascii[out_i] = '\0';

    constexpr float du = 1.0f / 16.0f;
    constexpr float dv = 1.0f / 6.0f;
    const float cell_w = font_h * (64.0f / 96.0f);
    const float adv = font_h * 0.56f;
    float cx = x;
    for (size_t i = 0; i < out_i; i++) {
        int idx = static_cast<int>(ascii[i]) - 32;
        if (idx > 0 && idx < 95) {
            int col = idx & 15;
            int row = idx >> 4;
            float u0 = col * du + 0.001f, u1 = (col + 1) * du - 0.001f;
            float v0 = row * dv + 0.001f, v1 = (row + 1) * dv - 0.001f;
            float x0 = cx - (cell_w - adv) * 0.5f;
            float x1 = x0 + cell_w;
            float y0 = y, y1 = y + font_h;
            hud_verts_.push_back({x0, y0, u0, v0, r, g, b, a});
            hud_verts_.push_back({x1, y0, u1, v0, r, g, b, a});
            hud_verts_.push_back({x1, y1, u1, v1, r, g, b, a});
            hud_verts_.push_back({x0, y0, u0, v0, r, g, b, a});
            hud_verts_.push_back({x1, y1, u1, v1, r, g, b, a});
            hud_verts_.push_back({x0, y1, u0, v1, r, g, b, a});
        }
        cx += adv;
    }
}

void AircraftAndHud::render_hud(
    int win_w, int win_h,
    const AtmosphereState &atmo,
    const RegionalAtmosphere &reg_atmo,
    const StreamStats &st, const CdlodDrawStats &cd,
    int quality, int total_bldgs,
    double fps, double avg_ms, double p95_ms,
    const WeatherSnapshot *wx_snap,
    const char *regime_name,
    const char *wx_provenance,
    bool show_diagnostics, const DebugOverlayView *debug
) {
    hud_verts_.clear();
    const float wf = static_cast<float>(win_w), hf = static_cast<float>(win_h);
    static const char *kQNames[4] = {"LOW iGPU", "MEDIUM", "HIGH", "ULTRA"};
    static const char *kVNames[3] = {"COCKPIT", "CHASE", "FREE CAM"};

    // HiDPI-aware responsive sizing: razor-sharp ~22px text @ 1080p, ~35px @ 1728p, auto-clamped so ZERO clip/overlap!
    const float ui = std::clamp(std::min(hf / 1080.0f, wf / 1920.0f), 0.78f, 2.30f);
    const float margin = 16.0f * ui;
    const float pad_x  = 16.0f * ui;
    const float pad_y  = 12.0f * ui;

    double agl = st_.alt_msl_m - st_.ground_m;
    double kt = st_.speed_mps * 1.94384;
    const nspain::FlightState &fs = fdm_.state();
    const char *wtag = last_air_.in_lee_rotor ? "ROTOR!" : (last_air_.in_storm_cell ? "STORM!" : (last_air_.in_thermal ? "THERMAL" : reg_atmo.regime_short()));

    char L[6][160];
    std::snprintf(L[0], sizeof(L[0]), "POS  %.5f N  %.5f E  [%s]", st_.lat_deg, st_.lon_deg, st_.slew_fast ? "350KT STRESS" : (st_.paused ? "PAUSED" : "JSBSIM"));
    std::snprintf(L[1], sizeof(L[1]), "ALT  %4.0f m MSL | AGL %4.0f m | GND %4.0f m | %s", st_.alt_msl_m, agl, st_.ground_m, last_air_.layer_name());
    std::snprintf(L[2], sizeof(L[2]), "SPD  %3.0f KT | HDG %03d | VSI %+4.0f fpm | OAT %+.0fC", kt, static_cast<int>(std::round(st_.heading_deg)) % 360, st_.vsi_mps * 196.85, last_air_.oat_c);
    std::snprintf(L[3], sizeof(L[3]), "WIND %03.0f/%2.0fkt G%+.0f | AIR %+.1fm/s (R%+.1f W%+.1f T%+.1f) %s",
        last_air_.from_dir_deg(), last_air_.speed_kt(), last_air_.gust_mps * 1.94384, last_air_.wind_up_mps,
        last_air_.ridge_lift_mps, last_air_.wave_lift_mps, last_air_.thermal_lift_mps, wtag);
    std::snprintf(L[4], sizeof(L[4]), "AERO AoA %+.1f | %.1fG | FLP %.0f | RPM %.0f | QNH %.0f", fs.alpha_deg, fs.g_load, systems_.get_flaps_actual_deg(), fs.rpm, last_air_.qnh_hpa);
    std::snprintf(L[5], sizeof(L[5]), "KEYS C:View R:Center RMB:Look Y:Yoke Esc:Controls F1-4:Quality");

    double ap_d = 0.0;
    const AirportInfo *near_ap = pack_->find_nearest_airport(st_.lat_deg, st_.lon_deg, &ap_d);
    char R[6][192];
    if (near_ap) {
        std::snprintf(R[0], sizeof(R[0]), "NEAR %s - %.1f km | %s", near_ap->id.c_str(), ap_d / 1000.0, near_ap->name.c_str());
    } else {
        std::snprintf(R[0], sizeof(R[0]), "OPENSKY VFR | REGIONAL WEATHER FIELD");
    }
    std::snprintf(R[1], sizeof(R[1]), "%s", atmo.hud_wx_line.c_str());
    if (wx_snap && !wx_snap->summary.empty()) {
        std::snprintf(R[2], sizeof(R[2]), "WX %s", wx_snap->summary.c_str());
        std::snprintf(R[3], sizeof(R[3]), "%s | cov %.0f%% base %.0fm vis %.0fkm",
                      wx_snap->label.c_str(), wx_snap->cloud_cov * 100.0f, wx_snap->cloud_base_m, wx_snap->visibility_m / 1000.0f);
    } else {
        std::snprintf(R[2], sizeof(R[2]), "WX SYNTHETIC FORCING | no live observation");
        std::snprintf(R[3], sizeof(R[3]), "%s", regime_name ? regime_name : reg_atmo.regime_name());
    }
    std::snprintf(R[4], sizeof(R[4]), "PERF %.1f FPS (avg %.1fms p95 %.1fms) | %s | %s",
                  fps, avg_ms, p95_ms, kQNames[quality], kVNames[st_.view_mode]);
    std::snprintf(R[5], sizeof(R[5]), "CDLOD %d(L%d) | CACHE %d/%d | WORLD %d bldgs | CANOPY SHADE | %s %d hits / %d new",
                  cd.visible_nodes, cd.max_active_lod, st.resident_pages, st.max_pages,
                  total_bldgs, pack_->online_provider_name(), pack_->online_tiles_used(), pack_->online_tiles_fetched());

    size_t max_l = 0, max_r = 0;
    for (int i = 0; i < 6; i++) max_l = std::max(max_l, std::strlen(L[i]));
    for (int i = 0; i < 6; i++) max_r = std::max(max_r, std::strlen(R[i]));
    float max_panel_w = wf * 0.475f;
    float fh = 22.5f * ui;
    float need_w = static_cast<float>(std::max(max_l, max_r)) * (fh * 0.56f) + pad_x * 2.2f;
    if (need_w > max_panel_w) fh *= (max_panel_w / need_w);
    const float adv = fh * 0.56f;
    const float row_h = fh * 1.24f;

    const float lh = pad_y * 2.0f + 6.0f * row_h;
    if (show_diagnostics && !st_.menu_open) {
    // Top-left Flight Telemetry Glass Panel
    const float lw = std::min(max_panel_w, static_cast<float>(max_l) * adv + pad_x * 2.1f);
    hud_rect(margin, margin, lw, lh, 0.035f, 0.060f, 0.095f, 0.82f);
    hud_border(margin, margin, lw, lh, std::max(1.0f, 1.5f * ui), 0.36f, 0.62f, 0.85f, 0.55f);
    hud_rect(margin, margin, lw, std::max(2.0f, 3.0f * ui), 0.32f, 0.78f, 0.98f, 0.90f);

    hud_text(margin + pad_x, margin + pad_y + 0.0f * row_h, fh, L[0], 0.96f, 0.98f, 1.00f, 1.0f);
    hud_text(margin + pad_x, margin + pad_y + 1.0f * row_h, fh, L[1], 0.92f, 0.96f, 1.00f, 1.0f);
    hud_text(margin + pad_x, margin + pad_y + 2.0f * row_h, fh, L[2], 0.92f, 0.96f, 1.00f, 1.0f);
    hud_text(margin + pad_x, margin + pad_y + 3.0f * row_h, fh, L[3],
        last_air_.in_lee_rotor ? 1.00f : 0.55f, last_air_.in_lee_rotor ? 0.54f : 0.96f, last_air_.in_lee_rotor ? 0.34f : 0.92f, 1.0f);
    hud_text(margin + pad_x, margin + pad_y + 4.0f * row_h, fh, L[4], 0.80f, 0.95f, 0.86f, 1.0f);
    hud_text(margin + pad_x, margin + pad_y + 5.0f * row_h, fh * 0.92f, L[5], 0.74f, 0.84f, 0.94f, 0.96f);

    // Top-right Engine, Weather & 10-TOD Telemetry Glass Panel
    const float rw = std::min(max_panel_w, static_cast<float>(max_r) * adv + pad_x * 2.1f);
    const float rh = pad_y * 2.0f + 6.0f * row_h;
    const float rx = wf - rw - margin;
    hud_rect(rx, margin, rw, rh, 0.035f, 0.060f, 0.095f, 0.82f);
    hud_border(rx, margin, rw, rh, std::max(1.0f, 1.5f * ui), 0.36f, 0.62f, 0.85f, 0.55f);
    hud_rect(rx, margin, rw, std::max(2.0f, 3.0f * ui), 0.96f, 0.78f, 0.36f, 0.92f);

    hud_text(rx + pad_x, margin + pad_y + 0.0f * row_h, fh, R[0], 1.00f, 0.95f, 0.78f, 1.0f);
    hud_text(rx + pad_x, margin + pad_y + 1.0f * row_h, fh, R[1], 0.86f, 0.94f, 1.00f, 1.0f);
    hud_text(rx + pad_x, margin + pad_y + 2.0f * row_h, fh, R[2], 0.86f, 0.94f, 1.00f, 1.0f);
    hud_text(rx + pad_x, margin + pad_y + 3.0f * row_h, fh, R[3], 0.80f, 0.90f, 0.96f, 1.0f);
    hud_text(rx + pad_x, margin + pad_y + 4.0f * row_h, fh, R[4], 0.42f, 0.98f, 0.64f, 1.0f);
    hud_text(rx + pad_x, margin + pad_y + 5.0f * row_h, fh * 0.94f, R[5], 0.82f, 0.88f, 0.94f, 0.96f);

    } else if (!st_.menu_open) {
        char flight[128];
        const char *season_tag = wx_snap ? WeatherLibrary::season_name(wx_snap->season) : "AUTO";
        std::snprintf(flight, sizeof(flight), "%.0f KT  %.0f FT MSL  HDG %03d  %s",
                      kt, st_.alt_msl_m * 3.28084, static_cast<int>(std::round(st_.heading_deg)) % 360, season_tag);
        const float band = 365.0f * ui;
        const float footer = hf - 48.0f * ui;
        hud_rect(margin, footer, band, 36.0f * ui, 0.026f, 0.048f, 0.070f, 0.84f);
        hud_text(margin + 12.0f * ui, footer + 6.0f * ui, 19.0f * ui, flight, 0.93f, 0.97f, 1.0f, 1.0f);
        hud_text(wf - 480.0f * ui, footer + 6.0f * ui, 16.0f * ui,
                 "ESC Menu   N Season   F5 Profile   F6 Weather   F7 Physics", 0.94f, 0.97f, 1.0f, 1.0f);
    }

    if (collision_msg_s_ > 0.0 && !collision_msg_.empty() && !st_.menu_open) {
        const float bw = 540.0f * ui;
        const float bx = 0.5f * (wf - bw);
        const float by = margin + 96.0f * ui;
        hud_rect(bx, by, bw, 38.0f * ui, 0.38f, 0.05f, 0.04f, 0.88f);
        hud_border(bx, by, bw, 38.0f * ui, 1.5f * ui, 1.0f, 0.42f, 0.25f, 0.95f);
        hud_text(bx + 16.0f * ui, by + 8.0f * ui, 19.0f * ui, collision_msg_.c_str(), 1.0f, 0.88f, 0.72f, 1.0f);
    }

    if (debug && !st_.menu_open)
        render_debug_overlays(wf, hf, margin + (show_diagnostics ? lh + 10.0f * ui : 0.0f), reg_atmo, *debug);

    flush_hud(wf, hf);
}

void AircraftAndHud::flush_hud(float wf, float hf, GLuint font) {
    if (hud_verts_.empty()) return;
    if (hud_verts_.size() > hud_vbo_cap_) {
        while (hud_vbo_cap_ < hud_verts_.size()) hud_vbo_cap_ = std::max<size_t>(32768, hud_vbo_cap_ * 2);
        glNamedBufferData(hud_vbo_, static_cast<GLsizeiptr>(hud_vbo_cap_ * sizeof(HudVert)), nullptr, GL_STREAM_DRAW);
    }
    const size_t max_v = hud_verts_.size();
    glNamedBufferSubData(hud_vbo_, 0, static_cast<GLsizeiptr>(max_v * sizeof(HudVert)), hud_verts_.data());

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(hud_prog_);
    glUniform2f(u_hud_scr_, wf, hf);
    glBindTextureUnit(0, font ? font : (hud_font_tex_ ? hud_font_tex_ : white_tex_));
    glBindVertexArray(hud_vao_);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(max_v));
    glBindVertexArray(0);
    glDisable(GL_BLEND);
}

} // namespace nsim
