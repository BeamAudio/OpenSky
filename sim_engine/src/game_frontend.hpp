#pragma once

#include "aircraft_hud.hpp"
#include "gl_loader.hpp"
#include "math3d.hpp"
#include "pack_reader.hpp"
#include "platform_window.hpp"
#include "regional_atmosphere.hpp"

#include <cstdint>
#include <future>
#include <optional>
#include <string>
#include <vector>

namespace nsim {
class SkyAndClouds;

class GameFrontend {
public:
    struct Location {
        double lat = 0, lon = 0, alt = 0, heading = 0, pitch = 0;
    };
    struct FlightLogEntry {
        std::string dep_id, arr_id, grade;
        double minutes = 0.0, distance_nm = 0.0, fuel_gal = 0.0;
        double touchdown_fpm = 0.0, crosswind_kt = 0.0;
    };
    struct PassportStamp {
        std::string airport_id;
        int visits = 0;
        double best_fpm = 9999.0;
    };
    enum class Request { None, Walk, Board, Park, Editor, Quit };

    ~GameFrontend();
    bool init(const PackReader &pack, const std::string &shader_root, const std::string &asset_root);
    void draw_menu(AircraftAndHud &aircraft, SkyAndClouds &sky, const PlatformWindow &win,
                   bool preparing_airfield, int &time_of_day,
                   RegionalAtmosphere *reg_atmo = nullptr, double sim_time_sec = 0.0);
    void draw_walk(AircraftAndHud &aircraft, const PlatformWindow &win,
                   const RegionalAtmosphere *reg_atmo = nullptr, const AtmosphereState *atmo = nullptr,
                   double sim_time_sec = 0.0);
    void draw_navigation(AircraftAndHud &aircraft, const PlatformWindow &win);
    void draw_loading_screen(AircraftAndHud &aircraft, int w, int h,
                             const char *title, const char *stage, float progress01);
    void toggle_tutorial() { tutorial_open_ = !tutorial_open_; }
    bool tutorial_open() const { return tutorial_open_; }
    void toggle_kneeboard() { kneeboard_open_ = !kneeboard_open_; }
    bool kneeboard_open() const { return kneeboard_open_; }
    void step_walk(const PlatformWindow &win, double dt);
    void step_flight(AircraftAndHud &aircraft, double dt);
    void open_airport(const AirportInfo &airport, const LocalAirSample *parked_weather = nullptr);
    void board(AircraftAndHud &aircraft, double ambient_temp_c = 15.0);
    void park(AircraftAndHud &aircraft, const AirportInfo &airport);
    void save_profile() const;

    const AirportInfo *parked_airport() const;
    bool can_park(const AircraftAndHud &aircraft) const;
    const Location &walker() const { return walker_; }
    const Location &parking_spot() const { return parking_spot_; }
    bool walking() const { return walking_; }
    bool inspecting() const { return inspecting_; }
    bool active_flight() const { return flight_active_; }
    Request take_request();

private:
    const AirportInfo *home_airport() const;
    bool is_full_service_airport(const std::string &id) const;
    enum class Page { World, Logbook, Settings, Controls };
    enum class WalkStation { Nose = 0, LeftWing, RightWing, Gear, Tail, Door };
    struct RoutePoint { std::string label; double lat, lon; };
    struct RegionMapPreview {
        double center_lat = 0.0, center_lon = 0.0;
        float zoom = 0.0f;
        // RG: sampled elevation in metres and installed-region coverage mask.
        std::vector<float> elevation_rg;
    };
    void draw_world(AircraftAndHud &aircraft, const PlatformWindow &win,
                    RegionalAtmosphere *reg_atmo, const AtmosphereState *atmo, double sim_time_sec);
    void draw_logbook(AircraftAndHud &aircraft);
    void draw_settings(AircraftAndHud &aircraft, int &time_of_day, RegionalAtmosphere *reg_atmo);
    void draw_controls(AircraftAndHud &aircraft);
    void load_profile();
    void focus_airport(const AirportInfo &airport, float zoom);
    void add_point(RoutePoint point);
    float distance_to_plane() const;
    WalkStation nearest_walk_station(float &out_dist_m) const;
    PassportStamp *find_stamp(const std::string &id);
    const PassportStamp *find_stamp(const std::string &id) const;

    void read_pointer(const PlatformWindow &win);
    bool button(AircraftAndHud &ui, PixelRect area, const char *label, bool enabled = true, bool selected = false);
    bool contains(PixelRect area) const;
    void label(AircraftAndHud &ui, float x, float y, float size, const char *text, bool muted = false);
    void panel(AircraftAndHud &ui, PixelRect area);
    void draw_globe();
    void update_region_map_preview();

    const PackReader *pack_ = nullptr;
    GLuint backdrop_ = 0, land_ = 0, bounds_ = 0, vao_ = 0, font_ = 0, relief_ = 0, region_map_ = 0;
    GLint globe_uniform_ = -1, region_count_uniform_ = -1, aspect_uniform_ = -1, relief_bounds_uniform_ = -1,
          region_map_ready_uniform_ = -1;
    Vec4 relief_bounds_{};
    int region_count_ = 0;
    Page page_ = Page::World;
    Request request_ = Request::None;
    Vec2 pointer_{};
    bool click_ = false;
    bool map_dragging_ = false;
    double globe_lat_ = 42.5, globe_lon_ = 0.48;
    float globe_zoom_ = 1.0f;
    int airport_page_ = 0, route_page_ = 0, passport_page_ = 0;
    std::string parked_id_, selected_id_;
    std::vector<RoutePoint> route_;
    int model_history_page_ = 0;
    std::future<RegionMapPreview> region_map_future_;
    RegionMapPreview region_map_preview_;
    bool region_map_preview_dirty_ = true;
    size_t active_leg_ = 0;
    bool add_waypoint_ = false;
    bool walking_ = false, inspecting_ = false, flight_active_ = false;
    WalkStation active_station_ = WalkStation::Door;
    bool walk_checked_[6] = {};
    bool tutorial_open_ = true;
    bool kneeboard_open_ = false;
    Location walker_{}, parking_spot_{};
    bool has_custom_park_ = false;
    AircraftCareSnapshot care_{};
    double hours_ = 0.0;
    int64_t last_park_epoch_ = 0;
    double remote_fuel_added_ = 0.0; // Caps remote-strip jerrycan refills per visit
    // Flight tracking & touchdown debrief
    std::string dep_airport_id_;
    double flight_elapsed_s_ = 0.0, flight_airborne_total_s_ = 0.0;
    double flight_dist_m_ = 0.0, flight_start_fuel_ = 0.0;
    double prev_lat_ = 0.0, prev_lon_ = 0.0;
    double airborne_seconds_ = 0.0;
    double last_sink_ = 0.0, last_g_ = 1.0, save_elapsed_ = 0.0;
    bool was_grounded_ = true;
    bool has_touchdown_ = false;
    double td_fpm_ = 0.0, td_xwind_kt_ = 0.0, td_g_ = 1.0, td_banner_s_ = 0.0;
    std::string td_surface_, td_grade_;
    // Pilot Logbook & Aragon Airfield Passport
    int total_landings_ = 0;
    double best_landing_fpm_ = 9999.0;
    std::vector<PassportStamp> passport_;
    std::vector<FlightLogEntry> logbook_;
    std::string status_;
    std::string walk_notice_;
};

} // namespace nsim
