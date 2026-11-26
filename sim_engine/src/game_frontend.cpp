#include "game_frontend.hpp"

#include "aircraft_hud.hpp"
#include "frontend_font_metrics.hpp"
#include "../third_party/stb_image.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <utility>

namespace nsim {
namespace {
constexpr PixelRect map_area{64, 225, 1200, 750};
constexpr int w = GAME_OUTPUT_W, h = GAME_OUTPUT_H;
constexpr double nm_m = 1852.0;
constexpr float globe_radius = 0.46f;

struct WorldPlace {
    const char *id;
    const char *name;
    const char *kind;
    const char *note;
    double lat, lon;
    bool scenic;
};

constexpr WorldPlace kWorldPlaces[] = {
    {"zaragoza_pilar", "Basílica del Pilar", "Landmark", "Zaragoza basilica beside the Ebro riverfront.", 41.6569, -0.8787, false},
    {"zaragoza_bridge", "Puente de Piedra", "Landmark", "Historic stone bridge across the Ebro.", 41.6578, -0.8755, false},
    {"zaragoza_tower", "Torre del Agua", "Landmark", "Tall landmark on Zaragoza's Expo riverfront.", 41.6693, -0.9073, false},
    {"huesca_cathedral", "Catedral de Huesca", "Landmark", "Cathedral tower above Huesca's old town.", 42.1404, -0.4086, false},
    {"loarre_castle", "Castillo de Loarre", "Landmark", "Romanesque hilltop castle north of Huesca.", 42.3257, -0.6119, false},
    {"mediano_tower", "Torre de Mediano", "Scenic landmark", "The old bell tower rises from Embalse de Mediano.", 42.3175, 0.2037, true},
    {"barcelona_sagrada_familia", "Sagrada Família", "Landmark", "Gaudí's soaring basilica in central Barcelona.", 41.40369, 2.17433, false},
    {"montserrat_abbey", "Abbey of Montserrat", "Scenic landmark", "Mountain monastery set among Montserrat's distinctive peaks.", 41.59361, 1.83777, true},
    {"girona_cathedral", "Girona Cathedral", "Landmark", "Cathedral above Girona's medieval old town.", 41.98737, 2.826259, false},
    {"tarragona_amphitheatre", "Roman Amphitheatre", "Landmark", "Roman arena beside the Mediterranean in Tarragona.", 41.1145413, 1.2587789, false},
    {"lleida_seu_vella", "La Seu Vella", "Landmark", "Old cathedral overlooking Lleida from the hilltop.", 41.618143, 0.626891, false},
    {"ebro_delta", "Ebro Delta", "Scenic landmark", "Broad wetlands and rice fields at the Ebro's Mediterranean mouth.", 40.7167, 0.7333, true},
};

const WorldPlace *find_world_place(const std::string &id) {
    for (const auto &place : kWorldPlaces) if (id == place.id) return &place;
    return nullptr;
}

bool inside(PixelRect box, Vec2 p) {
    return p.x >= box.x && p.x < box.x + box.w && p.y >= box.y && p.y < box.y + box.h;
}

double bearing(double a_lat, double a_lon, double b_lat, double b_lon) {
    const double a = a_lat * DEG2RAD_D, b = b_lat * DEG2RAD_D;
    const double d = (b_lon - a_lon) * DEG2RAD_D;
    return std::fmod(360.0 + std::atan2(std::sin(d) * std::cos(b),
        std::cos(a) * std::sin(b) - std::sin(a) * std::cos(b) * std::cos(d)) * RAD2DEG_D, 360.0);
}

const char *wind_cardinal(float degrees) {
    static constexpr const char *directions[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    const int index = static_cast<int>(std::lround(std::fmod(degrees + 360.0f, 360.0f) / 45.0f)) % 8;
    return directions[index];
}

Vec3 globe_xyz(double lat_deg, double lon_deg) {
    const double lat = lat_deg * DEG2RAD_D, lon = lon_deg * DEG2RAD_D;
    return {float(std::cos(lat) * std::cos(lon)), float(std::cos(lat) * std::sin(lon)), float(std::sin(lat))};
}

std::pair<double, double> globe_geo(Vec3 p) {
    return {std::asin(std::clamp(double(p.z), -1.0, 1.0)) * RAD2DEG_D,
            std::atan2(double(p.y), double(p.x)) * RAD2DEG_D};
}

struct GlobeProjection {
    Vec3 forward, east, north;
    Vec2 centre{map_area.x + map_area.w * .5f, map_area.y + map_area.h * .5f};
    float radius;
    GlobeProjection(double lat, double lon, float zoom)
        : forward(globe_xyz(lat, lon)), east(globe_xyz(0, lon + 90)),
          north(cross(forward, east)), radius(map_area.h * globe_radius * zoom) {}
    std::optional<Vec2> project(Vec3 p) const {
        if (dot(forward, p) < 0) return {};
        return centre + Vec2{dot(east, p), -dot(north, p)} * radius;
    }
    std::optional<Vec3> unproject(Vec2 p) const {
        const Vec2 v = (p - centre) * (1.0f / radius);
        const float r = v.x * v.x + v.y * v.y;
        if (r > 1.0f) return {};
        return east * v.x - north * v.y + forward * std::sqrt(1.0f - r);
    }
};

} // namespace

GameFrontend::~GameFrontend() {
    if (vao_) glDeleteVertexArrays(1, &vao_);
    if (bounds_) glDeleteBuffers(1, &bounds_);
    if (land_) glDeleteTextures(1, &land_);
    if (font_) glDeleteTextures(1, &font_);
    if (relief_) glDeleteTextures(1, &relief_);
    if (region_wx_) glDeleteTextures(1, &region_wx_);
    if (backdrop_) glDeleteProgram(backdrop_);
}

bool GameFrontend::init(const PackReader &pack, const std::string &shader_root, const std::string &asset_root,
                        std::filesystem::path profile_file, const AircraftPackage &aircraft) {
    pack_ = &pack;
    if (!profile_file.empty()) {
        first_flight_state_file_ = profile_file.parent_path() / "first_flight.ini";
        std::ifstream tutorial_state(first_flight_state_file_);
        std::string key;
        int value = 0;
        while (tutorial_state >> key >> value) {
            if (key == "completed") first_flight_completed_ = value != 0;
        }
        first_flight_open_ = !first_flight_completed_;
    }
    backdrop_ = compile_program_files(shader_root + "/fullscreen.vert", shader_root + "/frontend.frag");
    if (!backdrop_) return false;
    globe_uniform_ = glGetUniformLocation(backdrop_, "u_globe");
    region_count_uniform_ = glGetUniformLocation(backdrop_, "u_region_count");
    aspect_uniform_ = glGetUniformLocation(backdrop_, "u_aspect");
    wx_geo_uniform_ = glGetUniformLocation(backdrop_, "u_wx_geo");
    wx_overlay_uniform_ = glGetUniformLocation(backdrop_, "u_wx_overlay");
    wx_time_uniform_ = glGetUniformLocation(backdrop_, "u_wx_time");
    wx_base_cloud_uniform_ = glGetUniformLocation(backdrop_, "u_wx_base_cloud");
    int tw, th, channels;
    unsigned char *pixels = stbi_load((asset_root + "/ui/globe_land.png").c_str(), &tw, &th, &channels, 1);
    if (!pixels) {
        std::fprintf(stderr, "[WORLD] Could not load the globe land mask\n");
        return false;
    }
    glCreateTextures(GL_TEXTURE_2D, 1, &land_);
    glTextureStorage2D(land_, 1, GL_R8, tw, th);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTextureSubImage2D(land_, 0, 0, 0, tw, th, GL_RED, GL_UNSIGNED_BYTE, pixels);
    stbi_image_free(pixels);
    glTextureParameteri(land_, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(land_, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(land_, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTextureParameteri(land_, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    pixels = stbi_load((asset_root + "/ui/frontend_font.png").c_str(), &tw, &th, &channels, 4);
    if (!pixels) return false;
    glCreateTextures(GL_TEXTURE_2D, 1, &font_);
    glTextureStorage2D(font_, 1, GL_RGBA8, tw, th);
    glTextureSubImage2D(font_, 0, 0, 0, tw, th, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    stbi_image_free(pixels);
    glTextureParameteri(font_, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(font_, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    constexpr int relief_width = 1536, relief_height = 1024, mip_levels = 11;
    std::vector<Vec4> bounds;
    int layer_count = 0;
    int wx_layer_count = 0;
    region_wx_previews_.clear();
    wx_preview_state_key_ = -1;
    for (const auto &region : pack.regions()) {
        const auto &b = region.hdr;
        auto path = std::filesystem::path(region.pack_path).replace_extension(".relief.png");
        bounds.emplace_back(b.lon_min, b.lat_min, b.lon_max, b.lat_max);
        std::error_code ec;
        const int layer = std::filesystem::is_regular_file(path, ec) ? layer_count++ : -1;
        int wx_layer = -1;
        if (!region.weather_path.empty()) {
            auto lib = std::make_unique<WeatherLibrary>();
            if (lib->load(region.weather_path)) {
                wx_layer = wx_layer_count++;
                RegionWeatherPreview preview;
                preview.layer = wx_layer;
                preview.lon_min = b.lon_min;
                preview.lat_min = b.lat_min;
                preview.lon_max = b.lon_max;
                preview.lat_max = b.lat_max;
                preview.lib = std::move(lib);
                region_wx_previews_.push_back(std::move(preview));
            }
        }
        bounds.emplace_back(static_cast<float>(layer), static_cast<float>(wx_layer), 0, 0);
    }
    region_count_ = static_cast<int>(pack.regions().size());
    if (wx_layer_count > 0) {
        glCreateTextures(GL_TEXTURE_2D_ARRAY, 1, &region_wx_);
        glTextureStorage3D(region_wx_, 1, GL_RGBA16F, 256, 192, wx_layer_count);
        glTextureParameteri(region_wx_, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTextureParameteri(region_wx_, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri(region_wx_, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(region_wx_, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTextureParameteri(region_wx_, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    }
    if (layer_count > 0) {
        glCreateTextures(GL_TEXTURE_2D_ARRAY, 1, &relief_);
        glTextureStorage3D(relief_, mip_levels, GL_RGBA8, relief_width, relief_height, layer_count);
        glTextureParameteri(relief_, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTextureParameteri(relief_, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri(relief_, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(relief_, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        for (const auto &region : pack.regions()) {
            Vec4 &layer_entry = bounds[2 * region.idx + 1];
            const int layer = static_cast<int>(layer_entry.x);
            if (layer < 0) continue;
            auto path = std::filesystem::path(region.pack_path).replace_extension(".relief.png");
            const auto name = path.string();
            if (!stbi_info(name.c_str(), &tw, &th, &channels) ||
                tw != relief_width || th != relief_height ||
                !(pixels = stbi_load(name.c_str(), &tw, &th, &channels, 4))) {
                layer_entry.x = -1;
                std::fprintf(stderr, "[WORLD] Invalid relief image %s\n", name.c_str());
                continue;
            }
            glTextureSubImage3D(relief_, 0, 0, 0, layer, tw, th, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
            stbi_image_free(pixels);
        }
        glGenerateTextureMipmap(relief_);
    }
    if (bounds.empty()) bounds.emplace_back();
    glCreateBuffers(1, &bounds_);
    glNamedBufferStorage(bounds_, bounds.size() * sizeof(Vec4), bounds.data(), 0);
    glCreateVertexArrays(1, &vao_);

    session_.init(pack, std::move(profile_file), aircraft);
    if (const auto *parked = parked_airport()) focus_airport(*parked, 1.0f);
    return true;
}

void GameFrontend::save_profile() const {
    session_.save_profile();
}

const AirportInfo *GameFrontend::parked_airport() const {
    return session_.profile().parked_id_.empty() ? nullptr : pack_->find_airport(session_.profile().parked_id_);
}

const AirportInfo *GameFrontend::home_airport() const {
    if (const auto *home = pack_->find_airport("ES-0205")) return home;
    return pack_->find_airport("LECS");
}

bool GameFrontend::is_full_service_airport(const std::string &id) const {
    return AircraftSession::full_service(id);
}

const GameFrontend::PassportStamp *GameFrontend::find_stamp(const std::string &id) const {
    for (const auto &s : session_.profile().passport_) if (s.airport_id == id) return &s;
    return nullptr;
}

GameFrontend::Request GameFrontend::take_request() {
    return std::exchange(request_, Request::None);
}

void GameFrontend::focus_airport(const AirportInfo &airport, float zoom) {
    selected_id_ = airport.id;
    selected_place_id_.clear();
    globe_lat_ = airport.lat;
    globe_lon_ = airport.lon;
    globe_zoom_ = zoom;
}

void GameFrontend::add_point(RoutePoint point) {
    if (session_.profile().route_.size() >= max_route_points) {
        status_ = "Route is full. Remove a waypoint first.";
        return;
    }
    if (session_.add_route_point(std::move(point))) {
        route_page_ = static_cast<int>((session_.profile().route_.size() - 1) / 2);
        status_.clear();
    }
}

namespace {
} // namespace

void GameFrontend::open_airport(const AirportInfo &ap, const LocalAirSample *parked_weather) {
    double mlat, mlon;
    nspain::meters_per_degree(ap.lat, mlat, mlon);
    walk_notice_ = session_.begin_walkaround(ap, parked_weather);
    for (bool &c : walk_checked_) c = false;

    const double walk_angle = (session_.profile().parking_spot_.heading - 55.0) * DEG2RAD_D;
    walker_.lat = session_.profile().parking_spot_.lat + 10.5 * std::cos(walk_angle) / mlat;
    walker_.lon = session_.profile().parking_spot_.lon + 10.5 * std::sin(walk_angle) / mlon;
    walker_.alt = pack_->sample_elevation(walker_.lat, walker_.lon) + 1.68;
    walker_.heading = bearing(walker_.lat, walker_.lon, session_.profile().parking_spot_.lat, session_.profile().parking_spot_.lon);
    walker_.pitch = -4;
    walking_ = true;
    inspecting_ = false;
    status_.clear();
}

float GameFrontend::distance_to_plane() const {
    return static_cast<float>(nspain::haversine_m(walker_.lat, walker_.lon, session_.profile().parking_spot_.lat, session_.profile().parking_spot_.lon));
}

GameFrontend::WalkStation GameFrontend::nearest_walk_station(float &out_dist_m) const {
    double mlat, mlon;
    nspain::meters_per_degree(session_.profile().parking_spot_.lat, mlat, mlon);
    const double dn = (walker_.lat - session_.profile().parking_spot_.lat) * mlat;
    const double de = (walker_.lon - session_.profile().parking_spot_.lon) * mlon;
    const double hdg = session_.profile().parking_spot_.heading * DEG2RAD_D;
    // Aircraft-local frame: fwd (+nose / -tail), right (+starboard / -port)
    const double fwd = dn * std::cos(hdg) + de * std::sin(hdg);
    const double right = de * std::cos(hdg) - dn * std::sin(hdg);
    struct Pt { WalkStation st; double f, r; };
    static constexpr Pt pts[] = {
        {WalkStation::Nose,      +2.6,  0.0},
        {WalkStation::LeftWing,  +0.2, -3.2},
        {WalkStation::RightWing, +0.2, +3.2},
        {WalkStation::Gear,      +0.6, +1.2},
        {WalkStation::Gear,      +0.6, -1.2},
        {WalkStation::Tail,      -3.8,  0.0},
        {WalkStation::Door,      -0.4, -1.0},
    };
    WalkStation best = WalkStation::Door;
    double best_d = 1e9;
    for (const auto &p : pts) {
        const double d = std::hypot(fwd - p.f, right - p.r);
        if (d < best_d) { best_d = d; best = p.st; }
    }
    out_dist_m = static_cast<float>(best_d);
    return best;
}

void GameFrontend::step_walk(const PlatformWindow &win, double dt) {
    if (win.key_pressed(KEY_ESC)) {
        if (inspecting_) inspecting_ = false;
        else walking_ = false;
        return;
    }
    if (win.key_pressed(KEY_E) && distance_to_plane() < 9.0f) {
        if (!inspecting_) {
            float sd = 0.0f;
            active_station_ = nearest_walk_station(sd);
            walk_checked_[static_cast<int>(active_station_)] = true;
        }
        inspecting_ = !inspecting_;
    }
    if (inspecting_) {
        read_pointer(win);
        if ((!win.mouse_left_down() && !win.mouse_right_down()) || contains({1160, 145, 720, 795})) return;
    }
    if (win.mouse_left_down() || win.mouse_right_down()) {
        walker_.heading = std::fmod(walker_.heading + win.mouse_dx() * .19 + 360.0, 360.0);
        walker_.pitch = std::clamp(walker_.pitch - win.mouse_dy() * .16, -75.0, 75.0);
    }
    if (inspecting_) return;
    walker_.heading = std::fmod(walker_.heading +
        (int(win.key_down(KEY_RIGHT)) - int(win.key_down(KEY_LEFT))) * 80.0 * dt + 360.0, 360.0);
    walker_.pitch = std::clamp(walker_.pitch +
        (int(win.key_down(KEY_UP)) - int(win.key_down(KEY_DOWN))) * 65.0 * dt, -75.0, 75.0);
    const int f = int(win.key_down(KEY_W)) - int(win.key_down(KEY_S));
    const int s = int(win.key_down(KEY_D)) - int(win.key_down(KEY_A));
    if (!f && !s) return;
    const double travel = dt * (win.key_down(KEY_SHIFT) ? 3.2 : 1.65) / std::hypot(f, s);
    const double angle = walker_.heading * DEG2RAD_D;
    double mlat, mlon;
    nspain::meters_per_degree(walker_.lat, mlat, mlon);
    const double lat = walker_.lat + (f * std::cos(angle) - s * std::sin(angle)) * travel / mlat;
    const double lon = walker_.lon + (f * std::sin(angle) + s * std::cos(angle)) * travel / mlon;
    const double dist = nspain::haversine_m(lat, lon, session_.profile().parking_spot_.lat, session_.profile().parking_spot_.lon);
    if (dist > 180 || (dist < 1.9 && dist < distance_to_plane()) || pack_->is_near_building(lat, lon, 0.3)) return;
    walker_.lat = lat;
    walker_.lon = lon;
    walker_.alt = pack_->sample_elevation(lat, lon) + 1.68;
}

void GameFrontend::board(AircraftAndHud &aircraft, double ambient_temp_c) {
    walking_ = inspecting_ = false;
    kneeboard_open_ = !session_.profile().care_.engine_running;
    session_.board(aircraft, ambient_temp_c);
}

bool GameFrontend::can_park(const AircraftAndHud &aircraft) const {
    return session_.can_park(aircraft);
}

void GameFrontend::park(AircraftAndHud &aircraft, const AirportInfo &airport) {
    if (!session_.park(aircraft, airport)) return;
    kneeboard_open_ = false;
    focus_airport(airport, 30.0f);
    page_ = Page::World;
    aircraft.state().menu_open = aircraft.state().paused = true;
    open_airport(airport, &aircraft.last_air());
    session_.save_profile();
}

void GameFrontend::step_flight(AircraftAndHud &aircraft, double dt) {
    session_.step_flight(aircraft, dt);
}

void GameFrontend::read_pointer(const PlatformWindow &win) {
    const auto v = win.presentation_rect(w, h);
    pointer_ = {float(win.mouse_x() - v.x) * w / std::max(1, v.w),
                float(win.mouse_y() - v.y) * h / std::max(1, v.h)};
    click_ = win.mouse_left_clicked();
}

bool GameFrontend::contains(PixelRect area) const {
    return inside(area, pointer_);
}

void GameFrontend::label(AircraftAndHud &ui, float x, float y, float size, const char *value, bool muted) {
    const float scale = size / 52.0f;
    const float color = muted ? .70f : .95f;
    for (const auto *p = reinterpret_cast<const unsigned char *>(value); *p; ++p) {
        int ch = *p;
        if (ch == 0xc3 && p[1] >= 0x80 && p[1] <= 0xbf) ch = 0xc0 + (*++p - 0x80);
        const int i = ch < 127 && ch >= 32 ? ch - 32 : ch >= 192 && ch <= 255 ? 95 + ch - 192 : '?' - 32;
        if (ch != ' ') {
            const float u = (i % 16) / 16.0f, v = (i / 16) / 10.0f;
            const float x0 = x - 3 * scale, y0 = y - 6 * scale;
            const float x1 = x0 + 64 * scale, y1 = y0 + 72 * scale;
            const auto quad = [&](float px, float py, float tu, float tv) {
                return AircraftAndHud::HudVert{px, py, tu, tv, color, color + .025f, color + .04f, 1};
            };
            const auto a = quad(x0,y0,u,v), b = quad(x1,y0,u+1.0f/16,v);
            const auto c = quad(x1,y1,u+1.0f/16,v+.1f), d = quad(x0,y1,u,v+.1f);
            ui.hud_verts_.insert(ui.hud_verts_.end(), {a,b,c,a,c,d});
        }
        x += frontend_glyph_advance[i] * scale;
    }
}

void GameFrontend::panel(AircraftAndHud &ui, PixelRect a) {
    ui.hud_rect(a.x, a.y, a.w, a.h, .035f, .061f, .085f, .98f);
    ui.hud_border(a.x, a.y, a.w, a.h, 1, .20f, .33f, .39f, .9f);
    ui.hud_rect(a.x + 1, a.y + 1, a.w - 2, 2, .23f, .72f, .61f, .82f);
}

bool GameFrontend::button(AircraftAndHud &ui, PixelRect a, const char *value, bool enabled, bool selected) {
    const bool hover = enabled && contains(a);
    if (hover || (selected && enabled)) ui.hud_rect(a.x, a.y, a.w, a.h, .09f, .25f, .27f, 1);
    else ui.hud_rect(a.x, a.y, a.w, a.h, .055f, .095f, .125f, 1);
    ui.hud_border(a.x, a.y, a.w, a.h, 1, selected ? .32f : .22f, selected ? .72f : .37f, .44f, enabled ? .92f : .38f);
    if (selected && enabled) ui.hud_rect(a.x + 2, a.y + 1, a.w - 4, 2, .35f, .88f, .69f, 1);
    label(ui, a.x + 13, a.y + (a.h - 18) * .5f, 18, value, !enabled);
    if (!hover || !click_) return false;
    click_ = false;
    return true;
}

void GameFrontend::sync_region_weather_previews(const RegionalAtmosphere *reg_atmo) {
    if (!region_wx_ || region_wx_previews_.empty() || !pack_) return;
    int season = 1, slot = 2, wtype = 0, sev_bucket = 1;
    float solar = 0.45f;
    if (reg_atmo) {
        const auto &w = reg_atmo->weather();
        const auto &lib = reg_atmo->library();
        season = std::clamp(lib.valid() ? lib.snapshot().season : w.season, 0, nspain::NSWX_SEASONS - 1);
        slot = std::clamp(lib.valid() ? lib.snapshot().slot : w.slot, 0, nspain::NSWX_SLOTS - 1);
        wtype = reg_atmo->manual_weather_override()
            ? std::clamp(static_cast<int>(reg_atmo->regime()), 0, nspain::NSWX_TYPES - 1)
            : std::clamp(lib.valid() ? lib.snapshot().wtype : static_cast<int>(reg_atmo->regime()), 0, nspain::NSWX_TYPES - 1);
        sev_bucket = std::clamp(static_cast<int>(std::round(std::max(w.precip, w.storm_int) * 4.0f)), 0, 4);
        solar = (slot == nspain::NSWX_NIGHT_SLOT) ? 0.0f : std::sin(std::clamp((slot + 0.5f) / 10.0f, 0.0f, 1.0f) * 3.14159265f) * 0.72f;
    }
    const int key = (((season * 16 + slot) * 8 + wtype) * 8) + sev_bucket;
    if (key == wx_preview_state_key_) return;
    wx_preview_state_key_ = key;
    const float severity = sev_bucket * 0.25f;
    constexpr int kW = 256, kH = 192;
    constexpr size_t kCells = static_cast<size_t>(kW) * kH;
    constexpr int kSrcW = nspain::NSWX_GRID_W, kSrcH = nspain::NSWX_GRID_H;
    constexpr size_t kSrcCells = static_cast<size_t>(kSrcW) * kSrcH;
    std::vector<float> rgba(kCells * 4, 0.0f);
    auto smooth01 = [](float e0, float e1, float x) {
        float t = std::clamp((x - e0) / std::max(1e-5f, e1 - e0), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    };
    for (auto &preview : region_wx_previews_) {
        if (!preview.lib || !preview.lib->valid()) continue;
        // Lazily bake terrain gradients once per region for realistic valley/ridge flow deflection.
        if (!preview.terrain_ready) {
            std::vector<float> elev(kCells, 0.0f), horiz(kCells, 0.0f), sm(kCells, 0.0f);
            double mlat = 111320.0, mlon = 82000.0;
            nspain::meters_per_degree(0.5 * (preview.lat_min + preview.lat_max), mlat, mlon);
            const float dx = static_cast<float>(std::max(50.0, (preview.lon_max - preview.lon_min) * mlon / (kW - 1)));
            const float dy = static_cast<float>(std::max(50.0, (preview.lat_max - preview.lat_min) * mlat / (kH - 1)));
            for (int y = 0; y < kH; ++y) {
                const double lat = preview.lat_min + (preview.lat_max - preview.lat_min) * (static_cast<double>(y) / (kH - 1));
                for (int x = 0; x < kW; ++x) {
                    const double lon = preview.lon_min + (preview.lon_max - preview.lon_min) * (static_cast<double>(x) / (kW - 1));
                    elev[static_cast<size_t>(y) * kW + x] = static_cast<float>(pack_->sample_elevation(lat, lon));
                }
            }
            for (int y = 0; y < kH; ++y) {
                const size_t row = static_cast<size_t>(y) * kW;
                for (int x = 0; x < kW; ++x) {
                    float s = 0.0f;
                    for (int ox = -2; ox <= 2; ++ox) s += elev[row + static_cast<size_t>(std::clamp(x + ox, 0, kW - 1))];
                    horiz[row + x] = s * 0.2f;
                }
            }
            for (int y = 0; y < kH; ++y) {
                for (int x = 0; x < kW; ++x) {
                    float s = 0.0f;
                    for (int oy = -2; oy <= 2; ++oy)
                        s += horiz[static_cast<size_t>(std::clamp(y + oy, 0, kH - 1)) * kW + x];
                    sm[static_cast<size_t>(y) * kW + x] = s * 0.2f;
                }
            }
            preview.slope_e.assign(kCells, 0.0f);
            preview.slope_n.assign(kCells, 0.0f);
            preview.ridge.assign(kCells, 0.0f);
            preview.basin.assign(kCells, 0.0f);
            for (int y = 0; y < kH; ++y) {
                const int yd = std::max(0, y - 1), yu = std::min(kH - 1, y + 1);
                for (int x = 0; x < kW; ++x) {
                    const int xl = std::max(0, x - 1), xr = std::min(kW - 1, x + 1);
                    const size_t i = static_cast<size_t>(y) * kW + x;
                    const float ge = (sm[static_cast<size_t>(y) * kW + xr] - sm[static_cast<size_t>(y) * kW + xl]) / std::max(1.0f, dx * (xr - xl));
                    const float gn = (sm[static_cast<size_t>(yu) * kW + x] - sm[static_cast<size_t>(yd) * kW + x]) / std::max(1.0f, dy * (yu - yd));
                    preview.slope_e[i] = ge;
                    preview.slope_n[i] = gn;
                    const float sl = std::hypot(ge, gn);
                    const float rh = elev[i] - sm[i];
                    preview.ridge[i] = std::clamp(std::max(smooth01(0.025f, 0.20f, sl) * 0.72f, smooth01(35.0f, 220.0f, rh) * 0.62f), 0.0f, 1.0f);
                    preview.basin[i] = std::clamp(smooth01(25.0f, 180.0f, -rh) * (1.0f - smooth01(0.035f, 0.16f, sl)), 0.0f, 1.0f);
                }
            }
            preview.terrain_ready = true;
        }
        preview.lib->select(season, static_cast<float>(slot), wtype, severity);
        const auto &src = preview.lib->field_rgba();
        const auto &snap = preview.lib->snapshot();
        if (src.size() != kSrcCells * 4) continue;
        for (int y = 0; y < kH; ++y) {
            const float sy = static_cast<float>(y) / (kH - 1) * (kSrcH - 1);
            const int y0 = std::min(kSrcH - 2, static_cast<int>(sy));
            const float ty = sy - y0;
            for (int x = 0; x < kW; ++x) {
                const float sx = static_cast<float>(x) / (kW - 1) * (kSrcW - 1);
                const int x0 = std::min(kSrcW - 2, static_cast<int>(sx));
                const float tx = sx - x0;
                const size_t a = (static_cast<size_t>(y0) * kSrcW + x0) * 4;
                const size_t b = a + 4, c = a + static_cast<size_t>(kSrcW) * 4, d = c + 4;
                auto bilerp = [&](int ch) {
                    return std::lerp(std::lerp(src[a + ch], src[b + ch], tx),
                                     std::lerp(src[c + ch], src[d + ch], tx), ty);
                };
                const size_t i = static_cast<size_t>(y) * kW + x;
                float u = 0.0f, v = 0.0f;
                orographic_deflect_wind(bilerp(0), bilerp(1), preview.slope_e[i], preview.slope_n[i],
                                        preview.ridge[i], preview.basin[i], 0.9926f, solar, u, v);
                const float cloud = std::clamp(snap.cloud_cov + bilerp(3) * 0.35f, 0.0f, 1.0f);
                // Localize precipitation to denser cloud cores so regions do not tint as solid boxes.
                const float core = std::clamp((cloud - 0.58f) / 0.36f, 0.0f, 1.0f);
                rgba[i * 4 + 0] = u;
                rgba[i * 4 + 1] = v;
                rgba[i * 4 + 2] = std::clamp(snap.precip_mm_hr * core * core, 0.0f, 48.0f);
                rgba[i * 4 + 3] = cloud;
            }
        }
        glTextureSubImage3D(region_wx_, 0, 0, 0, preview.layer, kW, kH, 1, GL_RGBA, GL_FLOAT, rgba.data());
    }
}

void GameFrontend::draw_globe(const RegionalAtmosphere *reg_atmo, double /*sim_time_sec*/) {
    const PixelRect a = map_area;
    glViewport(a.x, h - a.y - a.h, a.w, a.h);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glUseProgram(backdrop_);
    glUniform1i(region_count_uniform_, region_count_);
    glUniform1f(aspect_uniform_, float(a.w) / a.h);
    glUniform3f(globe_uniform_, float(globe_lat_ * DEG2RAD_D), float(globe_lon_ * DEG2RAD_D), globe_radius * globe_zoom_);
    glBindTextureUnit(0, land_);
    glBindTextureUnit(1, relief_);
    Vec4 wx_geo{0.0f, 0.0f, 0.0f, 0.0f};
    const bool has_wx = weather_overlay_ && reg_atmo && reg_atmo->has_model() && reg_atmo->field_tex() != 0 && pack_;
    if (weather_overlay_) sync_region_weather_previews(reg_atmo);
    if (has_wx) {
        const Vec4 db = reg_atmo->domain_bounds();
        const double span_e = 1.0 / std::max(1e-6f, db.z);
        const double span_n = 1.0 / std::max(1e-6f, db.w);
        const double lon_min = pack_->anchor_lon() + db.x / pack_->anchor_m_lon();
        const double lat_min = pack_->anchor_lat() + db.y / pack_->anchor_m_lat();
        wx_geo = {static_cast<float>(lon_min), static_cast<float>(lat_min),
                  static_cast<float>(pack_->anchor_m_lon() / std::max(1.0, span_e)),
                  static_cast<float>(pack_->anchor_m_lat() / std::max(1.0, span_n))};
        glBindTextureUnit(2, reg_atmo->field_tex());
    }
    if (region_wx_) glBindTextureUnit(3, region_wx_);
    const bool show_flow = weather_overlay_ && (has_wx || region_wx_ != 0);
    static const double anim_t0 = PlatformWindow::now_sec();
    const float flow_time = static_cast<float>(PlatformWindow::now_sec() - anim_t0);
    if (wx_geo_uniform_ >= 0) glUniform4f(wx_geo_uniform_, wx_geo.x, wx_geo.y, wx_geo.z, wx_geo.w);
    if (wx_overlay_uniform_ >= 0) glUniform1f(wx_overlay_uniform_, show_flow ? 1.0f : 0.0f);
    if (wx_base_cloud_uniform_ >= 0) glUniform1f(wx_base_cloud_uniform_, has_wx ? reg_atmo->weather().cloud_cov : 0.35f);
    if (wx_time_uniform_ >= 0) glUniform1f(wx_time_uniform_, flow_time);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, bounds_);
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
}

void GameFrontend::draw_menu(AircraftAndHud &aircraft, SkyAndClouds &sky, const PlatformWindow &win,
                             bool preparing_airfield, int &time_of_day,
                             RegionalAtmosphere *reg_atmo, double sim_time_sec) {
    read_pointer(win);
    aircraft.poll_yoke();
    sky.begin_native_cockpit_pass();
    glClearColor(.022f, .039f, .058f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    aircraft.hud_verts_.clear();
    aircraft.hud_rect(0, 0, w, 82, .033f, .057f, .078f, 1);
    aircraft.hud_rect(0, 81, w, 1, .18f, .38f, .40f, .9f);
    aircraft.hud_rect(64, 18, 46, 46, .055f, .13f, .15f, 1);
    aircraft.hud_border(64, 18, 46, 46, 1, .28f, .68f, .61f, .9f);
    const Vec3 brand_accent{.42f, .91f, .72f};
    aircraft.hud_line({87, 25}, {101, 41}, 2.2f, brand_accent);
    aircraft.hud_line({101, 41}, {87, 57}, 2.2f, brand_accent);
    aircraft.hud_line({87, 57}, {73, 41}, 2.2f, brand_accent);
    aircraft.hud_line({73, 41}, {87, 25}, 2.2f, brand_accent);
    aircraft.hud_line({87, 31}, {87, 51}, 1.2f, {.77f, .86f, .80f});
    aircraft.hud_line({77, 41}, {97, 41}, 1.2f, {.77f, .86f, .80f});
    label(aircraft, 124, 18, 25, "OPENSKY");
    label(aircraft, 126, 48, 12, "VFR  /  FREE ROAM", true);
    const bool menu_enabled = !first_flight_open_;
    if (button(aircraft, {380, 17, 130, 48}, "World", menu_enabled, page_ == Page::World)) page_ = Page::World;
    if (button(aircraft, {520, 17, 152, 48}, "Logbook", menu_enabled, page_ == Page::Logbook)) page_ = Page::Logbook;
    if (button(aircraft, {682, 17, 140, 48}, "Settings", menu_enabled, page_ == Page::Settings)) page_ = Page::Settings;
    if (button(aircraft, {832, 17, 140, 48}, "Controls", menu_enabled, page_ == Page::Controls)) page_ = Page::Controls;
    if (button(aircraft, {982, 17, 106, 48}, "Credits", menu_enabled, page_ == Page::Credits)) page_ = Page::Credits;
    if (button(aircraft, {1754, 17, 102, 48}, "Quit", menu_enabled)) request_ = Request::Quit;
    if (session_.flight_active_) {
        if (button(aircraft, {1100, 17, 174, 48}, "Resume flight", menu_enabled, true) ||
            (menu_enabled && win.key_pressed(KEY_ESC)))
            aircraft.st_.menu_open = aircraft.st_.paused = false;
        const bool park_ok = can_park(aircraft);
        double re = 0.0, mu = 0.0, rough = 0.0;
        std::string rid;
        const bool on_rwy = aircraft.jsb_state().on_ground &&
                            pack_->query_runway(aircraft.state().lat_deg, aircraft.state().lon_deg, re, mu, rough, rid);
        if (button(aircraft, {1286, 17, 306, 48},
                   on_rwy ? "Taxi off runway to park" : "Park and step outside", park_ok && menu_enabled))
            request_ = Request::Park;
    } else if (button(aircraft, {1100, 17, 174, 48}, "Flight guide", menu_enabled)) {
        first_flight_step_ = 0;
        first_flight_open_ = true;
    }
#ifndef OPENSKY_PLAYER_PACKAGE
    if (button(aircraft, {1606, 17, 140, 48}, "Region tools", menu_enabled)) request_ = Request::Editor;
#endif
    if (first_flight_open_) {
        draw_first_flight_tutorial(aircraft);
    } else if (preparing_airfield) {
        panel(aircraft, {640, 444, 640, 164});
        label(aircraft, 668, 474, 36, "Preparing the airfield");
        label(aircraft, 668, 536, 20, "Loading ground detail around your parking spot.", true);
    } else if (page_ == Page::World) draw_world(aircraft, win, reg_atmo, &sky.atmo(), sim_time_sec);
    else if (page_ == Page::Logbook) draw_logbook(aircraft);
    else if (page_ == Page::Settings) draw_settings(aircraft, time_of_day, reg_atmo);
    else if (page_ == Page::Controls) draw_controls(aircraft);
    else draw_credits(aircraft);
    glViewport(0, 0, w, h);
    aircraft.flush_hud(w, h, font_);
}

void GameFrontend::finish_first_flight_tutorial() {
    first_flight_open_ = false;
    first_flight_completed_ = true;
    if (first_flight_state_file_.empty()) return;

    std::error_code ec;
    std::filesystem::create_directories(first_flight_state_file_.parent_path(), ec);
    const auto temp = first_flight_state_file_.string() + ".tmp";
    {
        std::ofstream out(temp, std::ios::trunc);
        if (!out) return;
        out << "completed 1\n";
        if (!out) return;
    }
    std::filesystem::rename(temp, first_flight_state_file_, ec);
    if (ec) {
        std::filesystem::remove(first_flight_state_file_, ec);
        ec.clear();
        std::filesystem::rename(temp, first_flight_state_file_, ec);
        if (ec) std::filesystem::remove(temp, ec);
    }
}

void GameFrontend::draw_first_flight_tutorial(AircraftAndHud &ui) {
    static constexpr const char *steps[] = {"WELCOME", "WORLD MAP", "WALKAROUND", "TAKEOFF", "PARKING"};
    static constexpr const char *titles[] = {
        "Explore at your own pace",
        "Plan without moving the aircraft",
        "Start with a walkaround",
        "Taxi, take off, and enjoy the view",
        "Park to save your new base"
    };
    static constexpr const char *body[][4] = {
        {"OpenSky is a Spain-based free-roam VFR simulator.",
         "Aragón and Catalunya ship as the baseline world.",
         "The World Editor can add compatible regions worldwide.",
         "Your Cessna 172S starts at Castejon de Sos (ES-0205)."},
        {"Drag the globe and use + or - to zoom. Select an airfield or",
         "landmark, then add airfields to a route if you want a plan.",
         "Browsing, selecting and route planning never move the aircraft.", ""},
        {"Choose Walk to aircraft | Walkaround from the World screen.",
         "W/A/S/D move, E inspects a nearby part, and Enter boards.",
         "Remove chocks, tie-downs and the pitot cover before taxiing.", ""},
        {"K runs the quick-start helper. X releases the brakes; W adds power.",
         "Q/E steer while taxiing. A/D roll; Up/Down control pitch.",
         "Taxi onto a clear runway and rotate at about 55 kt.", ""},
        {"Land at a suitable airfield, taxi fully clear, shut down with K,",
         "then press Esc and choose Park. A successful parking saves your",
         "temporary base. Leaving before parking keeps the last saved base.", ""}
    };
    static constexpr const char *tips[] = {
        "Use the World Editor to add regions beyond the Spanish baseline.",
        "Map browsing and route planning are safe: your aircraft stays put.",
        "The Controls page has the complete keyboard reference.",
        "Press T any time in flight to show the cockpit controls guide.",
        "Parking also preserves location, heading, condition, fuel and logbook."
    };

    first_flight_step_ = std::clamp(first_flight_step_, 0, 4);
    ui.hud_rect(0, 0, w, h, .006f, .014f, .022f, .72f);
    panel(ui, {460, 160, 1000, 760});
    label(ui, 492, 188, 16, "OPENSKY  /  FIRST FLIGHT GUIDE", true);
    label(ui, 492, 218, 30, "Your first flight, made simple");
    if (button(ui, {1312, 187, 116, 38}, "Skip guide")) finish_first_flight_tutorial();

    const float track_x = 492.0f, track_y = 266.0f, gap = 8.0f, segment_w = 181.0f;
    for (int i = 0; i < 5; ++i) {
        const float x = track_x + i * (segment_w + gap);
        ui.hud_rect(x, track_y, segment_w, 4,
                    i <= first_flight_step_ ? .27f : .11f,
                    i <= first_flight_step_ ? .78f : .18f,
                    i <= first_flight_step_ ? .65f : .23f, .92f);
    }

    panel(ui, {492, 292, 278, 564});
    char number[16];
    std::snprintf(number, sizeof(number), "%02d", first_flight_step_ + 1);
    label(ui, 530, 342, 78, number);
    label(ui, 532, 414, 15, "YOUR FLIGHT", true);
    label(ui, 532, 455, 22, steps[first_flight_step_]);
    ui.hud_rect(534, 502, 190, 1, .20f, .34f, .40f, .9f);
    // A small route motif gives each step a visible sense of progress.
    const Vec2 route[] = {{548, 625}, {592, 574}, {635, 604}, {684, 545}, {716, 565}};
    for (int i = 0; i + 1 < 5; ++i)
        ui.hud_line(route[i], route[i + 1], 3.0f,
                    i < first_flight_step_ ? Vec3{.42f, .91f, .72f} : Vec3{.20f, .38f, .43f});
    for (int i = 0; i < 5; ++i) {
        const float radius = i == first_flight_step_ ? 8.0f : 5.0f;
        const Vec3 c = i <= first_flight_step_ ? Vec3{.42f, .91f, .72f} : Vec3{.32f, .48f, .52f};
        ui.hud_rect(route[i].x - radius, route[i].y - radius, radius * 2, radius * 2,
                    c.x, c.y, c.z, .98f);
    }
    label(ui, 532, 705, 15, "PLAN  /  BOARD  /  FLY", true);
    label(ui, 532, 740, 15, "At your own pace.", true);

    label(ui, 812, 324, 28, titles[first_flight_step_]);
    for (int i = 0; i < 4; ++i) {
        if (body[first_flight_step_][i][0] != '\0')
            label(ui, 814, 382 + i * 34, 18, body[first_flight_step_][i], i == 3);
    }
    panel(ui, {812, 574, 616, 150});
    label(ui, 838, 598, 14, "KEEP IN MIND", true);
    label(ui, 838, 634, 17, tips[first_flight_step_]);
    if (first_flight_step_ == 4)
        label(ui, 838, 666, 16, "Your aircraft state and logbook are saved with the parked base.", true);
    else if (first_flight_step_ == 2)
        label(ui, 838, 666, 16, "Enter boards the aircraft after you finish inspecting it.", true);
    else if (first_flight_step_ == 3)
        label(ui, 838, 666, 16, "Open Controls from the menu for the complete key list.", true);

    ui.hud_rect(812, 776, 616, 1, .18f, .30f, .36f, .82f);
    label(ui, 812, 792, 15, "You can reopen this guide from the World screen.", true);
    if (button(ui, {812, 832, 154, 48}, "Previous", first_flight_step_ > 0)) --first_flight_step_;
    char progress[32];
    std::snprintf(progress, sizeof(progress), "STEP %d OF 5", first_flight_step_ + 1);
    label(ui, 1038, 848, 15, progress, true);
    if (first_flight_step_ < 4) {
        if (button(ui, {1250, 832, 178, 48}, "Next step")) ++first_flight_step_;
    } else if (button(ui, {1234, 832, 194, 48}, "Finish & explore")) {
        finish_first_flight_tutorial();
    }
}

bool GameFrontend::capture_world(AircraftAndHud &aircraft, SkyAndClouds &sky, const PlatformWindow &win,
                                 int &time_of_day, RegionalAtmosphere *weather, const std::string &path) {
    draw_menu(aircraft, sky, win, false, time_of_day, weather);
    return PlatformWindow::save_framebuffer_png(path, GAME_OUTPUT_W, GAME_OUTPUT_H);
}

void GameFrontend::draw_world(AircraftAndHud &ui, const PlatformWindow &win,
                              RegionalAtmosphere *reg_atmo, const AtmosphereState *atmo, double sim_time_sec) {
    label(ui, 66, 101, 12, "VFR FREE ROAM  /  REGIONAL MAP", true);
    label(ui, 64, 132, 38, "Plan your next flight");
    label(ui, 66, 174, 17, "Pick a runway or landmark, sketch a route, then explore at your own pace.", true);
    char line[192];
    std::snprintf(line, sizeof(line), "%zu region pack%s  |  %zu / %zu airfields visited",
                  pack_->regions().size(), pack_->regions().size() == 1 ? "" : "s",
                  session_.profile().passport_.size(), pack_->airports().size());
    ui.hud_rect(790, 151, 450, 34, .04f, .085f, .10f, .94f);
    ui.hud_border(790, 151, 450, 34, 1, .17f, .36f, .38f, .8f);
    label(ui, 805, 159, 14, line, true);

    const auto *parked = parked_airport();
    constexpr size_t kWorldPlaceCount = sizeof(kWorldPlaces) / sizeof(kWorldPlaces[0]);
    std::array<const WorldPlace *, kWorldPlaceCount> installed_places{};
    int installed_place_count = 0;
    for (const auto &place : kWorldPlaces) {
        if (pack_->find_region_at(place.lat, place.lon))
            installed_places[installed_place_count++] = &place;
    }
    auto focus_place = [&](const WorldPlace &place) {
        selected_place_id_ = place.id;
        selected_id_.clear();
        for (int i = 0; i < installed_place_count; ++i) {
            if (selected_place_id_ == installed_places[i]->id) {
                place_page_ = i / 4;
                break;
            }
        }
        globe_lat_ = place.lat;
        globe_lon_ = place.lon;
        globe_zoom_ = 36.0f;
    };
    if (button(ui, {64, 986, 149, 47}, "Globe view")) {
        globe_zoom_ = 1.0f;
    }
    if (button(ui, {225, 986, 194, 47}, "Find aircraft", parked)) {
        focus_airport(*parked, 90.0f);
        airport_page_ = 0;
    }
    if (button(ui, {431, 986, 62, 47}, "-")) {
        globe_zoom_ = std::max(1.0f, globe_zoom_ / 1.8f);
    }
    if (button(ui, {505, 986, 62, 47}, "+")) {
        globe_zoom_ = std::min(180.0f, globe_zoom_ * 1.8f);
    }
    if (button(ui, {579, 986, 235, 47}, add_waypoint_ ? "Cancel map point" : "Add map point", !session_.flight_active_, add_waypoint_))
        add_waypoint_ = !add_waypoint_;
    label(ui, 837, 1000, 16, add_waypoint_ ? "Click inside installed terrain" : "Drag to move the globe", true);
    if (win.key_pressed(KEY_ESC)) add_waypoint_ = false;

    if (!win.mouse_left_down()) map_dragging_ = false;
    if (map_dragging_ && !win.mouse_left_clicked()) {
        const double scale = .19 / globe_zoom_;
        globe_lat_ = std::clamp(globe_lat_ + win.mouse_dy() * scale, -88.0, 88.0);
        const double lon_scale = scale / std::max(.21, std::cos(globe_lat_ * DEG2RAD_D));
        globe_lon_ = std::remainder(globe_lon_ - win.mouse_dx() * lon_scale, 360.0);
    }

    GlobeProjection projection(globe_lat_, globe_lon_, globe_zoom_);
    draw_globe(reg_atmo, sim_time_sec);
    ui.hud_border(map_area.x, map_area.y, map_area.w, map_area.h, 1, .29f, .42f, .50f, .8f);

    if (weather_overlay_ && (reg_atmo != nullptr || region_wx_ != 0)) {
        ui.hud_rect(762, 238, 280, 34, .025f, .052f, .072f, .90f);
        ui.hud_border(762, 238, 280, 34, 1, .19f, .36f, .42f, .82f);
        label(ui, 774, 248, 13, "FLOW", true);
        static const Vec3 kSwatches[4] = {
            {.38f, .78f, .98f}, {.32f, .94f, .86f}, {.95f, .88f, .34f}, {1.00f, .46f, .28f}
        };
        static constexpr const char *kSpeeds[4] = {"8", "18", "30", "40kt"};
        for (int i = 0; i < 4; ++i) {
            const float sx = 824.0f + i * 52.0f;
            ui.hud_rect(sx, 250, 12, 10, kSwatches[i].x, kSwatches[i].y, kSwatches[i].z, 1.0f);
            label(ui, sx + 16.0f, 248, 12, kSpeeds[i], true);
        }
    }
    if (button(ui, {1052, 238, 198, 34}, weather_overlay_ ? "Flowmap: ON" : "Flowmap: OFF",
               reg_atmo != nullptr || region_wx_ != 0, weather_overlay_)) {
        weather_overlay_ = !weather_overlay_;
    }

    if (parked) {
        Vec3 from = globe_xyz(parked->lat, parked->lon);
        for (const auto &p : session_.profile().route_) {
            Vec3 to = globe_xyz(p.lat, p.lon);
            const float angle = std::acos(std::clamp(dot(from, to), -1.0f, 1.0f));
            const float sin_angle = std::sin(angle);
            auto previous = projection.project(from);
            const int segments = std::clamp(int(angle * projection.radius / 6.0f), 4, 256);
            for (int i = 1; i <= segments; ++i) {
                const float t = float(i) / segments;
                Vec3 sample = (sin_angle > .0001f)
                    ? (from * std::sin((1-t)*angle) + to * std::sin(t*angle)) / sin_angle
                    : normalize(from * (1-t) + to * t);
                auto next = projection.project(sample);
                if (previous && next && inside(map_area, *previous) && inside(map_area, *next))
                    ui.hud_line(*previous, *next, 2.5f, {.98f, .70f, .33f});
                previous = next;
            }
            if (previous && inside(map_area, *previous))
                ui.hud_rect(previous->x-3, previous->y-3, 6, 6, .98f, .70f, .33f, 1);
            from = to;
        }
    }

    const AirportInfo *hit = nullptr;
    float closest = 17.0f;
    for (const auto &ap : pack_->airports()) {
        const bool is_parked = parked && ap.id == parked->id;
        const bool selected = ap.id == selected_id_;
        if (globe_zoom_ < 8 && !is_parked && !selected) continue;
        auto p = projection.project(globe_xyz(ap.lat, ap.lon));
        if (!p || !inside({map_area.x+10, map_area.y+10, map_area.w-20, map_area.h-20}, *p)) continue;
        const int radius = is_parked ? 7 : 4;
        const bool stamped = find_stamp(ap.id) != nullptr;
        ui.hud_rect(p->x-radius, p->y-radius, radius*2, radius*2, .05f, .13f, .15f, 1);
        ui.hud_rect(p->x-radius+1, p->y-radius+1, (radius-1)*2, (radius-1)*2,
            is_parked ? .96f : (stamped ? .32f : .40f),
            is_parked ? .68f : (stamped ? .92f : .82f),
            stamped ? .42f : .57f, 1);
        if ((selected || is_parked) && p->x < map_area.x + map_area.w - 185) {
            ui.hud_rect(p->x+9, p->y-13, is_parked ? 172 : 107, 24, .028f, .062f, .088f, .96f);
            std::snprintf(line, sizeof(line), "%s%s", is_parked ? "C172S / " : "", ap.id.c_str());
            label(ui, p->x+15, p->y-9, 15, line);
        }
        const float distance = std::hypot(p->x - pointer_.x, p->y - pointer_.y);
        if (distance < closest) { closest = distance; hit = &ap; }
    }
    const WorldPlace *place_hit = nullptr;
    float closest_place = 17.0f;
    for (int place_idx = 0; place_idx < installed_place_count; ++place_idx) {
        const auto &place = *installed_places[place_idx];
        const bool selected = place.id == selected_place_id_;
        auto p = projection.project(globe_xyz(place.lat, place.lon));
        if (!p || !inside({map_area.x + 10, map_area.y + 10, map_area.w - 20, map_area.h - 20}, *p)) continue;
        const float distance = std::hypot(p->x - pointer_.x, p->y - pointer_.y);
        const bool hovered = distance < closest_place;
        if (hovered) { closest_place = distance; place_hit = &place; }
        if (globe_zoom_ < 6.0f && !selected && !hovered) continue;

        const float radius = selected ? 7.0f : 5.0f;
        ui.hud_rect(p->x - radius - 2, p->y - radius - 2, (radius + 2) * 2, (radius + 2) * 2,
                    .025f, .055f, .070f, .98f);
        const Vec3 color = selected ? Vec3{1.0f, .95f, .70f}
            : (place.scenic ? Vec3{.28f, .88f, .78f} : Vec3{1.0f, .70f, .30f});
        const Vec2 top{p->x, p->y - radius}, right{p->x + radius, p->y};
        const Vec2 bottom{p->x, p->y + radius}, left{p->x - radius, p->y};
        ui.hud_line(top, right, 2.2f, color);
        ui.hud_line(right, bottom, 2.2f, color);
        ui.hud_line(bottom, left, 2.2f, color);
        ui.hud_line(left, top, 2.2f, color);

        if (selected || hovered || (show_places_ && globe_zoom_ >= 24.0f)) {
            const float label_w = static_cast<float>(std::min<size_t>(230, std::char_traits<char>::length(place.name) * 9 + 18));
            const float raw_lx = (p->x + label_w + 16 < map_area.x + map_area.w) ? p->x + 10 : p->x - label_w - 10;
            const float lx = std::clamp(raw_lx, map_area.x + 4.0f, map_area.x + map_area.w - label_w - 4.0f);
            const float ly = std::clamp(p->y - 12.0f, map_area.y + 8.0f, map_area.y + map_area.h - 30.0f);
            ui.hud_rect(lx, ly, label_w, 24, .025f, .055f, .070f, .96f);
            ui.hud_border(lx, ly, label_w, 24, 1, color.x, color.y, color.z, .9f);
            label(ui, lx + 7, ly + 4, 14, place.name);
        }
    }
    if (click_ && contains(map_area) && !inside({82, 916, 870, 42}, pointer_) && !inside({1052, 238, 198, 34}, pointer_)) {
        if (add_waypoint_ && !session_.flight_active_) {
            if (place_hit && closest_place < closest) {
                focus_place(*place_hit);
                add_point({place_hit->name, place_hit->lat, place_hit->lon});
                add_waypoint_ = false;
                show_places_ = true;
            } else if (auto p = projection.unproject(pointer_)) {
                const auto [lat,lon] = globe_geo(*p);
                if (pack_->find_region_at(lat, lon)) {
                    std::snprintf(line, sizeof(line), "Point %zu", session_.profile().route_.size() + 1);
                    add_point({line, lat, lon});
                    add_waypoint_ = false;
                } else status_ = "This point is outside the installed scenery.";
            }
        } else if (place_hit && closest_place < closest) {
            focus_place(*place_hit);
            show_places_ = true;
        } else if (hit) {
            show_places_ = false;
            focus_airport(*hit, globe_zoom_ < 8.0f ? 75.0f : globe_zoom_);
        } else map_dragging_ = true;
        click_ = false;
    }

    // Map key: make the aircraft, visited airfields, open airfields, and route easy to distinguish.
    ui.hud_rect(82, 916, 870, 42, .025f, .048f, .068f, .91f);
    ui.hud_border(82, 916, 870, 42, 1, .20f, .31f, .38f, .80f);
    const auto legend_item = [&](float x, float r, float g, float b, const char *text) {
        ui.hud_rect(x, 930, 12, 12, r, g, b, 1.0f);
        label(ui, x + 20, 930, 14, text, true);
    };
    legend_item(98, .96f, .68f, .57f, "YOUR CESSNA");
    legend_item(260, .32f, .92f, .42f, "VISITED");
    legend_item(383, .40f, .82f, .57f, "AIRFIELD");
    legend_item(525, .98f, .70f, .33f, "PLANNED ROUTE");
    legend_item(700, 1.0f, .70f, .30f, "LANDMARK");

    // Top-right: Parked Aircraft Ownership Card
    panel(ui, {1292,225,564,218});
    std::snprintf(line, sizeof(line), "%s%s%s", session_.flight_active_ ? "LAST PARKED AT  |  " : "YOUR AIRCRAFT  |  ",
                  ui.aircraft_.registration.empty() ? "" : ui.aircraft_.registration.c_str(),
                  ui.aircraft_.registration.empty() ? "" : "  /  INSTALLED PACKAGE");
    label(ui, 1318, 244, 17, line, true);
    label(ui, 1318, 270, 28, ui.aircraft_.name.c_str());
    if (parked) {
        const bool at_home = parked == home_airport();
        std::snprintf(line, sizeof(line), "%s: %s", at_home ? "Home hangar" : "Temporary base",
                      at_home ? "Castejón de Sos" : parked->name.c_str());
        label(ui, 1318, 308, 18, line);
    } else label(ui, 1318, 308, 18, session_.profile().parked_id_.empty()
        ? "Install the region for Castejón de Sos."
        : "Install the parked aircraft's region.", true);
    std::snprintf(line, sizeof(line), "Fuel %.0f gal  |  Oil %.1f qt  |  Bat %.0f%%  |  %.1f hr",
                  session_.profile().care_.left_fuel_gal + session_.profile().care_.right_fuel_gal, session_.profile().care_.oil_quarts,
                  session_.profile().care_.battery_soc * 100.0, session_.profile().hours_);
    label(ui, 1318, 339, 16, line, true);
    std::snprintf(line, sizeof(line), "%s  |  %s%s",
                  session_.profile().care_.engine_running ? "Engine running" : "Cold & dark",
                  (session_.profile().care_.chocks_on || session_.profile().care_.tie_downs_on || session_.profile().care_.pitot_cover_on) ? "Secured outside" : "Ready to roll",
                  session_.profile().care_.oil_hours >= 10.0 ? "  |  Oil change due" : "");
    label(ui, 1318, 363, 15, line, true);
    if (button(ui, {1318, 389, 512, 42}, "Walk to aircraft  |  Walkaround", parked && !session_.flight_active_, true))
        request_ = Request::Walk;

    // Middle-right: Airfields or landmarks, with a live local weather briefing for selected airfields.
    panel(ui, {1292,455,564,296});
    label(ui, 1318, 471, 21, show_places_ ? "Places to explore" : "Installed airfields");
    if (button(ui, {1532,462,92,30}, "Airfields", true, !show_places_)) {
        show_places_ = false;
        airport_page_ = 0;
        selected_place_id_.clear();
    }
    if (button(ui, {1630,462,96,30}, "Landmarks", true, show_places_)) {
        show_places_ = true;
        place_page_ = 0;
        selected_id_.clear();
    }
    const auto &airports = pack_->airports();
    const int place_count = installed_place_count;
    int &page_index = show_places_ ? place_page_ : airport_page_;
    const int item_count = show_places_ ? place_count : static_cast<int>(airports.size());
    const int pages = std::max(1, (item_count + 3) / 4);
    page_index = std::clamp(page_index, 0, pages - 1);
    if (button(ui, {1738,462,40,30}, "<", page_index > 0)) --page_index;
    if (button(ui, {1790,462,40,30}, ">", page_index + 1 < pages)) ++page_index;
    for (int i = 0; i < 4 && page_index * 4 + i < item_count; ++i) {
        if (show_places_) {
            const auto &place = *installed_places[page_index * 4 + i];
            std::snprintf(line, sizeof(line), "%s  %s", place.kind, place.name);
            if (button(ui, {1318, 504 + i * 36, 512, 31}, line, true, place.id == selected_place_id_))
                focus_place(place);
        } else {
            const auto &ap = airports[page_index * 4 + i];
            const bool stamped = find_stamp(ap.id) != nullptr;
            std::snprintf(line, sizeof(line), "%s %-7s %.28s  (%.0fm)",
                          stamped ? "*" : " ", ap.id.c_str(), ap.name.c_str(), ap.elev_m);
            if (button(ui, {1318, 504 + i * 36, 512, 31}, line, true, ap.id == selected_id_))
                focus_airport(ap, std::max(48.0f, globe_zoom_));
        }
    }
    const auto *selected = show_places_ ? nullptr : pack_->find_airport(selected_id_);
    const WorldPlace *selected_place = show_places_ ? find_world_place(selected_place_id_) : nullptr;
    if (show_places_) {
        if (selected_place) {
            label(ui, 1318, 648, 13, selected_place->kind, true);
            label(ui, 1318, 670, 15, selected_place->note, true);
        } else if (installed_place_count == 0) {
            label(ui, 1318, 670, 15, "No landmarks in the installed scenery.", true);
        } else {
            label(ui, 1318, 670, 15, "Select a landmark to focus it on the map.", true);
        }
    } else if (selected) {
        label(ui, 1318, 648, 13, "IN-GAME MODEL NOW - " , true);
        label(ui, 1495, 648, 13, selected->id.c_str(), true);
        const double len_m = std::max(100.0, nspain::haversine_m(selected->rwy_lat_a, selected->rwy_lon_a,
                                                                selected->rwy_lat_b, selected->rwy_lon_b));
        const char *srf = selected->surface.empty() ? "strip" : selected->surface.c_str();
        if (reg_atmo && atmo) {
            if (reg_atmo->covers(selected->lat, selected->lon)) {
                const float wx_est = std::clamp((atmo->cloud_cov - 0.16f) / 0.78f, 0.0f, 1.0f);
                const LocalAirSample wx = reg_atmo->sample_at(selected->lat, selected->lon, selected->elev_m + 10.0,
                                                              atmo->sun_dir, sim_time_sec, wx_est);
                const double w_kt = std::hypot(wx.wind_east_mps, wx.wind_north_mps) * 1.94384;
                const int w_from = int(std::lround(std::fmod(std::atan2(-wx.wind_east_mps, -wx.wind_north_mps) * RAD2DEG_D + 360.0, 360.0))) % 360;
                std::snprintf(line, sizeof(line), "%s %.0fm | W %03d/%.0fkt | T %.0fC | QNH %.0f | C %.0f%% | R %.1fmm/h",
                              srf, len_m, w_from, w_kt, wx.oat_c, wx.qnh_hpa,
                              wx.local_cloud_frac * 100.0f, wx.precipitation_mm_hr);
            } else {
                std::snprintf(line, sizeof(line), "Airfield is outside the active regional model grid.");
            }
        } else {
            std::snprintf(line, sizeof(line), "Surface: %s  %.0fm  |  Elev %.0fm  |  %s",
                          srf, len_m, selected->elev_m,
                          is_full_service_airport(selected->id) ? "Full hangar" : "Remote strip");
        }
        label(ui, 1318, 670, 15, line, true);
    }
    if (show_places_) {
        if (button(ui, {1318, 704, 512, 38}, "Add selected landmark to route",
                   parked && selected_place && !session_.flight_active_))
            add_point({selected_place->name, selected_place->lat, selected_place->lon});
    } else if (button(ui, {1318, 704, 512, 38}, "Add selected airfield to route",
                      parked && selected && !session_.flight_active_ && (selected != parked || !session_.profile().route_.empty()))) {
        add_point({selected->id, selected->lat, selected->lon});
    }

    // Bottom-right: actual regional model history and an editable route.
    panel(ui, {1292,763,564,270});
    const auto *history = reg_atmo ? &reg_atmo->model_history() : nullptr;
    const size_t history_count = history ? history->size() : 0;
    const int history_pages = std::max(1, static_cast<int>((history_count + 3) / 4));
    model_history_page_ = std::clamp(model_history_page_, 0, history_pages - 1);
    const auto source = reg_atmo ? reg_atmo->model_source() : WeatherSource::Default;
    const char *input_label = source == WeatherSource::Live ? "Open-Meteo input"
        : source == WeatherSource::Baked ? "Baked climate" : source == WeatherSource::Manual
        ? "Manual weather" : "Default weather";
    if (button(ui, {1598,770,130,30}, show_outlook_ ? "History" : "Outlook", reg_atmo != nullptr)) {
        show_outlook_ = !show_outlook_;
        if (show_outlook_ && reg_atmo->outlook().status == WeatherOutlook::Status::Idle) reg_atmo->request_outlook();
    }
    const auto outlook = show_outlook_ && reg_atmo ? reg_atmo->outlook() : WeatherOutlook{};
    if (show_outlook_) {
        label(ui, 1318, 778, 18, "REGIONAL MODEL OUTLOOK");
        if (outlook.status == WeatherOutlook::Status::Running) {
            if (button(ui, {1738,770,92,30}, "Cancel")) reg_atmo->cancel_outlook();
        } else if (button(ui, {1738,770,92,30}, "Run", reg_atmo && reg_atmo->has_model())) {
            reg_atmo->request_outlook();
        }
        if (outlook.status != WeatherOutlook::Status::Idle) {
            std::snprintf(line, sizeof(line), "Fixed inputs | issued at T+%.2fm", outlook.issue_sim_time / 60.0);
            label(ui, 1318, 803, 12, line, true);
        }
        if (outlook.predictions.empty())
            label(ui, 1320, 842, 15, outlook.status == WeatherOutlook::Status::Running
                  ? "Calculating outlook for +1, +3 and +5 minutes..." : "Run an outlook from the current model.", true);
    } else {
        label(ui, 1318, 778, 18, "REGIONAL MODEL HISTORY");
        label(ui, 1318, 802, 12, input_label, true);
        if (button(ui, {1738,770,40,30}, "<", model_history_page_ > 0)) --model_history_page_;
        if (button(ui, {1790,770,40,30}, ">", model_history_page_ + 1 < history_pages)) ++model_history_page_;
        if (history_count == 0)
            label(ui, 1318, 842, 15, "History appears as the weather model advances.", true);
    }

    for (int card = 0; card < 4; ++card) {
        const size_t idx = static_cast<size_t>(model_history_page_ * 4 + card);
        if (show_outlook_ ? size_t(card) >= outlook.predictions.size() : idx >= history_count) break;
        const auto &entry = show_outlook_ ? outlook.predictions[card] : (*history)[history_count - 1 - idx];
        const float x = 1318.0f + card * 128.0f;
        ui.hud_rect(x, 818, 120, 74, .065f, .105f, .133f, 1.0f);
        ui.hud_border(x, 818, 120, 74, 1, .20f, .31f, .38f, .78f);
        if (show_outlook_) {
            std::snprintf(line, sizeof(line), "+%.0fm | T+%.2fm",
                          (entry.sim_time_sec - outlook.issue_sim_time) / 60.0, entry.sim_time_sec / 60.0);
        } else {
            const double age_min = std::max(0.0, (sim_time_sec - entry.sim_time_sec) / 60.0);
            const char *region = entry.region_id.find("catalonia") != std::string::npos ? "CAT"
                : entry.region_id.find("aragon") != std::string::npos ? "ARA" : entry.region_id.c_str();
            std::snprintf(line, sizeof(line), "T-%.1fm %.7s", age_min, region);
        }
        label(ui, x + 8, 822, 12, line, true);
        std::snprintf(line, sizeof(line), "%.0f C  CLOUD %.0f%%", entry.temperature_c, entry.cloud_cover_pct);
        label(ui, x + 8, 838, 12, line);
        std::snprintf(line, sizeof(line), "%s %.0fkt", wind_cardinal(entry.wind_direction_from_deg),
                      entry.wind_speed_10m_mps * 1.94384f);
        label(ui, x + 8, 854, 12, line, true);
        std::snprintf(line, sizeof(line), "RAIN %.1fmm/h", entry.precipitation_mm_hr);
        label(ui, x + 8, 870, 12, line, true);
    }
    ui.hud_rect(1318, 895, 512, 1, .20f, .31f, .38f, .78f);
    double total = 0;
    double route_wind_kt = 0.0, route_rain_max = 0.0;
    int route_wind_dir = 0, route_wx_pts = 0;
    if (parked) {
        double a = parked->lat, b = parked->lon;
        const float wx_est = atmo ? std::clamp((atmo->cloud_cov - 0.16f) / 0.78f, 0.0f, 1.0f) : 0.25f;
        for (const auto &p : session_.profile().route_) {
            total += nspain::haversine_m(a,b,p.lat,p.lon);
            if (reg_atmo && atmo && reg_atmo->covers(p.lat, p.lon)) {
                const LocalAirSample rw = reg_atmo->sample_at(p.lat, p.lon, pack_->sample_elevation(p.lat, p.lon) + 600.0,
                                                              atmo->sun_dir, sim_time_sec, wx_est);
                route_wind_kt += std::hypot(rw.wind_east_mps, rw.wind_north_mps) * 1.94384;
                route_rain_max = std::max(route_rain_max, rw.precipitation_mm_hr);
                route_wind_dir = int(std::lround(std::fmod(std::atan2(-rw.wind_east_mps, -rw.wind_north_mps) * RAD2DEG_D + 360.0, 360.0))) % 360;
                ++route_wx_pts;
            }
            a=p.lat; b=p.lon;
        }
    }
    if (route_wx_pts > 0) {
        std::snprintf(line, sizeof(line), "ROUTE %.1f NM | W %03d/%.0fkt R %.1fmm/h",
                      total / nm_m, route_wind_dir, route_wind_kt / route_wx_pts, route_rain_max);
    } else {
        std::snprintf(line, sizeof(line), "ROUTE  %.1f NM", total / nm_m);
    }
    label(ui, 1318, 901, 18, line);
    if (button(ui, {1738,898,40,28}, "<", route_page_ > 0)) --route_page_;
    if (button(ui, {1790,898,40,28}, ">", (route_page_+1)*2 < int(session_.profile().route_.size()))) ++route_page_;
    if (session_.profile().route_.empty()) label(ui, 1318, 940, 15, "Add an airfield or a visual waypoint to plan a route.", true);
    for (int i = 0; i < 2 && size_t(route_page_*2+i) < session_.profile().route_.size(); ++i) {
        const int index = route_page_*2+i;
        const auto &p = session_.profile().route_[index];
        std::snprintf(line, sizeof(line), "%02d   %s    %+.3f  %+.3f", index+1, p.label.c_str(), p.lat, p.lon);
        label(ui, 1320, 934+i*28, 15, line);
        if (button(ui, {1784, 928+i*28, 46, 25}, "X", !session_.flight_active_)) {
            session_.remove_route_point(index);
            route_page_ = std::min(route_page_, session_.profile().route_.empty() ? 0 : int((session_.profile().route_.size()-1)/2));
            break;
        }
    }
    if (button(ui, {1318, 985, 150, 36}, "Clear route", !session_.profile().route_.empty() && !session_.flight_active_)) {
        session_.clear_route();
        route_page_ = 0;
    }
    if (button(ui, {1480, 985, 200, 36}, "Watch replay", replay_available_)) {
        request_ = Request::WatchReplay;
    }
    const auto &save_error = session_.profile_error();
    if (!save_error.empty()) label(ui, 67, 200, 16, save_error.c_str());
    else if (!status_.empty()) label(ui, 67, 200, 16, status_.c_str());
}

void GameFrontend::draw_logbook(AircraftAndHud &ui) {
    label(ui, 64, 111, 44, "Logbook & Passport");
    label(ui, 66, 166, 21, "Your Cessna 172S maintenance ledger, flight log, and Aragon airfield stamps.", true);

    char line[192];
    // Left panel: Aircraft Maintenance Status + Flight Logbook
    panel(ui, {64, 225, 930, 800});
    label(ui, 92, 250, 26, "Aircraft Care & Maintenance (EC-VFR)");
    std::snprintf(line, sizeof(line), "Engine Time: %.1f hr   |   Landings: %d   |   Best Touchdown: %s",
                  session_.profile().hours_, session_.profile().total_landings_,
                  session_.profile().best_landing_fpm_ < 9000.0 ? "" : "none yet");
    if (session_.profile().best_landing_fpm_ < 9000.0) {
        std::snprintf(line, sizeof(line), "Engine Time: %.1f hr   |   Landings: %d   |   Best Touchdown: %.0f fpm",
                      session_.profile().hours_, session_.profile().total_landings_, session_.profile().best_landing_fpm_);
    }
    label(ui, 92, 294, 18, line, true);

    const auto health_row = [&](int row, const char *left_txt, const char *right_txt) {
        const int y = 336 + row * 34;
        label(ui, 92, y, 17, left_txt);
        label(ui, 540, y, 17, right_txt);
    };
    char rcol[96];
    std::snprintf(line, sizeof(line), "Oil: %.1f / %.1f qt  (%.1f / 10.0 hr)%s",
                  session_.profile().care_.oil_quarts, ui.aircraft_.oil_capacity_quarts,
                  session_.profile().care_.oil_hours, session_.profile().care_.oil_hours >= 10.0 ? "  DUE" : "");
    std::snprintf(rcol, sizeof(rcol), "Battery (%dV): %.0f%%",
                  static_cast<int>(std::round(ui.aircraft_.bus_voltage * 6.0 / 7.0)),
                  session_.profile().care_.battery_soc * 100.0);
    health_row(0, line, rcol);

    std::snprintf(line, sizeof(line), "Cylinders: %.0f%%  (shock wear %.0f%%)",
                  (1.0 - session_.profile().care_.cylinder_wear) * 100.0, session_.profile().care_.shock_cooling * 100.0);
    std::snprintf(rcol, sizeof(rcol), "Spark Plugs: %.0f%% clean", (1.0 - session_.profile().care_.spark_fouling) * 100.0);
    health_row(1, line, rcol);

    std::snprintf(line, sizeof(line), "Airframe: %.0f%%   |   Propeller: %.0f%%",
                  (1.0 - session_.profile().care_.airframe_damage) * 100.0, (1.0 - session_.profile().care_.prop_nicks) * 100.0);
    std::snprintf(rcol, sizeof(rcol), "Fuel Sumps: %s", session_.profile().care_.sump_water > 0.12 ? "WATER DETECTED" : "Clear");
    health_row(2, line, rcol);

    std::snprintf(line, sizeof(line), "Tires: %.0f%%   |   Brakes: %.0f%%",
                  (1.0 - session_.profile().care_.tire_wear) * 100.0, (1.0 - session_.profile().care_.brake_wear) * 100.0);
    std::snprintf(rcol, sizeof(rcol), "Gear Align: %s",
                  std::abs(session_.profile().care_.gear_alignment) < 0.08 ? "True" : (session_.profile().care_.gear_alignment > 0 ? "Pulls Right" : "Pulls Left"));
    health_row(3, line, rcol);

    std::snprintf(line, sizeof(line), "Secured: Chocks %s | Pitot %s | Tie-downs %s",
                  session_.profile().care_.chocks_on ? "ON" : "off",
                  session_.profile().care_.pitot_cover_on ? "COVERED" : "clear",
                  session_.profile().care_.tie_downs_on ? "TIED" : "off");
    label(ui, 92, 478, 16, line, true);

    ui.hud_rect(92, 514, 874, 1, .21f, .32f, .39f, .85f);
    label(ui, 92, 534, 24, "Recent Flights");
    if (button(ui, {742, 526, 224, 34}, "Watch route video", replay_available_)) {
        request_ = Request::WatchReplay;
    }
    if (session_.profile().logbook_.empty()) {
        label(ui, 92, 584, 18, "No completed flights logged yet.", true);
        label(ui, 92, 618, 17, "Take off, land at any Aragon strip, taxi clear of the runway, and park to log a leg.", true);
    } else {
        label(ui, 92, 574, 15, "ROUTE                  TIME     DIST     FUEL    TOUCHDOWN        GRADE", true);
        for (size_t i = 0; i < session_.profile().logbook_.size() && i < 11; ++i) {
            const auto &e = session_.profile().logbook_[i];
            std::snprintf(line, sizeof(line), "%-7s -> %-7s   %4.0fm   %5.1fNM  %4.1fgal  %4.0f fpm (%2.0fkt)  %s",
                          e.dep_id.c_str(), e.arr_id.c_str(), e.minutes, e.distance_nm,
                          e.fuel_gal, e.touchdown_fpm, e.crosswind_kt, e.grade.c_str());
            label(ui, 92, 606 + static_cast<float>(i) * 35.0f, 16, line);
        }
    }

    // Right panel: Aragon Airfield Passport
    panel(ui, {1020, 225, 836, 800});
    const auto &airports = pack_->airports();
    std::snprintf(line, sizeof(line), "Aragon Airfield Passport   (%zu / %zu visited)",
                  session_.profile().passport_.size(), airports.size());
    label(ui, 1048, 250, 25, line);
    const int per_page = 12;
    const int pages = std::max(1, (int(airports.size()) + per_page - 1) / per_page);
    if (button(ui, {1738, 246, 40, 32}, "<", passport_page_ > 0)) --passport_page_;
    if (button(ui, {1790, 246, 40, 32}, ">", passport_page_ + 1 < pages)) ++passport_page_;
    label(ui, 1048, 294, 15, "Click any airfield to focus it on the World map.  * = Visited & Parked", true);

    for (int i = 0; i < per_page && size_t(passport_page_ * per_page + i) < airports.size(); ++i) {
        const auto &ap = airports[passport_page_ * per_page + i];
        const PassportStamp *st = find_stamp(ap.id);
        const char *srf = ap.surface.empty() ? "strip" : ap.surface.c_str();
        if (st) {
            std::snprintf(line, sizeof(line), "* %-7s %-26.26s  %-7s %4.0fm | %dx  best %.0f fpm",
                          ap.id.c_str(), ap.name.c_str(), srf, ap.elev_m, st->visits,
                          st->best_fpm < 9000.0 ? st->best_fpm : 0.0);
        } else {
            std::snprintf(line, sizeof(line), "  %-7s %-26.26s  %-7s %4.0fm | unvisited",
                          ap.id.c_str(), ap.name.c_str(), srf, ap.elev_m);
        }
        if (button(ui, {1048, 334 + i * 54, 780, 44}, line, true, st != nullptr)) {
            focus_airport(ap, 65.0f);
            page_ = Page::World;
        }
    }
}

void GameFrontend::draw_settings(AircraftAndHud &ui, int &time_of_day, RegionalAtmosphere *reg_atmo) {
    label(ui, 64, 111, 44, "Settings");
    label(ui, 66, 166, 21, "Tune performance, cockpit POV feel, audio, clarity, and live weather.", true);
    panel(ui, {64,225,1100,720});
    label(ui, 91, 246, 24, "Graphics & Streaming");
    auto &s = ui.graphics_settings_;
    bool changed = false;
    auto setting = [&](int row, const char *name, const char *value) {
        const int y = 290 + row * 50;
        ui.hud_rect(91,y+43,1046,1,.21f,.32f,.39f,.85f);
        label(ui, 91, y+8, 19, name);
        label(ui, 763, y+8, 18, value);
        if (button(ui, {697,y,44,36}, "<")) { changed = true; return -1; }
        if (button(ui, {1093,y,44,36}, ">")) { changed = true; return 1; }
        return 0;
    };
    static const char *presets[] = {"Low (F1 / Cooler)", "Medium (F2 / 60 FPS)", "High (F3 / Balanced)", "Ultra (F4 / dGPU)"};
    static const char *weather[] = {"Follow preset", "Low", "Medium", "High"};
    static const char *aa[] = {"Spatial FXAA", "Balanced TAA", "Crisp TAA"};
    static const char *clarity[] = {"Crisp VFR", "Natural", "Hazy"};
    char value[48];
    s.quality_preset = (s.quality_preset + setting(0, "Performance preset", presets[s.quality_preset]) + 4) % 4;
    std::snprintf(value, sizeof(value), "%d%%", s.pixel_percent);
    s.pixel_percent = std::clamp(s.pixel_percent + 5 * setting(1, "Render resolution budget", value), 40, 100);
    s.aa_mode = (s.aa_mode + setting(2, "Anti-aliasing", aa[s.aa_mode]) + 3) % 3;
    s.atmosphere_clarity = (s.atmosphere_clarity + setting(3, "Atmosphere clarity", clarity[s.atmosphere_clarity]) + 3) % 3;
    std::snprintf(value, sizeof(value), "%d%%", s.terrain_lod_percent);
    s.terrain_lod_percent = std::clamp(s.terrain_lod_percent + 25 * setting(4, "Terrain LOD radius", value), 75, 200);
    if (setting(5, "Online PNOA streaming", s.terrain_streaming ? "On / IGN PNOA" : "Off / installed packs"))
        s.terrain_streaming = !s.terrain_streaming;
    s.weather_detail = (s.weather_detail + setting(6, "Cloud & weather detail", weather[s.weather_detail]) + 4) % 4;
    if (setting(7, "Cockpit detail / native res", s.cockpit_native ? (s.aircraft_detail ? "Native + High" : "Native + Std") : "Scaled")) {
        if (!s.cockpit_native) { s.cockpit_native = true; s.aircraft_detail = 0; }
        else if (s.aircraft_detail == 0) { s.aircraft_detail = 1; }
        else { s.cockpit_native = false; s.aircraft_detail = 0; }
    }
    if (setting(8, "In-flight overlays", s.show_flight_hud ? "On" : "Off"))
        s.show_flight_hud = !s.show_flight_hud;
    static const char *display_modes[] = {"Windowed", "Fullscreen", "Fullscreen windowed"};
    s.display_mode = (s.display_mode + setting(9, "Display mode", display_modes[s.display_mode]) + 3) % 3;
    if (setting(10, "VSync", s.vsync ? "On (Tear-free)" : "Off (Low latency)"))
        s.vsync = !s.vsync;
    static constexpr int kFpsLimits[] = {30, 45, 60, 75, 90, 120, 144, 0};
    constexpr int kFpsCount = static_cast<int>(sizeof(kFpsLimits) / sizeof(kFpsLimits[0]));
    int fps_idx = kFpsCount - 1;
    if (s.fps_limit > 0) {
        fps_idx = 0;
        for (int i = 1; i < kFpsCount - 1; ++i)
            if (std::abs(kFpsLimits[i] - s.fps_limit) < std::abs(kFpsLimits[fps_idx] - s.fps_limit)) fps_idx = i;
    }
    if (s.fps_limit > 0) std::snprintf(value, sizeof(value), "%d FPS", s.fps_limit);
    else std::snprintf(value, sizeof(value), "Unlimited");
    if (int delta = setting(11, "FPS limit", value))
        s.fps_limit = kFpsLimits[(fps_idx + delta + kFpsCount) % kFpsCount];
    label(ui, 91, 900, 13, "Display mode applies after restart; VSync and FPS limit apply live.", true);

    panel(ui, {1192,225,664,720});
    label(ui, 1218, 246, 24, "Cockpit POV & Audio");
    auto rsetting = [&](int row, const char *name, const char *val) {
        const int y = 296 + row * 68;
        ui.hud_rect(1218, y + 47, 612, 1, .21f, .32f, .39f, .85f);
        label(ui, 1218, y + 9, 19, name);
        label(ui, 1598, y + 9, 18, val);
        if (button(ui, {1540, y, 42, 38}, "<")) { changed = true; return -1; }
        if (button(ui, {1788, y, 42, 38}, ">")) { changed = true; return 1; }
        return 0;
    };
    static const char *head_modes[] = {"Off", "Subtle", "Realistic"};
    std::snprintf(value, sizeof(value), "%d deg", s.cockpit_fov_deg);
    s.cockpit_fov_deg = std::clamp(s.cockpit_fov_deg + 2 * rsetting(0, "Cockpit vertical FOV", value), 42, 64);
    s.head_motion_mode = (s.head_motion_mode + rsetting(1, "Head G-feel & turn look", head_modes[s.head_motion_mode]) + 3) % 3;
    std::snprintf(value, sizeof(value), "%d%%", s.audio_volume_percent);
    s.audio_volume_percent = std::clamp(s.audio_volume_percent + 5 * rsetting(2, "Master audio volume", value), 0, 100);

    label(ui, 1218, 514, 24, "Time of day");
    int time_index = time_of_day < 0 ? 0 : (time_of_day == 10 ? 11 : time_of_day + 1);
    if (button(ui, {1218, 558, 48, 44}, "<")) {
        time_index = (time_index + 11) % 12;
        changed = true;
    }
    if (button(ui, {1782, 558, 48, 44}, ">")) {
        time_index = (time_index + 1) % 12;
        changed = true;
    }
    time_of_day = time_index == 0 ? -1 : (time_index == 11 ? 10 : time_index - 1);
    ui.graphics_settings_.time_of_day = time_of_day;
    const char *time_label = time_of_day < 0 ? "Advancing 5-hour day / night cycle"
        : (time_of_day == 10 ? "Fixed night" : kTodLabels[time_of_day]);
    label(ui, 1284, 570, 17, time_label);
    label(ui, 1218, 624, 16, "Cycle mode runs dawn through a short night every 5 hours.", true);
    if (reg_atmo) {
        ui.hud_rect(1218, 668, 612, 1, .21f, .32f, .39f, .85f);
        label(ui, 1218, 690, 24, "Weather scenario");
        static constexpr const char *kWxModes[7] = {
            "Live / Auto Dynamic VFR",
            "Pyrenean Thermals (Fair VFR)",
            "Cierzo NW (Mountain Wave & Rotors)",
            "Foehn Sur (Orographic Cap Clouds)",
            "Dawn Valley Fog (Basin IMC)",
            "Pyrenean Storm Front (Heavy Rain)",
            "Winter Icing & Snow (Frost & Ice)"
        };
        int mode_idx = reg_atmo->manual_weather_override() ? (1 + static_cast<int>(reg_atmo->regime())) : 0;
        mode_idx = std::clamp(mode_idx, 0, 6);
        int delta = 0;
        if (button(ui, {1218, 738, 48, 44}, "<")) delta = -1;
        if (button(ui, {1782, 738, 48, 44}, ">")) delta = +1;
        if (delta != 0) {
            mode_idx = (mode_idx + delta + 7) % 7;
            if (mode_idx == 0) {
                reg_atmo->set_manual_weather_override(false);
                reg_atmo->set_regime(WindRegime::PYRENEAN_THERMAL);
            } else {
                reg_atmo->set_manual_weather_override(true);
                reg_atmo->set_regime(static_cast<WindRegime>(mode_idx - 1));
            }
        }
        label(ui, 1284, 750, 17, kWxModes[mode_idx]);
        label(ui, 1218, 792, 20, "Weather grid resolution");
        static constexpr const char *kGridModes[3] = {
            "Regional only",
            "Local 192 x 192 | 42 m cells",
            "Local 384 x 384 | 21 m cells"
        };
        int grid_delta = 0;
        if (button(ui, {1218, 824, 48, 44}, "<")) grid_delta = -1;
        if (button(ui, {1782, 824, 48, 44}, ">")) grid_delta = +1;
        if (grid_delta != 0) {
            s.weather_grid_detail = (s.weather_grid_detail + grid_delta + 3) % 3;
            reg_atmo->set_grid_detail(s.weather_grid_detail, ui.state().lat_deg, ui.state().lon_deg);
            changed = true;
        }
        label(ui, 1284, 837, 18, kGridModes[s.weather_grid_detail]);
        label(ui, 1218, 884, 14, "8 km aircraft-following valley tile; regional coverage stays active.", true);
        label(ui, 1218, 905, 14, "Added solver work: about 33% / 133% of regional mode.", true);
        label(ui, 1218, 927, 14, "Press M to cycle weather; F1-F4 change graphics.", true);
    }

    panel(ui, {64, 956, 1792, 88});
    const auto &yk = ui.yoke_state();
    char yk_line[224];
    if (yk.connected) {
        std::snprintf(yk_line, sizeof(yk_line), "Flight Yoke & Pedals: %.72s%s",
                      yk.device_summary.c_str(), yk.has_brakes ? "  [Toe Brakes]" : "");
        label(ui, 91, 965, 18, yk_line);
        std::snprintf(yk_line, sizeof(yk_line), "Roll %+4.0f%%  |  Pitch %+4.0f%%  |  Rudder %+4.0f%%  |  Throttle %3.0f%%",
                      yk.roll * 100.0, yk.pitch * 100.0, yk.rudder * 100.0,
                      (yk.has_throttle ? yk.throttle : ui.state().throttle) * 100.0);
        label(ui, 1180, 966, 16, yk_line, true);
    } else {
        label(ui, 91, 965, 18, "Flight Yoke & Pedals: No USB controller detected (plug & play via Linux evdev)");
        label(ui, 1180, 966, 16, "Connect a yoke, stick, throttle quadrant, or rudder pedals.", true);
    }
    if (button(ui, {91, 996, 228, 36}, s.yoke_direct ? "Mode: Direct Yoke" : "Mode: Flight Assist", true, s.yoke_direct)) {
        s.yoke_direct = !s.yoke_direct;
        changed = true;
    }
    label(ui, 335, 1004, 17, "Deadzone");
    if (button(ui, {434, 996, 36, 36}, "<")) { s.yoke_deadzone_pct = std::clamp(s.yoke_deadzone_pct - 1, 0, 20); changed = true; }
    std::snprintf(value, sizeof(value), "%d%%", s.yoke_deadzone_pct);
    label(ui, 478, 1004, 17, value);
    if (button(ui, {528, 996, 36, 36}, ">")) { s.yoke_deadzone_pct = std::clamp(s.yoke_deadzone_pct + 1, 0, 20); changed = true; }
    label(ui, 586, 1004, 17, "Curve");
    if (button(ui, {656, 996, 36, 36}, "<")) { s.yoke_curve_pct = std::clamp(s.yoke_curve_pct - 5, 0, 80); changed = true; }
    std::snprintf(value, sizeof(value), "%d%%", s.yoke_curve_pct);
    label(ui, 700, 1004, 17, value);
    if (button(ui, {750, 996, 36, 36}, ">")) { s.yoke_curve_pct = std::clamp(s.yoke_curve_pct + 5, 0, 80); changed = true; }
    if (button(ui, {812, 996, 152, 36}, "Inv Pitch", true, s.yoke_invert_pitch)) {
        s.yoke_invert_pitch = !s.yoke_invert_pitch;
        changed = true;
    }
    if (button(ui, {976, 996, 144, 36}, "Inv Roll", true, s.yoke_invert_roll)) {
        s.yoke_invert_roll = !s.yoke_invert_roll;
        changed = true;
    }
    if (button(ui, {1132, 996, 152, 36}, "Inv Thr", true, s.yoke_invert_throttle)) {
        s.yoke_invert_throttle = !s.yoke_invert_throttle;
        changed = true;
    }
    if (button(ui, {1320, 996, 228, 36}, "Center Yoke", yk.connected)) {
        ui.calibrate_yoke();
    }
    if (button(ui, {1564, 996, 124, 36}, "Rescan")) {
        ui.rescan_yokes();
    }
    if (button(ui, {1704, 996, 126, 36}, "Reset")) {
        s.yoke_deadzone_pct = 5;
        s.yoke_curve_pct = 35;
        s.yoke_direct = true;
        s.yoke_invert_pitch = s.yoke_invert_roll = s.yoke_invert_throttle = false;
        ui.reset_yoke_calibration();
        changed = true;
    }
    if (changed) save_graphics_settings(s);
}

void GameFrontend::draw_loading_screen(AircraftAndHud &ui, int win_w, int win_h,
                                       const char *title, const char *stage, float progress01) {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, win_w, win_h);
    glDisable(GL_DEPTH_TEST);
    glClearColor(0.032f, 0.052f, 0.070f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ui.hud_verts_.clear();
    panel(ui, {460, 390, 1000, 270});
    label(ui, 504, 428, 30, title ? title : "OpenSky VFR - Preparing Flight");
    label(ui, 504, 484, 20, stage ? stage : "Streaming terrain & world data...", true);
    const float p = std::clamp(progress01, 0.0f, 1.0f);
    ui.hud_rect(504, 540, 912, 22, 0.09f, 0.14f, 0.19f, 0.95f);
    ui.hud_rect(506, 542, std::max(8.0f, 908.0f * p), 18, 0.24f, 0.64f, 0.78f, 0.98f);
    char pct[48];
    std::snprintf(pct, sizeof(pct), "%d%%  |  VRAM-safe terrain streaming", static_cast<int>(std::lround(p * 100.0f)));
    label(ui, 504, 584, 17, pct, true);
    ui.flush_hud(1920.0f, 1080.0f, font_);
}

void GameFrontend::draw_controls(AircraftAndHud &ui) {
    label(ui, 64, 111, 44, "Controls");
    label(ui, 66, 169, 20, "Walk around your Skyhawk to inspect & service it, start up at parking, and taxi to the runway.", true);
    panel(ui, {64, 220, 846, 760});
    panel(ui, {942, 220, 914, 760});
    label(ui, 90, 245, 28, "Walkaround & Engine Care");
    label(ui, 90, 294, 19, "W / A / S / D + Shift   Walk around aircraft (6 inspection spots)");
    label(ui, 90, 332, 19, "Drag / Arrows              Look around");
    label(ui, 90, 370, 19, "E                                  Inspect nearest part (Nose/Wings/Gear/Tail/Door)");
    label(ui, 90, 408, 19, "Enter                           Board aircraft while inspecting");
    label(ui, 90, 446, 19, "Esc                               Close panel / return to World");
    ui.hud_rect(90, 492, 790, 1, .21f, .32f, .39f, .85f);
    label(ui, 90, 512, 24, "Cockpit Systems (In Flight)");
    label(ui, 90, 554, 19, "Tab                              Toggle clickable Systems & Checklist Kneeboard");
    label(ui, 90, 592, 19, "K                                  Quick Start / Clean Shutdown helper");
    label(ui, 90, 630, 19, "1 / 2                            Master (BAT+ALT)  /  Avionics Master");
    label(ui, 90, 668, 19, "3 / Hold 4                    Cycle Magnetos (OFF/R/L/BOTH)  /  Crank Starter");
    label(ui, 90, 706, 19, "5 / 6                            Mixture Lean / Rich (pull to 0% for shutdown)");
    label(ui, 90, 744, 19, "7 / 8                            Carb Heat  /  Prime (stopped) or Pitot Heat (running)");
    label(ui, 90, 798, 18, "Remove chocks, tie-downs & pitot cover before takeoff!", true);
    label(ui, 90, 830, 18, "Full hangar service at Castejon, Santa Cilia, Huesca & Zaragoza.", true);
    label(ui, 90, 862, 18, "Remote strips have a field kit only - maintain before mountain legs.", true);
    label(ui, 90, 894, 18, "After landing, taxi off the runway, shut down, and park to log the flight.", true);

    label(ui, 968, 245, 28, "Flying & Camera");
    label(ui, 968, 294, 19, "Right-drag mouse     Look around (any camera view)");
    label(ui, 968, 334, 19, "C / R                          Cycle view (Cockpit/Chase/Free) / center");
    label(ui, 968, 374, 19, "W / S                         Increase / reduce throttle");
    label(ui, 968, 414, 19, "A / D                         Roll left / right");
    label(ui, 968, 454, 19, "Up / Down               Pitch down / up");
    label(ui, 968, 494, 19, "Q / E                         Rudder left / right (nosewheel steering)");
    label(ui, 968, 534, 19, "X                                Release parking brake / wheel brakes");
    label(ui, 968, 574, 19, "F / V                          Flaps down / up");
    label(ui, 968, 614, 19, "Y                                Toggle mouse yoke (holds position; R centers)");
    label(ui, 968, 654, 19, "T                                Toggle in-flight controls tutorial");
    label(ui, 968, 694, 19, "PgUp / PgDn           Seat height (or Free Cam up/down, Chase zoom)");
    label(ui, 968, 734, 19, "Home / End             Seat fore/aft (or Free Cam forward/back)");
    label(ui, 968, 774, 19, "Left / Right             Seat left/right (or Free Cam / Chase orbit)");
    label(ui, 968, 814, 19, "Backspace / Esc     Reset seat/cam   |   Pause & Menu");
    label(ui, 968, 870, 18, "Tip: Lean mixture (5) above 1,500 m MSL for full Pyrenean climb.", true);
    label(ui, 968, 902, 18, "Warm oil past 110 F before full power to preserve cylinder health.", true);

    panel(ui, {64, 988, 1792, 56});
    const auto &yk = ui.yoke_state();
    char yk_msg[224];
    if (yk.connected) {
        std::snprintf(yk_msg, sizeof(yk_msg),
                      "USB Yoke Active: %.56s  |  Trigger: Brakes   Thumb: Park   Rockers: Trim/Flaps   Hat: Look",
                      yk.device_summary.c_str());
        label(ui, 90, 1007, 16, yk_msg);
        if (button(ui, {1628, 998, 202, 36}, "Center Yoke")) ui.calibrate_yoke();
    } else {
        label(ui, 90, 1007, 16,
              "USB Yoke / Rudder Pedals / Throttle: Plug & Play via Linux evdev  (Settings -> Deadzone, Curve, Direct Mode, Calibration)", true);
        if (button(ui, {1628, 998, 202, 36}, "Yoke Settings")) page_ = Page::Settings;
    }
}

void GameFrontend::draw_credits(AircraftAndHud &ui) {
    label(ui, 66, 101, 12, "OPEN SOURCE  /  COMMUNITY BUILT", true);
    label(ui, 64, 132, 38, "About OpenSky");
    label(ui, 66, 174, 17, "Flight sim source, regional content, and the rules each piece travels with.", true);

    panel(ui, {64, 225, 860, 326});
    label(ui, 90, 250, 24, "Open-source engine");
    label(ui, 90, 294, 18, "OpenSky source is MIT licensed. See LICENSE.");
    label(ui, 90, 330, 17, "JSBSim is a separate LGPL-2.1-or-later shared library.");
    label(ui, 90, 366, 17, "Third-party code and assets keep their own licenses.");
    label(ui, 90, 422, 16, "Source contributions and source-level mods are welcome.", true);

    panel(ui, {950, 225, 906, 326});
    label(ui, 976, 250, 24, "Scenery, data & attribution");
    label(ui, 976, 294, 17, "Terrain and imagery: IGN Spain, CC BY 4.0.");
    label(ui, 976, 328, 17, "Roads, buildings and city data: OpenStreetMap contributors.");
    label(ui, 976, 362, 17, "OSM-derived packs retain ODbL 1.0 attribution and share-alike.");
    label(ui, 976, 396, 17, "OurAirports data is public domain; other feeds keep source terms.");
    label(ui, 976, 440, 16, "Live weather uses Open-Meteo and follows its current service terms.", true);
    label(ui, 976, 478, 15, "Full notices: THIRD_PARTY_LICENSES.md", true);

    panel(ui, {64, 579, 860, 366});
    label(ui, 90, 604, 24, "Make and install community content");
    label(ui, 90, 648, 17, "Install regions in ~/.local/share/opensky-vfr/packs.");
    label(ui, 90, 682, 17, "The simulator discovers region packs from that folder.");
    label(ui, 90, 716, 17, "Aircraft folders load package data, models, and a JSBSim flight model.");
    label(ui, 90, 750, 17, "Systems currently support one piston engine and two fuel tanks.");
    label(ui, 90, 784, 17, "Lua aircraft scripts are planned; complex systems still need engine work.");
    label(ui, 90, 828, 17, "Mods must include their own source, asset, and data license notes.");
    label(ui, 90, 874, 16, "Guide: docs/MODDING.md", true);

    panel(ui, {950, 579, 906, 366});
    label(ui, 976, 604, 24, "Simulation use");
    label(ui, 976, 648, 17, "OpenSky is for simulation and entertainment only.");
    label(ui, 976, 682, 17, "It is not certified for real-world navigation or flight training.");
    label(ui, 976, 716, 17, "Use current official charts, weather and aircraft procedures.");
    ui.hud_rect(976, 777, 854, 1, .18f, .34f, .38f, .85f);
    label(ui, 976, 805, 16, "Community aircraft and regions are independent works.", true);
    label(ui, 976, 839, 16, "Their authors are responsible for rights to included content.", true);

    label(ui, 66, 1002, 14, "OpenSky VFR  |  Built for curious pilots, careful modders, and scenic flights.", true);
}

void GameFrontend::draw_walk(AircraftAndHud &ui, const PlatformWindow &win,
                             const RegionalAtmosphere *reg_atmo, const AtmosphereState *atmo, double sim_time_sec) {
    read_pointer(win);
    ui.hud_verts_.clear();
    char line[192];
    const auto *home = parked_airport();
    const bool full_svc = home ? is_full_service_airport(home->id) : true;

    // Top-left: Airport + Service Tier + Live Local Weather
    if (home) {
        panel(ui, {28, 28, 580, 128});
        std::snprintf(line, sizeof(line), "%s  -  %s", home->id.c_str(), home->name.c_str());
        label(ui, 48, 42, 21, line);
        label(ui, 48, 76, 16, full_svc
            ? "Full Hangar Service (free oil change, tires, overhaul, charger)"
            : "Remote Strip - Field Kit Only (top-ups, sumps, plugs, patches)", true);
        if (reg_atmo && atmo) {
            const float wx_est = std::clamp((atmo->cloud_cov - 0.16f) / 0.78f, 0.0f, 1.0f);
            const LocalAirSample wx = reg_atmo->sample_at(session_.profile().parking_spot_.lat, session_.profile().parking_spot_.lon,
                                                          session_.profile().parking_spot_.alt, atmo->sun_dir, sim_time_sec, wx_est);
            const double w_kt = std::hypot(wx.wind_east_mps, wx.wind_north_mps) * 1.94384;
            const int w_from = int(std::lround(std::fmod(std::atan2(-wx.wind_east_mps, -wx.wind_north_mps) * RAD2DEG_D + 360.0, 360.0))) % 360;
            std::snprintf(line, sizeof(line), "WX: Wind %03d/%.0f kt | OAT %.0f C | QNH %.0f%s",
                          w_from, w_kt, wx.oat_c, wx.qnh_hpa, wx.precipitation_mm_hr > 0.2 ? " | RAIN" : "");
            label(ui, 48, 108, 16, line, true);
        }
    }

    // Top-right (when walking outside): Live Walkaround Checklist strip
    if (!inspecting_) {
        panel(ui, {1360, 28, 532, 232});
        label(ui, 1384, 44, 20, "Walkaround Checklist (EC-VFR)");
        std::snprintf(line, sizeof(line), "[%c] Nose: Oil %.1f qt (%.1f hr)  Prop %.0f%%",
                      (walk_checked_[0] && session_.profile().care_.oil_quarts >= 5.5) ? 'x' : ' ',
                      session_.profile().care_.oil_quarts, session_.profile().care_.oil_hours, (1.0 - session_.profile().care_.prop_nicks) * 100.0);
        label(ui, 1384, 78, 15, line, session_.profile().care_.oil_quarts >= 5.5);
        std::snprintf(line, sizeof(line), "[%c] L Wing: %.1f gal | Pitot %s | Sumps %s",
                      (!session_.profile().care_.pitot_cover_on && session_.profile().care_.sump_water < 0.12) ? 'x' : ' ',
                      session_.profile().care_.left_fuel_gal, session_.profile().care_.pitot_cover_on ? "COVER ON!" : "clear",
                      session_.profile().care_.sump_water > 0.12 ? "WATER!" : "clean");
        label(ui, 1384, 104, 15, line, !session_.profile().care_.pitot_cover_on && session_.profile().care_.sump_water < 0.12);
        std::snprintf(line, sizeof(line), "[%c] R Wing: %.1f gal | Tie-downs %s",
                      (!session_.profile().care_.tie_downs_on) ? 'x' : ' ',
                      session_.profile().care_.right_fuel_gal, session_.profile().care_.tie_downs_on ? "TIED" : "removed");
        label(ui, 1384, 130, 15, line, !session_.profile().care_.tie_downs_on);
        std::snprintf(line, sizeof(line), "[%c] Gear: Chocks %s | Tires %.0f%% | Brakes %.0f%%",
                      (!session_.profile().care_.chocks_on && session_.profile().care_.tire_wear < 0.6) ? 'x' : ' ',
                      session_.profile().care_.chocks_on ? "IN PLACE" : "removed",
                      (1.0 - session_.profile().care_.tire_wear) * 100.0, (1.0 - session_.profile().care_.brake_wear) * 100.0);
        label(ui, 1384, 156, 15, line, !session_.profile().care_.chocks_on);
        std::snprintf(line, sizeof(line), "[%c] Tail & Door: Airframe %.0f%% | Bat %.0f%% | Glass %.0f%%",
                      (session_.profile().care_.airframe_damage < 0.25 && session_.profile().care_.battery_soc > 0.35) ? 'x' : ' ',
                      (1.0 - session_.profile().care_.airframe_damage) * 100.0, session_.profile().care_.battery_soc * 100.0,
                      (1.0 - session_.profile().care_.windshield_dirt) * 100.0);
        label(ui, 1384, 182, 15, line, session_.profile().care_.battery_soc > 0.35);
        if (!walk_notice_.empty()) {
            label(ui, 1384, 214, 15, walk_notice_.c_str());
        } else {
            label(ui, 1384, 214, 15, "Walk to Nose, Wings, Gear, Tail, or Door and press E.", true);
        }
    }

    static constexpr const char *kStationNames[6] = {
        "Nose & Cowling", "Left Wing & Pitot", "Right Wing & Tie-Downs",
        "Landing Gear & Chocks", "Tail & Controls", "Cabin Door & Battery"
    };
    static constexpr const char *kStationShort[6] = {"Nose", "L Wing", "R Wing", "Gear", "Tail", "Door"};
    static constexpr const char *kStationHint[6] = {
        "check oil dipstick, spark plugs & propeller",
        "check left fuel, drain sump & pitot cover",
        "check right fuel, drain sump & tie-downs",
        "check tires, brakes, alignment & wheel chocks",
        "check control surfaces & airframe structure",
        "check battery, clean windshield & board"
    };

    const float distance = distance_to_plane();
    float st_dist = 99.0f;
    const WalkStation near_st = nearest_walk_station(st_dist);

    if (!inspecting_) {
        panel(ui, {540, 920, 840, 118});
        if (distance < 9.0f) {
            std::snprintf(line, sizeof(line), "%s   |   %.1f m",
                          kStationNames[static_cast<int>(near_st)], st_dist);
            label(ui, 566, 938, 22, line);
            std::snprintf(line, sizeof(line), "E  Inspect (%s)", kStationHint[static_cast<int>(near_st)]);
            label(ui, 566, 976, 17, line);
            label(ui, 566, 1004, 15, "WASD Walk around aircraft   /   Drag or arrows Look   /   Esc World", true);
        } else {
            std::snprintf(line, sizeof(line), "Cessna 172S (EC-VFR)   |   %.0f m", distance);
            label(ui, 566, 940, 22, line);
            label(ui, 566, 982, 17, "WASD Walk toward aircraft   /   Shift Run   /   Drag or arrows Look   /   Esc World", true);
        }
        ui.hud_rect(w/2-4, h/2, 9, 1, .94f, .98f, 1, .8f);
        ui.hud_rect(w/2, h/2-4, 1, 9, .94f, .98f, 1, .8f);
    } else {
        // 6-Station Spatial Walkaround & Servicing Clipboard
        panel(ui, {1160, 145, 720, 795});
        label(ui, 1188, 168, 28, "Cessna 172S Walkaround");
        label(ui, 1188, 206, 16, full_svc
            ? "Home / Regional Hangar - full servicing available (all free)"
            : "Remote Strip - Field Kit only (major overhaul/swap at Castejon, LECI, LEHC, LEZG)", true);

        // Station selector tabs (all 6 spots directly reachable)
        for (int i = 0; i < 6; ++i) {
            const bool sel = (static_cast<int>(active_station_) == i);
            if (button(ui, {1188 + i * 111, 240, 105, 36}, kStationShort[i], true, sel)) {
                active_station_ = static_cast<WalkStation>(i);
                walk_checked_[i] = true;
            }
        }
        ui.hud_rect(1188, 288, 664, 1, .21f, .32f, .39f, .85f);
        label(ui, 1188, 302, 22, kStationNames[static_cast<int>(active_station_)]);

        const double total_fuel = session_.profile().care_.left_fuel_gal + session_.profile().care_.right_fuel_gal;
        switch (active_station_) {
            case WalkStation::Nose: {
                std::snprintf(line, sizeof(line), "Oil Dipstick: %.1f / %.1f qt   |   Oil Age: %.1f / 10.0 hr%s",
                              session_.profile().care_.oil_quarts, ui.aircraft_.oil_capacity_quarts,
                              session_.profile().care_.oil_hours, session_.profile().care_.oil_hours >= 10.0 ? "  (CHANGE DUE)" : "");
                label(ui, 1188, 344, 18, line);
                std::snprintf(line, sizeof(line), "Cylinders: %.0f%%   |   Spark Plugs: %.0f%% clean   |   Propeller: %.0f%%",
                              (1.0 - session_.profile().care_.cylinder_wear) * 100.0, (1.0 - session_.profile().care_.spark_fouling) * 100.0,
                              (1.0 - session_.profile().care_.prop_nicks) * 100.0);
                label(ui, 1188, 376, 17, line, true);

                if (button(ui, {1188, 418, 322, 44}, "Top up oil (+1 qt)", session_.profile().care_.oil_quarts < ui.aircraft_.oil_capacity_quarts - 0.05)) {
                    session_.service(AircraftService::TopOil);
                }
                if (button(ui, {1530, 418, 322, 44}, full_svc ? "Oil & filter change" : "Oil change (hangar only)",
                           full_svc && (session_.profile().care_.oil_hours > 0.2 || session_.profile().care_.oil_quarts < 7.5))) {
                    session_.service(AircraftService::ChangeOil);
                }
                if (button(ui, {1188, 474, 322, 44}, "Clean spark plugs (field ok)", session_.profile().care_.spark_fouling > 0.01)) {
                    session_.service(AircraftService::CleanPlugs);
                }
                if (button(ui, {1530, 474, 322, 44}, full_svc ? "Dress / replace prop" : "File minor prop nicks",
                           session_.profile().care_.prop_nicks > 0.01)) {
                    session_.service(AircraftService::RepairProp);
                }
                if (button(ui, {1188, 530, 664, 44},
                           full_svc ? "Overhaul engine cylinders (hangar)" : "Engine overhaul (return to full-service airport)",
                           full_svc && (session_.profile().care_.cylinder_wear > 0.01 || session_.profile().care_.shock_cooling > 0.01))) {
                    session_.service(AircraftService::Overhaul);
                }
                std::snprintf(line, sizeof(line), "Keep oil above %.1f qt. Warm oil >110 F before full power.",
                              ui.aircraft_.oil_capacity_quarts * 0.75);
                label(ui, 1188, 592, 16, line, true);
                break;
            }
            case WalkStation::LeftWing: {
                std::snprintf(line, sizeof(line), "Left Tank: %.1f / %.1f US gal   |   Total: %.1f gal",
                              session_.profile().care_.left_fuel_gal, ui.aircraft_.left_tank_capacity_gal, total_fuel);
                label(ui, 1188, 344, 18, line);
                const bool can_add_l = session_.can_add_fuel(true);
                if (button(ui, {1188, 384, 210, 44}, "-5 gal Left", session_.profile().care_.left_fuel_gal > 0.05)) {
                    session_.adjust_fuel(true, false);
                }
                if (button(ui, {1415, 384, 210, 44}, full_svc ? "+5 gal Left" : "+5 gal (jerrycan)", can_add_l)) {
                    session_.adjust_fuel(true, true);
                }
                char fill_label[64];
                std::snprintf(fill_label, sizeof(fill_label), "Fill Left (%.1f)", ui.aircraft_.left_tank_capacity_gal);
                if (button(ui, {1642, 384, 210, 44}, full_svc ? fill_label : "Fill (hangar only)",
                           full_svc && session_.profile().care_.left_fuel_gal < ui.aircraft_.left_tank_capacity_gal - 0.05)) {
                    session_.service(AircraftService::FillLeft);
                }
                std::snprintf(line, sizeof(line), "Left Fuel Sump: %s",
                              session_.profile().care_.sump_water > 0.12 ? "WATER DETECTED - drain before flight!" : "Clean avgas (no water)");
                label(ui, 1188, 448, 17, line, session_.profile().care_.sump_water <= 0.12);
                if (button(ui, {1188, 482, 664, 44}, "Drain fuel sumps (clear condensation / water)", session_.profile().care_.sump_water > 0.01)) {
                    session_.service(AircraftService::DrainSumps);
                }
                std::snprintf(line, sizeof(line), "Pitot Tube Cover: %s",
                              session_.profile().care_.pitot_cover_on ? "FITTED (airspeed indicator will read 0!)" : "REMOVED (clear)");
                label(ui, 1188, 546, 17, line, !session_.profile().care_.pitot_cover_on);
                if (button(ui, {1188, 580, 664, 44},
                           session_.profile().care_.pitot_cover_on ? "Remove red pitot cover" : "Fit red pitot cover (for parking)",
                           true, session_.profile().care_.pitot_cover_on)) {
                    session_.service(AircraftService::TogglePitot);
                }
                break;
            }
            case WalkStation::RightWing: {
                std::snprintf(line, sizeof(line), "Right Tank: %.1f / %.1f US gal   |   Total: %.1f gal",
                              session_.profile().care_.right_fuel_gal, ui.aircraft_.right_tank_capacity_gal, total_fuel);
                label(ui, 1188, 344, 18, line);
                const bool can_add_r = session_.can_add_fuel(false);
                if (button(ui, {1188, 384, 210, 44}, "-5 gal Right", session_.profile().care_.right_fuel_gal > 0.05)) {
                    session_.adjust_fuel(false, false);
                }
                if (button(ui, {1415, 384, 210, 44}, full_svc ? "+5 gal Right" : "+5 gal (jerrycan)", can_add_r)) {
                    session_.adjust_fuel(false, true);
                }
                if (button(ui, {1642, 384, 210, 44}, full_svc ? "Fill Both Tanks" : "Fill (hangar only)",
                           full_svc && (session_.profile().care_.left_fuel_gal < ui.aircraft_.left_tank_capacity_gal - 0.05 ||
                                        session_.profile().care_.right_fuel_gal < ui.aircraft_.right_tank_capacity_gal - 0.05))) {
                    session_.service(AircraftService::FillBoth);
                }
                if (button(ui, {1188, 446, 664, 44},
                           session_.profile().care_.sump_water > 0.12 ? "Drain right fuel sump (WATER DETECTED)" : "Check / drain right sump (clear)",
                           session_.profile().care_.sump_water > 0.01)) {
                    session_.service(AircraftService::DrainSumps);
                }
                std::snprintf(line, sizeof(line), "Wing Tie-Down Ropes: %s",
                              session_.profile().care_.tie_downs_on ? "SECURED (remove before taxi)" : "REMOVED (secure in gusty wind!)");
                label(ui, 1188, 514, 17, line, !session_.profile().care_.tie_downs_on);
                if (button(ui, {1188, 548, 664, 44},
                           session_.profile().care_.tie_downs_on ? "Untie wing tie-downs" : "Secure wing tie-downs (protect in wind)",
                           true, session_.profile().care_.tie_downs_on)) {
                    session_.service(AircraftService::ToggleTieDowns);
                }
                label(ui, 1188, 606, 15, "Tip: Fly lighter fuel out of high-altitude grass strips like Castejon (900m).", true);
                break;
            }
            case WalkStation::Gear: {
                std::snprintf(line, sizeof(line), "Tires: %.0f%%   |   Brakes: %.0f%%   |   Alignment: %s",
                              (1.0 - session_.profile().care_.tire_wear) * 100.0, (1.0 - session_.profile().care_.brake_wear) * 100.0,
                              std::abs(session_.profile().care_.gear_alignment) < 0.08 ? "True" :
                              (session_.profile().care_.gear_alignment > 0 ? "Pulls Right" : "Pulls Left"));
                label(ui, 1188, 344, 18, line);
                std::snprintf(line, sizeof(line), "Wheel Chocks: %s",
                              session_.profile().care_.chocks_on ? "IN PLACE (aircraft cannot roll until removed)" : "REMOVED");
                label(ui, 1188, 384, 17, line, !session_.profile().care_.chocks_on);
                if (button(ui, {1188, 418, 664, 46},
                           session_.profile().care_.chocks_on ? "Remove wheel chocks" : "Place wheel chocks (for parking)",
                           true, session_.profile().care_.chocks_on)) {
                    session_.service(AircraftService::ToggleChocks);
                }
                const bool need_gear = session_.profile().care_.tire_wear > 0.02 || session_.profile().care_.brake_wear > 0.02 || std::abs(session_.profile().care_.gear_alignment) > 0.03;
                if (button(ui, {1188, 482, 664, 46},
                           full_svc ? "Replace tires, brake pads & align gear (hangar)"
                                    : "Field patch tires & adjust brakes (up to 75%)",
                           need_gear)) {
                    session_.service(AircraftService::RepairGear);
                }
                label(ui, 1188, 548, 16, "Hard landings and gravel strips wear tires and knock gear alignment.", true);
                break;
            }
            case WalkStation::Tail: {
                std::snprintf(line, sizeof(line), "Airframe Structure: %.0f%%   |   Engine Time: %.1f hr",
                              (1.0 - session_.profile().care_.airframe_damage) * 100.0, session_.profile().hours_);
                label(ui, 1188, 344, 18, line);
                label(ui, 1188, 380, 17, "Elevator, rudder & trim tab: free and correct.", true);
                if (button(ui, {1188, 424, 664, 46},
                           session_.profile().care_.airframe_damage > 0.01
                               ? (full_svc ? "Full airframe inspection & repair (hangar)" : "Field airframe patch (up to 75%)")
                               : "Airframe clean - no repairs needed",
                           session_.profile().care_.airframe_damage > 0.01)) {
                    session_.service(AircraftService::RepairAirframe);
                }
                label(ui, 1188, 492, 16, "Always secure tie-downs in gusts >16 kt to prevent tail & control wear.", true);
                break;
            }
            case WalkStation::Door: {
                std::snprintf(line, sizeof(line), "%dV Battery: %.0f%%   |   Windshield: %.0f%% clean, %.0f%% intact",
                              static_cast<int>(std::round(ui.aircraft_.bus_voltage * 6.0 / 7.0)),
                              session_.profile().care_.battery_soc * 100.0,
                              (1.0 - session_.profile().care_.windshield_dirt) * 100.0,
                              (1.0 - session_.profile().care_.windshield_crack) * 100.0);
                label(ui, 1188, 344, 18, line);
                std::snprintf(line, sizeof(line), "Cockpit left as: %s  |  Master %s  |  Mags %s",
                              session_.profile().care_.engine_running ? "Running" : "Cold & Dark",
                              session_.profile().care_.master_on ? "ON" : "OFF",
                              session_.profile().care_.magnetos == 0 ? "OFF" : (session_.profile().care_.magnetos == 3 ? "BOTH" : "L/R"));
                label(ui, 1188, 378, 16, line, true);
                if (button(ui, {1188, 418, 212, 44},
                           full_svc ? "Charge battery" : "Jump battery",
                           session_.profile().care_.battery_soc < (full_svc ? 0.96 : 0.54))) {
                    session_.service(AircraftService::ChargeBattery);
                }
                if (button(ui, {1414, 418, 212, 44}, "Clean windshield", session_.profile().care_.windshield_dirt > 0.02)) {
                    session_.service(AircraftService::CleanWindshield);
                }
                if (button(ui, {1640, 418, 212, 44},
                           full_svc ? "Replace glass" : "Patch crack",
                           session_.profile().care_.windshield_crack > 0.02)) {
                    session_.service(AircraftService::ReplaceWindshield);
                }
                const bool secured = session_.profile().care_.chocks_on || session_.profile().care_.pitot_cover_on || session_.profile().care_.tie_downs_on;
                if (button(ui, {1188, 476, 664, 44},
                           secured ? "Quick Pre-Flight: remove chocks, pitot cover & tie-downs"
                                   : "Quick Post-Flight: fit chocks, pitot cover & tie-downs")) {
                    session_.service(AircraftService::ToggleSecuring);
                    if (!session_.profile().care_.chocks_on) {
                        for (bool &c : walk_checked_) c = true;
                    }
                }
                label(ui, 1188, 536, 16, "Remember to switch Master OFF before parking or the battery will drain.", true);
                break;
            }
        }

        // Bottom persistent Boarding & Return footer on every station tab
        ui.hud_rect(1188, 664, 664, 1, .21f, .32f, .39f, .85f);
        if (session_.profile().care_.chocks_on || session_.profile().care_.pitot_cover_on || session_.profile().care_.tie_downs_on || session_.profile().care_.sump_water > 0.12) {
            std::snprintf(line, sizeof(line), "Heads-up: %s%s%s%s",
                          session_.profile().care_.chocks_on ? "[Chocks ON] " : "",
                          session_.profile().care_.pitot_cover_on ? "[Pitot Cover ON] " : "",
                          session_.profile().care_.tie_downs_on ? "[Tied Down] " : "",
                          session_.profile().care_.sump_water > 0.12 ? "[Sump Water]" : "");
            label(ui, 1188, 678, 16, line);
        } else {
            label(ui, 1188, 678, 16, "Exterior clear for boarding. Cold & dark? Press Tab or K inside cockpit.", true);
        }
        const bool can_board = session_.profile().care_.airframe_damage < 0.85;
        if (button(ui, {1188, 714, 664, 54},
                   can_board ? "Board aircraft at parking (Enter)" : "Airframe too damaged - repair before boarding",
                   can_board, true) ||
            (win.key_pressed(KEY_ENTER) && can_board)) {
            request_ = Request::Board;
        }
        if (button(ui, {1188, 782, 322, 46}, "Back to walkaround (Esc)")) inspecting_ = false;
        if (button(ui, {1530, 782, 322, 46}, "Return to World map")) { inspecting_ = false; walking_ = false; }
    }
    ui.flush_hud(w, h, font_);
}

void GameFrontend::draw_navigation(AircraftAndHud &ui, const PlatformWindow &win) {
    if (!session_.flight_active_) return;
    read_pointer(win);
    ui.hud_verts_.clear();
    char line[192];

    const bool show_yoke = ui.state().mouse_yoke_enabled && ui.state().view_mode != 2;
    const bool on_gnd = ui.jsb_state().on_ground;
    panel(ui, {28, 28, 768, ui.state().runway_tracking || show_yoke ? 128 : 96});
    label(ui, 48, 44, 17, "Tab: Systems  K: Quick Start/Stop  T: Tutorial  C: View  Esc: Menu", true);
    label(ui, 48, 76, 17, on_gnd
        ? (show_yoke
            ? "TAXI ASSIST ON | Trackpad/A-D: steer  W/S: gas/brake  Click/R: straight  Y: yoke"
            : "TAXI ASSIST ON | A/D: steer  W/S: gas/brake  Y: trackpad yoke  R: straight")
        : (show_yoke
            ? "FLIGHT ASSIST ON | Cursor-yoke: drag around anchor  Click/R: level  Y: toggle"
            : "FLIGHT ASSIST ON | A/D: bank  Up/Dn: VSI pitch  Y: cursor yoke  R: level"), true);
    if (ui.state().runway_tracking)
        label(ui, 48, 108, 17, "Runway centreline assist ON   /   A-D, Q-E or trackpad: manual steer", true);
    if (show_yoke) {
        constexpr float x = 740, y = 92, radius = 38;
        const bool at_limit = std::abs(ui.mouse_yoke_roll_) > 0.94 || std::abs(ui.mouse_yoke_pitch_) > 0.94;
        const float br = at_limit ? 0.98f : 0.50f;
        const float bg = at_limit ? 0.68f : 0.72f;
        const float bb = at_limit ? 0.28f : 0.76f;
        ui.hud_border(x - radius, y - radius, 2 * radius, 2 * radius, at_limit ? 2.0f : 1.0f, br, bg, bb, 0.92f);
        ui.hud_rect(x - radius, y, 2 * radius, 1, .50f, .70f, .74f, .45f);
        ui.hud_rect(x, y - radius, 1, 2 * radius, .50f, .70f, .74f, .45f);
        // Show magnetic neutral detent box (radius * 0.16) so trackpad leveling is immediately visible.
        const float det = radius * 0.16f;
        ui.hud_border(x - det, y - det, 2 * det, 2 * det, 1, .45f, .86f, .82f, .65f);
        const float marker_x = x + (radius - 4) * static_cast<float>(ui.mouse_yoke_roll_);
        const float marker_y = y + (radius - 4) * static_cast<float>(ui.mouse_yoke_pitch_);
        ui.hud_rect(marker_x - 3.5f, marker_y - 3.5f, 7, 7, at_limit ? 1.0f : .62f, at_limit ? .74f : .94f, at_limit ? .32f : .91f, 1.0f);
        // Subtle floating center-screen trackpad reticle: springs smoothly to center when finger leaves trackpad!
        {
            const float ax = GAME_OUTPUT_W * 0.50f;
            const float ay = GAME_OUTPUT_H * 0.50f;
            const float px = ax + static_cast<float>(ui.mouse_yoke_roll_) * 125.0f;
            const float py = ay + static_cast<float>(ui.mouse_yoke_pitch_) * 100.0f;
            const bool centered = std::abs(ui.mouse_yoke_roll_) < 0.015 && std::abs(ui.mouse_yoke_pitch_) < 0.015;
            const float cr = centered ? 0.42f : 0.65f, cg = centered ? 0.94f : 0.90f, cb = centered ? 0.78f : 1.0f;
            ui.hud_border(ax - 20.0f, ay - 20.0f, 40.0f, 40.0f, 1.5f, cr, cg, cb, 0.45f);
            ui.hud_rect(ax - 5.0f, ay - 0.5f, 10.0f, 1.0f, cr, cg, cb, 0.55f);
            ui.hud_rect(ax - 0.5f, ay - 5.0f, 1.0f, 10.0f, cr, cg, cb, 0.55f);
            if (!centered) {
                ui.hud_line({ax, ay}, {px, py}, 1.5f, {cr, cg, cb});
                ui.hud_rect(px - 4.0f, py - 4.0f, 8.0f, 8.0f, 0.98f, 0.96f, 0.65f, 0.92f);
            }
        }
    }

    // Live Symptom & Aircraft Care Caution Banner (only shown when something needs attention!)
    const auto &sys = ui.systems_;
    std::string caution;
    if (ui.is_chocked_or_tied() && ui.jsb_state().on_ground)
        caution = "CHOCKS / TIE-DOWNS FITTED - Press Esc -> Park & step outside to remove!";
    else if (sys.is_pitot_blocked() && (ui.state().speed_mps > 8.0 || !ui.jsb_state().on_ground))
        caution = sys.is_pitot_cover_on()
            ? "PITOT COVER LEFT ON - Airspeed indicator inoperative! Abort or fly attitude."
            : "PITOT ICED OVER - Press 8 for Pitot Heat!";
    else if (ui.windscreen_frost() > 0.25f || ui.airframe_ice01() > 0.20f)
        caution = "STRUCTURAL & WINDSCREEN ICING - Turn Pitot Heat ON (8) & exit freezing cloud!";
    else if (ui.last_air().in_storm_cell)
        caution = "SEVERE STORM CELL - Heavy rain, lightning & downdrafts! Divert around shaft.";
    else if (ui.last_air().in_lee_rotor)
        caution = "MOUNTAIN LEE ROTOR - Strong turbulence & sink! Gain altitude clear of ridge.";
    else if (sys.has_overtemp_failure() || sys.has_oil_starvation_failure())
        caution = "ENGINE FAILURE (OVERTEMP / OIL) - Land and service at an airfield.";
    else if (sys.is_sputtering())
        caution = "ENGINE SPUTTERING (WATER IN FUEL SUMPS) - Drain sumps on walkaround.";
    else if (sys.get_carb_ice_level() > 0.18)
        caution = "CARBURETOR ICE - RPM dropping! Press 7 for Carb Heat.";
    else if (sys.is_engine_running() && sys.get_oil_temp_f() < 110.0 && ui.state().throttle > 0.72)
        caution = "COLD OIL (<110 F) - Reduce throttle until engine warms to avoid wear.";
    else if (sys.is_engine_running() && sys.get_oil_press_psi() < 32.0)
        caution = "LOW OIL PRESSURE - Check oil quantity on walkaround.";
    else if (sys.get_master_battery() && sys.get_battery_soc() < 0.18)
        caution = "LOW AIRCRAFT BATTERY - Start engine for alternator or charge on walkaround.";
    else if (session_.profile().care_.airframe_damage >= 0.5)
        caution = "AIRFRAME DAMAGED - Engine power reduced. Inspect & repair at an airport.";

    if (!caution.empty()) {
        panel(ui, {28, 170, 748, 56});
        label(ui, 48, 189, 17, caution.c_str());
    }

    // Touchdown Debrief Banner (auto-clears after 14 s)
    if (session_.td_banner_s_ > 0.0 && session_.has_touchdown_) {
        panel(ui, {560, 822, 800, 102});
        std::snprintf(line, sizeof(line), "TOUCHDOWN:  %.0f fpm   |   %.1f kt X-wind   |   %s   |   %s",
                      session_.td_fpm_, session_.td_xwind_kt_, session_.td_surface_.c_str(), session_.td_grade_.c_str());
        label(ui, 584, 842, 19, line);
        label(ui, 584, 880, 16, "Taxi clear of the runway, shut down (K), then Esc -> Park to log this flight.", true);
    }

    // Clickable In-Flight Systems & Checklist Kneeboard (Tab)
    if (kneeboard_open_) {
        const int kx = 28, ky = caution.empty() ? 174 : 238;
        panel(ui, {kx, ky, 590, 566});
        label(ui, kx + 22, ky + 18, 22, "Cockpit Systems & Checklist");
        if (button(ui, {kx + 458, ky + 12, 112, 32}, "Hide (Tab)")) kneeboard_open_ = false;

        // Live Engine & Electrical Telemetry
        const auto &fs = ui.jsb_state();
        std::snprintf(line, sizeof(line), "RPM %4.0f | Oil %.1fqt %.0fF %.0fpsi | CHT %.0fF",
                      fs.rpm, sys.get_oil_quarts(), sys.get_oil_temp_f(), sys.get_oil_press_psi(), sys.get_cht_f());
        label(ui, kx + 22, ky + 56, 16, line);
        std::snprintf(line, sizeof(line), "Bus %.1fV (Bat %.0f%%) | Mix %.0f%% | CarbIce %.0f%% | Plugs %.0f%%",
                      sys.get_bus_volts(), sys.get_battery_soc() * 100.0, ui.state().mixture * 100.0,
                      sys.get_carb_ice_level() * 100.0, (1.0 - sys.get_spark_fouling()) * 100.0);
        label(ui, kx + 22, ky + 84, 15, line, true);
        std::snprintf(line, sizeof(line), "Fuel: L %.1f / R %.1f gal  |  Flow %.1f gph  |  Flaps %.0f deg",
                      sys.get_left_tank_gal(), sys.get_right_tank_gal(),
                      sys.get_fuel_flow_gph(), sys.get_flaps_actual_deg());
        label(ui, kx + 22, ky + 110, 15, line, true);

        // Clickable Cockpit Controls (mirror keys 1..8 and K)
        if (button(ui, {kx + 22, ky + 144, 174, 38},
                   sys.get_master_battery() ? "[1] Master: ON" : "[1] Master: OFF", true, sys.get_master_battery())) {
            const bool next = !sys.get_master_battery();
            ui.systems_.set_master_battery(next);
            ui.systems_.set_master_alternator(next);
            ui.systems_.set_light_switch(0, next);
        }
        if (button(ui, {kx + 208, ky + 144, 174, 38},
                   sys.get_avionics_master() ? "[2] Avionics: ON" : "[2] Avionics: OFF", true, sys.get_avionics_master())) {
            ui.systems_.set_avionics_master(!sys.get_avionics_master());
        }
        static constexpr const char *kMagNames[5] = {"OFF", "R", "L", "BOTH", "START"};
        std::snprintf(line, sizeof(line), "[3] Mags: %s", kMagNames[std::clamp(sys.get_magnetos(), 0, 4)]);
        if (button(ui, {kx + 394, ky + 144, 174, 38}, line, true, sys.get_magnetos() == 3)) {
            ui.systems_.set_magnetos((sys.get_magnetos() + 1) % 4);
        }

        if (button(ui, {kx + 22, ky + 192, 174, 38}, "[5] Mix -10%", ui.state().mixture > 0.01)) {
            ui.state().mixture = std::max(0.0, ui.state().mixture - 0.10);
        }
        if (button(ui, {kx + 208, ky + 192, 174, 38}, "[6] Mix +10%", ui.state().mixture < 0.99)) {
            ui.state().mixture = std::min(1.0, ui.state().mixture + 0.10);
        }
        static constexpr const char *kFuelNames[4] = {"OFF", "LEFT", "BOTH", "RIGHT"};
        std::snprintf(line, sizeof(line), "Fuel: %s", kFuelNames[std::clamp(sys.get_fuel_selector(), 0, 3)]);
        if (button(ui, {kx + 394, ky + 192, 174, 38}, line, true, sys.get_fuel_selector() == 2)) {
            ui.systems_.cycle_fuel_selector();
        }

        if (button(ui, {kx + 22, ky + 240, 174, 38},
                   sys.get_carb_heat() > 0.5 ? "[7] CarbHt: ON" : "[7] CarbHt: OFF", true, sys.get_carb_heat() > 0.5)) {
            ui.systems_.set_carb_heat(sys.get_carb_heat() > 0.5 ? 0.0 : 1.0);
        }
        if (!sys.is_engine_running()) {
            std::snprintf(line, sizeof(line), "[8] Prime (%d)", sys.get_primer_strokes());
            if (button(ui, {kx + 208, ky + 240, 174, 38}, line, true, sys.get_primer_strokes() >= 2))
                ui.systems_.actuate_primer();
        } else {
            if (button(ui, {kx + 208, ky + 240, 174, 38},
                       sys.get_pitot_heat_switch() ? "[8] PitotHt: ON" : "[8] PitotHt: OFF", true, sys.get_pitot_heat_switch()))
                ui.systems_.set_pitot_heat_switch(!sys.get_pitot_heat_switch());
        }
        if (button(ui, {kx + 394, ky + 240, 174, 38},
                   sys.is_starter_cranking() ? "Cranking..." : "[4] Hold Start",
                   !sys.is_engine_running() && sys.get_master_battery(), sys.is_starter_cranking())) {
            // Clicking Start arms magnetos=4 briefly; holding key 4 cranks continuously
            ui.systems_.set_magnetos(4);
        }

        if (button(ui, {kx + 22, ky + 292, 546, 42},
                   sys.is_engine_running() ? "[K] Clean Shutdown (idle-cutoff, mags & master OFF)"
                                           : "[K] Quick Start Engine (helper)",
                   true, !sys.is_engine_running())) {
            ui.quick_start_or_shutdown();
        }

        // Dynamic Contextual Checklist (Start / Run-Up / Shutdown)
        ui.hud_rect(kx + 22, ky + 348, 546, 1, .21f, .32f, .39f, .85f);
        if (!sys.is_engine_running()) {
            label(ui, kx + 22, ky + 362, 18, "Cold-and-Dark Start Checklist:");
            std::snprintf(line, sizeof(line), "[%c] 1. Exterior chocks & pitot cover removed", !ui.is_chocked_or_tied() && !sys.is_pitot_cover_on() ? 'x' : ' ');
            label(ui, kx + 22, ky + 394, 15, line, !ui.is_chocked_or_tied() && !sys.is_pitot_cover_on());
            std::snprintf(line, sizeof(line), "[%c] 2. Fuel Selector BOTH & Master ON (key 1)", sys.get_fuel_selector() == 2 && sys.get_master_battery() ? 'x' : ' ');
            label(ui, kx + 22, ky + 422, 15, line, sys.get_fuel_selector() == 2 && sys.get_master_battery());
            std::snprintf(line, sizeof(line), "[%c] 3. Mixture Rich (key 6) & Prime 2x if cold (key 8)", ui.state().mixture >= 0.8 ? 'x' : ' ');
            label(ui, kx + 22, ky + 450, 15, line, ui.state().mixture >= 0.8);
            std::snprintf(line, sizeof(line), "[%c] 4. Hold key 4 to crank starter (or press K Quick Start)", sys.is_engine_running() ? 'x' : ' ');
            label(ui, kx + 22, ky + 478, 15, line, false);
            label(ui, kx + 22, ky + 514, 15, "After start: switch Avionics ON (key 2) and warm oil >110 F.", true);
        } else if (ui.jsb_state().on_ground) {
            label(ui, kx + 22, ky + 362, 18, "Run-Up & Before-Takeoff Checklist:");
            std::snprintf(line, sizeof(line), "[%c] 1. Oil temp >110 F & pressure green (%.0f F / %.0f psi)",
                          sys.get_oil_temp_f() >= 110.0 && sys.get_oil_press_psi() >= 40.0 ? 'x' : ' ',
                          sys.get_oil_temp_f(), sys.get_oil_press_psi());
            label(ui, kx + 22, ky + 394, 15, line, sys.get_oil_temp_f() >= 110.0);
            std::snprintf(line, sizeof(line), "[%c] 2. Magnetos BOTH (test L/R drop with key 3 at 1700 RPM)", sys.get_magnetos() == 3 ? 'x' : ' ');
            label(ui, kx + 22, ky + 422, 15, line, sys.get_magnetos() == 3);
            std::snprintf(line, sizeof(line), "[%c] 3. Carb Heat OFF (key 7) & Avionics ON (key 2)",
                          sys.get_carb_heat() < 0.1 && sys.get_avionics_master() ? 'x' : ' ');
            label(ui, kx + 22, ky + 450, 15, line, sys.get_carb_heat() < 0.1 && sys.get_avionics_master());
            std::snprintf(line, sizeof(line), "[%c] 4. Release brake (X), taxi onto clear runway, rotate 55 kt",
                          !ui.state().parking_brake ? 'x' : ' ');
            label(ui, kx + 22, ky + 478, 15, line, !ui.state().parking_brake);
            label(ui, kx + 22, ky + 514, 15, "After landing: clear runway, press K to shut down, then Esc -> Park.", true);
        } else {
            label(ui, kx + 22, ky + 362, 18, "In-Flight Engine Management:");
            label(ui, kx + 22, ky + 394, 15, "- Lean mixture (5) above 1,500 m MSL for Pyrenean climbs.", true);
            label(ui, kx + 22, ky + 422, 15, "- Pull Carb Heat (7) before low-power descents in moist air.", true);
            label(ui, kx + 22, ky + 450, 15, "- Reduce power gradually on descent to avoid shock cooling.", true);
            label(ui, kx + 22, ky + 478, 15, "- Turn Pitot Heat ON (8) in freezing clouds or precipitation.", true);
            label(ui, kx + 22, ky + 514, 15, "- Land smoothly, clear runway, shut down (K), then Esc -> Park.", true);
        }
    }

    if (tutorial_open_) {
        panel(ui, {1346, 28, 546, 366});
        label(ui, 1368, 44, 21, "Flight Controls Tutorial");
        if (button(ui, {1786, 38, 86, 30}, "Hide (T)")) tutorial_open_ = false;
        const bool on_ground = ui.jsb_state().on_ground;
        label(ui, 1368, 84, 16, !sys.is_engine_running()
            ? "1. STARTUP: Press Tab for Checklist, or press K Quick Start"
            : (on_ground ? "1. TAXI & TAKEOFF: X releases brake, W adds power, Q/E steer"
                         : "1. CRUISE: W/S throttle, 5/6 mixture, F/V flaps"), false);
        label(ui, 1368, 112, 16, on_ground
            ? "   Taxi onto the clear runway, rotate at 55 kt. Park off runway."
            : "   After landing, taxi clear of runway, press K, then Esc -> Park.", true);
        label(ui, 1368, 148, 16, "2. YOKE & FLIGHT CONTROLS:");
        label(ui, 1368, 176, 16, "   Y toggles mouse yoke (holds deflection, R centers).", true);
        label(ui, 1368, 202, 16, "   Mouse down = pitch UP | Keyboard: A/D, Up/Down, Q/E.", true);
        label(ui, 1368, 238, 16, "3. ENGINE & SYSTEMS (Tab Kneeboard):");
        label(ui, 1368, 266, 16, "   1 Master | 2 Avn | 3 Mags | 4 Start | 5/6 Mix | 7 Carb | 8 Prime", true);
        label(ui, 1368, 302, 16, "4. CAMERA & SEAT ADJUSTMENT:");
        label(ui, 1368, 330, 16, "   RMB drag: look | C: view | PgUp/Dn Home/End Arrows: seat/cam", true);
        label(ui, 1368, 356, 16, "   Backspace resets seat/cam | T hides tutorial.", true);
    }

    // Integrated Cockpit KAP-140 2-Axis Autopilot Strip (powered by Master + Avionics bus, drives real 3D yoke/trim!)
    if (!ui.jsb_state().on_ground && sys.get_master_battery() && sys.get_avionics_master()) {
        auto &st = ui.state();
        constexpr int ax = 1140, ay = 946;
        panel(ui, {ax, ay, 752, 91});
        label(ui, ax + 16, ay + 10, 15, "KAP-140 COCKPIT AUTOPILOT  (coupled to 3D Yoke & Trim)", true);
        if (button(ui, {ax + 16, ay + 38, 82, 40}, st.ap_master ? "AP ON" : "AP OFF", true, st.ap_master)) {
            st.ap_master = !st.ap_master;
            if (st.ap_master) {
                st.ap_hdg_deg = std::round(st.heading_deg);
                st.ap_alt_ft = std::round((st.alt_msl_m * 3.28084) / 100.0) * 100.0;
            }
        }
        std::snprintf(line, sizeof(line), "HDG %03d", (int(std::round(st.ap_hdg_deg)) % 360 + 360) % 360);
        if (button(ui, {ax + 108, ay + 38, 108, 40}, line, true, st.ap_master && st.ap_hdg_hold && !st.ap_nav_hold)) {
            st.ap_hdg_hold = true;
            st.ap_nav_hold = false;
            st.ap_master = true;
        }
        if (button(ui, {ax + 222, ay + 38, 40, 40}, "-", true)) st.ap_hdg_deg = std::fmod(st.ap_hdg_deg + 350.0, 360.0);
        if (button(ui, {ax + 268, ay + 38, 40, 40}, "+", true)) st.ap_hdg_deg = std::fmod(st.ap_hdg_deg + 10.0, 360.0);

        std::snprintf(line, sizeof(line), "ALT %.0f", st.ap_alt_ft);
        if (button(ui, {ax + 322, ay + 38, 120, 40}, line, true, st.ap_master && st.ap_alt_hold)) {
            st.ap_alt_hold = !st.ap_alt_hold;
            st.ap_master = true;
        }
        if (button(ui, {ax + 448, ay + 38, 42, 40}, "-5", true)) st.ap_alt_ft = std::max(1500.0, st.ap_alt_ft - 500.0);
        if (button(ui, {ax + 496, ay + 38, 42, 40}, "+5", true)) st.ap_alt_ft = std::min(13500.0, st.ap_alt_ft + 500.0);

        if (button(ui, {ax + 552, ay + 38, 184, 40},
                   st.ap_nav_valid ? (st.ap_nav_hold ? "NAV: TRACKING" : "NAV: DIRECT WP") : "NAV: NO ROUTE",
                   st.ap_nav_valid, st.ap_master && st.ap_nav_hold && st.ap_nav_valid)) {
            st.ap_nav_hold = !st.ap_nav_hold;
            st.ap_master = true;
        }
    }

    if (ui.state().parking_brake && !ui.is_chocked_or_tied()) {
        panel(ui, {1240, 946, 652, 91});
        label(ui, 1261, 974, 19, sys.is_engine_running()
            ? "Parking brake set. X releases it, W adds power."
            : "Engine stopped (cold & dark). Press Tab for Checklist or K to start.");
    }
    if (session_.profile().route_.empty()) {
        ui.flush_hud(w, h, font_);
        return;
    }
    panel(ui, {28, 946, 558, 91});
    if (session_.active_leg_ < session_.profile().route_.size()) {
        const auto &p = session_.profile().route_[session_.active_leg_];
        const auto &s = ui.state();
        const auto brg = int(std::lround(bearing(s.lat_deg, s.lon_deg, p.lat, p.lon))) % 360;
        std::snprintf(line, sizeof(line), "VFR  %zu/%zu   %s   %03d T   %.1f NM",
                      session_.active_leg_ + 1, session_.profile().route_.size(), p.label.c_str(),
                      brg, nspain::haversine_m(s.lat_deg, s.lon_deg, p.lat, p.lon) / nm_m);
    } else std::snprintf(line, sizeof(line), "VFR route complete");
    label(ui, 49, 974, 19, line);
    ui.flush_hud(w, h, font_);
}

GameFrontend::ReplayAction GameFrontend::draw_replay_bar(
    AircraftAndHud &ui, const PlatformWindow &win,
    const std::string &title, double cur_t, double total_t,
    bool playing, float speed_mult, int view_mode, double *out_seek_t
) {
    read_pointer(win);
    ui.hud_verts_.clear();
    const float w = GAME_OUTPUT_W, h = GAME_OUTPUT_H;
    panel(ui, {28, 932, 1864, 118});

    const double dur = std::max(0.1, total_t);
    const double clamped_t = std::clamp(cur_t, 0.0, dur);
    const float prog = static_cast<float>(clamped_t / dur);

    const PixelRect track{48, 948, 1824, 14};
    ui.hud_rect(track.x, track.y + 3, track.w, 8, 0.12f, 0.18f, 0.24f, 0.95f);
    ui.hud_rect(track.x, track.y + 3, std::max(6.0f, track.w * prog), 8, 0.28f, 0.72f, 0.88f, 0.98f);
    if (out_seek_t && click_ && contains({track.x, track.y - 8, track.w, track.h + 16})) {
        *out_seek_t = std::clamp(static_cast<double>(pointer_.x - track.x) / track.w, 0.0, 1.0) * total_t;
    }

    auto fmt_time = [](double s, char *buf, size_t n) {
        int ts = std::max(0, static_cast<int>(std::floor(s)));
        std::snprintf(buf, n, "%02d:%02d", ts / 60, ts % 60);
    };
    char t0[24], t1[24], info[192];
    fmt_time(clamped_t, t0, sizeof(t0));
    fmt_time(total_t, t1, sizeof(t1));
    const char *vname = (view_mode == 0) ? "Cockpit" : (view_mode == 1 ? "Chase" : "Orbit");
    std::snprintf(info, sizeof(info), "ROUTE REPLAY  |  %s  |  %s / %s  |  View: %s (C)  RMB: Orbit  Left/Right: Seek",
                  title.empty() ? "Last Flown Route" : title.c_str(), t0, t1, vname);
    label(ui, 48, 974, 17, info);

    ReplayAction act = ReplayAction::None;
    if (button(ui, {48, 1002, 130, 36}, playing ? "Pause" : "Play", true, playing)) act = ReplayAction::TogglePlay;
    if (button(ui, {188, 1002, 92, 36}, "-10s")) act = ReplayAction::Back10;
    if (button(ui, {290, 1002, 92, 36}, "+10s")) act = ReplayAction::Fwd10;
    if (button(ui, {398, 1002, 74, 36}, "0.5x", true, std::abs(speed_mult - 0.5f) < 0.05f)) act = ReplayAction::Speed05;
    if (button(ui, {480, 1002, 68, 36}, "1x", true, std::abs(speed_mult - 1.0f) < 0.05f)) act = ReplayAction::Speed1;
    if (button(ui, {556, 1002, 68, 36}, "2x", true, std::abs(speed_mult - 2.0f) < 0.05f)) act = ReplayAction::Speed2;
    if (button(ui, {632, 1002, 68, 36}, "4x", true, std::abs(speed_mult - 4.0f) < 0.05f)) act = ReplayAction::Speed4;
    if (button(ui, {1540, 1002, 160, 36}, "View (C)")) act = ReplayAction::SwitchView;
    if (button(ui, {1712, 1002, 160, 36}, "Exit (Esc)")) act = ReplayAction::Exit;

    ui.flush_hud(w, h, font_);
    return act;
}

} // namespace nsim
