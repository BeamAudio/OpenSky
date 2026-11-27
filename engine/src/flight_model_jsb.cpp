#include "flight_model_jsb.hpp"

#include <FGFDMExec.h>
#include <initialization/FGInitialCondition.h>
#include <initialization/FGTrim.h>
#include <input_output/FGGroundCallback.h>
#include <models/FGAtmosphere.h>
#include <models/FGAuxiliary.h>
#include <models/FGFCS.h>
#include <models/FGGroundReactions.h>
#include <models/FGInertial.h>
#include <models/FGMassBalance.h>
#include <models/FGPropagate.h>
#include <models/FGPropulsion.h>
#include <models/atmosphere/FGStandardAtmosphere.h>
#include <models/atmosphere/FGWinds.h>
#include <models/propulsion/FGPiston.h>
#include <models/propulsion/FGPropeller.h>
#include <models/propulsion/FGTank.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <numbers>

namespace nspain {
namespace {

constexpr double ft_to_m = 0.3048;
constexpr double rad_to_deg = 180.0 / std::numbers::pi;
constexpr double lbf_to_n = 4.4482216153;
constexpr double slug_ft3_to_kg_m3 = 515.3788184;
constexpr double ft_lbf_per_sec_per_hp = 550.0;
constexpr std::array<const char *, 3> gust_properties = {
    "opensky/weather/p-gust-rad-s", "opensky/weather/q-gust-rad-s", "opensky/weather/r-gust-rad-s"
};

class TerrainCallback final : public JSBSim::FGDefaultGroundCallback {
public:
    TerrainCallback(const FlightModel::GroundProvider &provider, const FlightEnvironment &env,
                    JSBSim::FGGroundReactions &reactions, double semimajor, double semiminor)
        : FGDefaultGroundCallback(semimajor, semiminor), provider_(provider), env_(env), reactions_(reactions),
          semimajor_(semimajor), semiminor_(semiminor) {}

    void SetEllipse(double semimajor, double semiminor) override {
        FGDefaultGroundCallback::SetEllipse(semimajor, semiminor);
        semimajor_ = semimajor;
        semiminor_ = semiminor;
    }

    double GetAGLevel(double, const JSBSim::FGLocation &loc, JSBSim::FGLocation &contact,
                      JSBSim::FGColumnVector3 &normal, JSBSim::FGColumnVector3 &vel,
                      JSBSim::FGColumnVector3 &rotation) const override {
        JSBSim::FGLocation aircraft = loc;
        aircraft.SetEllipse(semimajor_, semiminor_);
        FlightGround ground{env_.terrain_elev_m};
        // Airborne substeps can reuse the surface height sampled at the aircraft.
        // Close to the ground, resolve every contact at its own runway/DEM point.
        if (provider_ && aircraft.GetGeodAltitude() * ft_to_m - env_.terrain_elev_m < 60.0)
            ground = provider_(aircraft.GetGeodLatitudeDeg(), aircraft.GetLongitudeDeg());

        contact.SetEllipse(semimajor_, semiminor_);
        contact.SetPositionGeodetic(aircraft.GetLongitude(), aircraft.GetGeodLatitudeRad(), ground.elev_m / ft_to_m);
        normal = contact.GetTl2ec() * JSBSim::FGColumnVector3(0, 0, -1);
        vel.InitMatrix();
        rotation.InitMatrix();
        reactions_.SetStaticFFactor(ground.friction);
        reactions_.SetRollingFFactor(1.0 + ground.roughness);
        // Scale JSBSim's 0.4 ft max bump amplitude to realistic turf ripples rather than wheel-stopping potholes.
        reactions_.SetBumpiness(ground.roughness * 0.25);
        reactions_.SetSolid(ground.solid);
        const double ecef[3] = {aircraft(1), aircraft(2), aircraft(3)};
        reactions_.SetPosition(ecef);
        return aircraft.GetGeodAltitude() - ground.elev_m / ft_to_m;
    }

private:
    const FlightModel::GroundProvider &provider_;
    const FlightEnvironment &env_;
    JSBSim::FGGroundReactions &reactions_;
    double semimajor_, semiminor_;
};

} // namespace

class FlightModel::Impl {
public:
    JSBSim::FGFDMExec exec;
    JSBSim::FGPiston *piston = nullptr;
    JSBSim::FGPropeller *prop = nullptr;
    bool engine_permitted = false;
};

FlightModel::FlightModel() = default;
FlightModel::~FlightModel() = default;

const std::string &FlightModel::version() {
    return JSBSim::FGJSBBase::GetVersion();
}

bool FlightModel::load(const std::string &aircraft_dir) {
    const auto dir = std::filesystem::absolute(aircraft_dir).lexically_normal();
    auto next = std::make_unique<Impl>();
    auto &exec = next->exec;
    exec.SetDebugLevel(0);
    exec.Setdt(step_sec);
    try {
        if (!exec.LoadModel(SGPath(dir.string()), SGPath((dir / "engine").string()),
                            SGPath(dir.string()), dir.filename().string(), false)) {
            std::fprintf(stderr, "[JSBSIM] Failed to load aircraft from %s\n", dir.string().c_str());
            return false;
        }

        const auto propulsion = exec.GetPropulsion();
        const auto reactions = exec.GetGroundReactions();
        if (propulsion->GetNumEngines() != 1 || propulsion->GetNumTanks() != 2 || reactions->GetNumGearUnits() < 3) {
            std::fprintf(stderr, "[JSBSIM] Aircraft package needs one piston engine, two fuel tanks and at least three gear units\n");
            return false;
        }
        next->piston = dynamic_cast<JSBSim::FGPiston *>(propulsion->GetEngine(0).get());
        if (next->piston) next->prop = dynamic_cast<JSBSim::FGPropeller *>(next->piston->GetThruster());
        if (!next->prop) {
            std::fprintf(stderr, "[JSBSIM] Aircraft package needs a piston engine with a propeller\n");
            return false;
        }
        for (int i = 0; i < reactions->GetNumGearUnits(); ++i) reactions->GetGearUnit(i)->SetReport(false);
        const auto inertial = exec.GetInertial();
        exec.GetInertial()->SetGroundCallback(new TerrainCallback(
            ground_, env_, *reactions, inertial->GetSemimajor(), inertial->GetSemiminor()));
        exec.GetWinds()->SetTurbType(JSBSim::FGWinds::ttNone);
        propulsion->SetFuelFreeze(true);
        impl_ = std::move(next);
        return true;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "[JSBSIM] %s\n", error.what());
        return false;
    }
}

void FlightModel::set_ground_provider(GroundProvider provider) {
    ground_ = std::move(provider);
}

void FlightModel::apply_controls() {
    const auto fcs = impl_->exec.GetFCS();
    fcs->SetDaCmd(std::clamp(controls_.aileron + neutral_aileron_, -1.0, 1.0));
    fcs->SetDeCmd(-std::clamp(controls_.elevator, -1.0, 1.0));
    fcs->SetDrCmd(std::clamp(controls_.rudder + neutral_rudder_, -1.0, 1.0));
    fcs->SetDsCmd(std::clamp(controls_.rudder, -1.0, 1.0));
    fcs->SetPitchTrimCmd(-std::clamp(controls_.elevator_trim, -1.0, 1.0));
    // Mechanical idle stop keeps a running IO-360 idling smoothly when throttle is closed
    // (shutdown requires mixture idle-cutoff, magnetos OFF, or fuel starvation).
    constexpr double kIdleThrottleStop = 0.032;
    const double raw_throttle = std::clamp(controls_.throttle, 0.0, 1.0);
    const double eff_throttle = impl_->engine_permitted
        ? (kIdleThrottleStop + (1.0 - kIdleThrottleStop) * raw_throttle)
        : raw_throttle;
    fcs->SetThrottleCmd(0, eff_throttle * std::clamp(engine_.power_multiplier, 0.0, 1.0));
    fcs->SetMixtureCmd(0, std::clamp(controls_.mixture, 0.0, 1.0));
    fcs->SetDfCmd(std::clamp(controls_.flaps, 0.0, 1.0));
    fcs->SetLBrake(std::clamp(controls_.brake_left, 0.0, 1.0));
    fcs->SetRBrake(std::clamp(controls_.brake_right, 0.0, 1.0));
}

void FlightModel::apply_environment(double altitude_m) {
    auto &exec = impl_->exec;
    const auto atmosphere = exec.GetAtmosphere();
    atmosphere->SetPressureSL(JSBSim::FGAtmosphere::eMillibars, std::clamp(env_.qnh_hpa, 850.0, 1100.0));
    atmosphere->SetTemperature(std::clamp(env_.air_temp_c, -75.0, 60.0), altitude_m / ft_to_m,
                               JSBSim::FGAtmosphere::eCelsius);
    if (auto *standard = dynamic_cast<JSBSim::FGStandardAtmosphere *>(atmosphere.get()))
        standard->SetRelativeHumidity(std::clamp(env_.relative_humidity, 0.0, 1.0) * 100.0);
    exec.GetWinds()->SetWindNED(env_.wind_north_mps / ft_to_m, env_.wind_east_mps / ft_to_m,
                                env_.wind_down_mps / ft_to_m);
    const double gust[] = {env_.p_turb_rps, env_.q_turb_rps, env_.r_turb_rps};
    for (size_t i = 0; i < gust_properties.size(); ++i) exec.SetPropertyValue(gust_properties[i], gust[i]);
    exec.SetPropertyValue("opensky/weather/ice-factor", std::clamp(env_.airframe_ice_factor, 0.0, 1.0));
    exec.SetPropertyValue("opensky/weather/rain-drag", std::clamp(env_.rain_drag_factor, 0.0, 1.0));
    exec.SetPropertyValue("inertia/pointmass-weight-lbs[3]", std::clamp(env_.airframe_ice_kg, 0.0, 35.0) * 2.2046226);
}

void FlightModel::apply_engine() {
    const auto propulsion = impl_->exec.GetPropulsion();
    // The systems bus owns fuel selection and consumption. JSBSim uses the
    // same tank contents for its mass, inertia and CG calculations.
    propulsion->GetTank(0)->SetContents(engine_.left_fuel_gal * 6.0);
    propulsion->GetTank(1)->SetContents(engine_.right_fuel_gal * 6.0);
    const bool permit = engine_.running && !engine_.fuel_starved && engine_.magnetos != 0;
    impl_->piston->SetStarved(engine_.fuel_starved);
    if (permit && !impl_->piston->GetRunning()) propulsion->InitRunning(0);
    if (!permit && !engine_.starter) impl_->piston->SetRunning(false);
    propulsion->SetMagnetos(engine_.magnetos == 4 ? 3 : engine_.magnetos);
    propulsion->SetStarter(engine_.starter);
    impl_->engine_permitted = permit;
}

bool FlightModel::initialize(double lat_deg, double lon_deg, double altitude_m, double heading_deg, double cas_kt, bool on_ground) {
    if (!impl_) return false;
    accumulator_ = 0;
    neutral_aileron_ = neutral_rudder_ = 0;
    auto &exec = impl_->exec;
    const FlightControls requested = controls_;
    auto reset = [&]() {
        exec.ResetToInitialConditions(JSBSim::FGFDMExec::DONT_EXECUTE_RUN_IC);
        exec.GetPropulsion()->SetFuelFreeze(true);
        exec.GetWinds()->SetTurbType(JSBSim::FGWinds::ttNone);
        impl_->engine_permitted = false;
        const auto ic = exec.GetIC();
        ic->SetGeodLatitudeDegIC(lat_deg);
        ic->SetLongitudeDegIC(lon_deg);
        ic->SetAltitudeASLFtIC(altitude_m / ft_to_m);
        ic->SetPhiDegIC(0);
        ic->SetThetaDegIC(on_ground ? 0 : 3.0);
        ic->SetPsiDegIC(heading_deg);
        ic->SetWindNEDFpsIC(env_.wind_north_mps / ft_to_m, env_.wind_east_mps / ft_to_m,
                            env_.wind_down_mps / ft_to_m);
        apply_environment(altitude_m);
        if (on_ground) {
            const double heading_rad = heading_deg / rad_to_deg;
            const double speed_fps = cas_kt * 1852.0 / 3600.0 / ft_to_m;
            ic->SetVNorthFpsIC(speed_fps * std::cos(heading_rad) + env_.wind_north_mps / ft_to_m);
            ic->SetVEastFpsIC(speed_fps * std::sin(heading_rad) + env_.wind_east_mps / ft_to_m);
            ic->SetVDownFpsIC(env_.wind_down_mps / ft_to_m);
        } else {
            ic->SetVcalibratedKtsIC(std::max(5.0, cas_kt));
            ic->SetClimbRateFpmIC(0);
        }
        apply_engine();
        apply_controls();
        return exec.RunIC();
    };

    try {
        if (!reset()) return false;
        if (!on_ground && engine_.running && !engine_.fuel_starved && cas_kt >= 65) {
            JSBSim::FGTrim trim(&exec, JSBSim::tFull);
            if (trim.DoTrim()) {
                const auto fcs = exec.GetFCS();
                constexpr double kIdleThrottleStop = 0.032;
                const double trimmed_cmd = fcs->GetThrottleCmd(0);
                controls_.throttle = impl_->engine_permitted
                    ? std::clamp((trimmed_cmd - kIdleThrottleStop) / (1.0 - kIdleThrottleStop), 0.0, 1.0)
                    : std::clamp(trimmed_cmd, 0.0, 1.0);
                controls_.elevator_trim = -fcs->GetPitchTrimCmd();
                controls_.elevator = -fcs->GetDeCmd();
                neutral_aileron_ = fcs->GetDaCmd();
                neutral_rudder_ = fcs->GetDrCmd();
                controls_.aileron = controls_.rudder = 0;
            } else {
                controls_ = requested;
                if (!reset()) return false;
                std::fprintf(stderr, "[JSBSIM] Spawn trim did not converge at %.0f kt, %.0f m\n", cas_kt, altitude_m);
            }
        }
        if (on_ground && cas_kt < 1.0) {
            // Settle gear springs calmly onto terrain/runway at spawn so wheels never clip or bounce.
            for (int i = 0; i < 30; ++i) {
                if (!exec.Run()) break;
            }
        }
        read_state();
        return std::isfinite(state_.alt_msl_m) && std::isfinite(state_.ias_kt);
    } catch (const std::exception &error) {
        std::fprintf(stderr, "[JSBSIM] Initial conditions failed: %s\n", error.what());
        return false;
    }
}

bool FlightModel::advance(double dt) {
    if (!impl_ || !std::isfinite(dt) || dt < 0) return false;
    accumulator_ += std::min(dt, 0.05);
    if (accumulator_ + 1.0e-10 < step_sec) return true;
    apply_environment(state_.alt_msl_m);
    apply_engine();
    apply_controls();
    while (accumulator_ + 1.0e-10 >= step_sec) {
        if (!impl_->exec.Run()) {
            std::fprintf(stderr, "[JSBSIM] Executive ended at %.3f s\n", impl_->exec.GetSimTime());
            return false;
        }
        accumulator_ -= step_sec;
    }
    read_state();
    const bool valid = std::isfinite(state_.alt_msl_m) && std::isfinite(state_.ias_kt) && std::isfinite(state_.roll_deg);
    if (!valid)
        std::fprintf(stderr, "[JSBSIM] Invalid state at %.3f s: lat/lon %.7f %.7f, alt %.2f m, AGL %.2f m, CAS %.2f kt, roll %.2f, RPM %.0f\n",
                     impl_->exec.GetSimTime(), state_.lat_deg, state_.lon_deg, state_.alt_msl_m,
                     state_.agl_m, state_.ias_kt, state_.roll_deg, state_.rpm);
    return valid;
}

void FlightModel::apply_impact_impulse(
    double push_east_m, double push_up_m, double push_north_m,
    double vel_east_mps, double vel_up_mps, double vel_north_mps,
    double p_rps, double q_rps, double r_rps
) {
    if (!impl_) return;
    auto propagate = impl_->exec.GetPropagate();
    const double lat_rad = state_.lat_deg / rad_to_deg;
    const double m_lat = 111132.92 - 559.82 * std::cos(2.0 * lat_rad);
    const double m_lon = std::max(1000.0, 111412.84 * std::cos(lat_rad) - 93.5 * std::cos(3.0 * lat_rad));

    if (!std::isfinite(push_east_m) || !std::isfinite(push_up_m) || !std::isfinite(push_north_m) ||
        !std::isfinite(vel_east_mps) || !std::isfinite(vel_up_mps) || !std::isfinite(vel_north_mps) ||
        !std::isfinite(p_rps) || !std::isfinite(q_rps) || !std::isfinite(r_rps)) return;

    // Copy current FGLocation so planet ellipsoid parameters (mEllipseSet) stay intact.
    const double cur_gc_lat = propagate->GetLatitudeDeg();
    const double cur_gc_lon = propagate->GetLongitudeDeg();
    const double cur_asl_m  = propagate->GetAltitudeASLmeters();
    JSBSim::FGLocation loc = propagate->GetLocation();
    loc.SetPosition((cur_gc_lon + push_east_m / m_lon) / rad_to_deg,
                    (cur_gc_lat + push_north_m / m_lat) / rad_to_deg,
                    loc.GetSeaLevelRadius() + (cur_asl_m + push_up_m) / ft_to_m);
    propagate->SetLocation(loc);

    const JSBSim::FGColumnVector3 v_ned_fps(vel_north_mps / ft_to_m, vel_east_mps / ft_to_m, -vel_up_mps / ft_to_m);
    const JSBSim::FGColumnVector3 v_body_fps = propagate->GetTl2b() * v_ned_fps;
    propagate->SetUVW(1, v_body_fps(1));
    propagate->SetUVW(2, v_body_fps(2));
    propagate->SetUVW(3, v_body_fps(3));
    propagate->SetPQR(1, p_rps);
    propagate->SetPQR(2, q_rps);
    propagate->SetPQR(3, r_rps);
    read_state();
}

void FlightModel::read_state() {
    auto &exec = impl_->exec;
    const auto propagate = exec.GetPropagate();
    const auto aux = exec.GetAuxiliary();
    const auto atmo = exec.GetAtmosphere();
    const auto reactions = exec.GetGroundReactions();
    const auto &position = propagate->GetLocation();
    state_.lat_deg = position.GetGeodLatitudeDeg();
    state_.lon_deg = position.GetLongitudeDeg();
    state_.alt_msl_m = propagate->GetAltitudeASLmeters();
    state_.agl_m = propagate->GetDistanceAGL() * ft_to_m;
    state_.roll_deg = propagate->GetEulerDeg(1);
    state_.pitch_deg = propagate->GetEulerDeg(2);
    state_.heading_deg = std::fmod(propagate->GetEulerDeg(3) + 360.0, 360.0);
    state_.vel_north_mps = propagate->GetVel(1) * ft_to_m;
    state_.vel_east_mps = propagate->GetVel(2) * ft_to_m;
    state_.vsi_mps = -propagate->GetVel(3) * ft_to_m;
    state_.ground_speed_mps = std::hypot(state_.vel_north_mps, state_.vel_east_mps);
    state_.air_north_mps = state_.vel_north_mps - env_.wind_north_mps;
    state_.air_east_mps = state_.vel_east_mps - env_.wind_east_mps;
    state_.air_up_mps = state_.vsi_mps + env_.wind_down_mps;
    state_.ias_kt = aux->GetVcalibratedKTS();
    state_.tas_kt = aux->GetVtrueKTS();
    state_.p_rps = propagate->GetPQR(1);
    state_.q_rps = propagate->GetPQR(2);
    state_.r_rps = propagate->GetPQR(3);
    state_.alpha_deg = aux->Getalpha() * rad_to_deg;
    state_.beta_deg = aux->Getbeta() * rad_to_deg;
    state_.cl = exec.GetPropertyValue("opensky/aero/CL");
    state_.cd = exec.GetPropertyValue("opensky/aero/CD");
    state_.g_load = aux->GetNlf();
    state_.air_density_kg_m3 = atmo->GetDensity() * slug_ft3_to_kg_m3;
    state_.density_alt_ft = atmo->GetDensityAltitude();
    state_.weight_lb = exec.GetMassBalance()->GetWeight();
    state_.rpm = impl_->prop->GetRPM();
    state_.thrust_n = impl_->piston->GetThrust() * lbf_to_n;
    state_.shaft_hp = impl_->piston->GetPowerAvailable() / ft_lbf_per_sec_per_hp;
    state_.fuel_flow_gph = impl_->piston->GetFuelFlowRateGPH();
    state_.engine_running = impl_->piston->GetRunning();
    state_.on_ground = false;
    for (size_t i = 0; i < state_.wheel_wow.size(); ++i) {
        const auto wheel = reactions->GetGearUnit(static_cast<int>(i));
        state_.wheel_wow[i] = wheel->GetWOW();
        state_.on_ground |= state_.wheel_wow[i];
        state_.wheel_comp_m[i] = state_.wheel_wow[i] ? wheel->GetCompLen() * ft_to_m : 0;
        for (int axis = 0; axis < 3; ++axis)
            state_.wheel_offset_m[i][axis] = wheel->GetBodyLocation(axis + 1) * ft_to_m;
    }
    const double stall_deg = 15.5 - 2.5 * controls_.flaps;
    state_.stalled = !state_.on_ground && state_.alpha_deg >= stall_deg;
    state_.stall_warn = !state_.on_ground && state_.alpha_deg >= stall_deg - 3.5;
}

} // namespace nspain
