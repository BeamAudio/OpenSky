#pragma once

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include "app_paths.hpp"

namespace nsim {

struct GraphicsSettings {
    // Percentage of native output pixels shaded before the spatial/temporal resolve.
    int pixel_percent = 68;
    // 0 = spatial AA, 1 = balanced TAA, 2 = crisp TAA.
    int aa_mode = 2;
    // Multiplier for terrain mesh LOD transition distances.
    int terrain_lod_percent = 100;
    // Render the cockpit after scene reconstruction at output resolution.
    bool cockpit_native = true;
    bool terrain_streaming = true;
    int aircraft_detail = 1; // Standard, High
    int weather_detail = 0; // Follow preset, Low, Medium, High
    int time_of_day = -1; // -1 = advancing five-hour cycle, 0..10 = fixed solar preset
    int cockpit_fov_deg = 50; // 42..64 deg vertical cockpit POV
    int head_motion_mode = 2; // 0 = Off, 1 = Subtle, 2 = Realistic (G-inertia, turn-look, touchdown jolt)
    int audio_volume_percent = 85; // 0..100 master audio level
    int atmosphere_clarity = 0; // 0 = Crisp VFR, 1 = Natural, 2 = Hazy
    int quality_preset = 2; // 0 = Low, 1 = Medium, 2 = High, 3 = Ultra
};

inline std::filesystem::path graphics_settings_path() {
    return opensky_config_dir() / "graphics.ini";
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
        else if (key == "aircraft_detail") settings.aircraft_detail = std::clamp(value, 0, 1);
        else if (key == "weather_detail") settings.weather_detail = std::clamp(value, 0, 3);
        else if (key == "time_of_day") settings.time_of_day = std::clamp(value, -1, 10);
        else if (key == "cockpit_fov_deg") settings.cockpit_fov_deg = std::clamp(value, 42, 64);
        else if (key == "head_motion_mode") settings.head_motion_mode = std::clamp(value, 0, 2);
        else if (key == "audio_volume_percent") settings.audio_volume_percent = std::clamp(value, 0, 100);
        else if (key == "atmosphere_clarity") settings.atmosphere_clarity = std::clamp(value, 0, 2);
        else if (key == "quality_preset") settings.quality_preset = std::clamp(value, 0, 3);
    }
    settings.pixel_percent = std::clamp(settings.pixel_percent, 40, 100);
    settings.aa_mode = std::clamp(settings.aa_mode, 0, 2);
    settings.terrain_lod_percent = std::clamp(settings.terrain_lod_percent, 75, 200);
    settings.cockpit_fov_deg = std::clamp(settings.cockpit_fov_deg, 42, 64);
    settings.head_motion_mode = std::clamp(settings.head_motion_mode, 0, 2);
    settings.audio_volume_percent = std::clamp(settings.audio_volume_percent, 0, 100);
    settings.atmosphere_clarity = std::clamp(settings.atmosphere_clarity, 0, 2);
    settings.quality_preset = std::clamp(settings.quality_preset, 0, 3);
    return settings;
}

inline void save_graphics_settings(const GraphicsSettings &input) {
    GraphicsSettings settings = input;
    settings.pixel_percent = std::clamp(settings.pixel_percent, 40, 100);
    settings.aa_mode = std::clamp(settings.aa_mode, 0, 2);
    settings.terrain_lod_percent = std::clamp(settings.terrain_lod_percent, 75, 200);
    settings.cockpit_fov_deg = std::clamp(settings.cockpit_fov_deg, 42, 64);
    settings.head_motion_mode = std::clamp(settings.head_motion_mode, 0, 2);
    settings.audio_volume_percent = std::clamp(settings.audio_volume_percent, 0, 100);
    settings.atmosphere_clarity = std::clamp(settings.atmosphere_clarity, 0, 2);
    settings.quality_preset = std::clamp(settings.quality_preset, 0, 3);
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
            << "time_of_day " << settings.time_of_day << '\n'
            << "cockpit_fov_deg " << settings.cockpit_fov_deg << '\n'
            << "head_motion_mode " << settings.head_motion_mode << '\n'
            << "audio_volume_percent " << settings.audio_volume_percent << '\n'
            << "atmosphere_clarity " << settings.atmosphere_clarity << '\n'
            << "quality_preset " << settings.quality_preset << '\n';
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
