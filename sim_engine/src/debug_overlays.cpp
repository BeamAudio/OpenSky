#include "aircraft_hud.hpp"
#include "flight_profiler.hpp"

#include <cstdio>

namespace nsim {

void AircraftAndHud::hud_line(Vec2 a, Vec2 b, float width, Vec3 color) {
    Vec2 delta = b - a;
    const float d = std::hypot(delta.x, delta.y);
    if (d < 0.5f) return;
    const Vec2 n{-delta.y * width / (2.0f * d), delta.x * width / (2.0f * d)};
    auto vert = [&](Vec2 p) {
        return HudVert{p.x, p.y, -1.0f, -1.0f, color.x, color.y, color.z, 0.94f};
    };
    const auto p0 = vert(a - n), p1 = vert(b - n), p2 = vert(b + n), p3 = vert(a + n);
    hud_verts_.insert(hud_verts_.end(), {p0, p1, p2, p0, p2, p3});
}

void AircraftAndHud::hud_arrow(Vec2 a, Vec2 b, float width, Vec3 color) {
    const Vec2 delta = b - a;
    const float d = std::hypot(delta.x, delta.y);
    if (d < 3.0f) return;
    const Vec2 t = delta * (std::min(8.0f, d * 0.35f) / d);
    const Vec2 n{-t.y * 0.55f, t.x * 0.55f};
    hud_line(a, b, width, color);
    hud_line(b, b - t + n, width, color);
    hud_line(b, b - t - n, width, color);
}

void AircraftAndHud::render_debug_overlays(float w, float h, float top, const RegionalAtmosphere &atmo,
                                            const DebugOverlayView &view) {
    const float scale = std::min(w / 1920.0f, h / 1080.0f);
    const float pad = 16.0f * scale, font = 18.0f * scale, row = 21.0f * scale;
    const Vec3 white{0.89f, 0.94f, 0.98f}, green{0.48f, 0.98f, 0.68f}, blue{0.47f, 0.80f, 1.0f};
    const Vec3 yellow{1.0f, 0.79f, 0.36f}, red{1.0f, 0.45f, 0.38f};
    char line[176];
    auto project = [&](Vec3 point, Vec2 &screen) {
        const Vec4 clip = view.projection_view.mul({point.x, point.y, point.z, 1.0f});
        if (clip.w <= 0.13f || !std::isfinite(clip.w) ||
            std::abs(clip.x) > clip.w || std::abs(clip.y) > clip.w) return false;
        screen = {(clip.x / clip.w + 1.0f) * w * 0.5f, (1.0f - clip.y / clip.w) * h * 0.5f};
        return true;
    };
    auto text_at = [&](float x, float y, const char *msg, Vec3 color) {
        hud_text(x, y, font, msg, color.x, color.y, color.z, 1.0f);
    };
    auto panel = [&](float x, float y, float width, float height, Vec3 color) {
        hud_rect(x, y, width, height, 0.022f, 0.045f, 0.068f, 0.92f);
        hud_border(x, y, width, height, scale, color.x, color.y, color.z, 0.65f);
    };

    if (view.weather_level >= 0) {
        const auto slice = atmo.debug_slice(view.east_m, view.north_m, view.weather_level);
        const WeatherDebugCell &center = slice.cells[slice.cells.size() / 2];
        auto color = [&](const WeatherDebugCell &cell) {
            const float cloud = std::clamp(cell.cloud_fraction, 0.0f, 1.0f);
            const float rain = std::clamp(cell.rain_mm_hr / 10.0f, 0.0f, 1.0f);
            return mix(mix(Vec3{0.08f, 0.17f, 0.22f}, Vec3{0.62f, 0.72f, 0.83f}, cloud), blue, rain);
        };
        for (const auto &cell : slice.cells) {
            if (!cell.valid) continue;
            const auto &p = cell.position_enu_m;
            const auto &v = cell.wind_enu_mps;
            const Vec3 base{static_cast<float>(p.x - view.east_m), static_cast<float>(p.z - st_.alt_msl_m),
                            static_cast<float>(view.north_m - p.y)};
            if (dot(base, base) > 6500.0f * 6500.0f || dot(base, base) < 180.0f * 180.0f) continue;
            Vec2 a, b;
            if (!project(base, a)) continue;
            const float span = std::clamp(length(base) * 0.004f, 4.0f, 24.0f);
            const auto ink = v.z < -0.1f ? blue : yellow;
            hud_rect(a.x - 2 * scale, a.y - 2 * scale, 4 * scale, 4 * scale, ink.x, ink.y, ink.z, 0.9f);
            if (project(base + Vec3{v.x, v.z, -v.y} * span, b)) hud_arrow(a, b, 1.5f * scale, ink);
        }

        const float pw = 492.0f * scale, x = w - pw - pad, y = top, cell_px = 20.0f * scale;
        const float grid_x = x + (pw - slice.width * cell_px) * 0.5f, grid_y = y + 5 * row;
        panel(x, y, pw, 5.5f * row + slice.width * cell_px, blue);
        std::snprintf(line, sizeof(line), "F6  WEATHER  L%d  %.0f m AGL", view.weather_level, slice.agl_m);
        text_at(x + pad, y + row * 0.45f, line, blue);
        if (center.valid) {
            std::snprintf(line, sizeof(line), "Cloud %.0f%%  liquid %.2f g/kg  rain %.1f mm/h",
                          center.cloud_fraction * 100.0f, center.liquid_gkg, center.rain_mm_hr);
            text_at(x + pad, y + row * 1.4f, line, white);
            std::snprintf(line, sizeof(line), "E %+.1f  N %+.1f  Up %+.1f m/s  gen %llu",
                          center.wind_enu_mps.x, center.wind_enu_mps.y, center.wind_enu_mps.z,
                          static_cast<unsigned long long>(slice.generation));
            text_at(x + pad, y + row * 2.4f, line, white);
            std::snprintf(line, sizeof(line), "Terrain ridge %.2f  sheltered basin %.2f",
                          center.ridge_exposure, center.valley_basin);
            text_at(x + pad, y + row * 3.2f, line, yellow);
        } else {
            text_at(x + pad, y + row * 1.4f, "Aircraft outside published slice", white);
        }
        std::snprintf(line, sizeof(line), "N up | %.1f x %.1f km per marker", slice.step_e_m * 0.001f, slice.step_n_m * 0.001f);
        text_at(x + pad, y + row * 4.2f, line, white);
        for (int iy = 0; iy < slice.width; ++iy) {
            for (int ix = 0; ix < slice.width; ++ix) {
                const auto &cell = slice.cells[iy * slice.width + ix];
                if (!cell.valid) continue;
                const float cx = grid_x + ix * cell_px, cy = grid_y + iy * cell_px;
                const Vec3 c = color(cell);
                hud_rect(cx, cy, cell_px - scale, cell_px - scale, c.x, c.y, c.z, 1.0f);
                const Vec2 mid{cx + cell_px * 0.5f, cy + cell_px * 0.5f};
                const auto &wind = cell.wind_enu_mps;
                const Vec2 d{wind.x, -wind.y};
                const float spd = std::hypot(d.x, d.y);
                const Vec2 half = d * (cell_px * 0.40f / std::max(8.0f, spd));
                hud_arrow(mid - half, mid + half, scale, yellow);
                if (ix == slice.width / 2 && iy == slice.width / 2)
                    hud_border(cx, cy, cell_px, cell_px, scale, 1.0f, 1.0f, 1.0f, 1.0f);
            }
        }
    }

    if (view.physics) {
        const auto &f = fdm_.state();
        const auto &controls = fdm_.controls();
        auto body = [&](Vec3 v) {
            const Vec4 p = view.aircraft_rotation.mul({v.x, v.y, v.z, 0.0f});
            return Vec3{p.x, p.y, p.z};
        };
        const Vec3 air{static_cast<float>(f.air_east_mps), static_cast<float>(f.air_up_mps),
                       static_cast<float>(-f.air_north_mps)};
        auto vector = [&](Vec3 start, Vec3 end, Vec3 color, const char *name) {
            Vec2 a, b;
            if (!project(start, a) || !project(end, b)) return;
            hud_arrow(a, b, 2.0f * scale, color);
            text_at(b.x + 4.0f * scale, b.y, name, color);
        };
        if (!st_.slew_fast && st_.view_mode == 1) {
            vector({}, body({2.8f, 0, 0}), red, "RIGHT");
            vector({}, body({0, 2.5f, 0}), green, "UP");
            vector({}, body({0, 0, -4.5f}), white, "FWD");
            if (length(air) > 0.5f) vector({}, normalize(air) * 6.5f, blue, "AIR PATH");
            if (f.thrust_n > 1.0) vector(body({0, 0, -1.4f}), body({0, 0, -1.4f - static_cast<float>(f.thrust_n * 0.002)}), yellow, "THRUST");
            for (size_t i = 0; i < f.wheel_wow.size(); ++i) {
                const auto &wheel = f.wheel_offset_m[i];
                const Vec3 hub = body({static_cast<float>(wheel[1]),
                                       static_cast<float>(-wheel[2] + f.wheel_comp_m[i]),
                                       static_cast<float>(-wheel[0])});
                Vec2 marker;
                if (project(hub, marker)) {
                    const auto c = f.wheel_wow[i] ? green : red;
                    hud_rect(marker.x - 3 * scale, marker.y - 3 * scale, 6 * scale, 6 * scale, c.x, c.y, c.z, 0.95f);
                }
            }
        } else if (!st_.slew_fast && st_.view_mode == 0 && length(air) > 0.5f) {
            Vec2 marker;
            if (project(normalize(air) * 24.0f, marker)) {
                const Vec2 dx{9.0f * scale, 0}, dy{0, 9.0f * scale};
                hud_line(marker - dx, marker + dx, 1.6f * scale, blue);
                hud_line(marker - dy, marker + dy, 1.6f * scale, blue);
                text_at(marker.x + dx.x + scale * 4.0f, marker.y, "AIR PATH", blue);
            }
        }

        const float pw = 540.0f * scale, x = pad, y = h - 247.0f * scale;
        panel(x, y, pw, row * 8.8f, green);
        auto row_text = [&](int i, const char *msg, Vec3 c) { text_at(x + pad, y + row * (0.45f + i), msg, c); };
        row_text(0, st_.slew_fast ? "F7  SLEW ACTIVE / LAST FDM STATE" : "F7  FLIGHT PHYSICS / UPSTREAM JSBSIM", green);
        std::snprintf(line, sizeof(line), "IAS %.0f kt  TAS %.0f kt   AoA %+.1f  beta %+.1f%s",
                      f.ias_kt, f.tas_kt, f.alpha_deg, f.beta_deg, f.stalled ? "  STALL" : "");
        row_text(1, line, white);
        std::snprintf(line, sizeof(line), "CL %.3f  CD %.3f  %.2f g  density %.3f kg/m3", f.cl, f.cd, f.g_load, f.air_density_kg_m3);
        row_text(2, line, white);
        std::snprintf(line, sizeof(line), "Roll/Pitch/Yaw rates %+.1f / %+.1f / %+.1f deg/s",
                      f.p_rps * RAD2DEG_D, f.q_rps * RAD2DEG_D, f.r_rps * RAD2DEG_D);
        row_text(3, line, white);
        std::snprintf(line, sizeof(line), "Ail %+.2f  Ele %+.2f  Rud %+.2f  Trim %+.2f",
                      controls.aileron, controls.elevator, controls.rudder, controls.elevator_trim);
        row_text(4, line, white);
        std::snprintf(line, sizeof(line), "Throttle %.0f%%  RPM %.0f  thrust %.0f N  flaps %.0f deg",
                      st_.throttle * 100.0, f.rpm, f.thrust_n, systems_.get_flaps_actual_deg());
        row_text(5, line, white);
        std::snprintf(line, sizeof(line), "WOW N/L/R %d/%d/%d  strut %.0f/%.0f/%.0f mm",
                      f.wheel_wow[0], f.wheel_wow[1], f.wheel_wow[2],
                      f.wheel_comp_m[0] * 1000, f.wheel_comp_m[1] * 1000, f.wheel_comp_m[2] * 1000);
        row_text(6, line, white);
        std::snprintf(line, sizeof(line), "Fuel L/R %.1f/%.1f gal  oil %.0f psi  flow %.1f gph",
                      systems_.get_left_tank_gal(), systems_.get_right_tank_gal(),
                      systems_.get_oil_press_psi(), systems_.get_fuel_flow_gph());
        row_text(7, line, white);
    }

    if (const FlightProfiler *prof = view.profiler) {
        const auto &host = prof->host();
        const auto &gpu = prof->latest_gpu_ms();
        const auto &phase = prof->latest_phases_ms();
        const float pw = 550.0f * scale, x = w - pw - pad, y = h - 247.0f * scale;
        panel(x, y, pw, row * 8.8f, yellow);
        auto row_text = [&](int i, const char *msg, Vec3 c) { text_at(x + pad, y + row * (0.45f + i), msg, c); };
        row_text(0, "F5  PERFORMANCE", yellow);
        std::snprintf(line, sizeof(line), "%.1f FPS  |  frame %.1f ms  p95 %.1f  p99 %.1f",
                      prof->frame_avg_ms() > 0.0 ? 1000.0 / prof->frame_avg_ms() : 0.0,
                      prof->frame_avg_ms(), prof->frame_p95_ms(), prof->frame_p99_ms());
        row_text(1, line, white);
        if (host.cpu_cores >= 0.0 && host.main_cpu_percent >= 0.0) {
            std::snprintf(line, sizeof(line), "CPU %.2f cores  |  main %.0f%%  |  RSS %.0f MiB",
                          host.cpu_cores, host.main_cpu_percent, host.rss_kib / 1024.0);
            row_text(2, line, white);
        } else row_text(2, "CPU/RSS sampling...", white);

        if (host.gpu_busy_pct >= 0 && host.vram_used_bytes >= 0 && host.gtt_used_bytes >= 0) {
            std::snprintf(line, sizeof(line), "GPU device %lld%%  |  VRAM %.0f  GTT %.0f MiB",
                          static_cast<long long>(host.gpu_busy_pct), host.vram_used_bytes / 1048576.0,
                          host.gtt_used_bytes / 1048576.0);
            row_text(3, line, white);
        } else row_text(3, "GPU device busy / memory unavailable", white);

        if (gpu[FlightProfiler::Sky] >= 0.0) {
            std::snprintf(line, sizeof(line), "GPU ms  terrain %.2f  world %.2f  aircraft %.2f",
                          gpu[FlightProfiler::Terrain], gpu[FlightProfiler::World], gpu[FlightProfiler::Aircraft]);
            row_text(4, line, white);
            std::snprintf(line, sizeof(line), "Sky %.2f  clouds/post %.2f  depth %.2f  upload %.2f",
                          gpu[FlightProfiler::Sky], gpu[FlightProfiler::CloudsPost], gpu[FlightProfiler::CloudDepth], gpu[FlightProfiler::Upload]);
            row_text(5, line, white);
        } else row_text(4, "Waiting for GPU timestamps...", white);

        std::snprintf(line, sizeof(line), "CPU ms  flight %.2f  weather %.2f  present %.2f",
                      phase[FlightProfiler::InputPhysics], phase[FlightProfiler::Weather], phase[FlightProfiler::Present]);
        row_text(6, line, white);
        std::snprintf(line, sizeof(line), "Terrain worker per page  build %.0f ms  BC7 %.0f ms",
                      prof->build_ms_per_page(), prof->encode_ms_per_page());
        row_text(7, line, white);
    }
}

} // namespace nsim
