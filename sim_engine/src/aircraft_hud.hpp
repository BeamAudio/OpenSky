#pragma once

#include "aircraft_condition.hpp"
#include "aircraft_package.hpp"

#include "../../engine/src/c172_systems.hpp"
#include "../../engine/src/opentrack_udp.hpp"
#include "../../engine/src/flight_yoke.hpp"
#include "../../engine/src/flight_model_jsb.hpp"
#include "async_streamer.hpp"
#include "cdlod_terrain.hpp"
#include "gl_loader.hpp"
#include "graphics_settings.hpp"
#include "math3d.hpp"
#include "pack_reader.hpp"
#include "platform_window.hpp"
#include "regional_atmosphere.hpp"
#include "volumetric_clouds.hpp"
#include "world_objects.hpp"

#include <string>
#include <vector>

namespace nsim {

class FlightProfiler;
class GameFrontend;

struct DebugOverlayView {
    const FlightProfiler *profiler = nullptr;
    int weather_level = -1;
    bool physics = false;
    Mat4 projection_view;
    Mat4 aircraft_rotation;
    double east_m = 0.0, north_m = 0.0;
    double simulation_time_s = 0.0;
    bool weather_volume_3d = false;
};

// C172S EYEPOINT is 25 inches above its CG in the flight model.
inline constexpr float COCKPIT_EYE_HEIGHT_M = 25.0f * 0.0254f;

struct FlightSimState {
    double lat_deg = 42.53131;
    double lon_deg = 0.47676;
    double alt_msl_m = 1014.0;
    double ground_m = 918.0;
    double heading_deg = 160.0;
    double pitch_deg = -4.0;
    double roll_deg = 0.0;
    double speed_mps = 52.0;
    double vel_east_mps = 17.0;
    double vel_north_mps = -49.0;
    double vsi_mps = 0.0;
    double aileron = 0.0, elevator = 0.0, rudder = 0.0, trim = 0.0;
    double throttle = 0.65, mixture = 1.0;
    bool paused = true;
    bool parking_brake = false;
    bool runway_tracking = false;
    bool menu_open = true;
    bool slew_fast = false; // 350 kt speed-stress test mode!
    bool mouse_yoke_enabled = false;
    bool ap_master = false;
    bool ap_hdg_hold = true;
    bool ap_alt_hold = true;
    bool ap_nav_hold = false;
    bool ap_vs_hold = false;
    bool ap_alt_arm = false;
    int ap_nav_source = 0;       // 0=GPS Route, 1=VOR1 (OBS/CDI)
    double ap_hdg_deg = 160.0;
    double ap_alt_ft = 4500.0;
    double ap_vs_fpm = 0.0;
    double ap_rol_bank_deg = 0.0;
    double ap_nav_brg_deg = 160.0;
    bool ap_nav_valid = false;
    double ind_alt_ft = 0.0;
    float ap_disconnect_warn_s = 0.0f;
    int ap_trim_annunc = 0;      // -1=TRIM DN, 0=none, +1=TRIM UP
    int view_mode = 0;      // 0=Cockpit, 1=Chase, 2=Free Cam
    float look_yaw_deg = 0.0f;
    float look_pitch_deg = 0.0f;
    float seat_height_m = 0.0f;
    float seat_fore_m = 0.0f;
    float seat_lat_m = 0.0f;
    float chase_dist_m = 10.6f;
    Vec3 free_cam_off_m{0.0f, 3.0f, 12.0f};
    double visual_rpm_override = -1.0;
    double visual_flaps_deg = 0.0;
    // Live GNS 430 / Cockpit Glass Avionics State
    struct GpsWaypoint {
        double lat = 0.0;
        double lon = 0.0;
        std::string label;
    };
    std::vector<GpsWaypoint> gps_route;
    size_t gps_active_leg = 0;
    bool gps_direct_active = false;
    double gps_direct_lat = 0.0;
    double gps_direct_lon = 0.0;
    std::string gps_direct_label;
    int gps_page = 0;         // 0=MAP, 1=TERR (TAWS), 2=NRST, 3=NAV (HSI)
    int gps_range_idx = 1;    // 0=5NM, 1=10NM, 2=20NM, 3=40NM
    bool gps_north_up = false;
    int gps_nrst_cursor = 0;
    uint8_t audio_sel_mask = 0x25; // bit0=COM1, 1=COM2, 2=NAV1, 3=MKR, 4=SPKR, 5=ICS
    double hobbs_hours = 1482.6;
};

struct FlightAssistOut {
    double aileron = 0.0;
    double elevator = 0.0;
    double rudder = 0.0;
    double brake_left_add = 0.0;
    double brake_right_add = 0.0;
    double target_bank_deg = 0.0;
    double target_pitch_deg = 0.0;
    bool holding_heading = false;
};

FlightAssistOut compute_flight_and_taxi_assist(
    double roll_cmd, double pitch_cmd, double manual_rudder,
    bool manual_rudder_active, double taxi_hold_hdg_deg,
    const nspain::FlightState &fs);

class AircraftAndHud {
public:
    AircraftAndHud() = default;
    ~AircraftAndHud();

    bool init(const PackReader *pack, const std::string &asset_root, const AircraftPackage &aircraft,
              const std::string &shader_root);
    bool init_physics(const PackReader *pack, const std::string &aircraft_dir);
    void shutdown();

    void reset_flight(double lat, double lon, double alt_m, double hdg_deg, double ias_kt, bool on_ground = false);
    void teleport_to_airport(const AirportInfo &ap, double approach_dist_m = 1400.0, double agl_m = 170.0);

    void step_inputs_and_physics(const PlatformWindow &win, double dt, const AtmosphereState &atmo, const RegionalAtmosphere &reg_atmo, double sim_time_sec);
    void step_autonomous_assist(double dt, double roll_cmd, double pitch_cmd, double throttle_tgt, double brake_cmd,
                                const AtmosphereState &atmo, const RegionalAtmosphere &reg_atmo, double sim_time_sec);
    void render_aircraft_3d(const Mat4 &vp, const Vec3 &cam_off, const AtmosphereState &atmo,
                           GLuint refl_env_tex, float sim_time_sec,
                           bool output_resolution = false, GLuint output_lut_tex = 0,
                           GLuint backdrop_hdr_tex = 0);
    void render_precip_streaks(const Mat4 &vp, const Vec3 &cam_world_en_alt,
                               const Vec3 &cam_right, const Vec3 &cam_up,
                               const AtmosphereState &atmo, float sim_time_sec, bool walking = false);
    void render_hud(
        int win_w, int win_h,
        const AtmosphereState &atmo,
        const RegionalAtmosphere &reg_atmo,
        const StreamStats &st, const CdlodDrawStats &cd,
        int quality, int total_bldgs,
        double fps, double avg_ms, double p95_ms,
        const WeatherSnapshot *wx_snap = nullptr,
        const char *regime_name = nullptr,
        const char *wx_provenance = nullptr,
        bool show_diagnostics = true,
        const DebugOverlayView *debug = nullptr
    );

    FlightSimState &state() { return st_; }
    const FlightSimState &state() const { return st_; }
    const LocalAirSample &last_air() const { return last_air_; }
    void render_weather_model_overlay(float width, float height, const RegionalAtmosphere &atmo,
                                      const Mat4 &projection_view, double east_m, double north_m,
                                      double simulation_time_s = 0.0);
    void render_cockpit_interaction(int win_w, int win_h, const PlatformWindow &win,
                                    float sim_time_sec, bool allow_pointer, bool ui_blocks_pointer = false);
    const nspain::FlightState &jsb_state() const { return fdm_.state(); }
    const GraphicsSettings &graphics_settings() const { return graphics_settings_; }
    void set_graphics_settings(const GraphicsSettings &settings) { graphics_settings_ = settings; }
    void poll_yoke();
    void calibrate_yoke() { yoke_.calibrate_center(); }
    void reset_yoke_calibration() { yoke_.reset_calibration(); }
    void rescan_yokes() { yoke_.rescan(); }
    const nspain::YokeState &yoke_state() const { return yoke_.state(); }
    void render_parked_aircraft(const Mat4 &vp, Vec3 offset, float heading,
                               const AtmosphereState &atmo, GLuint reflection, GLuint lut = 0);
    void start_owned_flight(double lat, double lon, double alt, double heading,
                            const AircraftCareSnapshot &care, double ambient_temp_c, bool on_runway);
    void sync_care_snapshot(AircraftCareSnapshot &care) const;
    void sync_walkaround_damage(const AircraftCareSnapshot &care);
    void quick_start_or_shutdown();
    bool is_chocked_or_tied() const { return chocks_on_ || tie_downs_on_; }
    const nspain::PistonAircraftSystems &systems() const { return systems_; }
    nspain::PistonAircraftSystems &systems() { return systems_; }
    Vec3 cockpit_shake_pos() const { return cockpit_shake_pos_; }
    Vec2 cockpit_shake_rot() const { return cockpit_shake_rot_; }
    Vec3 cockpit_look_feel() const { return cockpit_look_feel_; } // x=yaw_deg (look into bank), y=pitch_deg, z=roll_deg
    float windscreen_frost() const { return windscreen_frost_; }
    float airframe_ice01() const { return airframe_ice01_; }
    float ground_wetness() const { return static_cast<float>(ground_wetness_); }

private:
    friend class GameFrontend;
    void apply_autopilot(const nspain::FlightState &fs, double dt, double &roll_cmd, double &pitch_cmd,
                         double &ap_ail_out, double &ap_elv_out, double &ap_rud_out);
    void init_avionics_display_texture();
    void update_avionics_display_texture(float sim_time_sec, const AtmosphereState &atmo);
    void update_davtron_and_steam_textures(float sim_time_sec, const AtmosphereState &atmo);
    void advance_aircraft(double dt, const AtmosphereState &atmo, const RegionalAtmosphere &reg_atmo,
                          double sim_time_sec, double rudder, double brake_left, double brake_right);
    void resolve_world_collisions(double dt);
    void detach_structural_part(const char *node_name, Vec3 local_vel, Vec3 ang_vel);
    void trigger_catastrophic_breakup(const char *reason, double impact_spd);
    void apply_damage_rig(std::vector<Mat4> &anim, float flaps_rad, float ail, float elv, float trm, float rud) const;
    std::string visual_node_name(const GlbMeshData &mesh, const char *fallback) const;
    friend class AircraftSession;
    void flush_hud(float width, float height, GLuint font = 0);
    struct HudVert {
        float x, y;
        float u, v;
        float r, g, b, a;
    };
    struct WeatherFlowTrace {
        double east_m = 0.0, north_m = 0.0;
        int level = 0;
        float life_s = 0.0f;
        float max_life_s = 48.0f;
        std::vector<Vec3> points;
    };

    void hud_rect(float x, float y, float w, float h, float r, float g, float b, float a);
    void hud_border(float x, float y, float w, float h, float t, float r, float g, float b, float a);
    void hud_text(float x, float y, float font_h, const char *text, float r, float g, float b, float a);
    void hud_line(Vec2 a, Vec2 b, float width, Vec3 color, float alpha = 0.94f);
    void hud_arrow(Vec2 a, Vec2 b, float width, Vec3 color);
    void render_debug_overlays(float w, float h, float top, const RegionalAtmosphere &atmo, const DebugOverlayView &view);
    void draw_glb(
        const GlbMeshData &mesh,
        const Mat4 &vp, const Mat4 &root_world, const Mat4 &root_cam_rel,
        const std::vector<Mat4> &node_anim,
        const std::vector<float> &node_alpha,
        const std::vector<float> &node_emiss,
        const AtmosphereState &atmo, GLuint refl_env_tex, bool is_interior, float sim_time_sec,
        bool output_resolution
    );

    const PackReader *pack_ = nullptr;
    AircraftPackage aircraft_{};
    nspain::FlightModel fdm_;
    double departure_lat_ = 0, departure_lon_ = 0, departure_heading_ = 0;
    nspain::PistonAircraftSystems systems_;
    nspain::OpenTrackUDP opentrack_;
    nspain::FlightYokeInput yoke_;
    bool throttle_hw_active_ = false;
    double throttle_hw_last_ = -1.0;
    bool mixture_hw_active_ = false;
    double mixture_hw_last_ = -1.0;
    LocalAirSample last_air_{};
    FlightSimState st_{};
    float prop_angle_rad_ = 0.0f;
    float ai_gyro_spool_ = 0.0f;
    float prev_render_t_ = -1.0f;
    Vec2 rain_flow_front_{0.0f, 0.0f}, rain_flow_side_{0.0f, 0.0f};
    Vec2 rain_vel_front_{0.0f, 0.15f}, rain_vel_side_{0.05f, 0.15f};
    float rain_speed01_ = 0.0f;
    double mouse_yoke_roll_ = 0.0, mouse_yoke_pitch_ = 0.0;
    double anchor_mx_ = 0.0, anchor_my_ = 0.0;
    double raw_yoke_x_ = 0.0, raw_yoke_y_ = 0.0;
    bool yoke_anchor_valid_ = false;
    bool was_rmb_down_ = false;
    double kb_bank_cmd_ = 0.0, kb_pitch_cmd_ = 0.0;
    double taxi_hold_hdg_deg_ = 0.0;
    bool taxi_hold_valid_ = false;
    double ground_wetness_ = 0.0;
    double damage_ = 0.0;
    double tire_wear_ = 0.0, brake_wear_ = 0.0, gear_alignment_ = 0.0, prop_nicks_ = 0.0;
    double windshield_dirt_ = 0.0, windshield_crack_ = 0.0;
    double dmg_nose_ = 0.0, dmg_wing_l_ = 0.0, dmg_wing_r_ = 0.0, dmg_tail_ = 0.0;
    struct BrokenPart {
        std::string node;
        Vec3 pos_off{0.0f, 0.0f, 0.0f};
        Vec3 vel{0.0f, 0.0f, 0.0f};
        Vec3 rot{0.0f, 0.0f, 0.0f};
        Vec3 ang_vel{0.0f, 0.0f, 0.0f};
        bool detached = false;
    };
    std::vector<BrokenPart> broken_parts_;
    bool broken_wing_l_ = false, broken_wing_r_ = false, broken_tail_ = false, broken_prop_ = false;
    bool broken_gear_n_ = false, broken_gear_l_ = false, broken_gear_r_ = false;
    bool airframe_destroyed_ = false;
    float impact_fx_s_ = 0.0f;
    double impact_cooldown_s_ = 0.0;
    bool prev_ground_coll_ = true;
    double prev_vsi_coll_ = 0.0;
    std::string collision_msg_;
    double collision_msg_s_ = 0.0;
    float windscreen_frost_ = 0.0f, cabin_mist_ = 0.0f, airframe_ice01_ = 0.0f;
    Vec3 cockpit_shake_pos_{0.0f, 0.0f, 0.0f};
    Vec3 cockpit_head_vel_{0.0f, 0.0f, 0.0f};
    Vec2 cockpit_shake_rot_{0.0f, 0.0f}; // x=pitch_deg, y=roll_deg
    Vec3 cockpit_look_feel_{0.0f, 0.0f, 0.0f};
    bool prev_on_ground_pov_ = true;
    double prev_vsi_pov_ = 0.0;
    double prev_g_load_ = 1.0;
    double shake_phase_ = 0.0;
    bool chocks_on_ = false, tie_downs_on_ = false;
    bool starter_held_ = false;
    bool owned_mode_ = false;
    GraphicsSettings graphics_settings_{};

    GLuint ac_prog_ = 0;
    GLuint precip_prog_ = 0, precip_vao_ = 0;
    GLint u_pr_vp_ = -1, u_pr_cam_ = -1, u_pr_vel_ = -1, u_pr_right_ = -1, u_pr_up_ = -1;
    GLint u_pr_time_ = -1, u_pr_snow_ = -1, u_pr_int_ = -1, u_pr_col_ = -1;
    GLuint white_tex_ = 0;
    GLint u_ac_mvp_ = -1, u_ac_model_ = -1, u_ac_node_ = -1, u_ac_sun_ = -1, u_ac_cool_ = -1, u_ac_warm_ = -1;
    GLint u_ac_tint_ = -1, u_ac_pbr_ = -1, u_ac_em_ = -1, u_ac_int_ = -1, u_ac_alpha_ = -1, u_ac_livery_ = -1;
    GLint u_ac_unlit_ = -1, u_ac_cabin_glass_ = -1, u_ac_glass_weather_ = -1;
    GLint u_ac_glass_flow_f_ = -1, u_ac_glass_flow_s_ = -1;
    GLint u_ac_glass_fx_ = -1, u_ac_plane_wx_ = -1, u_ac_dmg_zones_ = -1, u_ac_dmg_extra_ = -1;
    GLint u_ac_baked_reflection_ = -1, u_ac_detail_ = -1;
    GLint u_ac_native_output_ = -1, u_ac_output_exposure_ = -1, u_ac_output_lut_ = -1;
    GlbMeshData ext_mesh_{};
    GlbMeshData ckpt_mesh_{};
    GLuint windshield_rain_front_tex_ = 0, windshield_rain_side_tex_ = 0;

    GLuint hud_prog_ = 0;
    GLuint hud_font_tex_ = 0;
    GLint u_hud_scr_ = -1;
    GLuint hud_vao_ = 0;
    GLuint hud_vbo_ = 0;
    size_t hud_vbo_cap_ = 0;
    std::vector<HudVert> hud_verts_;
    std::vector<WeatherFlowTrace> weather_flow_traces_;
    double weather_flow_last_time_s_ = -1.0;
    double weather_flow_origin_e_ = 0.0, weather_flow_origin_n_ = 0.0;
    double ap_cmd_bank_deg_ = 0.0;
    GLuint avionics_tex_ = 0;
    int avionics_tex_w_ = 0, avionics_tex_h_ = 0;
    std::vector<uint8_t> avionics_base_rgba_;
    std::vector<uint8_t> avionics_work_rgba_;
    std::string avionics_last_key_;
    float avionics_last_upd_s_ = -1.0f;
    GLuint panel_face_tex_ = 0;
    int panel_face_w_ = 0, panel_face_h_ = 0;
    std::vector<uint8_t> davtron_base_rgba_;
    std::vector<uint8_t> davtron_work_rgba_;
    std::string davtron_last_key_;
    float davtron_last_upd_s_ = -1.0f;
    GLuint steam_dials_tex_ = 0;
    int steam_dials_w_ = 0, steam_dials_h_ = 0;
    std::vector<uint8_t> steam_base_rgba_;
    std::vector<uint8_t> steam_cell_rgba_;
    std::string steam_vor_key_, steam_eng_key_, steam_kol_key_, steam_tc_key_, steam_hobbs_key_;
    float steam_last_upd_s_ = -1.0f;
    int xpdr_edit_pos_ = 0;
    Mat4 last_ckpt_vp_{};
    Mat4 last_ckpt_root_{};
    std::vector<Mat4> last_ckpt_world_m_;
    bool last_ckpt_valid_ = false;
    bool mouse_starter_cranking_ = false;
    bool cockpit_hotspot_hovered_ = false;
    int vor_cycle_idx_ = 0;
};

} // namespace nsim
