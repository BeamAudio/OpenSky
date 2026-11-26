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
    init_avionics_display_texture();
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
    avionics_tex_ = 0;
    avionics_base_rgba_.clear();
    avionics_work_rgba_.clear();
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
    st_.ap_vs_hold = false;
    st_.ap_alt_arm = false;
    st_.ap_disconnect_warn_s = 0.0f;
    st_.ap_trim_annunc = 0;
    st_.ap_hdg_deg = hdg_deg;
    st_.ap_alt_ft = std::round(alt_m * 3.28084 / 100.0) * 100.0;
    st_.ind_alt_ft = alt_m * 3.28084 + (systems_.get_baro_hpa() - 1013.25) * 27.3;
    ai_gyro_spool_ = systems_.is_engine_running() ? 1.0f : 0.0f;
    prev_ground_coll_ = on_ground;
    prev_vsi_coll_ = 0.0;
    impact_cooldown_s_ = 0.0;
    impact_fx_s_ = 0.0f;
    collision_msg_.clear();
    collision_msg_s_ = 0.0;
    broken_parts_.clear();
    broken_wing_l_ = broken_wing_r_ = broken_tail_ = broken_prop_ = false;
    broken_gear_n_ = broken_gear_l_ = broken_gear_r_ = false;
    airframe_destroyed_ = false;
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

void AircraftAndHud::apply_autopilot(
    const nspain::FlightState &fs_cur, double dt, double &eff_roll, double &eff_pitch,
    double &ap_ail_out, double &ap_elv_out, double &ap_rud_out) {
    if (st_.ap_disconnect_warn_s > 0.0f) {
        st_.ap_disconnect_warn_s = std::max(0.0f, st_.ap_disconnect_warn_s - static_cast<float>(dt));
    }
    const bool elec_ok = systems_.is_avionics_bus_powered(1) &&
                         systems_.is_circuit_powered(nspain::CB_TURN_COORD) &&
                         systems_.get_bus_volts() >= 19.5;
    const bool gyro_ok = ai_gyro_spool_ > 0.70f;
    const bool env_ok = !fs_cur.on_ground && !st_.slew_fast &&
                        fs_cur.ias_kt >= 52.0 && fs_cur.ias_kt <= 160.0 &&
                        std::abs(fs_cur.roll_deg) <= 45.0 && std::abs(fs_cur.pitch_deg) <= 26.0;
    const bool manual_override = (std::abs(eff_roll) > 0.35 || std::abs(eff_pitch) > 0.35);

    if (!st_.ap_master) {
        st_.ap_trim_annunc = 0;
        ap_cmd_bank_deg_ = std::clamp(fs_cur.roll_deg, -22.0, 22.0);
        return;
    }
    if (!elec_ok || !gyro_ok || !env_ok || manual_override) {
        st_.ap_master = false;
        st_.ap_trim_annunc = 0;
        st_.ap_disconnect_warn_s = 3.5f;
        return;
    }

    // 1. Lateral axis: NAV (GPS Route or VOR1 OBS/CDI), HDG Bug, or ROL Hold
    double tgt_bank = st_.ap_rol_bank_deg;
    if (st_.ap_nav_hold) {
        if (st_.ap_nav_source == 1 && systems_.get_nav_to_from(1) != 0) {
            const double obs = static_cast<double>(systems_.get_nav_obs_deg(1));
            const double cdi = systems_.get_nav_cdi(1);
            const double intercept = obs + std::clamp(cdi * 38.0, -45.0, 45.0);
            const double err = std::remainder(intercept - fs_cur.heading_deg, 360.0);
            tgt_bank = std::clamp(err * 1.15, -22.0, 22.0);
        } else if (st_.ap_nav_source == 0 && st_.ap_nav_valid) {
            const double err = std::remainder(st_.ap_nav_brg_deg - fs_cur.heading_deg, 360.0);
            tgt_bank = std::clamp(err * 1.15, -22.0, 22.0);
        } else if (st_.ap_hdg_hold) {
            const double err = std::remainder(st_.ap_hdg_deg - fs_cur.heading_deg, 360.0);
            tgt_bank = std::clamp(err * 1.15, -22.0, 22.0);
        }
    } else if (st_.ap_hdg_hold) {
        const double err = std::remainder(st_.ap_hdg_deg - fs_cur.heading_deg, 360.0);
        tgt_bank = std::clamp(err * 1.15, -22.0, 22.0);
    }

    ap_cmd_bank_deg_ += std::clamp(tgt_bank - ap_cmd_bank_deg_, -14.0 * dt, 14.0 * dt);
    const double q_scale = std::clamp(92.0 / std::max(50.0, fs_cur.ias_kt), 0.65, 1.45);
    const double bank_err = ap_cmd_bank_deg_ - fs_cur.roll_deg;
    const double p_cmd = std::clamp(bank_err * 0.045, -0.35, 0.35);
    ap_ail_out = std::clamp(((p_cmd - fs_cur.p_rps) * 0.72 + bank_err * 0.024) * q_scale, -0.60, 0.60);
    ap_rud_out = std::clamp(ap_ail_out * 0.38 - fs_cur.beta_deg * 0.055 - fs_cur.r_rps * 0.38, -0.55, 0.55);

    // 2. Vertical axis: ALT ARM capture, Kollsman Baro ALT hold, VS hold, or PIT hold
    const double alt_true_ft = fs_cur.alt_msl_m * 3.28084;
    const double cur_ind_ft = alt_true_ft + (systems_.get_baro_hpa() - last_air_.qnh_hpa) * 27.3;
    st_.ind_alt_ft = cur_ind_ft;

    const double cur_vsi_fpm = fs_cur.vsi_mps * 196.85;
    const double alt_err_ft = st_.ap_alt_ft - cur_ind_ft;
    if (st_.ap_vs_hold && st_.ap_alt_arm) {
        const double cap_band = std::clamp(std::abs(cur_vsi_fpm) * 0.12, 45.0, 160.0);
        if (std::abs(alt_err_ft) <= cap_band && (alt_err_ft * st_.ap_vs_fpm >= -50.0)) {
            st_.ap_alt_hold = true;
            st_.ap_vs_hold = false;
            st_.ap_alt_arm = false;
        }
    }

    double tgt_vsi_fpm = cur_vsi_fpm;
    if (st_.ap_alt_hold) {
        tgt_vsi_fpm = std::clamp(alt_err_ft * 2.2, -800.0, 720.0);
    } else if (st_.ap_vs_hold) {
        tgt_vsi_fpm = std::clamp(st_.ap_vs_fpm, -1500.0, 1200.0);
    } else {
        tgt_vsi_fpm = std::clamp(cur_vsi_fpm, -900.0, 800.0);
    }

    if (fs_cur.ias_kt < 66.0 && tgt_vsi_fpm > 0.0) {
        const double fade = std::clamp((fs_cur.ias_kt - 56.0) / 10.0, 0.0, 1.0);
        tgt_vsi_fpm = std::lerp(-220.0, tgt_vsi_fpm, fade);
    }

    const double tgt_vsi_mps = tgt_vsi_fpm / 196.85;
    const double vsi_err_mps = tgt_vsi_mps - fs_cur.vsi_mps;
    const double turn_comp = std::min(2.4, std::abs(fs_cur.roll_deg) * 0.065);
    const double tgt_pitch_deg = std::clamp(2.2 + turn_comp + tgt_vsi_mps * 2.1 + vsi_err_mps * 1.45, -13.0, 14.5);
    const double pitch_ff = std::clamp((tgt_pitch_deg - 1.5) * 0.022, -0.20, 0.28);
    ap_elv_out = std::clamp(pitch_ff + ((tgt_pitch_deg - fs_cur.pitch_deg) * 0.088 - fs_cur.q_rps * 0.92) * q_scale, -0.68, 0.68);

    // Electric trim servo relieves sustained AP pitch deflection
    if (ap_elv_out > 0.055) {
        st_.ap_trim_annunc = +1;
        st_.trim = std::clamp(st_.trim + dt * 0.075, -0.55, 0.55);
    } else if (ap_elv_out < -0.055) {
        st_.ap_trim_annunc = -1;
        st_.trim = std::clamp(st_.trim - dt * 0.075, -0.55, 0.55);
    } else {
        st_.ap_trim_annunc = 0;
    }
}

void AircraftAndHud::step_autonomous_assist(double dt, double roll_cmd, double pitch_cmd, double throttle_tgt, double brake_cmd,
                                            const AtmosphereState &atmo, const RegionalAtmosphere &reg_atmo, double sim_time_sec) {
    const nspain::FlightState &fs = fdm_.state();
    if (!taxi_hold_valid_) { taxi_hold_hdg_deg_ = fs.heading_deg; taxi_hold_valid_ = true; }
    double eff_roll = std::clamp(roll_cmd, -1.0, 1.0);
    double eff_pitch = std::clamp(pitch_cmd, -1.0, 1.0);
    double ap_ail = 0.0, ap_elv = 0.0, ap_rud = 0.0;
    apply_autopilot(fs, dt, eff_roll, eff_pitch, ap_ail, ap_elv, ap_rud);
    double ail_tgt = 0.0, elv_tgt = 0.0, rud_tgt = 0.0, brk_l = 0.0, brk_r = 0.0;
    if (st_.ap_master) {
        ail_tgt = ap_ail;
        elv_tgt = ap_elv;
        rud_tgt = ap_rud;
    } else {
        const FlightAssistOut assist = compute_flight_and_taxi_assist(
            eff_roll, eff_pitch, 0.0, false, taxi_hold_hdg_deg_, fs);
        if (fs.on_ground && !assist.holding_heading) taxi_hold_hdg_deg_ = fs.heading_deg;
        ail_tgt = assist.aileron;
        elv_tgt = assist.elevator;
        rud_tgt = assist.rudder;
        brk_l = assist.brake_left_add;
        brk_r = assist.brake_right_add;
        if (!fs.on_ground && std::abs(pitch_cmd) < 0.14)
            st_.trim = std::clamp(st_.trim + assist.elevator * dt * 0.14, -0.55, 0.55);
    }
    st_.aileron  += (ail_tgt - st_.aileron)  * std::min(1.0, dt * 11.0);
    st_.elevator += (elv_tgt - st_.elevator) * std::min(1.0, dt * 11.0);
    st_.rudder   += (rud_tgt - st_.rudder)   * std::min(1.0, dt * 12.0);
    st_.throttle += (std::clamp(throttle_tgt, 0.0, 1.0) - st_.throttle) * std::min(1.0, dt * 3.5);

    advance_aircraft(dt, atmo, reg_atmo, sim_time_sec, st_.rudder,
                     std::clamp(brake_cmd + brk_l, 0.0, 1.0),
                     std::clamp(brake_cmd + brk_r, 0.0, 1.0));
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
        if (win.key_down(KEY_4) || mouse_starter_cranking_) {
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
    // 2-axis study-level cockpit autopilot (operates identically under Direct Yoke or Flight Assist;
    // disconnect occurs on breaker/bus loss, gyro failure, flight envelope exceedances, or stick override >35%).
    if (!st_.slew_fast) {
        double eff_roll = roll_cmd, eff_pitch = pitch_cmd;
        double ap_ail = 0.0, ap_elv = 0.0, ap_rud = 0.0;
        apply_autopilot(fs_cur, dt, eff_roll, eff_pitch, ap_ail, ap_elv, ap_rud);
        if (st_.ap_master) {
            ail_tgt = ap_ail;
            elv_tgt = ap_elv;
            rud_tgt = manual_rud ? man_rud : ap_rud;
        } else if (hw_yoke_active && graphics_settings_.yoke_direct) {
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
                if (std::abs(pitch_cmd) < 0.14 && !st_.paused)
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
        systems_.update_navigation(st_.lat_deg, st_.lon_deg, st_.alt_msl_m);
        st_.ind_alt_ft = st_.alt_msl_m * 3.28084 + (systems_.get_baro_hpa() - last_air_.qnh_hpa) * 27.3;
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
    systems_.update_navigation(fs.lat_deg, fs.lon_deg, fs.alt_msl_m);
    st_.ind_alt_ft = fs.alt_msl_m * 3.28084 + (systems_.get_baro_hpa() - last_air_.qnh_hpa) * 27.3;

    double g_here = pack_->sample_elevation(fs.lat_deg, fs.lon_deg);

    nspain::FlightControls ci{};
    ci.aileron = (broken_wing_l_ && broken_wing_r_) ? 0.0 : (broken_wing_l_ || broken_wing_r_ ? st_.aileron * 0.25 : st_.aileron);
    ci.elevator = broken_tail_ ? 0.0 : st_.elevator;
    ci.rudder = broken_tail_ ? 0.0 : rudder;
    ci.elevator_trim = broken_tail_ ? 0.0 : st_.trim;
    ci.throttle = (broken_prop_ || airframe_destroyed_) ? 0.0 : st_.throttle;
    ci.mixture = st_.mixture;
    ci.flaps = (broken_wing_l_ || broken_wing_r_) ? 0.0 : systems_.get_flaps01();
    const double belly_drag = (airframe_destroyed_ || broken_gear_n_ || broken_gear_l_ || broken_gear_r_) && fs.on_ground ? 0.92 : 0.0;
    ci.brake_left = std::max(brake_left, belly_drag);
    ci.brake_right = std::max(brake_right, belly_drag);
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
    ei.rain_drag_factor = std::clamp(last_air_.precipitation_mm_hr / 40.0 + (broken_wing_l_ || broken_wing_r_ || broken_tail_ ? 0.65 : 0.0), 0.0, 1.0);
    fdm_.set_environment(ei);
    nspain::FlightEngineInputs engine{};
    engine.left_fuel_gal = systems_.get_left_tank_gal();
    engine.right_fuel_gal = systems_.get_right_tank_gal();
    engine.magnetos = (broken_prop_ || airframe_destroyed_) ? 0 : systems_.get_magnetos();
    engine.power_multiplier = (broken_prop_ || airframe_destroyed_) ? 0.0
                              : (systems_.get_engine_power_mult() * (1.0 - damage_ * 0.55) * (1.0 - prop_nicks_ * 0.12));
    engine.starter = !broken_prop_ && !airframe_destroyed_ && systems_.is_starter_cranking();
    engine.fuel_starved = systems_.is_fuel_starved();
    engine.running = !broken_prop_ && !airframe_destroyed_ && systems_.is_engine_running();
    fdm_.set_engine(engine);

    if (!fdm_.advance(dt)) {
        std::fprintf(stderr, "[JSBSIM] Flight executive stopped; pausing flight\n");
        st_.paused = st_.menu_open = true;
        return;
    }
    const nspain::FlightState &ns = fdm_.state();
    // Missing wing / missing tail aerodynamic collapse: impossible to fly straight with a sheared wing or tail!
    if (!ns.on_ground && (broken_wing_l_ || broken_wing_r_ || broken_tail_)) {
        const double asym_roll = (broken_wing_l_ ? -3.4 : 0.0) + (broken_wing_r_ ? +3.4 : 0.0);
        const double tail_pitch = broken_tail_ ? -1.4 : 0.0;
        const double sink_add = (broken_wing_l_ || broken_wing_r_) ? -6.5 : -2.5;
        fdm_.apply_impact_impulse(
            0.0, 0.0, 0.0,
            ns.vel_east_mps, ns.vsi_mps + sink_add * dt, ns.vel_north_mps,
            std::clamp(ns.p_rps + asym_roll * dt, -4.5, 4.5),
            std::clamp(ns.q_rps + tail_pitch * dt, -3.5, 3.5),
            std::clamp(ns.r_rps - asym_roll * 0.45 * dt, -3.5, 3.5)
        );
    }
    // Integrate ballistic tumbling detached debris parts
    const float fdt_d = static_cast<float>(std::min(dt, 0.05));
    for (auto &bp : broken_parts_) {
        if (!bp.detached) continue;
        bp.vel.y -= 9.81f * fdt_d;
        bp.pos_off = bp.pos_off + bp.vel * fdt_d;
        bp.rot = bp.rot + bp.ang_vel * fdt_d;
        const float min_y = static_cast<float>(st_.ground_m - ns.alt_msl_m) + 0.15f;
        if (bp.pos_off.y < min_y) {
            bp.pos_off.y = min_y;
            bp.vel.y = std::abs(bp.vel.y) * 0.25f;
            bp.vel.x *= 0.72f;
            bp.vel.z *= 0.72f;
            bp.ang_vel = bp.ang_vel * 0.65f;
        }
    }
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

void AircraftAndHud::detach_structural_part(const char *node_name, Vec3 local_vel, Vec3 ang_vel) {
    for (const auto &bp : broken_parts_) {
        if (bp.node == node_name && bp.detached) return;
    }
    BrokenPart bp{};
    bp.node = node_name;
    bp.vel = local_vel;
    bp.ang_vel = ang_vel;
    bp.detached = true;
    broken_parts_.push_back(std::move(bp));
}

void AircraftAndHud::trigger_catastrophic_breakup(const char *reason, double impact_spd) {
    const float s = static_cast<float>(std::clamp(impact_spd / 22.0, 0.5, 2.5));
    airframe_destroyed_ = true;
    damage_ = 1.0;
    dmg_nose_ = 1.0;
    prop_nicks_ = 1.0;
    windshield_crack_ = 1.0;
    st_.ap_master = false;
    if (!broken_prop_) {
        broken_prop_ = true;
        detach_structural_part("PropBlades", {2.5f * s, 4.2f * s, -3.5f * s}, {9.0f, 14.0f, 11.0f});
    }
    if (!broken_gear_n_) {
        broken_gear_n_ = true;
        detach_structural_part("NoseGear", {-1.8f * s, 2.2f * s, 3.8f * s}, {6.0f, 3.0f, 8.0f});
    }
    if (!broken_gear_l_) broken_gear_l_ = true;
    if (!broken_gear_r_) broken_gear_r_ = true;
    if (!broken_wing_l_) {
        broken_wing_l_ = true;
        dmg_wing_l_ = 1.0;
        detach_structural_part("Wing_L", {-6.5f * s, 4.8f * s, 5.2f * s}, {3.2f, 2.4f, 4.8f});
    }
    if (impact_spd > 28.0 && !broken_tail_) {
        broken_tail_ = true;
        dmg_tail_ = 1.0;
        detach_structural_part("Tail", {2.0f * s, 5.5f * s, 7.5f * s}, {-4.2f, 3.5f, 2.6f});
    }
    systems_.set_magnetos(0);
    systems_.set_avionics_master(false);
    systems_.set_engine_running_state(false, last_air_.oat_c);
    impact_fx_s_ = 3.5f;
    collision_msg_ = reason;
    collision_msg_s_ = 12.0;
}

void AircraftAndHud::resolve_world_collisions(double dt) {
    if (impact_fx_s_ > 0.0f) impact_fx_s_ = std::max(0.0f, impact_fx_s_ - static_cast<float>(dt));
    if (impact_cooldown_s_ > 0.0) impact_cooldown_s_ = std::max(0.0, impact_cooldown_s_ - dt);
    if (collision_msg_s_ > 0.0) collision_msg_s_ = std::max(0.0, collision_msg_s_ - dt);
    if (st_.slew_fast) return;

    const nspain::FlightState &fs = fdm_.state();
    const double spd_total = std::hypot(std::hypot(fs.vel_east_mps, fs.vel_north_mps), fs.vsi_mps);

    // 0. Airborne structural overload (VNE overspeed > 178 kt or > +5.4G / < -2.8G snaps wing clean off!)
    if (!fs.on_ground) {
        if ((fs.ias_kt > 178.0 || fs.g_load > 5.4 || fs.g_load < -2.8) && (!broken_wing_l_ || !broken_wing_r_)) {
            const bool snap_left = !broken_wing_l_ && (fs.roll_deg >= 0.0 || broken_wing_r_);
            if (snap_left) {
                broken_wing_l_ = true;
                dmg_wing_l_ = 1.0;
                detach_structural_part("Wing_L", {-7.5f, 5.5f, 9.0f}, {3.5f, 2.2f, 6.0f});
                fdm_.apply_impact_impulse(0, 0, 0, fs.vel_east_mps, fs.vsi_mps - 2.0, fs.vel_north_mps, fs.p_rps - 2.6, fs.q_rps, fs.r_rps + 1.2);
            } else {
                broken_wing_r_ = true;
                dmg_wing_r_ = 1.0;
                detach_structural_part("Wing_R", {+7.5f, 5.5f, 9.0f}, {3.5f, -2.2f, -6.0f});
                fdm_.apply_impact_impulse(0, 0, 0, fs.vel_east_mps, fs.vsi_mps - 2.0, fs.vel_north_mps, fs.p_rps + 2.6, fs.q_rps, fs.r_rps - 1.2);
            }
            damage_ = std::min(1.0, damage_ + 0.65);
            st_.ap_master = false;
            impact_fx_s_ = 2.0f;
            collision_msg_ = (fs.ias_kt > 178.0) ? "OVERSPEED BREAKUP: WING SHEARED OFF!" : "OVER-G STRUCTURAL FAILURE: WING SHEARED OFF!";
            collision_msg_s_ = 8.0;
        }
        if (systems_.get_flaps_actual_deg() > 15.0 && fs.ias_kt > 122.0 && impact_cooldown_s_ <= 0.0) {
            systems_.set_flaps_target_deg(0.0);
            dmg_wing_l_ = std::min(1.0, dmg_wing_l_ + 0.35);
            dmg_wing_r_ = std::min(1.0, dmg_wing_r_ + 0.35);
            damage_ = std::min(1.0, damage_ + 0.25);
            impact_cooldown_s_ = 1.0;
            collision_msg_ = "FLAP OVERSPEED (>122 KT): FLAP ACTUATOR DAMAGED!";
            collision_msg_s_ = 5.5;
        }
    }

    // 1. Ground impact, gear collapse, prop strike, wingtip/tail strike & catastrophic crash breakup
    if (!prev_ground_coll_ && fs.on_ground) {
        const bool bad_attitude = (std::abs(fs.roll_deg) > 58.0 || std::abs(fs.pitch_deg) > 36.0);
        if (prev_vsi_coll_ < -8.0 || (bad_attitude && spd_total > 12.0)) {
            trigger_catastrophic_breakup("CRASH IMPACT: AIRFRAME DESTROYED! (Esc -> Park/Reset)", spd_total);
            // Kill rubber-ball rebound: slide/crumple onto ground
            fdm_.apply_impact_impulse(0, 0, 0, fs.vel_east_mps * 0.25, 0.0, fs.vel_north_mps * 0.25,
                                      fs.p_rps * 0.2, fs.q_rps * 0.2, fs.r_rps * 0.4);
        } else if (prev_vsi_coll_ < -4.2) {
            // Severe hard landing: collapses nose or main gear + prop strike kills engine!
            const double sev = std::clamp((-prev_vsi_coll_ - 4.0) / 5.0, 0.25, 1.0);
            damage_ = std::min(1.0, damage_ + sev * 0.65);
            gear_alignment_ = std::clamp(gear_alignment_ + (fs.roll_deg >= 0.0 ? 1.0 : -1.0) * sev * 0.85, -1.0, 1.0);
            tire_wear_ = 1.0;
            if (fs.pitch_deg < -0.5) {
                broken_gear_n_ = true;
                broken_prop_ = true;
                dmg_nose_ = std::min(1.0, dmg_nose_ + 0.85);
                prop_nicks_ = 1.0;
                systems_.set_magnetos(0);
                systems_.set_engine_running_state(false, last_air_.oat_c);
                collision_msg_ = "HARD NOSE IMPACT: NOSE GEAR COLLAPSED & PROP STRIKE!";
            } else if (fs.roll_deg < -6.0) {
                broken_gear_l_ = true;
                dmg_wing_l_ = std::min(1.0, dmg_wing_l_ + 0.65);
                collision_msg_ = "HARD LANDING: LEFT MAIN GEAR COLLAPSED!";
            } else if (fs.roll_deg > 6.0) {
                broken_gear_r_ = true;
                dmg_wing_r_ = std::min(1.0, dmg_wing_r_ + 0.65);
                collision_msg_ = "HARD LANDING: RIGHT MAIN GEAR COLLAPSED!";
            } else {
                broken_gear_l_ = broken_gear_r_ = true;
                collision_msg_ = "HARD LANDING: MAIN LANDING GEAR COLLAPSED!";
            }
            if (sev > 0.45) windshield_crack_ = std::min(1.0, windshield_crack_ + sev * 0.85);
            impact_fx_s_ = std::max(impact_fx_s_, 1.8f);
            cockpit_head_vel_.y -= static_cast<float>(sev * 1.8);
            collision_msg_s_ = 7.5;
        } else if (prev_vsi_coll_ < -3.0) {
            const double sev = std::clamp((-prev_vsi_coll_ - 3.0) / 4.0, 0.08, 0.40);
            damage_ = std::min(1.0, damage_ + sev * 0.35);
            tire_wear_ = std::min(1.0, tire_wear_ + sev * 0.40);
            cockpit_head_vel_.y -= static_cast<float>(sev * 1.1);
            collision_msg_ = "HARD LANDING: CHECK LANDING GEAR";
            collision_msg_s_ = 4.5;
        }
    }
    // Continuous ground-roll wingtip / nose-prop / tail strikes
    if (fs.on_ground && spd_total > 12.0 && impact_cooldown_s_ <= 0.0) {
        if (fs.roll_deg < -18.0 && !broken_wing_l_) {
            dmg_wing_l_ = std::min(1.0, dmg_wing_l_ + 0.55);
            damage_ = std::min(1.0, damage_ + 0.45);
            if (spd_total > 24.0 || fs.roll_deg < -32.0) {
                broken_wing_l_ = true;
                detach_structural_part("Wing_L", {-4.5f, 3.0f, 4.5f}, {2.5f, 3.0f, 4.0f});
                collision_msg_ = "WINGTIP GROUND STRIKE: LEFT WING BROKEN OFF!";
            } else {
                collision_msg_ = "LEFT WINGTIP GROUND STRIKE!";
            }
            collision_msg_s_ = 6.0;
            impact_fx_s_ = 1.5f;
            impact_cooldown_s_ = 0.5;
        } else if (fs.roll_deg > 18.0 && !broken_wing_r_) {
            dmg_wing_r_ = std::min(1.0, dmg_wing_r_ + 0.55);
            damage_ = std::min(1.0, damage_ + 0.45);
            if (spd_total > 24.0 || fs.roll_deg > 32.0) {
                broken_wing_r_ = true;
                detach_structural_part("Wing_R", {+4.5f, 3.0f, 4.5f}, {2.5f, -3.0f, -4.0f});
                collision_msg_ = "WINGTIP GROUND STRIKE: RIGHT WING BROKEN OFF!";
            } else {
                collision_msg_ = "RIGHT WINGTIP GROUND STRIKE!";
            }
            collision_msg_s_ = 6.0;
            impact_fx_s_ = 1.5f;
            impact_cooldown_s_ = 0.5;
        } else if (fs.pitch_deg < -10.0 && !broken_prop_) {
            broken_prop_ = true;
            broken_gear_n_ = true;
            prop_nicks_ = 1.0;
            dmg_nose_ = 1.0;
            systems_.set_magnetos(0);
            systems_.set_engine_running_state(false, last_air_.oat_c);
            detach_structural_part("PropBlades", {2.2f, 3.0f, -2.5f}, {10.0f, 12.0f, 8.0f});
            collision_msg_ = "PROP STRIKE ON GROUND: ENGINE SEIZED!";
            collision_msg_s_ = 7.0;
            impact_fx_s_ = 1.8f;
            impact_cooldown_s_ = 0.5;
        } else if (fs.pitch_deg > 17.0 && !broken_tail_) {
            dmg_tail_ = std::min(1.0, dmg_tail_ + 0.70);
            if (spd_total > 26.0) {
                broken_tail_ = true;
                detach_structural_part("Tail", {1.0f, 2.5f, 5.5f}, {-3.5f, 2.0f, 2.0f});
                collision_msg_ = "TAILSTRIKE: EMPENNAGE BROKEN OFF!";
            } else {
                collision_msg_ = "TAILSTRIKE: TAIL STRUCTURE DAMAGED!";
            }
            collision_msg_s_ = 6.0;
            impact_cooldown_s_ = 0.5;
        }
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

        damage_ = std::min(1.0, damage_ + sev * 0.85);
        if (best_zone == 0) {
            dmg_nose_ = std::min(1.0, dmg_nose_ + sev * 1.15);
            prop_nicks_ = 1.0;
            windshield_crack_ = std::min(1.0, windshield_crack_ + sev * 0.95);
            if (imp > 20.0) {
                trigger_catastrophic_breakup("HEAD-ON COLLISION: AIRFRAME DESTROYED!", imp);
            } else {
                broken_prop_ = true;
                broken_gear_n_ = true;
                detach_structural_part("PropBlades", {2.0f, 3.5f, -2.5f}, {9.0f, 11.0f, 8.0f});
                systems_.set_magnetos(0);
                systems_.set_engine_running_state(false, last_air_.oat_c);
            }
        } else if (best_zone == 1) {
            dmg_wing_l_ = std::min(1.0, dmg_wing_l_ + sev * 1.25);
            if (imp > 16.0 && !broken_wing_l_) {
                broken_wing_l_ = true;
                detach_structural_part("Wing_L", {-6.0f, 4.5f, 6.0f}, {3.5f, 2.5f, 5.0f});
            }
        } else if (best_zone == 2) {
            dmg_wing_r_ = std::min(1.0, dmg_wing_r_ + sev * 1.25);
            if (imp > 16.0 && !broken_wing_r_) {
                broken_wing_r_ = true;
                detach_structural_part("Wing_R", {+6.0f, 4.5f, 6.0f}, {3.5f, -2.5f, -5.0f});
            }
        } else if (best_zone == 3) {
            dmg_tail_ = std::min(1.0, dmg_tail_ + sev * 1.25);
            if (imp > 18.0 && !broken_tail_) {
                broken_tail_ = true;
                detach_structural_part("Tail", {1.5f, 4.2f, 6.5f}, {-4.0f, 2.5f, 2.2f});
            }
        } else {
            if (imp > 22.0) trigger_catastrophic_breakup("STRUCTURAL COLLISION: AIRFRAME DESTROYED!", imp);
            dmg_nose_ = std::min(1.0, dmg_nose_ + sev * 0.55);
            gear_alignment_ = std::clamp(gear_alignment_ + (fs.roll_deg >= 0.0 ? 1.0 : -1.0) * sev * 0.85, -1.0, 1.0);
            tire_wear_ = std::min(1.0, tire_wear_ + sev * 0.75);
            windshield_crack_ = std::min(1.0, windshield_crack_ + sev * 0.90);
        }

        const double bounce = airframe_destroyed_ ? 0.4 : ((1.0 + 0.18) * std::max(0.0, v_into) + 1.2);
        const double keep_v = airframe_destroyed_ ? 0.18 : 0.40;
        const double nve = std::clamp(ve * keep_v + best.normal_e * bounce, -45.0, 45.0);
        const double nvu = std::clamp(vu * keep_v + best.normal_up * bounce, -25.0, 14.0);
        const double nvn = std::clamp(vn * keep_v + best.normal_n * bounce, -45.0, 45.0);
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

namespace {
// Compact 5x7 ASCII glyph rows (MSB=left) for crisp live 3D cockpit AvionicsStack LCD updates.
struct Glyph5x7 { char ch; uint8_t r[7]; };
static constexpr Glyph5x7 kAvnFont[] = {
    {' ',{0,0,0,0,0,0,0}}, {'.',{0,0,0,0,0,12,12}}, {':',{0,12,12,0,12,12,0}},
    {'-',{0,0,0,30,0,0,0}}, {'+',{0,4,4,31,4,4,0}}, {'/',{1,2,4,8,16,0,0}},
    {'%',{24,25,2,4,8,19,3}}, {'_',{0,0,0,0,0,0,31}}, {'=',{0,0,31,0,31,0,0}},
    {'>',{16,8,4,2,4,8,16}}, {'|',{4,4,4,4,4,4,4}},
    {'0',{14,17,19,21,25,17,14}}, {'1',{4,12,4,4,4,4,14}}, {'2',{14,17,1,6,8,16,31}},
    {'3',{30,1,1,14,1,1,30}}, {'4',{2,6,10,18,31,2,2}}, {'5',{31,16,30,1,1,17,14}},
    {'6',{14,16,30,17,17,17,14}}, {'7',{31,1,2,4,8,8,8}}, {'8',{14,17,17,14,17,17,14}},
    {'9',{14,17,17,15,1,1,14}},
    {'A',{14,17,17,31,17,17,17}}, {'B',{30,17,17,30,17,17,30}}, {'C',{14,17,16,16,16,17,14}},
    {'D',{28,18,17,17,17,18,28}}, {'E',{31,16,16,30,16,16,31}}, {'F',{31,16,16,30,16,16,16}},
    {'G',{14,17,16,23,17,17,15}}, {'H',{17,17,17,31,17,17,17}}, {'I',{14,4,4,4,4,4,14}},
    {'J',{7,2,2,2,18,18,12}}, {'K',{17,18,20,24,20,18,17}}, {'L',{16,16,16,16,16,16,31}},
    {'M',{17,27,21,21,17,17,17}}, {'N',{17,25,21,19,17,17,17}}, {'O',{14,17,17,17,17,17,14}},
    {'P',{30,17,17,30,16,16,16}}, {'Q',{14,17,17,17,21,18,13}}, {'R',{30,17,17,30,20,18,17}},
    {'S',{15,16,16,14,1,1,30}}, {'T',{31,4,4,4,4,4,4}}, {'U',{17,17,17,17,17,17,14}},
    {'V',{17,17,17,17,17,10,4}}, {'W',{17,17,17,21,21,27,17}}, {'X',{17,17,10,4,10,17,17}},
    {'Y',{17,17,10,4,4,4,4}}, {'Z',{31,1,2,4,8,16,31}},
};

inline void avn_rect(std::vector<uint8_t> &buf, int W, int H, int x, int y, int w, int h,
                     uint8_t r, uint8_t g, uint8_t b) {
    const int x0 = std::max(0, x), y0 = std::max(0, y);
    const int x1 = std::min(W, x + w), y1 = std::min(H, y + h);
    for (int py = y0; py < y1; ++py) {
        uint8_t *row = buf.data() + static_cast<size_t>(py * W + x0) * 4u;
        for (int px = x0; px < x1; ++px, row += 4) {
            row[0] = r; row[1] = g; row[2] = b; row[3] = 255;
        }
    }
}

void avn_text(std::vector<uint8_t> &buf, int W, int H, int cx, int cy, int scale,
              const char *txt, uint8_t r, uint8_t g, uint8_t b, bool centered = true) {
    if (!txt || !*txt || scale < 1) return;
    const int len = static_cast<int>(std::strlen(txt));
    const int adv = 6 * scale;
    const int x0 = centered ? (cx - (len * adv - scale) / 2) : cx;
    const int y0 = centered ? (cy - (7 * scale) / 2) : cy;
    for (int ci = 0; ci < len; ++ci) {
        char c = txt[ci];
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        const Glyph5x7 *gl = nullptr;
        for (const auto &g_it : kAvnFont) {
            if (g_it.ch == c) { gl = &g_it; break; }
        }
        if (!gl) continue;
        for (int ry = 0; ry < 7; ++ry) {
            for (int rx = 0; rx < 5; ++rx) {
                if (gl->r[ry] & (16 >> rx))
                    avn_rect(buf, W, H, x0 + ci * adv + rx * scale, y0 + ry * scale, scale, scale, r, g, b);
            }
        }
    }
}
} // namespace

void AircraftAndHud::init_avionics_display_texture() {
    avionics_tex_ = 0;
    for (const auto &sm : ckpt_mesh_.submeshes) {
        if (sm.material_name == "AvionicsStack" && sm.albedo_tex != 0) {
            avionics_tex_ = sm.albedo_tex;
            break;
        }
    }
    if (!avionics_tex_) return;
    glGetTextureLevelParameteriv(avionics_tex_, 0, GL_TEXTURE_WIDTH, &avionics_tex_w_);
    glGetTextureLevelParameteriv(avionics_tex_, 0, GL_TEXTURE_HEIGHT, &avionics_tex_h_);
    if (avionics_tex_w_ != 1024 || avionics_tex_h_ != 1536) {
        avionics_tex_ = 0;
        return;
    }
    avionics_base_rgba_.resize(1024u * 1536u * 4u);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glGetTextureImage(avionics_tex_, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                      static_cast<GLsizei>(avionics_base_rgba_.size()), avionics_base_rgba_.data());
    avionics_work_rgba_ = avionics_base_rgba_;
    avionics_last_key_.clear();
    avionics_last_upd_s_ = -1.0f;
}

void AircraftAndHud::update_avionics_display_texture(float sim_time_sec) {
    if (!avionics_tex_ || avionics_base_rgba_.size() != 1024u * 1536u * 4u) return;
    if (avionics_last_upd_s_ >= 0.0f && (sim_time_sec - avionics_last_upd_s_) < 0.20f) return;

    const bool avn1 = systems_.is_avionics_bus_powered(1);
    const bool avn2 = systems_.is_avionics_bus_powered(2);
    const bool ap_ok = avn1 && systems_.is_circuit_powered(nspain::CB_TURN_COORD);
    const bool volts_w = systems_.get_low_volts_annunciator();
    const bool oil_w = !systems_.is_engine_running() || systems_.get_oil_press_psi() < 25.0 || systems_.get_oil_press_psi() > 115.0;
    const bool fuel_w = systems_.is_fuel_starved() || systems_.get_fuel_selector() == 0 ||
                        systems_.get_left_tank_gal() < 4.0 || systems_.get_right_tank_gal() < 4.0;
    const bool vac_w = systems_.get_vacuum_inhg() < 3.5;
    const bool disc_blink = st_.ap_disconnect_warn_s > 0.0f && std::fmod(sim_time_sec, 0.5f) < 0.25f;
    const int hdg_disp = ((int(std::round(st_.heading_deg)) % 360) + 360) % 360;
    const int ap_hdg = ((int(std::round(st_.ap_hdg_deg)) % 360) + 360) % 360;
    const int ap_alt = std::clamp(int(std::round(st_.ap_alt_ft / 100.0)) * 100, 0, 25000);
    const int ap_vs = std::clamp(int(std::round(st_.ap_vs_fpm / 100.0)) * 100, -1500, 1200);
    const int ind_fl = std::clamp(int(std::round(st_.ind_alt_ft / 100.0)), 0, 300);
    const int baro_i = int(std::round(systems_.get_baro_hpa()));
    const int cdi_q = int(std::round(systems_.get_nav_cdi(1) * 20.0));
    const int dme_q = int(std::round(systems_.get_nav_dme_nm(1) * 10.0));

    char key[256];
    std::snprintf(key, sizeof(key),
                  "%d:%d:%d:%d%d%d%d:%d:%d%d%d%d%d:%d:%03d:%05d:%+05d:%d:%d:%05d:%05d:%05d:%05d:%03d:%s:%d:%d:%05d:%05d:%05d:%05d:%d:%04d:%d:%03d:%03d",
                  avn1 ? 1 : 0, avn2 ? 1 : 0, ap_ok ? 1 : 0,
                  volts_w ? 1 : 0, oil_w ? 1 : 0, fuel_w ? 1 : 0, vac_w ? 1 : 0,
                  disc_blink ? 1 : 0,
                  st_.ap_master ? 1 : 0, st_.ap_hdg_hold ? 1 : 0, st_.ap_nav_hold ? 1 : 0,
                  st_.ap_vs_hold ? 1 : 0, st_.ap_alt_hold ? 1 : 0,
                  (st_.ap_alt_arm ? 10 : 0) + (st_.ap_nav_source * 3) + (st_.ap_trim_annunc + 1),
                  ap_hdg, ap_alt, ap_vs, baro_i, st_.ap_nav_valid ? 1 : 0,
                  int(std::round(systems_.get_com_active(1) * 1000.0)),
                  int(std::round(systems_.get_com_standby(1) * 1000.0)),
                  int(std::round(systems_.get_nav_active(1) * 1000.0)),
                  int(std::round(systems_.get_nav_standby(1) * 1000.0)),
                  systems_.get_nav_obs_deg(1), systems_.get_nav_ident(1).c_str(), cdi_q, dme_q,
                  int(std::round(systems_.get_com_active(2) * 1000.0)),
                  int(std::round(systems_.get_com_standby(2) * 1000.0)),
                  int(std::round(systems_.get_nav_active(2) * 1000.0)),
                  int(std::round(systems_.get_nav_standby(2) * 1000.0)),
                  systems_.get_xpdr_mode(), systems_.get_xpdr_squawk(),
                  systems_.is_xpdr_ident_active() ? 1 : 0, hdg_disp, ind_fl);
    if (key == avionics_last_key_) return;
    avionics_last_key_ = key;
    avionics_last_upd_s_ = sim_time_sec;

    std::memcpy(avionics_work_rgba_.data(), avionics_base_rgba_.data(), avionics_base_rgba_.size());
    constexpr int W = 1024, H = 1536;
    char txt[64];

    // 1. Annunciator bar (only lit when battery bus >= 19.5V)
    const bool bus_ok = systems_.get_master_battery() && systems_.get_bus_volts() >= 19.5;
    const struct { const char *lbl; bool warn; bool amber; bool ok_green; } ann[5] = {
        {"VOLTS",  bus_ok && volts_w, false, false},
        {"OIL P",  bus_ok && oil_w,   false, false},
        {"FUEL",   bus_ok && fuel_w,  true,  false},
        {"VAC",    bus_ok && vac_w,   true,  false},
        {st_.ap_disconnect_warn_s > 0.0f ? "AP DISC" : (st_.ap_master ? "AP ON" : "VFR OK"),
         disc_blink, false, !st_.ap_disconnect_warn_s && (st_.ap_master || avn1)},
    };
    for (int i = 0; i < 5; ++i) {
        const int x0 = 40 + i * 192;
        avn_rect(avionics_work_rgba_, W, H, x0, 34, 168, 60, 10, 12, 16);
        uint8_t r = 64, g = 68, b = 74;
        if (bus_ok) {
            if (ann[i].warn) { r = ann[i].amber ? 250 : 250; g = ann[i].amber ? 178 : 68; b = ann[i].amber ? 48 : 68; }
            else if (ann[i].ok_green) { r = 68; g = 242; b = 128; }
        }
        avn_text(avionics_work_rgba_, W, H, x0 + 84, 64, 3, ann[i].lbl, r, g, b);
    }

    // 2. Audio Panel LEDs
    for (int i = 0; i < 6; ++i) {
        const int bx = 264 + i * 120;
        const bool lit = (i == 0 && avn1) || (i == 1 && avn2) || (i == 2 && avn1) || (i == 5 && avn1);
        avn_rect(avionics_work_rgba_, W, H, bx + 16, 162, 68, 16,
                 lit ? 56 : 18, lit ? 242 : 22, lit ? 122 : 26);
    }

    // 3. NAV / GPS 1 Screen (356..748)
    avn_rect(avionics_work_rgba_, W, H, 62, 364, 244, 376, 8, 18, 28);
    if (avn1) {
        avn_text(avionics_work_rgba_, W, H, 184, 390, 3, "COM 1", 122, 206, 252);
        std::snprintf(txt, sizeof(txt), "%.2f", systems_.get_com_active(1));
        avn_text(avionics_work_rgba_, W, H, 184, 444, 5, txt, 80, 250, 150);
        std::snprintf(txt, sizeof(txt), "%.2f", systems_.get_com_standby(1));
        avn_text(avionics_work_rgba_, W, H, 184, 498, 4, txt, 184, 216, 236);
        avn_rect(avionics_work_rgba_, W, H, 68, 538, 232, 3, 52, 86, 116);
        avn_text(avionics_work_rgba_, W, H, 184, 574, 3, st_.ap_nav_source ? "VOR1 LOC" : "GPS NAV", 122, 206, 252);
        std::snprintf(txt, sizeof(txt), "%.2f", systems_.get_nav_active(1));
        avn_text(avionics_work_rgba_, W, H, 184, 626, 5, txt, 80, 250, 150);
        const std::string id1 = systems_.get_nav_ident(1);
        if (id1 != "---") {
            std::snprintf(txt, sizeof(txt), "%s %dN", id1.c_str(), int(std::round(systems_.get_nav_dme_nm(1))));
            avn_text(avionics_work_rgba_, W, H, 184, 678, 3, txt, 252, 212, 82);
        } else {
            std::snprintf(txt, sizeof(txt), "%.2f", systems_.get_nav_standby(1));
            avn_text(avionics_work_rgba_, W, H, 184, 678, 3, txt, 170, 200, 224);
        }
        std::snprintf(txt, sizeof(txt), "OBS %03d", systems_.get_nav_obs_deg(1));
        avn_text(avionics_work_rgba_, W, H, 184, 718, 3, txt, 190, 224, 240);

        // Overlay live heading, mode & CDI needle on Moving Map (328..812, 362..744)
        avn_rect(avionics_work_rgba_, W, H, 336, 366, 240, 44, 14, 32, 24);
        std::snprintf(txt, sizeof(txt), "HDG %03d", hdg_disp);
        avn_text(avionics_work_rgba_, W, H, 344, 376, 4, txt, 96, 242, 250, false);
        avn_rect(avionics_work_rgba_, W, H, 344, 694, 456, 48, 12, 26, 20);
        // Live lateral CDI scale & needle
        avn_rect(avionics_work_rgba_, W, H, 420, 680, 304, 4, 110, 140, 120);
        avn_rect(avionics_work_rgba_, W, H, 570, 672, 4, 20, 220, 240, 230);
        const double cdi = st_.ap_nav_source
            ? systems_.get_nav_cdi(1)
            : std::clamp(std::remainder(st_.ap_nav_brg_deg - st_.heading_deg, 360.0) / 30.0, -1.0, 1.0);
        const int nx = 572 + int(std::round(cdi * 140.0));
        avn_rect(avionics_work_rgba_, W, H, nx - 4, 668, 8, 28, 250, 88, 238);
        std::snprintf(txt, sizeof(txt), "%s  IND %05dFT", st_.ap_nav_source ? "VLOC" : "GPS", int(std::round(st_.ind_alt_ft)));
        avn_text(avionics_work_rgba_, W, H, 572, 720, 3, txt, 92, 238, 252);
    } else {
        avn_rect(avionics_work_rgba_, W, H, 328, 362, 484, 382, 8, 14, 18);
    }

    // 4. COM 2 / NAV 2 Screen (148..876, 880..996)
    avn_rect(avionics_work_rgba_, W, H, 148, 880, 728, 116, 20, 10, 8);
    if (avn2) {
        std::snprintf(txt, sizeof(txt), "%.2f %.2f", systems_.get_com_active(2), systems_.get_com_standby(2));
        avn_text(avionics_work_rgba_, W, H, 324, 938, 4, txt, 252, 118, 48);
        std::snprintf(txt, sizeof(txt), "%.2f %.2f", systems_.get_nav_active(2), systems_.get_nav_standby(2));
        avn_text(avionics_work_rgba_, W, H, 700, 938, 4, txt, 252, 118, 48);
    }

    // 5. 2-Axis Autopilot Screen (156..868, 1114..1228)
    avn_rect(avionics_work_rgba_, W, H, 156, 1114, 712, 114, 12, 14, 10);
    if (ap_ok) {
        if (disc_blink) {
            avn_text(avionics_work_rgba_, W, H, 238, 1140, 4, "DISC", 252, 72, 72);
        } else {
            avn_text(avionics_work_rgba_, W, H, 238, 1140, 4, st_.ap_master ? "AP ON" : "STBY",
                     st_.ap_master ? 82 : 165, st_.ap_master ? 248 : 170, st_.ap_master ? 140 : 148);
        }
        const char *lat_s = st_.ap_nav_hold ? (st_.ap_nav_source ? "NAV1" : "GPS") : (st_.ap_hdg_hold ? "HDG" : "ROL");
        avn_text(avionics_work_rgba_, W, H, 420, 1140, 4, lat_s, 252, 206, 74);
        if (st_.ap_alt_arm) avn_text(avionics_work_rgba_, W, H, 578, 1140, 4, "ARM", 84, 242, 148);
        const char *ver_s = st_.ap_alt_hold ? "ALT" : (st_.ap_vs_hold ? "VS" : "PIT");
        avn_text(avionics_work_rgba_, W, H, 720, 1140, 4, ver_s, 252, 206, 74);
        if (st_.ap_trim_annunc != 0) {
            avn_text(avionics_work_rgba_, W, H, 828, 1140, 3, st_.ap_trim_annunc > 0 ? "TRM+" : "TRM-", 252, 144, 52);
        }

        if (st_.ap_nav_hold && st_.ap_nav_source)
            std::snprintf(txt, sizeof(txt), "CRS %03d", systems_.get_nav_obs_deg(1));
        else
            std::snprintf(txt, sizeof(txt), "HDG %03d", ap_hdg);
        avn_text(avionics_work_rgba_, W, H, 286, 1198, 4, txt, 252, 200, 68);

        if (st_.ap_vs_hold)
            std::snprintf(txt, sizeof(txt), "VS %+05d  B%4d", ap_vs, baro_i);
        else
            std::snprintf(txt, sizeof(txt), "ALT %05d B%4d", ap_alt, baro_i);
        avn_text(avionics_work_rgba_, W, H, 650, 1198, 4, txt, 252, 200, 68);
    }

    // 6. Mode C Transponder Screen (156..696, 1336..1416)
    avn_rect(avionics_work_rgba_, W, H, 156, 1336, 540, 80, 18, 14, 10);
    if (avn1) {
        static constexpr const char *kXpdrModes[4] = {"OFF", "STBY", "ON", "ALT"};
        const int xm = std::clamp(systems_.get_xpdr_mode(), 0, 3);
        avn_text(avionics_work_rgba_, W, H, 232, 1362, 4, kXpdrModes[xm], 252, 208, 68);
        if (systems_.is_xpdr_ident_active()) {
            avn_text(avionics_work_rgba_, W, H, 232, 1396, 3, "IDENT", 92, 242, 250);
        } else if (xm == 3) {
            std::snprintf(txt, sizeof(txt), "FL%03d", ind_fl);
            avn_text(avionics_work_rgba_, W, H, 232, 1396, 3, txt, 210, 190, 110);
        }
        std::snprintf(txt, sizeof(txt), "%04d", systems_.get_xpdr_squawk());
        avn_text(avionics_work_rgba_, W, H, 504, 1376, 7, txt, 255, 224, 78);
    }

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTextureSubImage2D(avionics_tex_, 0, 0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, avionics_work_rgba_.data());
    glGenerateTextureMipmap(avionics_tex_);
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
    if (is_interior) {
        last_ckpt_vp_ = vp;
        last_ckpt_root_ = root_world;
        last_ckpt_world_m_ = world_m;
        last_ckpt_valid_ = true;
    }

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
                const bool batt_on = systems_.get_master_battery() && systems_.get_bus_volts() >= 19.5;
                const bool avn1_on = systems_.is_avionics_bus_powered(1);
                const bool avn2_on = systems_.is_avionics_bus_powered(2);
                const bool inst_lit = systems_.is_light_powered(4);
                const float crank_dip = systems_.is_starter_cranking() ? 0.56f : 1.0f;
                if (sm.material_name == "AvionicsStack") {
                    if (!avn1_on && !avn2_on) {
                        tint.x *= 0.14f; tint.y *= 0.14f; tint.z *= 0.16f;
                        emul = 0.0f;
                    } else {
                        emul *= crank_dip;
                    }
                } else if (sm.material_name == "SteamDials" || sm.material_name == "GaugeNeedle" ||
                           sm.material_name == "RotatingCards") {
                    emul *= (batt_on && inst_lit) ? crank_dip : 0.0f;
                }
            }
            glUniform4f(u_ac_tint_, tint.x, tint.y, tint.z, tint.w);
            glUniform4f(u_ac_pbr_, sm.metallic, sm.roughness, sm.normal_tex ? 1.0f : 0.0f, sm.mr_tex ? 1.0f : 0.0f);
            glUniform3f(u_ac_em_, sm.emissive_col.x * emul, sm.emissive_col.y * emul, sm.emissive_col.z * emul);
            glUniform1f(u_ac_alpha_, amul);
            glUniform1f(u_ac_livery_, (!is_interior && (sm.material_name == "C172_Airframe_CCBY" || ni == 0)) ? 1.0f : 0.0f);
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

    // Real structural node deformation, gear collapse, wing G-flex & detached ballistic tumbling debris!
    auto find_broken = [&](const char *name) -> const BrokenPart * {
        for (const auto &bp : broken_parts_) {
            if (bp.detached && bp.node == name) return &bp;
        }
        return nullptr;
    };
    const float g_flex = static_cast<float>(std::clamp((fdm_.state().g_load - 1.0) * 0.018, -0.04, 0.09));

    const float wob = (pn > 0.06f && prop_angle_rad_ > 0.0f && !broken_prop_) ? (pn * 0.045f) : 0.0f;
    set_node(ext_mesh_, anim, "PropHub", Mat4::rotate_yxz(wob, wob * 0.6f, broken_prop_ ? 0.35f : prop_angle_rad_));
    if (const auto *bp = find_broken("PropBlades")) {
        set_node(ext_mesh_, anim, "PropBlades",
                 Mat4::translate(bp->pos_off) * Mat4::rotate_yxz(bp->rot.y, bp->rot.x, bp->rot.z));
    } else if (broken_prop_ || pn > 0.05f) {
        const float b = broken_prop_ ? 0.92f : pn;
        set_node(ext_mesh_, anim, "PropBlades", Mat4::rotate_yxz(b * 0.25f, b * 0.85f, -b * 0.40f));
    }

    if (const auto *bp = find_broken("Wing_L")) {
        set_node(ext_mesh_, anim, "Wing_L",
                 Mat4::translate(bp->pos_off) * Mat4::rotate_yxz(bp->rot.y, bp->rot.x, bp->rot.z));
    } else {
        set_node(ext_mesh_, anim, "Wing_L", Mat4::rotate_yxz(-wl * 0.14f, wl * 0.08f, -(g_flex + wl * 0.22f)));
    }
    if (const auto *bp = find_broken("Wing_R")) {
        set_node(ext_mesh_, anim, "Wing_R",
                 Mat4::translate(bp->pos_off) * Mat4::rotate_yxz(bp->rot.y, bp->rot.x, bp->rot.z));
    } else {
        set_node(ext_mesh_, anim, "Wing_R", Mat4::rotate_yxz(+wr * 0.14f, wr * 0.08f, +(g_flex + wr * 0.22f)));
    }
    if (const auto *bp = find_broken("Tail")) {
        set_node(ext_mesh_, anim, "Tail",
                 Mat4::translate(bp->pos_off) * Mat4::rotate_yxz(bp->rot.y, bp->rot.x, bp->rot.z));
    } else if (td > 0.02f) {
        set_node(ext_mesh_, anim, "Tail", Mat4::rotate_yxz(td * 0.16f, td * 0.18f, td * 0.14f));
    }

    if (const auto *bp = find_broken("NoseGear")) {
        set_node(ext_mesh_, anim, "NoseGear",
                 Mat4::translate(bp->pos_off) * Mat4::rotate_yxz(bp->rot.y, bp->rot.x, bp->rot.z));
    } else if (broken_gear_n_) {
        set_node(ext_mesh_, anim, "NoseGear", Mat4::translate({0.0f, 0.34f, 0.28f}) * Mat4::rotate_yxz(0.25f, 1.25f, 0.20f));
    } else {
        set_node(ext_mesh_, anim, "NoseGear",
                 Mat4::rotate_yxz(-rud * 18.0f * DEG2RAD_F + ga * 0.46f, gd * 0.18f, gd * 0.28f));
    }
    if (broken_gear_l_) {
        set_node(ext_mesh_, anim, "Gear_L", Mat4::translate({0.22f, 0.42f, 0.18f}) * Mat4::rotate_yxz(-0.20f, 0.35f, -1.18f));
    }
    if (broken_gear_r_) {
        set_node(ext_mesh_, anim, "Gear_R", Mat4::translate({-0.22f, 0.42f, 0.18f}) * Mat4::rotate_yxz(+0.20f, 0.35f, +1.18f));
    }

    set_node(ext_mesh_, anim, "Flap_L",    Mat4::rotate_yxz(0.0f, flaps_rad + wl * 0.16f, wl * 0.08f));
    set_node(ext_mesh_, anim, "Flap_R",    Mat4::rotate_yxz(0.0f, flaps_rad + wr * 0.16f, -wr * 0.08f));
    set_node(ext_mesh_, anim, "Aileron_L", Mat4::rotate_yxz(0.0f, +ail * 20.0f * DEG2RAD_F - wl * 0.20f, 0.0f));
    set_node(ext_mesh_, anim, "Aileron_R", Mat4::rotate_yxz(0.0f, -ail * 20.0f * DEG2RAD_F - wr * 0.20f, 0.0f));
    set_node(ext_mesh_, anim, "Elevator",  Mat4::rotate_yxz(0.0f, -elv * 22.0f * DEG2RAD_F + td * 0.14f, td * 0.06f));
    set_node(ext_mesh_, anim, "TrimTab",   Mat4::rotate_yxz(0.0f, +trm * 15.0f * DEG2RAD_F, 0.0f));
    set_node(ext_mesh_, anim, "Rudder",    Mat4::rotate_yxz(+rud * 24.0f * DEG2RAD_F + td * 0.18f, 0.0f, td * 0.05f));
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
    // Engine-driven vacuum pump spools the attitude & directional gyros upright above 2.2 inHg, tumbles when off!
    const float gyro_target = (rpm > 450.0f && systems_.get_vacuum_inhg() >= 2.2) ? 1.0f : 0.0f;
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
        update_avionics_display_texture(sim_time_sec);
        // Pilot seated eye framing: crisp unobstructed 3D 6-pack + Nav/GPS/AP stack + cowling + spinning prop veil
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

        double alt_ft = std::max(0.0, st_.alt_msl_m * 3.28084 + (systems_.get_baro_hpa() - last_air_.qnh_hpa) * 27.3);
        float alt100_cw  = static_cast<float>(std::fmod(alt_ft, 1000.0)  / 1000.0  * 360.0);
        float alt1000_cw = static_cast<float>(std::fmod(alt_ft, 10000.0) / 10000.0 * 360.0);
        set_node(ckpt_mesh_, anim, "Needle_ALT_100",  Mat4::rotate_yxz(0.0f, 0.0f, -alt100_cw  * DEG2RAD_F));
        set_node(ckpt_mesh_, anim, "Needle_ALT_1000", Mat4::rotate_yxz(0.0f, 0.0f, -alt1000_cw * DEG2RAD_F));

        const bool tc_powered = systems_.is_circuit_powered(nspain::CB_TURN_COORD) && systems_.get_bus_volts() >= 19.5;
        float tc_bank = tc_powered ? std::clamp(static_cast<float>(fs.r_rps * RAD2DEG_D * 6.0), -26.0f, 26.0f) : 0.0f;
        set_node(ckpt_mesh_, anim, "Card_TC",  Mat4::rotate_yxz(0.0f, 0.0f, -tc_bank * DEG2RAD_F));
        const double hdg_disp_deg = st_.heading_deg + (1.0f - ai_gyro_spool_) * 18.0;
        set_node(ckpt_mesh_, anim, "Card_HDG", Mat4::rotate_yxz(0.0f, 0.0f, static_cast<float>(hdg_disp_deg * DEG2RAD_D)));

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

        // Exterior aviation lights coupled 100% to electrical bus, breakers & switches (all dark when Cold & Dark!)
        const float bus_mul = (systems_.get_bus_volts() >= 19.5) ? (cranking ? 0.56f : 1.0f) : 0.0f;
        float cyc = std::fmod(sim_time_sec, 1.25f);
        bool strobe_on = (cyc < 0.065f) || (cyc > 0.16f && cyc < 0.225f);
        float bcn_pulse = 0.25f + 2.5f * std::pow(0.5f + 0.5f * std::sin(sim_time_sec * 5.8f), 4.0f);
        set_f(ext_mesh_, emiss, "BeaconRed",    systems_.is_light_powered(0) ? (bcn_pulse * bus_mul) : 0.0f);
        set_f(ext_mesh_, emiss, "NavLightRed",  systems_.is_light_powered(2) ? (2.4f * bus_mul) : 0.0f);
        set_f(ext_mesh_, emiss, "NavLightGreen", systems_.is_light_powered(2) ? (2.4f * bus_mul) : 0.0f);
        set_f(ext_mesh_, emiss, "StrobeWhite",  systems_.is_light_powered(3) ? ((strobe_on ? 3.5f : 0.08f) * bus_mul) : 0.0f);
        set_f(ext_mesh_, emiss, "LandingLight", systems_.is_light_powered(1) ? (4.2f * bus_mul) : 0.0f);

        // Align visual tire bottom (-1.358 m in exterior GLB) with JSBSim static bogey height (~1.20 m);
        // drop hull onto belly/wing when landing gear has collapsed!
        const float belly_drop = (fs.on_ground && (broken_gear_n_ || broken_gear_l_ || broken_gear_r_)) ? -0.46f : 0.0f;
        const float tilt_roll = (fs.on_ground && broken_gear_l_ && !broken_gear_r_) ? -0.22f
                              : ((fs.on_ground && broken_gear_r_ && !broken_gear_l_) ? +0.22f : 0.0f);
        const float tilt_pitch = (fs.on_ground && broken_gear_n_ && !broken_gear_l_) ? -0.20f : 0.0f;
        const Mat4 hull_rot = ac_rot * Mat4::rotate_yxz(0.0f, tilt_pitch, tilt_roll);
        Mat4 root_cam_rel = Mat4::translate({-cam_off.x, -cam_off.y + 0.16f + belly_drop, -cam_off.z}) * hull_rot;
        draw_glb(ext_mesh_, vp, hull_rot, root_cam_rel, anim, alpha, emiss, atmo, refl_env_tex, false,
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
    if (damage_ < 0.05 && prop_nicks_ < 0.05) {
        broken_parts_.clear();
        broken_wing_l_ = broken_wing_r_ = broken_tail_ = broken_prop_ = false;
        broken_gear_n_ = broken_gear_l_ = broken_gear_r_ = false;
        airframe_destroyed_ = false;
    }
}

void AircraftAndHud::quick_start_or_shutdown() {
    if (systems_.is_engine_running()) {
        // Clean shutdown: idle-cutoff -> mags off -> avionics & master off
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

void AircraftAndHud::render_cockpit_interaction(
    int win_w, int win_h, const PlatformWindow &win,
    float sim_time_sec, bool allow_pointer, bool ui_blocks_pointer) {
    if (st_.view_mode != 0 || !last_ckpt_valid_ || last_ckpt_world_m_.empty()) {
        mouse_starter_cranking_ = false;
        return;
    }
    auto pi = ckpt_mesh_.node_index.find("PanelGroup");
    if (pi == ckpt_mesh_.node_index.end() || static_cast<size_t>(pi->second) >= last_ckpt_world_m_.size()) {
        mouse_starter_cranking_ = false;
        return;
    }

    const float wf = static_cast<float>(win_w);
    const float hf = static_cast<float>(win_h);
    const float ui = std::clamp(std::min(wf, hf) / 1080.0f, 0.75f, 1.4f);
    const Mat4 panel_mvp = last_ckpt_vp_ * last_ckpt_root_ * last_ckpt_world_m_[static_cast<size_t>(pi->second)];

    auto project_mvp = [&](const Mat4 &mvp, const Vec3 &p, float &sx, float &sy) -> bool {
        const float cx = mvp.m[0] * p.x + mvp.m[4] * p.y + mvp.m[8] * p.z + mvp.m[12];
        const float cy = mvp.m[1] * p.x + mvp.m[5] * p.y + mvp.m[9] * p.z + mvp.m[13];
        const float cw = mvp.m[3] * p.x + mvp.m[7] * p.y + mvp.m[11] * p.z + mvp.m[15];
        if (cw <= 0.05f) return false;
        const float nx = cx / cw, ny = cy / cw;
        if (nx < -1.05f || nx > 1.05f || ny < -1.05f || ny > 1.05f) return false;
        sx = (nx * 0.5f + 0.5f) * wf;
        sy = (0.5f - ny * 0.5f) * hf;
        return true;
    };
    auto project_node = [&](const char *node_name, const Vec3 &off, float &sx, float &sy) -> bool {
        auto it = ckpt_mesh_.node_index.find(visual_node_name(ckpt_mesh_, node_name));
        if (it == ckpt_mesh_.node_index.end() || static_cast<size_t>(it->second) >= last_ckpt_world_m_.size()) return false;
        const Mat4 mvp = last_ckpt_vp_ * last_ckpt_root_ * last_ckpt_world_m_[static_cast<size_t>(it->second)];
        return project_mvp(mvp, off, sx, sy);
    };
    auto avn_pt = [](float u, float v) -> Vec3 {
        // Matches export_glb_models.py av_rect = (0.214, 0.482, -0.102, 0.140), v=0 top -> 0.140
        return {0.214f + u * (0.482f - 0.214f), 0.140f - v * (0.140f - (-0.102f)), 0.014f};
    };

    enum class SpotKind { Panel, Node };
    struct Hotspot {
        const char *id;
        SpotKind kind;
        const char *node;
        Vec3 p;
        float r_px;
        std::string tip;
        bool hold_action = false;
    };

    const bool running = systems_.is_engine_running();
    const bool shift = win.key_down(KEY_SHIFT);
    const auto &fs = fdm_.state();
    static constexpr const char *kMagStr[5] = {"OFF", "R", "L", "BOTH", "START"};
    static constexpr const char *kFuelStr[4] = {"OFF", "LEFT", "BOTH", "RIGHT"};
    static constexpr const char *kXpdrStr[4] = {"OFF", "STBY", "ON", "ALT"};

    // Determine active Cold-and-Dark guided startup step (auto-highlights the exact next 3D switch!)
    const char *guide_id = nullptr;
    std::string guide_text;
    if (!running && fs.on_ground && !st_.slew_fast) {
        if (systems_.get_fuel_shutoff_pulled()) {
            guide_id = "fuel_shutoff";
            guide_text = "START 1/8: CLICK FUEL SHUTOFF VALVE -> OPEN";
        } else if (systems_.get_fuel_selector() != nspain::FUEL_SEL_BOTH) {
            guide_id = "fuel_sel";
            guide_text = "START 1/8: CLICK FUEL SELECTOR -> BOTH";
        } else if (!systems_.get_master_battery()) {
            guide_id = "bat";
            guide_text = "START 2/8: CLICK BAT MASTER -> ON";
        } else if (!systems_.get_master_alternator()) {
            guide_id = "alt";
            guide_text = "START 3/8: CLICK ALT MASTER -> ON";
        } else if (!systems_.get_light_switch(0)) {
            guide_id = "bcn";
            guide_text = "START 4/8: CLICK BEACON LIGHT -> ON";
        } else if (st_.mixture < 0.80) {
            guide_id = "mixture";
            guide_text = "START 5/8: CLICK RED MIXTURE -> FULL RICH";
        } else if (systems_.get_oil_temp_f() < 95.0 && systems_.get_primer_strokes() < 2) {
            guide_id = "pitot_prime";
            guide_text = "START 6/8: CLICK PRIME (2x COLD ENGINE)";
        } else if (st_.throttle < 0.04) {
            guide_id = "throttle";
            guide_text = "START 7/8: CLICK THROTTLE -> CRACK OPEN 10%";
        } else {
            guide_id = "mags";
            guide_text = "START 8/8: CLICK & HOLD IGNITION KEY -> START";
        }
    } else if (running && fs.on_ground && !systems_.get_avionics_master()) {
        guide_id = "avn";
        guide_text = "AFTER START: CLICK AVN MASTER -> ON";
    }

    char buf[128];
    std::vector<Hotspot> spots;
    spots.reserve(48);
    auto add_p = [&](const char *id, Vec3 p, float r, const char *tip, bool hold = false) {
        spots.push_back({id, SpotKind::Panel, nullptr, p, r * ui, tip, hold});
    };
    auto add_n = [&](const char *id, const char *node, float r, const char *tip) {
        spots.push_back({id, SpotKind::Node, node, {0.0f, 0.0f, 0.0f}, r * ui, tip, false});
    };

    // 1. Lower Switch Bar: MAGS, ALT, BAT + 8 Toggles (AVN, PUMP, BCN, LDG, TAXI/INST, NAV, STRB, PITOT/PRIME)
    std::snprintf(buf, sizeof(buf), "IGNITION MAGS: %s  (Click cycle / Hold crank)", kMagStr[std::clamp(systems_.get_magnetos(), 0, 4)]);
    add_p("mags", {-0.422f, -0.136f, 0.018f}, 24.0f, buf, true);
    add_p("alt",  {-0.370f, -0.136f, 0.018f}, 18.0f, systems_.get_master_alternator() ? "ALT MASTER: ON" : "ALT MASTER: OFF");
    add_p("bat",  {-0.354f, -0.136f, 0.018f}, 18.0f, systems_.get_master_battery() ? "BAT MASTER: ON" : "BAT MASTER: OFF");
    add_p("avn",  {-0.312f, -0.136f, 0.018f}, 18.0f, systems_.get_avionics_master() ? "AVIONICS MASTER: ON" : "AVIONICS MASTER: OFF");
    add_p("pump", {-0.280f, -0.136f, 0.018f}, 18.0f, systems_.get_fuel_pump_switch() ? "AUX FUEL PUMP: ON" : "AUX FUEL PUMP: OFF");
    add_p("bcn",  {-0.248f, -0.136f, 0.018f}, 18.0f, systems_.get_light_switch(0) ? "BEACON LIGHT: ON" : "BEACON LIGHT: OFF");
    add_p("ldg",  {-0.216f, -0.136f, 0.018f}, 18.0f, systems_.get_light_switch(1) ? "LANDING LIGHT: ON" : "LANDING LIGHT: OFF");
    add_p("inst", {-0.184f, -0.136f, 0.018f}, 18.0f, systems_.get_light_switch(4) ? "PANEL / TAXI LIGHT: ON" : "PANEL / TAXI LIGHT: OFF");
    add_p("nav",  {-0.152f, -0.136f, 0.018f}, 18.0f, systems_.get_light_switch(2) ? "NAV LIGHTS: ON" : "NAV LIGHTS: OFF");
    add_p("strb", {-0.120f, -0.136f, 0.018f}, 18.0f, systems_.get_light_switch(3) ? "STROBE LIGHTS: ON" : "STROBE LIGHTS: OFF");
    if (!running) {
        std::snprintf(buf, sizeof(buf), "ENGINE PRIMER (%d strokes)  (Click to prime)", systems_.get_primer_strokes());
        add_p("pitot_prime", {-0.088f, -0.136f, 0.018f}, 18.0f, buf);
    } else {
        add_p("pitot_prime", {-0.088f, -0.136f, 0.018f}, 18.0f, systems_.get_pitot_heat_switch() ? "PITOT HEAT: ON" : "PITOT HEAT: OFF");
    }

    // 2. 10 Physical 28V Circuit Breakers on Panel (click pulls/resets real CB!)
    static constexpr int kPanelCbs[10] = {
        nspain::CB_ALT_FIELD, nspain::CB_AVIONICS_BUS_1, nspain::CB_AVIONICS_BUS_2, nspain::CB_FUEL_PUMP, nspain::CB_FLAPS,
        nspain::CB_INST_LIGHTS, nspain::CB_BCN_LIGHT, nspain::CB_LAND_LIGHT, nspain::CB_NAV_LIGHT, nspain::CB_TURN_COORD
    };
    static constexpr const char *kCbIds[10] = {"cb0","cb1","cb2","cb3","cb4","cb5","cb6","cb7","cb8","cb9"};
    for (int i = 0; i < 10; ++i) {
        const float bx = 0.236f + (i % 5) * 0.028f;
        const float by = -0.138f - (i / 5) * 0.024f;
        const int cb = kPanelCbs[i];
        std::snprintf(buf, sizeof(buf), "CB %s: %s (Click pull/reset)",
                      nspain::PistonAircraftSystems::get_breaker_name(cb),
                      systems_.is_breaker_pulled(cb) ? "PULLED OUT" : "IN");
        add_p(kCbIds[i], {bx, by, 0.016f}, 14.0f, buf);
    }

    // 3. Movable 3D Engine & Flight Controls (node-tracked!)
    add_n("carb_heat", "CarbHeatKnob", 24.0f, systems_.get_carb_heat() > 0.5 ? "CARB HEAT / ALT AIR: ON" : "CARB HEAT / ALT AIR: OFF");
    std::snprintf(buf, sizeof(buf), "THROTTLE %.0f%% (Click +10%% / Shift -10%%)", st_.throttle * 100.0);
    add_n("throttle", "ThrottleKnob", 28.0f, buf);
    std::snprintf(buf, sizeof(buf), "MIXTURE %.0f%% (Click Rich / Shift Lean)", st_.mixture * 100.0);
    add_n("mixture", "MixtureKnob", 28.0f, buf);
    std::snprintf(buf, sizeof(buf), "FLAPS %.0f deg (Click +10 / Shift -10)", systems_.get_flaps_target_deg());
    add_n("flaps", "FlapLever", 26.0f, buf);
    std::snprintf(buf, sizeof(buf), "ELEVATOR TRIM %+.2f (Click UP / Shift DN)", st_.trim);
    add_n("trim", "TrimWheel", 26.0f, buf);
    add_n("park_brk", "ParkingBrake", 24.0f, st_.parking_brake ? "PARKING BRAKE: SET (Click release)" : "PARKING BRAKE: OFF (Click set)");
    std::snprintf(buf, sizeof(buf), "FUEL SELECTOR: %s (Click cycle)", kFuelStr[std::clamp(systems_.get_fuel_selector(), 0, 3)]);
    add_n("fuel_sel", "FuelSelector", 28.0f, buf);
    add_n("fuel_shutoff", "FuelShutoff", 24.0f, systems_.get_fuel_shutoff_pulled() ? "FUEL SHUTOFF: PULLED (Click open)" : "FUEL SHUTOFF: OPEN");

    // 4. 3D Gauge Corner Knobs (Baro Kollsman, HDG Bug, VOR1 OBS)
    std::snprintf(buf, sizeof(buf), "KOLLSMAN BARO %.0f hPa (Click +1 / Shift -1)", systems_.get_baro_hpa());
    add_p("baro_knob", {-0.043f, 0.025f, 0.022f}, 18.0f, buf);
    std::snprintf(buf, sizeof(buf), "VOR1 OBS %03d deg (Click +10 / Shift -10)", systems_.get_nav_obs_deg(1));
    add_p("obs_knob", {0.071f, 0.025f, 0.022f}, 18.0f, buf);
    std::snprintf(buf, sizeof(buf), "HDG BUG %03d deg (Click +10 / Shift -10)", ((int(std::round(st_.ap_hdg_deg)) % 360) + 360) % 360);
    add_p("hdg_knob", {-0.157f, -0.085f, 0.022f}, 18.0f, buf);

    // 5. Center AvionicsStack Live Interactive Displays & Buttons (zero fake buttons!)
    std::snprintf(buf, sizeof(buf), "COM 1 %.2f/%.2f (Click swap / Shift +25k)", systems_.get_com_active(1), systems_.get_com_standby(1));
    add_p("com1", avn_pt(0.18f, 0.29f), 26.0f, buf);
    std::snprintf(buf, sizeof(buf), "NAV 1 %.2f [%s] (Click swap / Shift next VOR)", systems_.get_nav_active(1), systems_.get_nav_ident(1).c_str());
    add_p("nav1", avn_pt(0.18f, 0.42f), 26.0f, buf);
    add_p("gps_src", avn_pt(0.89f, 0.35f), 24.0f, st_.ap_nav_source ? "NAV SOURCE: VOR1 (Click -> GPS)" : "NAV SOURCE: GPS (Click -> VOR1)");
    std::snprintf(buf, sizeof(buf), "COM 2 %.2f/%.2f (Click swap)", systems_.get_com_active(2), systems_.get_com_standby(2));
    add_p("com2", avn_pt(0.32f, 0.61f), 24.0f, buf);
    std::snprintf(buf, sizeof(buf), "NAV 2 %.2f/%.2f (Click swap)", systems_.get_nav_active(2), systems_.get_nav_standby(2));
    add_p("nav2", avn_pt(0.69f, 0.61f), 24.0f, buf);

    // 2-Axis Autopilot Softkeys & Knobs directly on the 3D radio stack!
    add_p("ap_btn",  avn_pt(0.196f, 0.824f), 16.0f, st_.ap_master ? "AUTOPILOT: ENGAGED (Click disconnect)" : "AUTOPILOT: STANDBY (Click engage)");
    add_p("ap_hdg",  avn_pt(0.296f, 0.824f), 16.0f, "AP HDG MODE (Click engage HDG)");
    add_p("ap_nav",  avn_pt(0.395f, 0.824f), 16.0f, st_.ap_nav_source ? "AP NAV MODE: VOR1" : "AP NAV MODE: GPS");
    add_p("ap_vs",   avn_pt(0.495f, 0.824f), 16.0f, "AP VS MODE (Click hold Vertical Speed)");
    add_p("ap_arm",  avn_pt(0.595f, 0.824f), 16.0f, st_.ap_alt_arm ? "AP ALT ARM: ARMED" : "AP ALT ARM: OFF (Click arm capture)");
    add_p("ap_dn",   avn_pt(0.694f, 0.824f), 16.0f, st_.ap_vs_hold ? "AP VS -100 fpm" : "AP ALT -100 ft");
    add_p("ap_up",   avn_pt(0.794f, 0.824f), 16.0f, st_.ap_vs_hold ? "AP VS +100 fpm" : "AP ALT +100 ft");
    add_p("ap_hdgk", avn_pt(0.082f, 0.744f), 18.0f, "AP HDG KNOB (Click +10 / Shift -10)");
    add_p("ap_altk", avn_pt(0.918f, 0.744f), 18.0f, "AP ALT KNOB (Click +500 / Shift -500)");

    std::snprintf(buf, sizeof(buf), "XPDR MODE: %s (Click cycle)", kXpdrStr[std::clamp(systems_.get_xpdr_mode(), 0, 3)]);
    add_p("xpdr_m", avn_pt(0.24f, 0.895f), 20.0f, buf);
    std::snprintf(buf, sizeof(buf), "XPDR %04d (Click 7000 VFR)", systems_.get_xpdr_squawk());
    add_p("xpdr_c", avn_pt(0.49f, 0.895f), 22.0f, buf);
    add_p("xpdr_i", avn_pt(0.918f, 0.942f), 18.0f, "XPDR IDENT KNOB (Click transmit IDENT)");

    const PixelRect pr = win.presentation_rect(win_w, win_h);
    const float mx = (static_cast<float>(win.mouse_x() - pr.x)) * (wf / std::max(1.0f, static_cast<float>(pr.w)));
    const float my = (static_cast<float>(win.mouse_y() - pr.y)) * (hf / std::max(1.0f, static_cast<float>(pr.h)));
    const bool pointer_active = allow_pointer && !ui_blocks_pointer && !st_.menu_open &&
                                !st_.mouse_yoke_enabled && !win.mouse_right_down();

    int hover_idx = -1;
    float hover_d2 = 1e9f;
    float hover_sx = 0.0f, hover_sy = 0.0f;
    int guide_idx = -1;
    float guide_sx = 0.0f, guide_sy = 0.0f;

    for (size_t i = 0; i < spots.size(); ++i) {
        float sx = 0.0f, sy = 0.0f;
        const bool ok = (spots[i].kind == SpotKind::Panel)
            ? project_mvp(panel_mvp, spots[i].p, sx, sy)
            : project_node(spots[i].node, spots[i].p, sx, sy);
        if (!ok) continue;
        if (guide_id && std::strcmp(spots[i].id, guide_id) == 0) {
            guide_idx = static_cast<int>(i);
            guide_sx = sx;
            guide_sy = sy;
        }
        if (pointer_active) {
            const float dx = mx - sx, dy = my - sy;
            const float d2 = dx * dx + dy * dy;
            if (d2 <= spots[i].r_px * spots[i].r_px && d2 < hover_d2) {
                hover_d2 = d2;
                hover_idx = static_cast<int>(i);
                hover_sx = sx;
                hover_sy = sy;
            }
        }
    }

    // Handle continuous starter crank while holding LMB on 3D MAGS key
    if (pointer_active && hover_idx >= 0 && std::strcmp(spots[hover_idx].id, "mags") == 0 &&
        win.mouse_left_down() && !running && systems_.get_magnetos() >= 3) {
        mouse_starter_cranking_ = true;
    } else if (!win.mouse_left_down()) {
        mouse_starter_cranking_ = false;
    }

    // Dispatch real system action on Left Click!
    if (pointer_active && hover_idx >= 0 && win.mouse_left_clicked()) {
        const std::string id = spots[hover_idx].id;
        static constexpr double kVorFreqs[10] = {113.00, 114.50, 112.30, 115.80, 113.60, 115.90, 112.50, 116.40, 117.20, 112.70};
        if (id == "mags") {
            if (!running && systems_.get_magnetos() == 3) { systems_.set_magnetos(4); mouse_starter_cranking_ = true; }
            else systems_.set_magnetos((systems_.get_magnetos() + 1) % 4);
        } else if (id == "alt") systems_.set_master_alternator(!systems_.get_master_alternator());
        else if (id == "bat") systems_.set_master_battery(!systems_.get_master_battery());
        else if (id == "avn") systems_.set_avionics_master(!systems_.get_avionics_master());
        else if (id == "pump") systems_.set_fuel_pump_switch(!systems_.get_fuel_pump_switch());
        else if (id == "bcn") systems_.set_light_switch(0, !systems_.get_light_switch(0));
        else if (id == "ldg") systems_.set_light_switch(1, !systems_.get_light_switch(1));
        else if (id == "inst") systems_.set_light_switch(4, !systems_.get_light_switch(4));
        else if (id == "nav") systems_.set_light_switch(2, !systems_.get_light_switch(2));
        else if (id == "strb") systems_.set_light_switch(3, !systems_.get_light_switch(3));
        else if (id == "pitot_prime") {
            if (!running) systems_.actuate_primer();
            else systems_.set_pitot_heat_switch(!systems_.get_pitot_heat_switch());
        } else if (id.size() == 3 && id[0] == 'c' && id[1] == 'b') {
            systems_.toggle_breaker(kPanelCbs[id[2] - '0']);
        } else if (id == "carb_heat") systems_.set_carb_heat(systems_.get_carb_heat() > 0.5 ? 0.0 : 1.0);
        else if (id == "throttle") {
            if (!running && !shift && st_.throttle < 0.08) st_.throttle = 0.12;
            else st_.throttle = std::clamp(st_.throttle + (shift ? -0.10 : 0.10), 0.0, 1.0);
            throttle_hw_active_ = false;
        } else if (id == "mixture") {
            if (!running && !shift) st_.mixture = 1.0;
            else st_.mixture = std::clamp(st_.mixture + (shift ? -0.15 : 0.15), 0.0, 1.0);
            mixture_hw_active_ = false;
        } else if (id == "flaps") {
            const double f = systems_.get_flaps_target_deg();
            systems_.set_flaps_target_deg(shift ? std::max(0.0, f - 10.0) : ((f >= 29.0) ? 0.0 : f + 10.0));
        } else if (id == "trim") st_.trim = std::clamp(st_.trim + (shift ? -0.06 : 0.06), -0.55, 0.55);
        else if (id == "park_brk") st_.parking_brake = !st_.parking_brake;
        else if (id == "fuel_sel") systems_.cycle_fuel_selector();
        else if (id == "fuel_shutoff") systems_.set_fuel_shutoff_pulled(!systems_.get_fuel_shutoff_pulled());
        else if (id == "baro_knob") systems_.set_baro_hpa(systems_.get_baro_hpa() + (shift ? -1.0 : 1.0));
        else if (id == "obs_knob") systems_.set_nav_obs_deg(1, systems_.get_nav_obs_deg(1) + (shift ? -10 : 10));
        else if (id == "hdg_knob" || id == "ap_hdgk") st_.ap_hdg_deg = std::fmod(st_.ap_hdg_deg + (shift ? 350.0 : 10.0), 360.0);
        else if (id == "com1") { if (shift) systems_.step_com_standby(1, 0.025); else systems_.swap_com(1); }
        else if (id == "nav1") {
            if (shift) { vor_cycle_idx_ = (vor_cycle_idx_ + 1) % 10; systems_.tune_nav_standby(1, kVorFreqs[vor_cycle_idx_]); }
            else systems_.swap_nav(1);
        } else if (id == "gps_src") st_.ap_nav_source = (st_.ap_nav_source + 1) % 2;
        else if (id == "com2") systems_.swap_com(2);
        else if (id == "nav2") systems_.swap_nav(2);
        else if (id == "ap_btn" && !fs.on_ground) {
            st_.ap_master = !st_.ap_master;
            st_.ap_disconnect_warn_s = 0.0f;
            if (st_.ap_master) {
                st_.ap_rol_bank_deg = std::clamp(st_.roll_deg, -22.0, 22.0);
                st_.ap_hdg_deg = std::round(st_.heading_deg);
                if (!st_.ap_vs_hold) st_.ap_alt_ft = std::round(st_.ind_alt_ft / 100.0) * 100.0;
            }
        } else if (id == "ap_hdg") { st_.ap_hdg_hold = true; st_.ap_nav_hold = false; if (!fs.on_ground) st_.ap_master = true; }
        else if (id == "ap_nav") { st_.ap_nav_hold = true; st_.ap_hdg_hold = false; if (!fs.on_ground) st_.ap_master = true; }
        else if (id == "ap_vs") {
            st_.ap_vs_hold = true; st_.ap_alt_hold = false;
            st_.ap_vs_fpm = std::clamp(std::round((st_.vsi_mps * 196.85) / 100.0) * 100.0, -1500.0, 1200.0);
            if (!fs.on_ground) st_.ap_master = true;
        } else if (id == "ap_arm") st_.ap_alt_arm = !st_.ap_alt_arm;
        else if (id == "ap_dn") {
            if (st_.ap_vs_hold) st_.ap_vs_fpm = std::max(-1500.0, st_.ap_vs_fpm - 100.0);
            else st_.ap_alt_ft = std::max(1000.0, st_.ap_alt_ft - 100.0);
        } else if (id == "ap_up") {
            if (st_.ap_vs_hold) st_.ap_vs_fpm = std::min(1200.0, st_.ap_vs_fpm + 100.0);
            else st_.ap_alt_ft = std::min(14500.0, st_.ap_alt_ft + 100.0);
        } else if (id == "ap_altk") st_.ap_alt_ft = std::clamp(st_.ap_alt_ft + (shift ? -500.0 : 500.0), 1000.0, 14500.0);
        else if (id == "xpdr_m") systems_.cycle_xpdr_mode();
        else if (id == "xpdr_c") systems_.set_xpdr_vfr_7000();
        else if (id == "xpdr_i") systems_.trigger_xpdr_ident();
    }

    // Draw 3D-tracked Pulsing Startup Guide Highlight & Hover Tooltip!
    if (guide_idx < 0 && hover_idx < 0) return;
    hud_verts_.clear();

    if (guide_idx >= 0) {
        const float pulse = 0.5f + 0.5f * std::sin(sim_time_sec * 6.5f);
        const float r = spots[guide_idx].r_px * (1.05f + 0.22f * pulse);
        hud_rect(guide_sx - r, guide_sy - r, 2.0f * r, 2.0f * r, 1.0f, 0.78f, 0.14f, 0.18f + 0.16f * pulse);
        hud_border(guide_sx - r, guide_sy - r, 2.0f * r, 2.0f * r, 2.6f * ui, 1.0f, 0.84f, 0.22f, 0.85f + 0.15f * pulse);
        const float tw = std::max(290.0f * ui, static_cast<float>(guide_text.size()) * 11.2f * ui + 28.0f * ui);
        const float th = 38.0f * ui;
        const float tx = std::clamp(guide_sx - tw * 0.5f, 14.0f * ui, wf - tw - 14.0f * ui);
        const float ty = std::clamp(guide_sy - r - th - 12.0f * ui, 14.0f * ui, hf - th - 14.0f * ui);
        hud_rect(tx, ty, tw, th, 0.04f, 0.07f, 0.11f, 0.93f);
        hud_border(tx, ty, tw, th, 2.2f * ui, 1.0f, 0.84f, 0.20f, 0.98f);
        hud_text(tx + 14.0f * ui, ty + 9.0f * ui, 19.0f * ui, guide_text.c_str(), 1.0f, 0.94f, 0.54f, 1.0f);
    }

    if (hover_idx >= 0) {
        const float r = spots[hover_idx].r_px;
        hud_rect(hover_sx - r, hover_sy - r, 2.0f * r, 2.0f * r, 0.30f, 0.88f, 0.96f, 0.18f);
        hud_border(hover_sx - r, hover_sy - r, 2.0f * r, 2.0f * r, 2.0f * ui, 0.42f, 0.94f, 1.0f, 0.95f);
        const std::string &tip = spots[hover_idx].tip;
        const float tw = std::max(180.0f * ui, static_cast<float>(tip.size()) * 9.2f * ui + 22.0f * ui);
        const float th = 30.0f * ui;
        const float tx = std::clamp(hover_sx - tw * 0.5f, 14.0f * ui, wf - tw - 14.0f * ui);
        float ty = hover_sy - r - th - 8.0f * ui;
        if (guide_idx == hover_idx) ty -= 38.0f * ui;
        ty = std::clamp(ty, 14.0f * ui, hf - th - 14.0f * ui);
        hud_rect(tx, ty, tw, th, 0.04f, 0.07f, 0.10f, 0.90f);
        hud_border(tx, ty, tw, th, 1.5f * ui, 0.35f, 0.88f, 0.95f, 0.92f);
        hud_text(tx + 10.0f * ui, ty + 6.0f * ui, 15.0f * ui, tip.c_str(), 0.90f, 0.97f, 1.0f, 1.0f);
    }

    glViewport(0, 0, win_w, win_h);
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
