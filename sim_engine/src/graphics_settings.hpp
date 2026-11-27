#pragma once

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include "app_paths.hpp"

namespace nsim {

// The Windows laptop profiles use a software presentation deadline. On WGL
// the swap-interval queue can bounce between one and two refresh intervals.
#ifdef _WIN32
inline constexpr bool kLaptopVsync = false;
#else
inline constexpr bool kLaptopVsync = true;
#endif

struct GraphicsSettings {
    // Percentage of native output pixels shaded before the spatial/temporal resolve (35..150%).
    int pixel_percent = 40;
    // 0 = spatial FXAA, 1 = balanced TAA, 2 = crisp TAA.
    int aa_mode = 1;
    // Multiplier for terrain mesh LOD transition distances (50..250%).
    int terrain_lod_percent = 75;
    // Render the cockpit after scene reconstruction at output resolution.
    bool cockpit_native = true;
    bool terrain_streaming = true;
    int aircraft_detail = 0; // Standard, High
    int weather_detail = 0; // Follow preset, Low, Medium, High, Ultra
    int weather_grid_detail = 0; // 0: regional only, 1: 192^2 local tile, 2: 384^2 local tile
    int terrain_detail = 1; // 0 = Follow preset, 1 = Fast, 2 = Balanced, 3 = High, 4 = Extreme
    int world_detail = 0; // 0 = Follow preset, 1 = Sparse, 2 = Medium, 3 = High, 4 = Max Cities & Traffic
    int vegetation_detail = 0; // 0 = Follow preset, 1 = Minimal, 2 = Standard, 3 = High, 4 = Dense
    int anisotropy = 4; // 1, 2, 4, 8, 16x anisotropic texture filtering
    int time_of_day = -1; // -1 = advancing five-hour cycle, 0..10 = fixed solar preset
    int cockpit_fov_deg = 50; // 42..64 deg vertical cockpit POV
    int head_motion_mode = 2; // 0 = Off, 1 = Subtle, 2 = Realistic (G-inertia, turn-look, touchdown jolt)
    int audio_volume_percent = 85; // 0..100 master audio level
    int atmosphere_clarity = 0; // 0 = Crisp VFR, 1 = Natural, 2 = Hazy
    int quality_preset = 1; // 0 = Low, 1 = Medium, 2 = High, 3 = Ultra
    bool show_flight_hud = false;
    int display_mode = 0; // 0 = Windowed, 1 = Fullscreen, 2 = Fullscreen windowed
    bool vsync = kLaptopVsync;
    int fps_limit = 60; // 0 = Unlimited, otherwise 24..240 FPS
    int yoke_deadzone_pct = 5;  // 0..20% hardware yoke deadzone
    int yoke_curve_pct = 35;    // 0..80% cubic S-curve sensitivity
    bool yoke_direct = true;    // true: direct C172S surfaces; false: assisted bank/VSI envelope
    bool yoke_invert_pitch = false;
    bool yoke_invert_roll = false;
    bool yoke_invert_throttle = false;
};

inline std::filesystem::path graphics_settings_path() {
    return opensky_config_dir() / "graphics.ini";
}

inline void clamp_graphics_settings(GraphicsSettings &settings) {
    settings.pixel_percent = std::clamp(settings.pixel_percent, 35, 150);
    settings.aa_mode = std::clamp(settings.aa_mode, 0, 2);
    settings.terrain_lod_percent = std::clamp(settings.terrain_lod_percent, 50, 250);
    settings.aircraft_detail = std::clamp(settings.aircraft_detail, 0, 1);
    settings.weather_detail = std::clamp(settings.weather_detail, 0, 4);
    settings.weather_grid_detail = std::clamp(settings.weather_grid_detail, 0, 2);
    settings.terrain_detail = std::clamp(settings.terrain_detail, 0, 4);
    settings.world_detail = std::clamp(settings.world_detail, 0, 4);
    settings.vegetation_detail = std::clamp(settings.vegetation_detail, 0, 4);
    if (settings.anisotropy <= 1) settings.anisotropy = 1;
    else if (settings.anisotropy <= 2) settings.anisotropy = 2;
    else if (settings.anisotropy <= 4) settings.anisotropy = 4;
    else if (settings.anisotropy <= 8) settings.anisotropy = 8;
    else settings.anisotropy = 16;
    settings.time_of_day = std::clamp(settings.time_of_day, -1, 10);
    settings.cockpit_fov_deg = std::clamp(settings.cockpit_fov_deg, 42, 64);
    settings.head_motion_mode = std::clamp(settings.head_motion_mode, 0, 2);
    settings.audio_volume_percent = std::clamp(settings.audio_volume_percent, 0, 100);
    settings.atmosphere_clarity = std::clamp(settings.atmosphere_clarity, 0, 2);
    settings.quality_preset = std::clamp(settings.quality_preset, 0, 3);
    settings.display_mode = std::clamp(settings.display_mode, 0, 2);
    settings.fps_limit = settings.fps_limit <= 0 ? 0 : std::clamp(settings.fps_limit, 24, 240);
    settings.yoke_deadzone_pct = std::clamp(settings.yoke_deadzone_pct, 0, 20);
    settings.yoke_curve_pct = std::clamp(settings.yoke_curve_pct, 0, 80);
}

inline void apply_graphics_profile(GraphicsSettings &settings, int profile_id) {
    // 0 = Laptop / Battery Saver, 1 = Balanced 60 FPS, 2 = High Quality, 3 = Max Dedicated GPU
    profile_id = std::clamp(profile_id, 0, 3);
    settings.quality_preset = profile_id;
    if (profile_id == 0) {
        settings.pixel_percent = 38;
        settings.aa_mode = 0;
        settings.terrain_lod_percent = 75;
        settings.cockpit_native = false;
        settings.aircraft_detail = 0;
        settings.weather_detail = 1;
        settings.weather_grid_detail = 0;
        settings.terrain_detail = 1;
        settings.world_detail = 1;
        settings.vegetation_detail = 1;
        settings.anisotropy = 2;
        settings.vsync = kLaptopVsync;
        settings.fps_limit = 60;
    } else if (profile_id == 1) {
        settings.pixel_percent = 40;
        settings.aa_mode = 1;
        settings.terrain_lod_percent = 75;
        settings.cockpit_native = true;
        settings.aircraft_detail = 0;
        settings.weather_detail = 0;
        settings.weather_grid_detail = 0;
        settings.terrain_detail = 1;
        settings.world_detail = 0;
        settings.vegetation_detail = 0;
        settings.anisotropy = 4;
        settings.vsync = kLaptopVsync;
        settings.fps_limit = 60;
    } else if (profile_id == 2) {
        settings.pixel_percent = 68;
        settings.aa_mode = 2;
        settings.terrain_lod_percent = 100;
        settings.cockpit_native = true;
        settings.aircraft_detail = 1;
        settings.weather_detail = 0;
        settings.weather_grid_detail = 0;
        settings.terrain_detail = 0;
        settings.world_detail = 0;
        settings.vegetation_detail = 0;
        settings.anisotropy = 4;
        settings.vsync = true;
        settings.fps_limit = 60;
    } else {
        settings.pixel_percent = 125;
        settings.aa_mode = 2;
        settings.terrain_lod_percent = 225;
        settings.cockpit_native = true;
        settings.aircraft_detail = 1;
        settings.weather_detail = 4;
        settings.weather_grid_detail = 2;
        settings.terrain_detail = 4;
        settings.world_detail = 4;
        settings.vegetation_detail = 4;
        settings.anisotropy = 16;
        settings.vsync = false;
        settings.fps_limit = 0;
    }
    clamp_graphics_settings(settings);
}

inline GraphicsSettings load_graphics_settings() {
    GraphicsSettings settings;
    std::ifstream in(graphics_settings_path());
    std::string key;
    int value = 0;
    while (in >> key >> value) {
        if (key == "pixel_percent") settings.pixel_percent = value;
        else if (key == "aa_mode") settings.aa_mode = value;
        else if (key == "terrain_lod_percent") settings.terrain_lod_percent = value;
        else if (key == "cockpit_native") settings.cockpit_native = value != 0;
        else if (key == "terrain_streaming") settings.terrain_streaming = value != 0;
        else if (key == "aircraft_detail") settings.aircraft_detail = value;
        else if (key == "weather_detail") settings.weather_detail = value;
        else if (key == "weather_grid_detail") settings.weather_grid_detail = value;
        else if (key == "terrain_detail") settings.terrain_detail = value;
        else if (key == "world_detail") settings.world_detail = value;
        else if (key == "vegetation_detail") settings.vegetation_detail = value;
        else if (key == "anisotropy") settings.anisotropy = value;
        else if (key == "time_of_day") settings.time_of_day = value;
        else if (key == "cockpit_fov_deg") settings.cockpit_fov_deg = value;
        else if (key == "head_motion_mode") settings.head_motion_mode = value;
        else if (key == "audio_volume_percent") settings.audio_volume_percent = value;
        else if (key == "atmosphere_clarity") settings.atmosphere_clarity = value;
        else if (key == "quality_preset") settings.quality_preset = value;
        else if (key == "show_flight_hud") settings.show_flight_hud = value != 0;
        else if (key == "display_mode") settings.display_mode = value;
        else if (key == "vsync") settings.vsync = value != 0;
        else if (key == "fps_limit") settings.fps_limit = value;
        else if (key == "yoke_deadzone_pct") settings.yoke_deadzone_pct = value;
        else if (key == "yoke_curve_pct") settings.yoke_curve_pct = value;
        else if (key == "yoke_direct") settings.yoke_direct = value != 0;
        else if (key == "yoke_invert_pitch") settings.yoke_invert_pitch = value != 0;
        else if (key == "yoke_invert_roll") settings.yoke_invert_roll = value != 0;
        else if (key == "yoke_invert_throttle") settings.yoke_invert_throttle = value != 0;
    }
    clamp_graphics_settings(settings);
    return settings;
}

inline void save_graphics_settings(const GraphicsSettings &input) {
    GraphicsSettings settings = input;
    clamp_graphics_settings(settings);
    const auto path = graphics_settings_path();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    const auto temp = path.string() + ".tmp";
    {
        std::ofstream out(temp, std::ios::trunc);
        if (!out) return;
        out << "pixel_percent " << settings.pixel_percent << '\n'
            << "aa_mode " << settings.aa_mode << '\n'
            << "terrain_lod_percent " << settings.terrain_lod_percent << '\n'
            << "cockpit_native " << settings.cockpit_native << '\n'
            << "terrain_streaming " << settings.terrain_streaming << '\n'
            << "aircraft_detail " << settings.aircraft_detail << '\n'
            << "weather_detail " << settings.weather_detail << '\n'
            << "weather_grid_detail " << settings.weather_grid_detail << '\n'
            << "terrain_detail " << settings.terrain_detail << '\n'
            << "world_detail " << settings.world_detail << '\n'
            << "vegetation_detail " << settings.vegetation_detail << '\n'
            << "anisotropy " << settings.anisotropy << '\n'
            << "time_of_day " << settings.time_of_day << '\n'
            << "cockpit_fov_deg " << settings.cockpit_fov_deg << '\n'
            << "head_motion_mode " << settings.head_motion_mode << '\n'
            << "audio_volume_percent " << settings.audio_volume_percent << '\n'
            << "atmosphere_clarity " << settings.atmosphere_clarity << '\n'
            << "quality_preset " << settings.quality_preset << '\n';
        out << "show_flight_hud " << settings.show_flight_hud << '\n'
            << "display_mode " << settings.display_mode << '\n'
            << "vsync " << settings.vsync << '\n'
            << "fps_limit " << settings.fps_limit << '\n'
            << "yoke_deadzone_pct " << settings.yoke_deadzone_pct << '\n'
            << "yoke_curve_pct " << settings.yoke_curve_pct << '\n'
            << "yoke_direct " << settings.yoke_direct << '\n'
            << "yoke_invert_pitch " << settings.yoke_invert_pitch << '\n'
            << "yoke_invert_roll " << settings.yoke_invert_roll << '\n'
            << "yoke_invert_throttle " << settings.yoke_invert_throttle << '\n';
        if (!out) return;
    }
    std::filesystem::rename(temp, path, ec);
    if (ec) {
        std::filesystem::remove(path, ec);
        ec.clear();
        std::filesystem::rename(temp, path, ec);
        if (ec) std::filesystem::remove(temp, ec);
    }
}

} // namespace nsim
