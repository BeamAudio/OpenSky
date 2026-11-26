#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace nspain {

struct YokeConfig {
    double deadzone = 0.05;     // 0.00..0.25 normalized radial/axis deadzone
    double curve = 0.35;        // 0.00..0.85 cubic S-curve blending weight
    bool invert_roll = false;
    bool invert_pitch = false;
    bool invert_rudder = false;
    bool invert_throttle = false;
    bool direct_control = true; // true: direct C172S aerodynamic surfaces; false: flight-assist envelope
};

struct YokeState {
    bool connected = false;
    bool has_yoke = false;      // Physical roll + pitch axes
    bool has_rudder = false;    // Physical yaw pedals or twist grip
    bool has_throttle = false;
    bool has_mixture = false;
    bool has_brakes = false;    // Differential toe brakes or analog brake axes
    std::string primary_name;
    std::string device_summary;
    int device_count = 0;

    double roll = 0.0;          // [-1, +1] (+right aileron)
    double pitch = 0.0;         // [-1, +1] (+pull back / nose up)
    double rudder = 0.0;        // [-1, +1] (+right yaw)
    double throttle = 0.0;      // [0, 1]
    double mixture = 1.0;       // [0, 1]
    double brake_left = 0.0;    // [0, 1]
    double brake_right = 0.0;   // [0, 1]
    double trim_rate = 0.0;     // [-1, +1] (-nose down, +nose up)
    double hat_x = 0.0;         // [-1, +1] POV view pan
    double hat_y = 0.0;         // [-1, +1] POV view tilt

    bool brake_held = false;
    bool parking_brake_pressed = false;
    bool flaps_down_pressed = false;
    bool flaps_up_pressed = false;
    bool recenter_pressed = false;
    bool ap_disc_pressed = false;
};

enum class YokeDeviceRole : uint8_t {
    YokeOrStick = 0,
    RudderPedals = 1,
    ThrottleQuadrant = 2,
};

struct YokeAxisCalib {
    bool present = false;
    int32_t min_val = -32767;
    int32_t max_val = 32767;
    int32_t value = 0;
    int32_t tare_center = 0;
    bool has_tare = false;
};

struct YokeDeviceState {
    static constexpr int kMaxAbs = 64;
    static constexpr int kMaxBtns = 16;

    std::string path;
    std::string name;
    YokeDeviceRole role = YokeDeviceRole::YokeOrStick;
    std::array<YokeAxisCalib, kMaxAbs> axes{};
    std::array<bool, kMaxBtns> buttons{};
    std::array<bool, kMaxBtns> prev_buttons{};

    int roll_axis = -1;
    int pitch_axis = -1;
    int rudder_axis = -1;
    int throttle_axis = -1;
    int mixture_axis = -1;
    int brake_left_axis = -1;
    int brake_right_axis = -1;
    int hat_x_axis = -1;
    int hat_y_axis = -1;

    static YokeDeviceRole classify_role(const std::string &dev_name, const std::array<YokeAxisCalib, kMaxAbs> &ax,
                                        bool has_joystick_btns, bool has_gamepad_btns);
    void assign_default_mapping(bool has_joystick_btns, bool has_gamepad_btns);
    void calibrate_center();
    void reset_calibration();
    static double normalize_bipolar(const YokeAxisCalib &a);
    static double normalize_unipolar(const YokeAxisCalib &a, bool invert = false);
};

class FlightYokeInput {
public:
    FlightYokeInput() = default;
    ~FlightYokeInput();

    FlightYokeInput(const FlightYokeInput &) = delete;
    FlightYokeInput &operator=(const FlightYokeInput &) = delete;

    void shutdown();
    void rescan();
    void calibrate_center();
    void reset_calibration();
    void poll(const YokeConfig &cfg);

    // Pure snapshot evaluation usable in both live runtime and headless unit tests.
    static YokeState evaluate_devices(std::vector<YokeDeviceState> &devices, const YokeConfig &cfg);
    static bool run_selftest();

    const YokeState &state() const { return state_; }
    const std::vector<YokeDeviceState> &devices() const { return devs_; }

private:
    struct OpenFd {
        int fd = -1;
        bool permission_warned = false;
    };
    void close_index(size_t i);

    std::vector<YokeDeviceState> devs_;
    std::vector<int> fds_;
    double next_scan_s_ = -1.0;
    YokeState state_{};
};

} // namespace nspain
