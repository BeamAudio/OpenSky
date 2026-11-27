#include "aircraft_hud.hpp"
#include "flight_profiler.hpp"

#include <cstdio>

namespace nsim {

void AircraftAndHud::hud_line(Vec2 a, Vec2 b, float width, Vec3 color, float alpha) {
    Vec2 delta = b - a;
    const float d = std::hypot(delta.x, delta.y);
    if (d < 0.5f) return;
    const Vec2 n{-delta.y * width / (2.0f * d), delta.x * width / (2.0f * d)};
    auto vert = [&](Vec2 p) {
        return HudVert{p.x, p.y, -1.0f, -1.0f, color.x, color.y, color.z, std::clamp(alpha, 0.0f, 1.0f)};
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

    if (view.weather_volume_3d) {
        const float wx_scale = std::clamp(std::min(w / 1024.0f, h / 1024.0f), 0.85f, 1.45f);
        const float wx_pad = 14.0f * wx_scale, wx_font = 20.0f * wx_scale, wx_row = 27.0f * wx_scale;
        auto wx_text = [&](float x, float y, const char *msg, Vec3 color) {
            hud_text(x, y, wx_font, msg, color.x, color.y, color.z, 1.0f);
        };
        static const Vec3 kLayerColors[4] = {
            {0.36f, 0.92f, 0.65f}, {0.28f, 0.84f, 1.00f},
            {1.00f, 0.74f, 0.34f}, {0.82f, 0.62f, 1.00f}
        };
        static const Vec3 kRainColor{0.18f, 0.48f, 1.00f};
        std::array<WeatherDebugSlice, 4> slices{};
        std::array<float, 4> level_agl{};
        uint64_t generation = 0;
        int visible_markers = 0;
        for (int level = 0; level < 4; ++level) {
            auto &slice = slices[static_cast<size_t>(level)];
            // Show a readable 13 x 13 sample window; the solver retains every cell.
            slice = atmo.debug_slice(view.east_m, view.north_m, level, 4);
            level_agl[static_cast<size_t>(level)] = slice.agl_m;
            generation = std::max(generation, slice.generation);
            for (int row = 0; row < slice.width; ++row) {
                for (int col = 0; col < slice.width; ++col) {
                    const auto &cell = slice.cells[static_cast<size_t>(row * slice.width + col)];
                    if (!cell.valid) continue;
                    const auto &p = cell.position_enu_m;
                    const Vec3 base{static_cast<float>(p.x - view.east_m),
                                    static_cast<float>(p.z - st_.alt_msl_m),
                                    static_cast<float>(view.north_m - p.y)};
                    const float horizontal_distance_sq = base.x * base.x + base.z * base.z;
                    if (horizontal_distance_sq < 180.0f * 180.0f ||
                        horizontal_distance_sq > 14000.0f * 14000.0f) continue;
                    Vec2 a;
                    if (!project(base, a)) continue;

                    const float cloud = std::clamp(cell.cloud_fraction, 0.0f, 1.0f);
                    const float rain = std::clamp(cell.rain_mm_hr / 12.0f, 0.0f, 1.0f);
                    const Vec3 marker = mix(kLayerColors[level], kRainColor, rain * 0.82f);
                    const float dot_scale = slice.locally_refined ? 0.72f : 0.85f;
                    const float marker_size = (3.4f + cloud * 5.2f) * wx_scale * dot_scale;
                    hud_rect(a.x - marker_size * 0.5f, a.y - marker_size * 0.5f,
                             marker_size, marker_size, marker.x, marker.y, marker.z, 0.78f);
                    if (rain > 0.12f) {
                        hud_border(a.x - marker_size * 0.5f, a.y - marker_size * 0.5f,
                                   marker_size, marker_size, 1.2f * wx_scale, 0.55f, 0.82f, 1.0f, 0.85f);
                    }
                    ++visible_markers;
                }
            }
        }

        auto world_point = [&](const WeatherDebugCell &cell, Vec3 &point) {
            if (!cell.valid) return false;
            const auto &p = cell.position_enu_m;
            point = {static_cast<float>(p.x - view.east_m),
                     static_cast<float>(p.z - st_.alt_msl_m),
                     static_cast<float>(view.north_m - p.y)};
            return true;
        };
        auto model_line = [&](const WeatherDebugCell &a_cell, const WeatherDebugCell &b_cell, Vec3 color, float alpha) {
            Vec3 a_world, b_world;
            Vec2 a_screen, b_screen;
            if (world_point(a_cell, a_world) && world_point(b_cell, b_world) &&
                project(a_world, a_screen) && project(b_world, b_screen))
                hud_line(a_screen, b_screen, 1.8f * wx_scale, color, alpha);
        };
        if (slices[0].locally_refined) {
            const auto &slice0 = slices[0];
            for (int i = 0; i < slice0.width - 1; ++i) {
                model_line(slice0.cells[static_cast<size_t>(i)], slice0.cells[static_cast<size_t>(i + 1)], kLayerColors[0], 0.42f);
                const size_t bottom = static_cast<size_t>((slice0.width - 1) * slice0.width + i);
                model_line(slice0.cells[bottom], slice0.cells[bottom + 1], kLayerColors[0], 0.42f);
                const size_t left = static_cast<size_t>(i * slice0.width);
                model_line(slice0.cells[left], slice0.cells[left + slice0.width], kLayerColors[0], 0.42f);
                const size_t right = static_cast<size_t>(i * slice0.width + slice0.width - 1);
                model_line(slice0.cells[right], slice0.cells[right + slice0.width], kLayerColors[0], 0.42f);
            }
        }

        // Dense, pre-seeded RK2 flow lines advected through the published 4-layer field across the camera frustum.
        static constexpr size_t kTracesPerLevel[4] = {40, 28, 18, 12};
        static constexpr size_t kTraceCount = kTracesPerLevel[0] + kTracesPerLevel[1] + kTracesPerLevel[2] + kTracesPerLevel[3];
        const float cam_agl = std::max(1200.0f, static_cast<float>(st_.alt_msl_m - st_.ground_m));
        const float half_v = slices[0].locally_refined
            ? std::clamp(cam_agl * 0.46f, 1600.0f, 3400.0f)
            : std::clamp(cam_agl * 0.46f, 2200.0f, 6600.0f);
        const float half_u = half_v * std::clamp(w / std::max(1.0f, h), 0.45f, 1.85f);
        const float min_seg_m = std::max(28.0f, std::min(half_u, half_v) * 0.032f);
        const float recycle_e = half_u * 1.25f, recycle_n = half_v * 1.25f;

        auto seed_trace = [&](WeatherFlowTrace &trace, size_t idx, bool prefill_curve) {
            // R2 quasirandom sequence guarantees even coverage with zero grid alignment.
            const double fx = std::fmod((idx + 1) * 0.7548776662466927, 1.0);
            const double fy = std::fmod((idx + 1) * 0.5698402909980532, 1.0);
            const double fz = std::fmod((idx + 1) * 0.3819660112501051, 1.0);
            trace.east_m = view.east_m + (fx * 2.0 - 1.0) * half_u;
            trace.north_m = view.north_m + (fy * 2.0 - 1.0) * half_v;
            trace.max_life_s = 42.0f + static_cast<float>(fz) * 28.0f;
            trace.life_s = prefill_curve ? static_cast<float>(fz) * (trace.max_life_s * 0.70f) : 0.0f;
            trace.points.clear();
            const int warm_pts = prefill_curve ? 16 : 8;
            const double step_m = min_seg_m * 1.65;
            for (int s = 0; s < warm_pts / 2; ++s) {
                const auto sb = atmo.debug_sample(trace.east_m, trace.north_m, trace.level);
                if (!sb.valid) break;
                const float spd = std::hypot(sb.wind_enu_mps.x, sb.wind_enu_mps.y);
                if (spd < 0.25f) break;
                const double cand_e = trace.east_m - (sb.wind_enu_mps.x / spd) * step_m;
                const double cand_n = trace.north_m - (sb.wind_enu_mps.y / spd) * step_m;
                if (!atmo.debug_sample(cand_e, cand_n, trace.level).valid) break;
                trace.east_m = cand_e;
                trace.north_m = cand_n;
            }
            for (int s = 0; s < warm_pts; ++s) {
                const auto s0 = atmo.debug_sample(trace.east_m, trace.north_m, trace.level);
                if (!s0.valid) break;
                trace.points.push_back(s0.position_enu_m);
                const float spd0 = std::hypot(s0.wind_enu_mps.x, s0.wind_enu_mps.y);
                if (spd0 < 0.25f) break;
                const double half_step = 0.5 * step_m / spd0;
                const auto sm = atmo.debug_sample(trace.east_m + s0.wind_enu_mps.x * half_step,
                                                  trace.north_m + s0.wind_enu_mps.y * half_step, trace.level);
                const Vec3 wmid = sm.valid ? sm.wind_enu_mps : s0.wind_enu_mps;
                const float spdm = std::max(0.25f, std::hypot(wmid.x, wmid.y));
                trace.east_m += (wmid.x / spdm) * step_m;
                trace.north_m += (wmid.y / spdm) * step_m;
            }
        };

        const bool reset_traces = weather_flow_traces_.size() != kTraceCount ||
            (weather_flow_last_time_s_ >= 0.0 && view.simulation_time_s + 0.1 < weather_flow_last_time_s_) ||
            std::hypot(view.east_m - weather_flow_origin_e_, view.north_m - weather_flow_origin_n_) > std::max(4000.0f, half_v * 1.6f);
        if (reset_traces) {
            weather_flow_traces_.clear();
            weather_flow_traces_.resize(kTraceCount);
            weather_flow_origin_e_ = view.east_m;
            weather_flow_origin_n_ = view.north_m;
            size_t cursor = 0;
            for (int level = 0; level < 4; ++level) {
                for (size_t s = 0; s < kTracesPerLevel[level]; ++s) {
                    auto &trace = weather_flow_traces_[cursor];
                    trace.level = level;
                    seed_trace(trace, cursor, true);
                    ++cursor;
                }
            }
            weather_flow_last_time_s_ = view.simulation_time_s;
        }
        const double flow_dt = weather_flow_last_time_s_ < 0.0 ? 0.0 :
            std::clamp(view.simulation_time_s - weather_flow_last_time_s_, 0.0, 2.0);
        weather_flow_last_time_s_ = view.simulation_time_s;
        for (size_t idx = 0; idx < weather_flow_traces_.size(); ++idx) {
            auto &trace = weather_flow_traces_[idx];
            const auto sample = atmo.debug_sample(trace.east_m, trace.north_m, trace.level);
            const bool expired = trace.life_s >= trace.max_life_s;
            const bool out_of_view = std::abs(trace.east_m - view.east_m) > recycle_e ||
                                     std::abs(trace.north_m - view.north_m) > recycle_n;
            if (!sample.valid || expired || out_of_view) {
                seed_trace(trace, idx + static_cast<size_t>(std::max(0.0, view.simulation_time_s) * 13.0), false);
                continue;
            }
            if (trace.points.empty()) trace.points.push_back(sample.position_enu_m);
            if (flow_dt > 0.0) {
                trace.life_s += static_cast<float>(flow_dt);
                const Vec3 w0 = sample.wind_enu_mps;
                const auto mid = atmo.debug_sample(trace.east_m + w0.x * flow_dt * 0.5,
                                                   trace.north_m + w0.y * flow_dt * 0.5, trace.level);
                const Vec3 wm = mid.valid ? mid.wind_enu_mps : w0;
                trace.east_m += wm.x * flow_dt;
                trace.north_m += wm.y * flow_dt;
                const auto next_sample = atmo.debug_sample(trace.east_m, trace.north_m, trace.level);
                if (next_sample.valid && length(next_sample.position_enu_m - trace.points.back()) >= min_seg_m)
                    trace.points.push_back(next_sample.position_enu_m);
            }
            if (trace.points.size() > 24) trace.points.erase(trace.points.begin(), trace.points.end() - 24);
        }
        for (const auto &trace : weather_flow_traces_) {
            const size_t count = trace.points.size();
            if (count < 2) continue;
            const float life01 = std::clamp(trace.life_s / std::max(1.0f, trace.max_life_s), 0.0f, 1.0f);
            const float fade_env = std::sin(life01 * 3.14159265f);
            const float env_alpha = 0.25f + 0.75f * std::sqrt(std::max(0.0f, fade_env));
            const Vec3 base_col = mix(kLayerColors[trace.level], white, 0.34f);
            for (size_t i = 1; i < count; ++i) {
                const auto &a_world = trace.points[i - 1];
                const auto &b_world = trace.points[i];
                Vec2 a_screen, b_screen;
                const Vec3 a{static_cast<float>(a_world.x - view.east_m),
                             static_cast<float>(a_world.z - st_.alt_msl_m),
                             static_cast<float>(view.north_m - a_world.y)};
                const Vec3 b{static_cast<float>(b_world.x - view.east_m),
                             static_cast<float>(b_world.z - st_.alt_msl_m),
                             static_cast<float>(view.north_m - b_world.y)};
                if (!project(a, a_screen) || !project(b, b_screen)) continue;
                const float age = static_cast<float>(i) / static_cast<float>(count - 1);
                const float taper = std::lerp(0.38f, 1.0f, age);
                hud_line(a_screen, b_screen, (4.8f * taper) * wx_scale, {0.010f, 0.018f, 0.028f}, (0.20f + age * 0.55f) * env_alpha);
                hud_line(a_screen, b_screen, (3.1f * taper) * wx_scale, base_col, (0.14f + age * 0.84f) * env_alpha);
                if (i + 1 == count) {
                    const Vec2 dir = b_screen - a_screen;
                    const float dl = std::hypot(dir.x, dir.y);
                    if (dl > 1.0f) {
                        const Vec2 fwd = dir * (std::min(7.0f * wx_scale, dl * 0.45f) / dl);
                        const Vec2 perp{-fwd.y * 0.50f, fwd.x * 0.50f};
                        hud_line(b_screen, b_screen - fwd + perp, 2.2f * wx_scale, white, 0.82f * env_alpha);
                        hud_line(b_screen, b_screen - fwd - perp, 2.2f * wx_scale, white, 0.82f * env_alpha);
                    }
                }
            }
        }

        const float x = wx_pad, y = wx_pad, pw = 760.0f * wx_scale, ph = 5.95f * wx_row;
        hud_rect(x, y, pw, ph, 0.012f, 0.027f, 0.046f, 0.97f);
        hud_border(x, y, pw, ph, 2.0f * wx_scale, 0.35f, 0.78f, 1.0f, 0.92f);
        if (slices[0].locally_refined) {
            std::snprintf(line, sizeof(line), "REGIONAL %d x %d x 4  +  LOCAL %d x %d / 8 km",
                          atmo.grid_width(), atmo.grid_height(), atmo.local_grid_width(), atmo.local_grid_height());
            wx_text(x + wx_pad, y + wx_row * 0.35f, line, white);
        } else {
            std::snprintf(line, sizeof(line), "REGIONAL WEATHER MODEL  |  %d x %d x 4  |  local tile off",
                          atmo.grid_width(), atmo.grid_height());
            wx_text(x + wx_pad, y + wx_row * 0.35f, line, white);
        }
        wx_text(x + wx_pad, y + wx_row * 1.25f, "Flow lines: model wind streamlines   Dot: cloud   Blue: rain", white);
        for (int level = 0; level < 4; ++level) {
            const float col_x = x + wx_pad + level * 150.0f * wx_scale;
            const float swatch = 13.0f * wx_scale;
            hud_rect(col_x, y + wx_row * 2.25f, swatch, swatch,
                     kLayerColors[level].x, kLayerColors[level].y, kLayerColors[level].z, 1.0f);
            char label[32];
            if (level_agl[static_cast<size_t>(level)] >= 1000.0f)
                std::snprintf(label, sizeof(label), "L%d %.1fkm", level, level_agl[static_cast<size_t>(level)] * 0.001f);
            else
                std::snprintf(label, sizeof(label), "L%d %.0fm", level, level_agl[static_cast<size_t>(level)]);
            wx_text(col_x + 18.0f * wx_scale, y + wx_row * 2.12f, label, white);
        }
        const char *source = "DEFAULT";
        switch (atmo.model_source()) {
            case WeatherSource::Baked: source = "BAKED"; break;
            case WeatherSource::Live: source = "LIVE"; break;
            case WeatherSource::Manual: source = "MANUAL"; break;
            case WeatherSource::Default: source = "DEFAULT"; break;
        }
        std::snprintf(line, sizeof(line), "Input: %s   Snapshot: %llu   Shown: %d   Step: %.0f m",
                      source, static_cast<unsigned long long>(generation), visible_markers,
                      slices[0].step_e_m);
        wx_text(x + wx_pad, y + wx_row * 3.12f, line, white);
        std::snprintf(line, sizeof(line), "Flow trails: accumulated pathlines advected by model wind");
        wx_text(x + wx_pad, y + wx_row * 4.12f, line, yellow);
        const int model_minutes = static_cast<int>(std::max(0.0, view.simulation_time_s) / 60.0);
        const int model_seconds = static_cast<int>(std::max(0.0, view.simulation_time_s)) % 60;
        std::snprintf(line, sizeof(line), "MODEL TIME  +%02d:%02d", model_minutes, model_seconds);
        wx_text(x + wx_pad, y + wx_row * 5.12f, line, yellow);
    } else if (view.weather_level >= 0) {
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
        const float pw = 550.0f * scale, x = w - pw - pad, y = h - 268.0f * scale;
        panel(x, y, pw, row * 9.8f, yellow);
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

void AircraftAndHud::render_weather_model_overlay(float w, float h, const RegionalAtmosphere &atmo,
                                                   const Mat4 &projection_view, double east_m, double north_m,
                                                   double simulation_time_s) {
    hud_verts_.clear();
    DebugOverlayView view{};
    view.projection_view = projection_view;
    view.east_m = east_m;
    view.north_m = north_m;
    view.simulation_time_s = simulation_time_s;
    view.weather_volume_3d = true;
    render_debug_overlays(w, h, 0.0f, atmo, view);
    flush_hud(w, h);
}

} // namespace nsim
