#pragma once

#include "aircraft_hud.hpp"
#include "aircraft_session.hpp"
#include "gl_loader.hpp"
#include "math3d.hpp"
#include "pack_reader.hpp"
#include "platform_window.hpp"
#include "regional_atmosphere.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace nsim {
class SkyAndClouds;

class GameFrontend {
public:
    using Location = AircraftLocation;
    using FlightLogEntry = FleetFlightLogEntry;
    using PassportStamp = FleetPassportStamp;
    enum class Request { None, Walk, Board, Park, Editor, WatchReplay, Quit };
    enum class ReplayAction { None, TogglePlay, Speed05, Speed1, Speed2, Speed4, Back10, Fwd10, SwitchView, Exit };

    ~GameFrontend();
    bool init(const PackReader &pack, const std::string &shader_root, const std::string &asset_root,
              std::filesystem::path profile_file, const AircraftPackage &aircraft);
    void draw_menu(AircraftAndHud &aircraft, SkyAndClouds &sky, const PlatformWindow &win,
                   bool preparing_airfield, int &time_of_day,
                   RegionalAtmosphere *reg_atmo = nullptr, double sim_time_sec = 0.0);
    void focus_map(const AirportInfo &airport) { focus_airport(airport, 48.0f); }
    bool capture_world(AircraftAndHud &aircraft, SkyAndClouds &sky, const PlatformWindow &win,
                       int &time_of_day, RegionalAtmosphere *weather, const std::string &path);
    void draw_walk(AircraftAndHud &aircraft, const PlatformWindow &win,
                   const RegionalAtmosphere *reg_atmo = nullptr, const AtmosphereState *atmo = nullptr,
                   double sim_time_sec = 0.0);
    void draw_navigation(AircraftAndHud &aircraft, const PlatformWindow &win);
    ReplayAction draw_replay_bar(AircraftAndHud &aircraft, const PlatformWindow &win,
                                 const std::string &title, double cur_t, double total_t,
                                 bool playing, float speed_mult, int view_mode,
                                 double *out_seek_t = nullptr);
    void set_replay_available(bool available) { replay_available_ = available; }
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
    const AircraftCareSnapshot &care() const { return session_.profile().care_; }
    bool can_park(const AircraftAndHud &aircraft) const;
    const Location &walker() const { return walker_; }
    const Location &parking_spot() const { return session_.profile().parking_spot_; }
    bool walking() const { return walking_; }
    bool inspecting() const { return inspecting_; }
    bool active_flight() const { return session_.flight_active_; }
    Request take_request();

private:
    const AirportInfo *home_airport() const;
    bool is_full_service_airport(const std::string &id) const;
    enum class Page { World, Logbook, Settings, Controls, Credits };
    enum class WalkStation { Nose = 0, LeftWing, RightWing, Gear, Tail, Door };
    using RoutePoint = FleetRoutePoint;
    void draw_world(AircraftAndHud &aircraft, const PlatformWindow &win,
                    RegionalAtmosphere *reg_atmo, const AtmosphereState *atmo, double sim_time_sec);
    void draw_logbook(AircraftAndHud &aircraft);
    void draw_settings(AircraftAndHud &aircraft, int &time_of_day, RegionalAtmosphere *reg_atmo);
    void draw_controls(AircraftAndHud &aircraft);
    void draw_credits(AircraftAndHud &aircraft);
    void draw_first_flight_tutorial(AircraftAndHud &aircraft);
    void finish_first_flight_tutorial();
    void load_profile();
    void focus_airport(const AirportInfo &airport, float zoom);
    void add_point(RoutePoint point);
    float distance_to_plane() const;
    WalkStation nearest_walk_station(float &out_dist_m) const;
    const PassportStamp *find_stamp(const std::string &id) const;

    void read_pointer(const PlatformWindow &win);
    bool button(AircraftAndHud &ui, PixelRect area, const char *label, bool enabled = true, bool selected = false);
    bool contains(PixelRect area) const;
    void label(AircraftAndHud &ui, float x, float y, float size, const char *text, bool muted = false);
    void panel(AircraftAndHud &ui, PixelRect area);
    struct RegionWeatherPreview {
        int layer = -1;
        double lon_min = 0, lat_min = 0, lon_max = 0, lat_max = 0;
        std::unique_ptr<WeatherLibrary> lib;
        bool terrain_ready = false;
        std::vector<float> slope_e, slope_n, ridge, basin;
    };
    void sync_region_weather_previews(const RegionalAtmosphere *reg_atmo);
    void draw_globe(const RegionalAtmosphere *reg_atmo, double sim_time_sec);

    const PackReader *pack_ = nullptr;
    GLuint backdrop_ = 0, land_ = 0, bounds_ = 0, vao_ = 0, font_ = 0, relief_ = 0, region_wx_ = 0;
    GLint globe_uniform_ = -1, region_count_uniform_ = -1, aspect_uniform_ = -1;
    GLint wx_geo_uniform_ = -1, wx_overlay_uniform_ = -1, wx_time_uniform_ = -1, wx_base_cloud_uniform_ = -1;
    std::vector<RegionWeatherPreview> region_wx_previews_;
    int wx_preview_state_key_ = -1;
    int region_count_ = 0;
    Page page_ = Page::World;
    Request request_ = Request::None;
    Vec2 pointer_{};
    bool click_ = false;
    bool map_dragging_ = false;
    double globe_lat_ = 42.5, globe_lon_ = 0.48;
    float globe_zoom_ = 1.0f;
    int airport_page_ = 0, place_page_ = 0, route_page_ = 0, passport_page_ = 0;
    std::string selected_id_;
    std::string selected_place_id_;
    bool show_places_ = false;
    AircraftSession session_;
    int model_history_page_ = 0;
    bool show_outlook_ = false;
    bool weather_overlay_ = true;
    bool add_waypoint_ = false;
    bool walking_ = false, inspecting_ = false;
    WalkStation active_station_ = WalkStation::Door;
    bool walk_checked_[6] = {};
    bool tutorial_open_ = false;
    bool first_flight_open_ = false;
    bool first_flight_completed_ = false;
    int first_flight_step_ = 0;
    std::filesystem::path first_flight_state_file_;
    bool kneeboard_open_ = false;
    bool replay_available_ = false;
    Location walker_{};
    std::string status_;
    std::string walk_notice_;
};

} // namespace nsim
