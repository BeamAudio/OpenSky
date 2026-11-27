#include "aircraft_hud.hpp"
#include "flight_profiler.hpp"

#include <cstdio>
#include <cstring>

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
    const bool portrait = (w < h);
    const bool promo = (view.promo_mode != PromoInspectMode::None);
    const float scale = portrait
        ? std::clamp(w / 1080.0f, 0.78f, 1.35f)
        : std::min(w / 1920.0f, h / 1080.0f);
    const float pad = 16.0f * scale, font = 18.0f * scale, row = 21.0f * scale;
    const Vec3 white{0.92f, 0.96f, 0.99f}, muted{0.62f, 0.72f, 0.82f};
    const Vec3 green{0.34f, 0.96f, 0.58f}, blue{0.32f, 0.84f, 1.0f};
    const Vec3 yellow{1.0f, 0.79f, 0.34f}, orange{1.0f, 0.50f, 0.24f}, red{1.0f, 0.36f, 0.34f};
    const Vec3 violet{0.72f, 0.52f, 1.0f};
    char line[200];
    auto project = [&](Vec3 point, Vec2 &screen) {
        const Vec4 clip = view.projection_view.mul({point.x, point.y, point.z, 1.0f});
        if (clip.w <= 0.13f || !std::isfinite(clip.w) ||
            std::abs(clip.x) > clip.w || std::abs(clip.y) > clip.w) return false;
        screen = {(clip.x / clip.w + 1.0f) * w * 0.5f, (1.0f - clip.y / clip.w) * h * 0.5f};
        return true;
    };
    auto text_at = [&](float x, float y, const char *msg, Vec3 color, float fh = -1.0f) {
        hud_text(x, y, fh > 0.0f ? fh : font, msg, color.x, color.y, color.z, 1.0f);
    };
    auto panel = [&](float x, float y, float width, float height, Vec3 color, float bg_a = 0.92f) {
        hud_rect(x, y, width, height, 0.018f, 0.036f, 0.058f, bg_a);
        hud_border(x, y, width, height, std::max(1.2f, 1.5f * scale), color.x, color.y, color.z, 0.72f);
        hud_rect(x, y, width, std::max(2.5f, 3.5f * scale), color.x, color.y, color.z, 0.95f);
    };
    auto draw_bar = [&](float x, float y, float bw, float bh, float frac, Vec3 col) {
        hud_rect(x, y, bw, bh, 0.06f, 0.10f, 0.16f, 0.95f);
        const float fill = std::clamp(frac, 0.0f, 1.0f) * bw;
        if (fill > 0.5f) hud_rect(x, y, fill, bh, col.x, col.y, col.z, 0.94f);
        hud_border(x, y, bw, bh, 1.0f * scale, 0.25f, 0.40f, 0.55f, 0.65f);
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
        static constexpr size_t kTracesPerLevel[4] = {64, 48, 36, 24};
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

        if (!promo) {
        const float x = wx_pad, y = wx_pad, pw = std::min(w - 2.0f * wx_pad, 760.0f * wx_scale), ph = 5.95f * wx_row;
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
        }
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
            hud_arrow(a, b, (promo ? 4.2f : 2.0f) * scale, color);
            text_at(b.x + 6.0f * scale, b.y - 6.0f * scale, name, color, (promo ? 24.0f : 18.0f) * scale);
        };
        if (!st_.slew_fast && st_.view_mode == 1) {
            vector({}, body({3.0f, 0, 0}), red, "RIGHT");
            vector({}, body({0, 2.8f, 0}), green, "UP / LIFT");
            vector({}, body({0, 0, -4.8f}), white, "FWD");
            if (length(air) > 0.5f) vector({}, normalize(air) * 7.2f, blue, "AIR PATH");
            if (f.thrust_n > 1.0) vector(body({0, 0, -1.4f}), body({0, 0, -1.4f - static_cast<float>(f.thrust_n * 0.0022)}), yellow, "THRUST");
            for (size_t i = 0; i < f.wheel_wow.size(); ++i) {
                const auto &wheel = f.wheel_offset_m[i];
                const Vec3 hub = body({static_cast<float>(wheel[1]),
                                       static_cast<float>(-wheel[2] + f.wheel_comp_m[i]),
                                       static_cast<float>(-wheel[0])});
                Vec2 marker;
                if (project(hub, marker)) {
                    const auto c = f.wheel_wow[i] ? green : red;
                    const float hs = (promo ? 6.0f : 3.0f) * scale;
                    hud_rect(marker.x - hs, marker.y - hs, 2.0f * hs, 2.0f * hs, c.x, c.y, c.z, 0.95f);
                    if (promo) {
                        static constexpr const char *kWn[3] = {"NOSE", "L", "R"};
                        text_at(marker.x + 8.0f * scale, marker.y - 6.0f * scale, kWn[i], c, 15.0f * scale);
                    }
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

        if (!promo) {
        const float pw = std::min(w - 2.0f * pad, 540.0f * scale), x = pad;
        const float y = portrait ? (h - 247.0f * scale - (view.profiler ? 268.0f * scale : 0.0f)) : (h - 247.0f * scale);
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
    }

    if (!promo) {
    if (const FlightProfiler *prof = view.profiler) {
        const auto &host = prof->host();
        const auto &gpu = prof->latest_gpu_ms();
        const auto &phase = prof->latest_phases_ms();
        const float pw = std::min(w - 2.0f * pad, 550.0f * scale), x = portrait ? pad : (w - pw - pad), y = h - 268.0f * scale;
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
            if (host.package_temp_c > 0.0) {
                std::snprintf(line, sizeof(line), "GPU %lld%%  VRAM %.0f GTT %.0f MiB  Temp %.0fC",
                              static_cast<long long>(host.gpu_busy_pct), host.vram_used_bytes / 1048576.0,
                              host.gtt_used_bytes / 1048576.0, host.package_temp_c);
            } else {
                std::snprintf(line, sizeof(line), "GPU device %lld%%  |  VRAM %.0f  GTT %.0f MiB",
                              static_cast<long long>(host.gpu_busy_pct), host.vram_used_bytes / 1048576.0,
                              host.gtt_used_bytes / 1048576.0);
            }
            row_text(3, line, white);
        } else if (host.package_temp_c > 0.0) {
            std::snprintf(line, sizeof(line), "Package temp %.0f C  |  GPU mem via GL", host.package_temp_c);
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
    return;
    }

    // === CLEAN HIGH-IMPACT PORTRAIT ENGINE OVERLAYS (PROMO SERIES 01..05) ===
    const float pw = w - 40.0f * scale;
    const float px = 20.0f * scale;
    const float top_y = 20.0f * scale;
    auto big_hdr = [&](float x, float y, float wbox, const char *title, const char *stage, Vec3 accent) {
        text_at(x + 20.0f * scale, y + 14.0f * scale, title, accent, 25.0f * scale);
        if (stage && *stage) {
            const float sw = std::min(wbox * 0.56f, static_cast<float>(std::strlen(stage)) * 11.4f * scale + 28.0f * scale);
            const float sx = x + wbox - sw - 18.0f * scale;
            const float sy = y + 10.0f * scale;
            hud_rect(sx, sy, sw, 32.0f * scale, accent.x * 0.24f, accent.y * 0.24f, accent.z * 0.24f, 0.96f);
            hud_border(sx, sy, sw, 32.0f * scale, 1.5f * scale, accent.x, accent.y, accent.z, 0.90f);
            text_at(sx + 12.0f * scale, sy + 6.0f * scale, stage, white, 19.0f * scale);
        }
    };

    if (view.promo_mode == PromoInspectMode::LoadBalancing) {
        panel(px, top_y, pw, 168.0f * scale, yellow, 0.92f);
        big_hdr(px, top_y, pw, "LOAD BALANCER", view.promo_stage, yellow);
        static constexpr const char *kTier[4] = {"F1 LOW", "F2 BALANCED", "F3 HIGH", "F4 ULTRA"};
        const int qi = std::clamp(view.quality, 0, 3);
        const double fps = view.cur_fps;
        const Vec3 fps_col = (fps >= 56.0) ? green : (fps >= 42.0 ? yellow : red);
        std::snprintf(line, sizeof(line), "%.1f FPS", fps);
        text_at(px + 20.0f * scale, top_y + 54.0f * scale, line, fps_col, 42.0f * scale);
        std::snprintf(line, sizeof(line), "%s   |   %.1f ms", kTier[qi], view.cur_avg_ms);
        text_at(px + 265.0f * scale, top_y + 62.0f * scale, line, white, 26.0f * scale);

        // Clean single-row stacked GPU timeline bar against 16.6ms deadline (0..26ms)
        const float bx = px + 20.0f * scale, by = top_y + 112.0f * scale, bw = pw - 40.0f * scale, bh = 32.0f * scale;
        hud_rect(bx, by, bw, bh, 0.06f, 0.09f, 0.14f, 0.96f);
        static constexpr float kMaxMs = 26.0f;
        const auto &gpu = view.profiler ? view.profiler->latest_gpu_ms() : std::array<double, FlightProfiler::PassCount>{};
        const struct { FlightProfiler::Pass p; Vec3 c; } segs[6] = {
            {FlightProfiler::Terrain, green}, {FlightProfiler::World, orange}, {FlightProfiler::CloudsPost, blue},
            {FlightProfiler::NativeCockpit, violet}, {FlightProfiler::Sky, yellow}, {FlightProfiler::Upload, {0.96f, 0.42f, 0.72f}},
        };
        float cur_x = bx;
        for (const auto &s : segs) {
            const float gms = std::max(0.0f, static_cast<float>(gpu[s.p]));
            const float sw = std::min((bx + bw) - cur_x, (gms / kMaxMs) * bw);
            if (sw > 0.5f) {
                hud_rect(cur_x, by, sw, bh, s.c.x, s.c.y, s.c.z, 0.94f);
                cur_x += sw;
            }
        }
        const float dl_x = bx + (16.67f / kMaxMs) * bw;
        hud_rect(dl_x - 2.0f * scale, by - 5.0f * scale, 4.0f * scale, bh + 10.0f * scale, 1.0f, 0.28f, 0.28f, 1.0f);
        hud_border(bx, by, bw, bh, 1.5f * scale, 0.45f, 0.65f, 0.85f, 0.80f);
        text_at(dl_x + 8.0f * scale, by + 6.0f * scale, "16.6ms LIMIT", white, 17.5f * scale);

        const float bot_y = h - 300.0f * scale;
        panel(px, bot_y, pw, 280.0f * scale, blue, 0.92f);
        const FlightBudgetDecision fb = view.budget ? *view.budget : FlightBudgetDecision{};
        const float lx = px + 24.0f * scale, bw2 = pw - 48.0f * scale;

        std::snprintf(line, sizeof(line), "INTERNAL SCENE:  %dx%d  (%d%%)", view.scene_w, view.scene_h, fb.pixel_percent);
        text_at(lx, bot_y + 22.0f * scale, line, yellow, 24.0f * scale);
        draw_bar(lx, bot_y + 54.0f * scale, bw2, 24.0f * scale, fb.pixel_percent / 100.0f, (fb.pixel_percent < 80 ? yellow : green));

        std::snprintf(line, sizeof(line), "NATIVE COCKPIT:  %dx%d  (100%% SHARP)", view.out_w, view.out_h);
        text_at(lx, bot_y + 104.0f * scale, line, blue, 24.0f * scale);
        draw_bar(lx, bot_y + 136.0f * scale, bw2, 24.0f * scale, 1.0f, blue);

        const float tier1_avg = 0.25f * (fb.cloud_step_scale + fb.terrain_lod_scale + fb.city_dist_scale + fb.veg_density_scale);
        std::snprintf(line, sizeof(line), "GPU BUDGET:  %.1f / 16.6 ms   |   LOD+CLD SCALE %.0f%%",
                      view.cur_avg_ms, tier1_avg * 100.0f);
        text_at(lx, bot_y + 186.0f * scale, line, white, 24.0f * scale);
        draw_bar(lx, bot_y + 218.0f * scale, bw2, 24.0f * scale, static_cast<float>(view.cur_avg_ms / 24.0),
                 view.cur_avg_ms <= 16.6 ? green : red);
        return;
    }

    if (view.promo_mode == PromoInspectMode::TileStreaming) {
        panel(px, top_y, pw, 166.0f * scale, green, 0.92f);
        big_hdr(px, top_y, pw, "CDLOD QUADTREE STREAMER", view.promo_stage, green);
        const double kt = st_.speed_mps * 1.94384;
        std::snprintf(line, sizeof(line), "%.0f KT   |   %d LEAVES (1 DRAW)   |   MAX L%d",
                      kt, view.cdlod.visible_nodes, view.cdlod.max_active_lod);
        text_at(px + 20.0f * scale, top_y + 62.0f * scale, line, white, 26.0f * scale);

        const int lcnt[5] = {
            view.cdlod.visible_by_lod[9], view.cdlod.visible_by_lod[8], view.cdlod.visible_by_lod[7],
            view.cdlod.visible_by_lod[6], view.cdlod.visible_by_lod[0] + view.cdlod.visible_by_lod[1] +
            view.cdlod.visible_by_lod[2] + view.cdlod.visible_by_lod[3] + view.cdlod.visible_by_lod[4] + view.cdlod.visible_by_lod[5]
        };
        const char *ltxt[5] = {"L9", "L8", "L7", "L6", "HORIZ"};
        const Vec3 lcolors[5] = {{0.08f, 0.96f, 1.0f}, {0.20f, 0.94f, 0.46f}, {0.98f, 0.80f, 0.16f}, {1.0f, 0.42f, 0.14f}, {0.64f, 0.36f, 1.0f}};
        for (int i = 0; i < 5; ++i) {
            const float bx = px + 20.0f * scale + i * 198.0f * scale, by = top_y + 114.0f * scale;
            hud_rect(bx, by + 2.0f * scale, 22.0f * scale, 22.0f * scale, lcolors[i].x, lcolors[i].y, lcolors[i].z, 1.0f);
            std::snprintf(line, sizeof(line), "%s:%d", ltxt[i], lcnt[i]);
            text_at(bx + 30.0f * scale, by, line, white, 22.0f * scale);
        }

        const float bot_y = h - 320.0f * scale;
        panel(px, bot_y, pw, 300.0f * scale, blue, 0.92f);
        const float rsz = 256.0f * scale, rx0 = px + 22.0f * scale, ry0 = bot_y + 22.0f * scale;
        hud_rect(rx0, ry0, rsz, rsz, 0.015f, 0.028f, 0.048f, 0.98f);
        hud_border(rx0, ry0, rsz, rsz, 2.0f * scale, 0.35f, 0.72f, 0.95f, 0.85f);
        static constexpr double kRadarSpanM = 28000.0;
        const float rc_x = rx0 + rsz * 0.5f, rc_y = ry0 + rsz * 0.5f;
        if (view.resident_keys) {
            for (const QuadKey &qk : *view.resident_keys) {
                if (qk.lod < 5) continue;
                double ce = 0.0, cn = 0.0, hs = 0.0;
                AsyncTerrainStreamer::quad_bounds_en(qk, ce, cn, hs);
                const float x0 = rc_x + static_cast<float>((ce - hs - view.east_m) / kRadarSpanM) * rsz;
                const float x1 = rc_x + static_cast<float>((ce + hs - view.east_m) / kRadarSpanM) * rsz;
                const float y0 = rc_y - static_cast<float>((cn + hs - view.north_m) / kRadarSpanM) * rsz;
                const float y1 = rc_y - static_cast<float>((cn - hs - view.north_m) / kRadarSpanM) * rsz;
                if (x1 <= rx0 || x0 >= rx0 + rsz || y1 <= ry0 || y0 >= ry0 + rsz) continue;
                const float cx0 = std::max(x0, rx0), cy0 = std::max(y0, ry0);
                const float cx1 = std::min(x1, rx0 + rsz), cy1 = std::min(y1, ry0 + rsz);
                const Vec3 &tc = lcolors[std::clamp(9 - static_cast<int>(qk.lod), 0, 4)];
                hud_rect(cx0, cy0, cx1 - cx0, cy1 - cy0, tc.x, tc.y, tc.z, qk.lod >= 8 ? 0.42f : 0.25f);
                hud_border(cx0, cy0, cx1 - cx0, cy1 - cy0, 1.2f, tc.x, tc.y, tc.z, 0.75f);
            }
        }
        const double hdg_r = st_.heading_deg * DEG2RAD_D;
        const Vec2 vdir{static_cast<float>(std::sin(hdg_r)), static_cast<float>(-std::cos(hdg_r))};
        const float arrow_len = std::clamp(static_cast<float>(kt * 0.34), 24.0f, 110.0f) * scale;
        hud_arrow({rc_x, rc_y}, {rc_x + vdir.x * arrow_len, rc_y + vdir.y * arrow_len}, 3.5f * scale, white);
        hud_rect(rc_x - 5.0f * scale, rc_y - 5.0f * scale, 10.0f * scale, 10.0f * scale, 1.0f, 0.28f, 0.28f, 1.0f);
        text_at(rx0 + 10.0f * scale, ry0 + 8.0f * scale, "CACHE MAP", white, 17.0f * scale);

        const float mx = px + 306.0f * scale, mw = pw - 328.0f * scale;
        const float cache_pct = view.stream.max_pages > 0 ? float(view.stream.resident_pages) / view.stream.max_pages : 0.0f;
        std::snprintf(line, sizeof(line), "CACHE:  %d / %d PAGES (%.0f MiB)",
                      view.stream.resident_pages, view.stream.max_pages,
                      (view.stream.resident_pages * double(view.stream.gpu_page_bytes)) / 1048576.0);
        text_at(mx, bot_y + 28.0f * scale, line, white, 24.0f * scale);
        draw_bar(mx, bot_y + 62.0f * scale, mw, 24.0f * scale, cache_pct, green);

        const double avg_b = view.profiler ? view.profiler->build_ms_per_page() : view.stream.avg_worker_ms;
        const double avg_e = view.profiler ? view.profiler->encode_ms_per_page() : 0.0;
        std::snprintf(line, sizeof(line), "BC7 WORKERS: %.0f ms  |  QUEUE %d", avg_b + avg_e, view.stream.queued_jobs);
        text_at(mx, bot_y + 114.0f * scale, line, yellow, 24.0f * scale);

        std::snprintf(line, sizeof(line), "CITY STREAM: %d CHUNKS  |  %d CARS", view.city_chunks, view.active_cars);
        text_at(mx, bot_y + 174.0f * scale, line, orange, 24.0f * scale);

        std::snprintf(line, sizeof(line), "REGION: %s", view.region_id ? view.region_id : "es_aragon");
        text_at(mx, bot_y + 232.0f * scale, line, blue, 22.0f * scale);
        return;
    }

    if (view.promo_mode == PromoInspectMode::WeatherCloudsWindshield) {
        panel(px, top_y, pw, 158.0f * scale, blue, 0.92f);
        big_hdr(px, top_y, pw, st_.view_mode == 0 ? "WINDSHIELD FLUID PDE" : "3D OROGRAPHIC WEATHER", view.promo_stage, blue);
        std::snprintf(line, sizeof(line), "WIND %03.0f/%.0f KT   |   RAIN %.1f mm/h   |   CLD %.0f%%",
                      last_air_.from_dir_deg(), last_air_.speed_kt(), last_air_.precipitation_mm_hr, last_air_.local_cloud_frac * 100.0f);
        text_at(px + 20.0f * scale, top_y + 60.0f * scale, line, white, 25.0f * scale);
        std::snprintf(line, sizeof(line), st_.view_mode == 0
            ? "AIRFLOW %.0f KT  |  XWIND %+.1f m/s  |  WIPERS %s"
            : "GRID 384x288x4 + 8KM LOCAL  |  UPDRAFT %+.1f m/s",
            st_.view_mode == 0 ? rain_airspeed_mps_ * 1.94384f : static_cast<float>(last_air_.wind_up_mps),
            rain_cross_mps_,
            st_.wiper_mode >= 2 ? "ACTIVE" : "AUTO");
        text_at(px + 20.0f * scale, top_y + 104.0f * scale, line, green, 23.0f * scale);

        const float bot_y = h - 320.0f * scale;
        panel(px, bot_y, pw, 300.0f * scale, blue, 0.92f);
        if (st_.view_mode != 0) {
            static const Vec3 kLC[4] = {{0.36f, 0.92f, 0.65f}, {0.28f, 0.84f, 1.0f}, {1.0f, 0.74f, 0.34f}, {0.82f, 0.62f, 1.0f}};
            for (int lvl = 0; lvl < 4; ++lvl) {
                const auto s = atmo.debug_slice(view.east_m, view.north_m, lvl, 1);
                const WeatherDebugCell c = s.cells.empty() ? WeatherDebugCell{} : s.cells[s.cells.size() / 2];
                const float spd_kt = std::hypot(c.wind_enu_mps.x, c.wind_enu_mps.y) * 1.94384f;
                const float ry = bot_y + 22.0f * scale + lvl * 66.0f * scale;
                std::snprintf(line, sizeof(line), "L%d %.0fm   WIND %2.0fKT   CLD %3.0f%%   RAIN %.1f",
                              lvl, s.agl_m, spd_kt, c.cloud_fraction * 100.0f, c.rain_mm_hr);
                text_at(px + 24.0f * scale, ry, line, white, 22.0f * scale);
                draw_bar(px + 24.0f * scale, ry + 30.0f * scale, pw - 48.0f * scale, 20.0f * scale, c.cloud_fraction, kLC[lvl]);
            }
        } else {
            const float pip_x = 42.0f * scale, pip_y = h - 294.0f * scale, pip_w = 400.0f * scale, pip_h = 250.0f * scale;
            text_at(pip_x, bot_y + 12.0f * scale, "LIVE GPU FLUID BUFFER", blue, 20.0f * scale);
            hud_rect(pip_x, pip_y, pip_w, pip_h, 0.02f, 0.05f, 0.09f, 0.98f);

            const float rx = px + 448.0f * scale, rw = pw - 472.0f * scale;
            std::snprintf(line, sizeof(line), "AIRFLOW: %.0f KT", rain_airspeed_mps_ * 1.94384f);
            text_at(rx, bot_y + 28.0f * scale, line, blue, 24.0f * scale);
            draw_bar(rx, bot_y + 60.0f * scale, rw, 22.0f * scale, rain_airspeed_mps_ / 62.0f, blue);

            std::snprintf(line, sizeof(line), "PROPWASH: %.0f%%", rain_propwash01_ * 100.0f);
            text_at(rx, bot_y + 110.0f * scale, line, green, 24.0f * scale);
            draw_bar(rx, bot_y + 142.0f * scale, rw, 22.0f * scale, rain_propwash01_, green);

            static constexpr const char *kWm[4] = {"OFF", "AUTO", "LOW", "HIGH"};
            std::snprintf(line, sizeof(line), "WIPERS: %s (%.0f%%)", kWm[std::clamp(st_.wiper_mode, 0, 3)], wiper_pos_ * 100.0f);
            text_at(rx, bot_y + 192.0f * scale, line, white, 24.0f * scale);
            draw_bar(rx, bot_y + 224.0f * scale, rw, 22.0f * scale, wiper_pos_, violet);
        }
        return;
    }

    if (view.promo_mode == PromoInspectMode::AircraftSystems) {
        panel(px, top_y, pw, 228.0f * scale, orange, 0.92f);
        big_hdr(px, top_y, pw, "28V ELECTRICAL BUS", view.promo_stage, orange);
        const bool m_bat = systems_.get_master_battery();
        const bool m_alt = systems_.get_master_alternator();
        const bool crank = systems_.is_starter_cranking();
        const double bus_v = systems_.get_bus_volts();
        const double amm_a = systems_.get_ammeter_amps();

        auto node_box = [&](float bx, float by, float bw, float bh, const char *l1, const char *l2, Vec3 c) {
            hud_rect(bx, by, bw, bh, c.x * 0.16f, c.y * 0.16f, c.z * 0.16f, 0.96f);
            hud_border(bx, by, bw, bh, 2.0f * scale, c.x, c.y, c.z, 0.92f);
            text_at(bx + 14.0f * scale, by + 8.0f * scale, l1, c, 19.0f * scale);
            text_at(bx + 14.0f * scale, by + 34.0f * scale, l2, white, 22.0f * scale);
        };
        const float ny = top_y + 58.0f * scale;
        char b_txt[40], m_txt[40], a_txt[40];
        std::snprintf(b_txt, sizeof(b_txt), "%s (%.0f%%)", m_bat ? "ON" : "OFF", systems_.get_battery_soc() * 100.0);
        std::snprintf(m_txt, sizeof(m_txt), "%.1fV | %+.0fA", bus_v, amm_a);
        std::snprintf(a_txt, sizeof(a_txt), "%s", crank ? "CRANK!" : (m_alt && bus_v > 25.0 ? "CHARGING" : "OFF"));
        node_box(px + 20.0f * scale, ny, 240.0f * scale, 68.0f * scale, "24V BAT", b_txt, m_bat ? green : red);
        hud_line({px + 260.0f * scale, ny + 34.0f * scale}, {px + 340.0f * scale, ny + 34.0f * scale}, 4.5f * scale, m_bat ? green : muted, 0.95f);
        node_box(px + 340.0f * scale, ny, 330.0f * scale, 68.0f * scale, crank ? "BUS (SAG)" : "MAIN BUS", m_txt, crank ? yellow : (bus_v > 20.0 ? blue : red));
        hud_line({px + 670.0f * scale, ny + 34.0f * scale}, {px + 750.0f * scale, ny + 34.0f * scale}, 4.5f * scale, (m_alt && bus_v > 25.0) ? green : muted, 0.95f);
        node_box(px + 750.0f * scale, ny, 260.0f * scale, 68.0f * scale, "60A ALT", a_txt, crank ? orange : (m_alt && bus_v > 25.0 ? green : muted));

        // 6 Critical Breakers in one bold readable row
        const int cb_ids[6] = {
            nspain::CB_ALT_FIELD, nspain::CB_AVIONICS_BUS_1, nspain::CB_AVIONICS_BUS_2,
            nspain::CB_FLAPS, nspain::CB_LAND_LIGHT, nspain::CB_PITOT_HEAT
        };
        const char *cb_lbl[6] = {"ALT", "AVN1", "AVN2", "FLAP", "LDG", "PITOT"};
        for (int i = 0; i < 6; ++i) {
            const float cw = 158.0f * scale, ch = 62.0f * scale;
            const float cx = px + 20.0f * scale + i * 166.0f * scale, cy = top_y + 144.0f * scale;
            const bool pulled = systems_.is_breaker_pulled(cb_ids[i]);
            const bool pwr = systems_.is_circuit_powered(cb_ids[i]);
            const Vec3 cc = pulled ? red : (pwr ? green : yellow);
            hud_rect(cx, cy, cw, ch, cc.x * 0.18f, cc.y * 0.18f, cc.z * 0.18f, 0.96f);
            hud_border(cx, cy, cw, ch, pulled ? 3.0f * scale : 1.5f * scale, cc.x, cc.y, cc.z, 0.92f);
            text_at(cx + 14.0f * scale, cy + 7.0f * scale, cb_lbl[i], white, 20.0f * scale);
            text_at(cx + 14.0f * scale, cy + 32.0f * scale, pulled ? "TRIP!" : (pwr ? "PWR" : "OFF"), cc, 19.0f * scale);
        }

        const float bot_y = h - 270.0f * scale;
        panel(px, bot_y, pw, 250.0f * scale, green, 0.92f);
        static constexpr const char *kFuelSel[4] = {"OFF", "LEFT", "BOTH", "RIGHT"};
        static constexpr const char *kMagPos[5] = {"OFF", "R", "L", "BOTH", "START"};
        std::snprintf(line, sizeof(line), "ENGINE: %4.0f RPM   |   MAG: %s   |   OIL %.0f PSI",
                      fdm_.state().rpm, kMagPos[std::clamp(systems_.get_magnetos(), 0, 4)], systems_.get_oil_press_psi());
        text_at(px + 24.0f * scale, bot_y + 24.0f * scale, line, white, 25.0f * scale);

        std::snprintf(line, sizeof(line), "FUEL: %.1f GAL (%s)   |   FLOW %.1f GPH   |   EGT %.0f F",
                      systems_.get_left_tank_gal() + systems_.get_right_tank_gal(),
                      kFuelSel[std::clamp(systems_.get_fuel_selector(), 0, 3)],
                      systems_.get_fuel_flow_gph(), systems_.get_egt_f());
        text_at(px + 24.0f * scale, bot_y + 80.0f * scale, line, yellow, 24.0f * scale);

        std::snprintf(line, sizeof(line), "AVN1 (GNS430): %s   |   FLAPS: %.0f -> %.0f DEG",
                      systems_.is_avionics_bus_powered(1) ? "ONLINE" : "OFFLINE",
                      systems_.get_flaps_actual_deg(), systems_.get_flaps_target_deg());
        text_at(px + 24.0f * scale, bot_y + 138.0f * scale, line,
                systems_.is_avionics_bus_powered(1) ? green : red, 25.0f * scale);
        draw_bar(px + 24.0f * scale, bot_y + 188.0f * scale, pw - 48.0f * scale, 26.0f * scale,
                 static_cast<float>(systems_.get_flaps01()), blue);
        return;
    }

    if (view.promo_mode == PromoInspectMode::AircraftPhysics) {
        const auto &f = fdm_.state();
        const auto &controls = fdm_.controls();
        const Vec3 top_col = f.stalled ? red : (f.stall_warn ? orange : green);
        panel(px, top_y, pw, 158.0f * scale, top_col, 0.92f);
        big_hdr(px, top_y, pw, "120 HZ JSBSIM PHYSICS", view.promo_stage, top_col);
        std::snprintf(line, sizeof(line), "%.0f KT   |   %+.2f G   |   %s",
                      f.ias_kt, f.g_load,
                      f.stalled ? "STALL CL_MAX!" : (f.on_ground ? "WEIGHT ON WHEELS" : "AIRBORNE 6-DOF"));
        text_at(px + 20.0f * scale, top_y + 62.0f * scale, line, f.stalled ? red : white, 30.0f * scale);
        std::snprintf(line, sizeof(line), "SURFACE FRICTION MU = %.2f   |   WOW N:%d  L:%d  R:%d",
                      last_surf_friction_, f.wheel_wow[0], f.wheel_wow[1], f.wheel_wow[2]);
        text_at(px + 20.0f * scale, top_y + 108.0f * scale, line, blue, 22.0f * scale);

        const float bot_y = h - 306.0f * scale;
        panel(px, bot_y, pw, 286.0f * scale, green, 0.92f);

        // Col 1: Bold 2D Yoke Crosshair
        const float c1x = px + 24.0f * scale, ssz = 200.0f * scale, sy0 = bot_y + 24.0f * scale;
        hud_rect(c1x, sy0, ssz, ssz, 0.02f, 0.04f, 0.07f, 0.96f);
        hud_border(c1x, sy0, ssz, ssz, 2.0f * scale, 0.40f, 0.72f, 0.95f, 0.82f);
        hud_line({c1x + ssz * 0.5f, sy0}, {c1x + ssz * 0.5f, sy0 + ssz}, 1.5f * scale, muted, 0.45f);
        hud_line({c1x, sy0 + ssz * 0.5f}, {c1x + ssz, sy0 + ssz * 0.5f}, 1.5f * scale, muted, 0.45f);
        const float stk_x = c1x + ssz * (0.5f + 0.45f * static_cast<float>(std::clamp(controls.aileron, -1.0, 1.0)));
        const float stk_y = sy0 + ssz * (0.5f - 0.45f * static_cast<float>(std::clamp(controls.elevator, -1.0, 1.0)));
        hud_line({c1x + ssz * 0.5f, sy0 + ssz * 0.5f}, {stk_x, stk_y}, 3.5f * scale, blue, 0.95f);
        hud_rect(stk_x - 9.0f * scale, stk_y - 9.0f * scale, 18.0f * scale, 18.0f * scale, 0.34f, 0.96f, 0.58f, 1.0f);
        text_at(c1x + 28.0f * scale, sy0 + ssz + 14.0f * scale, "YOKE INPUT", white, 21.0f * scale);

        // Col 2: 3 Clean Aerodynamic Bars
        const float c2x = px + 258.0f * scale, c2w = 420.0f * scale;
        std::snprintf(line, sizeof(line), "AoA %+.1f DEG %s", f.alpha_deg, f.stalled ? "STALL!" : "");
        text_at(c2x, bot_y + 24.0f * scale, line, f.stalled ? red : white, 23.0f * scale);
        draw_bar(c2x, bot_y + 56.0f * scale, c2w, 22.0f * scale, static_cast<float>((f.alpha_deg + 4.0) / 24.0), f.stalled ? red : green);
        const float crit_x = c2x + c2w * ((15.5f + 4.0f) / 24.0f);
        hud_rect(crit_x - 2.0f * scale, bot_y + 52.0f * scale, 4.0f * scale, 30.0f * scale, 1.0f, 0.3f, 0.3f, 1.0f);

        std::snprintf(line, sizeof(line), "CL %.2f   |   BETA %+.1f DEG", f.cl, f.beta_deg);
        text_at(c2x, bot_y + 108.0f * scale, line, blue, 23.0f * scale);
        draw_bar(c2x, bot_y + 140.0f * scale, c2w, 22.0f * scale, static_cast<float>(f.cl / 1.8), blue);

        std::snprintf(line, sizeof(line), "BRAKES: %.0f%%", std::max(controls.brake_left, controls.brake_right) * 100.0);
        text_at(c2x, bot_y + 192.0f * scale, line, orange, 23.0f * scale);
        draw_bar(c2x, bot_y + 224.0f * scale, c2w, 22.0f * scale, static_cast<float>(std::max(controls.brake_left, controls.brake_right)), orange);

        // Col 3: 3 Bold Gear Oleo Struts
        const float c3x = px + 708.0f * scale, c3w = pw - 728.0f * scale;
        static constexpr const char *kGearLbl[3] = {"N", "L", "R"};
        const float col_w = (c3w - 24.0f * scale) / 3.0f;
        for (int i = 0; i < 3; ++i) {
            const float gx = c3x + i * (col_w + 12.0f * scale);
            const float gy = bot_y + 24.0f * scale, gh = 176.0f * scale;
            const double mm = std::max(0.0, f.wheel_comp_m[i] * 1000.0);
            const Vec3 gc = f.wheel_wow[i] ? green : blue;
            hud_rect(gx, gy, col_w, gh, 0.03f, 0.06f, 0.10f, 0.96f);
            const float ch = std::clamp(static_cast<float>(mm / 140.0), 0.0f, 1.0f) * gh;
            if (ch > 1.0f) hud_rect(gx, gy + gh - ch, col_w, ch, gc.x, gc.y, gc.z, 0.94f);
            hud_border(gx, gy, col_w, gh, 2.0f * scale, gc.x, gc.y, gc.z, 0.90f);
            std::snprintf(line, sizeof(line), "%s %.0f", kGearLbl[i], mm);
            text_at(gx + 6.0f * scale, gy + gh + 16.0f * scale, line, gc, 21.0f * scale);
        }
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
