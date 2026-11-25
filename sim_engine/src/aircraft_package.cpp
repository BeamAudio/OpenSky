#include "aircraft_package.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <set>
#include <sstream>
#include <unordered_set>

namespace nsim {
namespace {

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool contained_by(const std::filesystem::path &root, const std::filesystem::path &path) {
    auto r = root.begin();
    auto p = path.begin();
    for (; r != root.end(); ++r, ++p) if (p == path.end() || *r != *p) return false;
    return true;
}

bool valid_id(const std::string &id) {
    if (id.empty() || id.size() > 48) return false;
    return std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '_' || c == '-';
    });
}

bool parse_number(const std::string &text, double &out) {
    try {
        size_t used = 0;
        out = std::stod(text, &used);
        return used == text.size() && std::isfinite(out);
    } catch (...) { return false; }
}

bool valid_package_asset(const std::filesystem::path &root, const std::string &value,
                         bool required, std::string &error) {
    if (value.empty() || value == "-") {
        if (required) error = "required asset path is empty";
        return !required;
    }
    if (value.rfind("@shared/", 0) == 0) {
        const std::filesystem::path rel(value.substr(8));
        if (rel.empty() || rel.is_absolute()) { error = "invalid shared asset path"; return false; }
        for (const auto &part : rel) if (part == "..") { error = "shared asset path escapes its root"; return false; }
        return true;
    }
    const std::filesystem::path rel(value);
    if (rel.is_absolute()) { error = "package asset paths must be relative"; return false; }
    for (const auto &part : rel) if (part == "..") { error = "package asset path escapes its root"; return false; }
    std::error_code ec;
    const auto full = std::filesystem::weakly_canonical(root / rel, ec);
    if (ec || !contained_by(root, full) || !std::filesystem::is_regular_file(full, ec) || ec) {
        error = "package asset is missing or resolves outside the package";
        return false;
    }
    return true;
}

bool load_one(const std::filesystem::path &directory, AircraftPackage &out, std::string &error) {
    std::error_code ec;
    const auto root = std::filesystem::canonical(directory, ec);
    if (ec || !std::filesystem::is_directory(root)) { error = "package directory is unavailable"; return false; }
    const auto manifest = root / "aircraft.ini";
    std::ifstream in(manifest);
    if (!in) { error = "missing aircraft.ini"; return false; }

    std::unordered_map<std::string, std::string> values;
    std::string line;
    size_t line_no = 0;
    while (std::getline(in, line)) {
        ++line_no;
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) { error = "expected key=value at line " + std::to_string(line_no); return false; }
        std::string key = trim(line.substr(0, eq));
        std::string value = trim(line.substr(eq + 1));
        if (key.empty() || value.empty()) { error = "empty key or value at line " + std::to_string(line_no); return false; }
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
        if (!values.emplace(std::move(key), std::move(value)).second) {
            error = "duplicate key at line " + std::to_string(line_no); return false;
        }
    }

    static const std::unordered_set<std::string> fixed_keys = {
        "format", "version", "id", "name", "manufacturer", "model", "registration", "systems_profile",
        "flight_model_dir", "exterior_model", "cockpit_model", "rain_front", "rain_side",
        "cockpit_eye_height_m", "max_flaps_deg", "left_tank_capacity_gal",
        "right_tank_capacity_gal", "oil_capacity_quarts", "bus_voltage"
    };
    for (const auto &[key, _] : values) if (!fixed_keys.contains(key) && key.rfind("node.", 0) != 0) {
        error = "unsupported manifest key '" + key + "'"; return false;
    }
    auto get = [&](const char *key) -> std::string {
        const auto it = values.find(key);
        return it == values.end() ? std::string{} : it->second;
    };
    if (get("format") != "opensky-aircraft" || get("version") != "1") {
        error = "format must be opensky-aircraft version 1"; return false;
    }
    out.id = get("id");
    out.name = get("name");
    out.manufacturer = get("manufacturer");
    out.model = get("model");
    out.registration = get("registration");
    out.systems_profile = get("systems_profile");
    if (!valid_id(out.id) || out.name.empty() || out.name.size() > 100 || out.manufacturer.size() > 100 ||
        out.model.size() > 100 || out.registration.size() > 24) { error = "invalid id or display metadata"; return false; }
    // This profile supplies the game's standard single-piston, two-tank systems.
    // Keep c172s as a manifest alias for early package prototypes.
    if (out.systems_profile != "single_piston_v1" && out.systems_profile != "c172s") {
        error = "unsupported systems_profile (v1 supports single_piston_v1)"; return false;
    }
    out.systems_profile = "single_piston_v1";
    out.root = root;
    const std::string model_dir = get("flight_model_dir");
    if (model_dir.empty() || model_dir == "-") { error = "flight_model_dir is required"; return false; }
    const auto rel_dir = std::filesystem::path(model_dir);
    if (rel_dir.is_absolute()) { error = "flight_model_dir must be relative"; return false; }
    for (const auto &part : rel_dir) if (part == "..") { error = "flight_model_dir escapes package root"; return false; }
    out.flight_model_dir = std::filesystem::canonical(root / rel_dir, ec);
    if (ec || !contained_by(root, out.flight_model_dir) || !std::filesystem::is_directory(out.flight_model_dir)) {
        error = "flight_model_dir is missing or outside package"; return false;
    }
    if (!std::filesystem::is_regular_file(out.flight_model_dir / (out.flight_model_dir.filename().string() + ".xml"), ec) || ec) {
        error = "flight_model_dir must contain a JSBSim file named after its directory"; return false;
    }
    out.exterior_model = get("exterior_model");
    out.cockpit_model = get("cockpit_model");
    out.rain_front = get("rain_front");
    out.rain_side = get("rain_side");
    for (const auto &[path, required] : {std::pair{&out.exterior_model, true},
                                         std::pair{&out.cockpit_model, true},
                                         std::pair{&out.rain_front, false},
                                         std::pair{&out.rain_side, false}}) {
        if (!valid_package_asset(root, *path, required, error)) return false;
        if (*path == "-") path->clear();
    }
    double numeric = 0.0;
    if (const auto eye = get("cockpit_eye_height_m"); !eye.empty()) {
        if (!parse_number(eye, numeric) || numeric < 0.0 || numeric > 5.0) { error = "cockpit_eye_height_m must be between 0 and 5"; return false; }
        out.cockpit_eye_height_m = numeric;
    }
    auto optional_number = [&](const char *key, double minimum, double maximum, double &target) {
        const auto text = get(key);
        if (text.empty()) return true;
        if (!parse_number(text, numeric) || numeric < minimum || numeric > maximum) {
            error = std::string(key) + " is outside its supported range";
            return false;
        }
        target = numeric;
        return true;
    };
    if (!optional_number("max_flaps_deg", 1.0, 60.0, out.max_flaps_deg) ||
        !optional_number("left_tank_capacity_gal", 1.0, 150.0, out.left_tank_capacity_gal) ||
        !optional_number("right_tank_capacity_gal", 1.0, 150.0, out.right_tank_capacity_gal) ||
        !optional_number("oil_capacity_quarts", 1.0, 40.0, out.oil_capacity_quarts)) return false;
    if (const auto volts = get("bus_voltage"); !volts.empty()) {
        if (!parse_number(volts, numeric) || (std::abs(numeric - 14.0) > 0.01 && std::abs(numeric - 28.0) > 0.01)) {
            error = "bus_voltage must be 14 or 28"; return false;
        }
        out.bus_voltage = numeric;
    }
    for (const auto &[key, value] : values) if (key.rfind("node.", 0) == 0) {
        const std::string role = key.substr(5);
        if (role.empty() || role.size() > 48 || value.empty() || value.size() > 100 ||
            !std::all_of(role.begin(), role.end(), [](unsigned char c) { return std::isalnum(c) || c == '_' || c == '-'; })) {
            error = "invalid visual node mapping"; return false;
        }
        out.nodes.emplace(key.substr(5), value);
    }
    return true;
}

} // namespace

AircraftPackageResult discover_aircraft_packages(const std::vector<std::filesystem::path> &roots) {
    AircraftPackageResult result;
    std::set<std::string> ids;
    for (const auto &root : roots) {
        std::error_code ec;
        if (!std::filesystem::is_directory(root, ec)) continue;
        for (std::filesystem::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
            if (!it->is_directory(ec) || ec) { ec.clear(); continue; }
            if (!std::filesystem::exists(it->path() / "aircraft.ini", ec)) { ec.clear(); continue; }
            AircraftPackage package;
            std::string error;
            if (!load_one(it->path(), package, error)) {
                result.warnings.push_back(it->path().string() + ": " + error);
                continue;
            }
            if (!ids.insert(package.id).second) {
                result.warnings.push_back(it->path().string() + ": duplicate aircraft id '" + package.id + "' (earlier package wins)");
                continue;
            }
            result.packages.push_back(std::move(package));
        }
        if (ec) result.warnings.push_back(root.string() + ": could not finish scanning aircraft packages");
    }
    return result;
}

bool resolve_aircraft_asset(const AircraftPackage &package, const std::string &asset,
                            const std::filesystem::path &shared_asset_root,
                            std::filesystem::path &resolved, std::string &error) {
    if (asset.empty()) { resolved.clear(); return true; }
    const bool shared = asset.rfind("@shared/", 0) == 0;
    const auto base = shared ? shared_asset_root : package.root;
    const auto value = shared ? asset.substr(8) : asset;
    const auto rel = std::filesystem::path(value);
    if (rel.empty() || rel.is_absolute()) { error = "invalid aircraft asset path"; return false; }
    for (const auto &part : rel) if (part == "..") { error = "aircraft asset path escapes its package"; return false; }
    std::error_code ec;
    const auto canonical_base = std::filesystem::canonical(base, ec);
    if (ec) { error = "asset root is unavailable"; return false; }
    resolved = std::filesystem::canonical(canonical_base / rel, ec);
    if (ec || !contained_by(canonical_base, resolved) || !std::filesystem::is_regular_file(resolved, ec) || ec) {
        error = "aircraft asset is missing or outside its allowed root";
        return false;
    }
    return true;
}

} // namespace nsim
