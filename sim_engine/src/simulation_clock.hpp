#pragma once

#include <algorithm>
#include "../../engine/src/flight_model_jsb.hpp"

namespace nsim {

// Controls are sampled once per event poll and held for the resulting JSBSim ticks.
class SimulationClock {
public:
    double consume(double wall_delta) {
        constexpr double step = nspain::FlightModel::step_sec;
        remainder_ += std::clamp(wall_delta, 0.0, 0.05);
        const auto ticks = static_cast<int>((remainder_ + 1e-10) / step);
        const double elapsed = ticks * step;
        remainder_ = std::max(0.0, remainder_ - elapsed);
        return elapsed;
    }
    void pause() { remainder_ = 0.0; }

private:
    double remainder_ = 0.0;
};

} // namespace nsim
