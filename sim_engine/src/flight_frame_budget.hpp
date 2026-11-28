#pragma once

#include "gl_loader.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace nsim {

struct FlightBudgetDecision {
    int pixel_percent = 56;
    float load_pressure = 0.0f;       // 0.0 = ample headroom, 1.0 = thermal/frame limit
    float cloud_step_scale = 1.0f;    // Zero-realloc raymarch step multiplier [0.78..1.0]
    float terrain_lod_scale = 1.0f;   // Zero-realloc terrain split multiplier [0.82..1.0]
    float city_dist_scale = 1.0f;     // Zero-realloc city/building draw distance scale [0.74..1.0]
    float veg_density_scale = 1.0f;   // Zero-realloc vegetation density scale [0.60..1.0]
    int max_uploads = 1;
    double upload_budget_ms = 0.58;
    bool limit_terrain_workers = false;
    bool reduce_distant_detail = false;
    double gpu_ema_ms = 0.0;
};

// Multi-tier thermal-aware load balancer for 60 FPS target headroom.
// Uses cheap continuous Tier-1 detail scaling before touching FBO pixel scale,
// avoiding temporal history wipes and thermal boost-throttle oscillation.
class FlightFrameBudget {
public:
    ~FlightFrameBudget() {
        if (queries_[0].begin) {
            for (const auto &query : queries_) {
                const GLuint ids[] = {query.begin, query.end};
                glDeleteQueries(2, ids);
            }
        }
    }

    int start(bool enabled, int ceiling, double now, uint64_t frame,
              double cpu_work_ms = 0.0, int queued_jobs = 0, double package_temp_c = -1.0) {
        active_ = nullptr;
        decision_ = {};
        if (!enabled) {
            ceiling_ = 0;
            reduced_detail_ = false;
            previous_present_s_ = 0.0;
            pressure_ = 0.0f;
            decision_.pixel_percent = ceiling;
            return ceiling;
        }
        if (ceiling_ != ceiling) {
            ceiling_ = ceiling;
            // Start at full requested ceiling (100% native when Balanced) and step down only if sustained GPU/thermal pressure demands it.
            pixels_ = ceiling_;
            start_frame_ = frame;
            gpu_ema_ms_ = 11.0;
            gpu_peak_ms_ = 12.0;
            pressure_ = 0.0f;
            last_resize_s_ = now;
            last_eval_s_ = now;
            reset_samples(now);
        }

        if (!queries_[0].begin) {
            for (auto &query : queries_) {
                GLuint ids[2]{};
                glGenQueries(2, ids);
                query.begin = ids[0];
                query.end = ids[1];
            }
        }
        if (frame % 4 == 0) {
            Query &query = queries_[(frame / 4) % queries_.size()];
            if (query.pending) {
                GLint ready = GL_FALSE;
                glGetQueryObjectiv(query.end, GL_QUERY_RESULT_AVAILABLE, &ready);
                if (ready) {
                    GLuint64 begin = 0, end = 0;
                    glGetQueryObjectui64v(query.begin, GL_QUERY_RESULT, &begin);
                    glGetQueryObjectui64v(query.end, GL_QUERY_RESULT, &end);
                    if (query.generation == generation_ && end > begin) {
                        const double ms = std::clamp((end - begin) * 1.0e-6, 0.2, 60.0);
                        const double alpha = (ms > gpu_ema_ms_) ? 0.35 : 0.14;
                        gpu_ema_ms_ = gpu_ema_ms_ + (ms - gpu_ema_ms_) * alpha;
                        gpu_peak_ms_ = std::max(ms, gpu_peak_ms_ * 0.90);
                    }
                    query.pending = false;
                }
            }
            if (!query.pending) {
                query.generation = generation_;
                query.pending = true;
                active_ = &query;
                glQueryCounter(query.begin, GL_TIMESTAMP);
            }
        }

        const float thermal_p = (package_temp_c > 0.0)
            ? std::clamp(static_cast<float>((package_temp_c - 82.0) / 9.0), 0.0f, 1.0f)
            : 0.0f;
        const double soft_budget_ms = 14.9 - thermal_p * 2.6;
        const double recover_budget_ms = soft_budget_ms - 1.4;

        if ((frame - start_frame_) >= 24 && (now - last_eval_s_) >= 0.25) {
            last_eval_s_ = now;
            const double gpu_excess = std::clamp((gpu_ema_ms_ - soft_budget_ms) / 2.5, -0.35, 1.0);
            const double cpu_excess = std::clamp((cpu_work_ms - 7.0) / 5.0, 0.0, 0.65);
            const double miss_excess = (missed_ >= 4) ? std::clamp(missed_ / 12.0, 0.0, 0.48) : 0.0;
            const double peak_bump = (gpu_peak_ms_ > 16.5) ? 0.10 : 0.0;

            float raw_p = static_cast<float>(std::clamp(
                gpu_excess * 0.85 + thermal_p * 0.72 + miss_excess + cpu_excess + peak_bump, 0.0, 1.0));
            if (gpu_ema_ms_ < recover_budget_ms && missed_ <= 2 && thermal_p < 0.15f && cpu_work_ms < 6.0) {
                raw_p = 0.0f;
            }
            const float rate = (raw_p > pressure_) ? 0.30f : 0.20f;
            pressure_ = std::clamp(pressure_ + (raw_p - pressure_) * rate, 0.0f, 1.0f);

            // Protect full-res rendering first via Tier-1 zero-realloc scaling; scale down to 44% (720p+) or 36% under thermal load to protect 60 FPS.
            const int min_pix = (thermal_p >= 0.75f) ? std::min(36, ceiling_) : std::min(44, ceiling_);
            const double down_cooldown_s = (frame - start_frame_) < 180 ? 0.50 : 2.0;
            if ((now - last_resize_s_) >= down_cooldown_s &&
                (pressure_ >= 0.58f || consec_missed_ >= 4 || missed_ >= 8 || gpu_ema_ms_ > soft_budget_ms + 0.9) &&
                pixels_ > min_pix) {
                const int step = (pixels_ >= 76 && pressure_ >= 0.55f) ? 14 : ((pixels_ >= 60) ? 10 : 6);
                pixels_ = std::max(min_pix, pixels_ - step);
                last_resize_s_ = now;
                reset_samples(now);
            } else if ((now - last_resize_s_) >= 2.2 && pressure_ <= 0.20f && gpu_ema_ms_ < recover_budget_ms &&
                       missed_ <= 2 && thermal_p < 0.20f && pixels_ < ceiling_) {
                const int step = (pixels_ >= 76) ? 8 : 6;
                pixels_ = std::min(ceiling_, pixels_ + step);
                last_resize_s_ = now;
                reset_samples(now);
            }

            if (!reduced_detail_ && ((pressure_ >= 0.85f && pixels_ <= min_pix) || thermal_p >= 0.85f)) {
                reduced_detail_ = true;
                reset_samples(now);
            } else if (reduced_detail_ && pressure_ <= 0.45f && thermal_p <= 0.45f &&
                       (now - last_change_s_) >= 3.0) {
                reduced_detail_ = false;
                reset_samples(now);
            }
        }

        auto s01 = [](float e0, float e1, float x) {
            const float t = std::clamp((x - e0) / std::max(1e-4f, e1 - e0), 0.0f, 1.0f);
            return t * t * (3.0f - 2.0f * t);
        };
        decision_.pixel_percent = pixels_;
        decision_.load_pressure = pressure_;
        decision_.cloud_step_scale = 1.0f - 0.22f * s01(0.06f, 0.62f, pressure_);
        decision_.terrain_lod_scale = 1.0f - 0.18f * s01(0.10f, 0.68f, pressure_);
        decision_.city_dist_scale = 1.0f - 0.26f * s01(0.12f, 0.75f, pressure_);
        decision_.veg_density_scale = 1.0f - 0.40f * s01(0.15f, 0.78f, pressure_);
        decision_.reduce_distant_detail = reduced_detail_;
        decision_.limit_terrain_workers = reduced_detail_ || thermal_p >= 0.65f || cpu_work_ms > 8.0;
        decision_.gpu_ema_ms = gpu_ema_ms_;

        if (reduced_detail_ || cpu_work_ms > 7.5 || pressure_ > 0.70f) {
            decision_.max_uploads = 1;
            decision_.upload_budget_ms = 0.40;
        } else if (queued_jobs > 12 && pressure_ < 0.35f && gpu_ema_ms_ < soft_budget_ms - 0.6 && cpu_work_ms < 6.0) {
            decision_.max_uploads = 3;
            decision_.upload_budget_ms = 1.25;
        } else if (queued_jobs > 4 && pressure_ < 0.55f && gpu_ema_ms_ < soft_budget_ms) {
            decision_.max_uploads = 2;
            decision_.upload_budget_ms = 0.85;
        } else {
            decision_.max_uploads = 1;
            decision_.upload_budget_ms = 0.58;
        }
        return pixels_;
    }

    void finish() {
        if (active_) glQueryCounter(active_->end, GL_TIMESTAMP);
        active_ = nullptr;
    }

    void presented(double now) {
        if (ceiling_ == 0) return;
        const bool first_present = (previous_present_s_ <= 0.0);
        const double interval = now - previous_present_s_;
        previous_present_s_ = now;
        if (first_present || interval > 0.25) {
            missed_ = 0;
            consec_missed_ = 0;
        } else if (interval > 0.0197) {
            ++consec_missed_;
            missed_ = std::min(missed_ + (consec_missed_ >= 2 ? 2 : 1), 16);
        } else {
            consec_missed_ = 0;
            if (missed_ > 0) --missed_;
        }
    }

    bool reduce_distant_detail() const { return reduced_detail_; }
    const FlightBudgetDecision &decision() const { return decision_; }

private:
    void reset_samples(double now) {
        ++generation_;
        missed_ = consec_missed_ = 0;
        last_change_s_ = now;
    }

    struct Query {
        GLuint begin = 0, end = 0;
        uint64_t generation = 0;
        bool pending = false;
    };
    std::array<Query, 4> queries_{};
    Query *active_ = nullptr;
    uint64_t generation_ = 0;
    uint64_t start_frame_ = 0;
    double last_change_s_ = 0.0, last_resize_s_ = 0.0, last_eval_s_ = 0.0, previous_present_s_ = 0.0;
    double gpu_ema_ms_ = 10.5, gpu_peak_ms_ = 11.5;
    float pressure_ = 0.0f;
    int ceiling_ = 0, pixels_ = 0, missed_ = 0, consec_missed_ = 0;
    bool reduced_detail_ = false;
    FlightBudgetDecision decision_{};
};

} // namespace nsim
