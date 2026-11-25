#pragma once

#include <cstdlib>
#include <filesystem>

namespace nsim {

inline std::filesystem::path app_config_root() {
#ifdef _WIN32
    if (const char *appdata = std::getenv("APPDATA")) return appdata;
#endif
    if (const char *xdg = std::getenv("XDG_CONFIG_HOME")) return xdg;
    if (const char *home = std::getenv("HOME")) return std::filesystem::path(home) / ".config";
    return std::filesystem::current_path();
}

inline std::filesystem::path app_data_root() {
#ifdef _WIN32
    if (const char *local = std::getenv("LOCALAPPDATA")) return local;
#endif
    if (const char *xdg = std::getenv("XDG_DATA_HOME")) return xdg;
    if (const char *home = std::getenv("HOME")) return std::filesystem::path(home) / ".local" / "share";
    return std::filesystem::current_path();
}

inline std::filesystem::path app_cache_root() {
#ifdef _WIN32
    if (const char *local = std::getenv("LOCALAPPDATA")) return local;
#endif
    if (const char *xdg = std::getenv("XDG_CACHE_HOME")) return xdg;
    if (const char *home = std::getenv("HOME")) return std::filesystem::path(home) / ".cache";
    return std::filesystem::current_path();
}

inline std::filesystem::path opensky_config_dir() { return app_config_root() / "opensky-vfr"; }
inline std::filesystem::path opensky_data_dir() { return app_data_root() / "opensky-vfr"; }
inline std::filesystem::path opensky_cache_dir() { return app_cache_root() / "opensky-vfr"; }

} // namespace nsim
