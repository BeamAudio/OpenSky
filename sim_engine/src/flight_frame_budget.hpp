#pragma once

#include "gl_loader.hpp"

#include <algorithm>
#include <array>
#include <cstdint>

namespace nsim {

// A 60 Hz frame lasts 16.67 ms. Leave room for physics, uploads, the driver,
// and the compositor even in a scene that could just meet the deadline.
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

    int start(bool enabled, int ceiling, double now, uint64_t frame) {
        active_ = nullptr;
        if (!enabled) {
            ceiling_ = 0;
            reduced_detail_ = false;
            previous_present_s_ = 0.0;
            return ceiling;
        }
        if (ceiling_ != ceiling) {
            ceiling_ = ceiling;
            pixels_ = std::min(40, ceiling_);
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
        if (frame % 8 != 0) return pixels_;
        Query &query = queries_[(frame / 8) % queries_.size()];
        if (query.pending) {
            GLint ready = GL_FALSE;
            glGetQueryObjectiv(query.end, GL_QUERY_RESULT_AVAILABLE, &ready);
            if (!ready) return pixels_;
            GLuint64 begin = 0, end = 0;
            glGetQueryObjectui64v(query.begin, GL_QUERY_RESULT, &begin);
            glGetQueryObjectui64v(query.end, GL_QUERY_RESULT, &end);
            if (query.generation == generation_ && end > begin) {
                const double ms = (end - begin) * 1.0e-6;
                slow_ = ms > 12.5 ? slow_ + 1 : 0;
                const double recovery_ms = reduced_detail_ ? 10.5 : 9.0;
                fast_ = ms < recovery_ms ? fast_ + 1 : 0;
            }
        }
        // Reallocation clears temporal history. Change the pixel budget at
        // spaced intervals and recover quality slower than we reduce it.
        const bool under_pressure = slow_ >= 4 || missed_ >= 6;
        if (now - last_change_s_ >= 1.5 && under_pressure && pixels_ > 35) {
            pixels_ = std::max(35, pixels_ - 4);
            reset_samples(now);
        } else if (now - last_change_s_ >= 1.5 && pixels_ == 35 && under_pressure && !reduced_detail_) {
            reduced_detail_ = true;
            reset_samples(now);
        } else if (missed_ == 0 && now - last_change_s_ >= 6.0 && fast_ >= 36 && (reduced_detail_ || pixels_ < ceiling_)) {
            if (reduced_detail_) reduced_detail_ = false;
            else pixels_ = std::min(ceiling_, pixels_ + 4);
            reset_samples(now);
        }
        query.generation = generation_;
        query.pending = true;
        active_ = &query;
        glQueryCounter(query.begin, GL_TIMESTAMP);
        return pixels_;
    }

    void finish() {
        if (active_) glQueryCounter(active_->end, GL_TIMESTAMP);
        active_ = nullptr;
    }

    void presented(double now) {
        if (ceiling_ == 0) return;
        const double interval = now - previous_present_s_;
        previous_present_s_ = now;
        if (interval > 0.25) missed_ = 0; // Menu, loading or capture gap.
        else if (interval > 0.0185) missed_ = std::min(missed_ + 2, 16);
        else if (missed_ > 0) --missed_;
    }

    bool reduce_distant_detail() const { return reduced_detail_; }

private:
    void reset_samples(double now) {
        ++generation_;
        slow_ = fast_ = missed_ = 0;
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
    double last_change_s_ = 0.0, previous_present_s_ = 0.0;
    int ceiling_ = 0, pixels_ = 0, slow_ = 0, fast_ = 0, missed_ = 0;
    bool reduced_detail_ = false;
};

} // namespace nsim
