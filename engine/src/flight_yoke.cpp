#include "flight_yoke.hpp"
#include "opentrack_udp.hpp"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>

#ifdef __linux__
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace nspain {
namespace {

enum LogicalBtn : int {
    LBTN_BRAKE = 0,
    LBTN_RECENTER = 1,
    LBTN_TRIM_DN = 2,
    LBTN_TRIM_UP = 3,
    LBTN_FLAPS_DN = 4,
    LBTN_FLAPS_UP = 5,
    LBTN_PARK_BRK = 6,
    LBTN_AP_DISC = 7,
};

std::string to_lower_ascii(std::string s) {
    for (char &ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

bool contains_ci(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}

#ifdef __linux__
bool test_bit(const uint8_t *bits, int bit) {
    return (bits[bit / 8] & (1u << (bit % 8))) != 0;
}

int map_evdev_button(uint16_t code) {
    switch (code) {
        case BTN_TRIGGER:
        case BTN_SOUTH:
            return LBTN_BRAKE;
        case BTN_THUMB:
        case BTN_EAST:
            return LBTN_PARK_BRK;
        case BTN_THUMB2:
        case BTN_TL:
        case BTN_TRIGGER_HAPPY5:
            return LBTN_TRIM_DN;
        case BTN_TOP:
        case BTN_TR:
        case BTN_TRIGGER_HAPPY6:
            return LBTN_TRIM_UP;
        case BTN_TOP2:
        case BTN_WEST:
        case BTN_TRIGGER_HAPPY7:
            return LBTN_FLAPS_DN;
        case BTN_PINKIE:
        case BTN_NORTH:
        case BTN_TRIGGER_HAPPY8:
            return LBTN_FLAPS_UP;
        case BTN_BASE:
        case BTN_SELECT:
        case BTN_THUMBL:
            return LBTN_RECENTER;
        case BTN_BASE2:
        case BTN_START:
        case BTN_THUMBR:
            return LBTN_AP_DISC;
        default:
            return -1;
    }
}
#endif

} // namespace

YokeDeviceRole YokeDeviceState::classify_role(const std::string &dev_name,
                                             const std::array<YokeAxisCalib, kMaxAbs> &ax,
                                             bool has_joystick_btns, bool has_gamepad_btns) {
    const std::string low = to_lower_ascii(dev_name);
    if (contains_ci(low, "rudder") || contains_ci(low, "pedal")) {
        return YokeDeviceRole::RudderPedals;
    }
    if ((contains_ci(low, "throttle") || contains_ci(low, "quadrant")) &&
        !contains_ci(low, "yoke") && !contains_ci(low, "hotas") && !contains_ci(low, "stick")) {
        return YokeDeviceRole::ThrottleQuadrant;
    }
#ifdef ABS_RZ
    // Buttonless 3-axis rudder pedals often expose uncentered X/Y toe brakes at minimum + RZ rudder.
    const bool has_yaw = ax[ABS_RZ].present || ax[ABS_RUDDER].present;
    if (!has_joystick_btns && !has_gamepad_btns && has_yaw && ax[ABS_X].present && ax[ABS_Y].present) {
        const double nx = normalize_bipolar(ax[ABS_X]);
        const double ny = normalize_bipolar(ax[ABS_Y]);
        if (nx < -0.75 && ny < -0.75) return YokeDeviceRole::RudderPedals;
    }
#endif
    return YokeDeviceRole::YokeOrStick;
}

void YokeDeviceState::assign_default_mapping(bool has_joystick_btns, bool has_gamepad_btns) {
    roll_axis = pitch_axis = rudder_axis = throttle_axis = mixture_axis = -1;
    brake_left_axis = brake_right_axis = hat_x_axis = hat_y_axis = -1;
    role = classify_role(name, axes, has_joystick_btns, has_gamepad_btns);

#ifdef ABS_X
    auto has = [&](int c) { return c >= 0 && c < kMaxAbs && axes[static_cast<size_t>(c)].present; };
    if (role == YokeDeviceRole::RudderPedals) {
        if (has(ABS_RZ)) rudder_axis = ABS_RZ;
        else if (has(ABS_RUDDER)) rudder_axis = ABS_RUDDER;
        else if (has(ABS_Z)) rudder_axis = ABS_Z;
        if (has(ABS_X) && has(ABS_Y)) {
            const std::string low = to_lower_ascii(name);
            // Thrustmaster TFRP swaps right/left compared to Saitek/Logitech pedals.
            if (contains_ci(low, "thrustmaster") || contains_ci(low, "t-rudder") || contains_ci(low, "tfrp")) {
                brake_right_axis = ABS_X;
                brake_left_axis = ABS_Y;
            } else {
                brake_left_axis = ABS_X;
                brake_right_axis = ABS_Y;
            }
        }
        return;
    }

    if (role == YokeDeviceRole::ThrottleQuadrant) {
        if (has(ABS_THROTTLE)) throttle_axis = ABS_THROTTLE;
        else if (has(ABS_Z)) throttle_axis = ABS_Z;
        else if (has(ABS_X)) throttle_axis = ABS_X;
        if (has(ABS_RY)) mixture_axis = ABS_RY;
        else if (has(ABS_Y) && throttle_axis != ABS_Y) mixture_axis = ABS_Y;
        return;
    }

    if (has(ABS_X)) roll_axis = ABS_X;
    if (has(ABS_Y)) pitch_axis = ABS_Y;
    if (has(ABS_HAT0X)) hat_x_axis = ABS_HAT0X;
    if (has(ABS_HAT0Y)) hat_y_axis = ABS_HAT0Y;

    if (has_gamepad_btns && !has_joystick_btns && has(ABS_RX) && has(ABS_RY)) {
        // Dual-stick gamepad: left stick X/Y = roll/pitch, right stick X = rudder, triggers Z/RZ = brakes.
        rudder_axis = ABS_RX;
        if (has(ABS_Z)) brake_left_axis = ABS_Z;
        if (has(ABS_RZ)) brake_right_axis = ABS_RZ;
        return;
    }

    if (has(ABS_RZ)) rudder_axis = ABS_RZ;
    else if (has(ABS_RUDDER)) rudder_axis = ABS_RUDDER;

    if (has(ABS_THROTTLE)) throttle_axis = ABS_THROTTLE;
    else if (has(ABS_Z)) throttle_axis = ABS_Z;

    // Desktop flight yokes with levers (CH Flight Sim Yoke, Logitech/Saitek Yoke + Quadrant) expose mixture on ABS_RY.
    if (has(ABS_RY) && mixture_axis < 0) mixture_axis = ABS_RY;
#else
    (void)has_joystick_btns;
    (void)has_gamepad_btns;
#endif
}

double YokeDeviceState::normalize_bipolar(const YokeAxisCalib &a) {
    if (!a.present || a.max_val <= a.min_val) return 0.0;
    const double mid = a.has_tare
        ? static_cast<double>(a.tare_center)
        : 0.5 * (static_cast<double>(a.min_val) + static_cast<double>(a.max_val));
    const double v = static_cast<double>(a.value);
    if (v >= mid) {
        const double span_hi = std::max(1.0, static_cast<double>(a.max_val) - mid);
        return std::clamp((v - mid) / span_hi, 0.0, 1.0);
    }
    const double span_lo = std::max(1.0, mid - static_cast<double>(a.min_val));
    return std::clamp((v - mid) / span_lo, -1.0, 0.0);
}

double YokeDeviceState::normalize_unipolar(const YokeAxisCalib &a, bool invert) {
    if (!a.present || a.max_val <= a.min_val) return 0.0;
    const double span = static_cast<double>(a.max_val) - static_cast<double>(a.min_val);
    double u = std::clamp((static_cast<double>(a.value) - static_cast<double>(a.min_val)) / span, 0.0, 1.0);
    return invert ? (1.0 - u) : u;
}

void YokeDeviceState::calibrate_center() {
    auto tare_axis = [&](int code) {
        if (code < 0 || code >= kMaxAbs) return;
        auto &a = axes[static_cast<size_t>(code)];
        if (!a.present || a.max_val <= a.min_val) return;
        const double nominal = 0.5 * (static_cast<double>(a.min_val) + static_cast<double>(a.max_val));
        const double span = static_cast<double>(a.max_val) - static_cast<double>(a.min_val);
        // Reject runaway calibration if the user is holding the yoke near full lock.
        if (std::abs(static_cast<double>(a.value) - nominal) <= span * 0.28) {
            a.tare_center = a.value;
            a.has_tare = true;
        }
    };
    tare_axis(roll_axis);
    tare_axis(pitch_axis);
    tare_axis(rudder_axis);
}

void YokeDeviceState::reset_calibration() {
    for (auto &a : axes) a.has_tare = false;
}

YokeState FlightYokeInput::evaluate_devices(std::vector<YokeDeviceState> &devices, const YokeConfig &cfg) {
    YokeState st{};
    const double dz = std::clamp(cfg.deadzone, 0.0, 0.25);
    const double crv = std::clamp(cfg.curve, 0.0, 0.85);
    double best_yoke_mag = -1.0;

    for (auto &d : devices) {
        st.connected = true;
        ++st.device_count;
        if (!st.device_summary.empty()) st.device_summary += " + ";
        st.device_summary += d.name;

        if (d.roll_axis >= 0 && d.pitch_axis >= 0) {
            const double rx = OpenTrackUDP::apply_axis_curve(
                YokeDeviceState::normalize_bipolar(d.axes[static_cast<size_t>(d.roll_axis)]), dz, crv, cfg.invert_roll);
            const double ry = OpenTrackUDP::apply_axis_curve(
                YokeDeviceState::normalize_bipolar(d.axes[static_cast<size_t>(d.pitch_axis)]), dz, crv, cfg.invert_pitch);
            const double mag = std::hypot(rx, ry);
            if (!st.has_yoke || mag > best_yoke_mag) {
                st.has_yoke = true;
                st.primary_name = d.name;
                st.roll = rx;
                st.pitch = ry;
                best_yoke_mag = mag;
            }
        }
        if (d.rudder_axis >= 0) {
            const double rud = OpenTrackUDP::apply_axis_curve(
                YokeDeviceState::normalize_bipolar(d.axes[static_cast<size_t>(d.rudder_axis)]), dz, crv, cfg.invert_rudder);
            if (!st.has_rudder || d.role == YokeDeviceRole::RudderPedals || std::abs(rud) > std::abs(st.rudder)) {
                st.has_rudder = true;
                if (st.primary_name.empty()) st.primary_name = d.name;
                st.rudder = rud;
            }
        }
        if (d.throttle_axis >= 0) {
            const double thr = YokeDeviceState::normalize_unipolar(
                d.axes[static_cast<size_t>(d.throttle_axis)], cfg.invert_throttle);
            if (!st.has_throttle || d.role == YokeDeviceRole::ThrottleQuadrant) {
                st.has_throttle = true;
                if (st.primary_name.empty()) st.primary_name = d.name;
                st.throttle = thr;
            }
        }
        if (d.mixture_axis >= 0) {
            st.has_mixture = true;
            st.mixture = YokeDeviceState::normalize_unipolar(
                d.axes[static_cast<size_t>(d.mixture_axis)], cfg.invert_throttle);
        }
        if (d.brake_left_axis >= 0 || d.brake_right_axis >= 0) {
            st.has_brakes = true;
            auto shape_brk = [&](int ax) {
                if (ax < 0) return 0.0;
                const double raw = YokeDeviceState::normalize_unipolar(d.axes[static_cast<size_t>(ax)], false);
                return raw <= 0.06 ? 0.0 : std::clamp((raw - 0.06) / 0.94, 0.0, 1.0);
            };
            st.brake_left = std::max(st.brake_left, shape_brk(d.brake_left_axis));
            st.brake_right = std::max(st.brake_right, shape_brk(d.brake_right_axis));
        }
        if (d.hat_x_axis >= 0) st.hat_x += YokeDeviceState::normalize_bipolar(d.axes[static_cast<size_t>(d.hat_x_axis)]);
        if (d.hat_y_axis >= 0) st.hat_y += YokeDeviceState::normalize_bipolar(d.axes[static_cast<size_t>(d.hat_y_axis)]);

        auto edge = [&](int slot) { return d.buttons[slot] && !d.prev_buttons[slot]; };
        if (d.buttons[LBTN_BRAKE]) st.brake_held = true;
        if (edge(LBTN_PARK_BRK)) st.parking_brake_pressed = true;
        if (edge(LBTN_FLAPS_DN)) st.flaps_down_pressed = true;
        if (edge(LBTN_FLAPS_UP)) st.flaps_up_pressed = true;
        if (edge(LBTN_RECENTER)) st.recenter_pressed = true;
        if (edge(LBTN_AP_DISC)) st.ap_disc_pressed = true;
        st.trim_rate += (d.buttons[LBTN_TRIM_UP] ? 1.0 : 0.0) - (d.buttons[LBTN_TRIM_DN] ? 1.0 : 0.0);
        d.prev_buttons = d.buttons;
    }

    st.hat_x = std::clamp(st.hat_x, -1.0, 1.0);
    st.hat_y = std::clamp(st.hat_y, -1.0, 1.0);
    st.trim_rate = std::clamp(st.trim_rate, -1.0, 1.0);
    return st;
}

FlightYokeInput::~FlightYokeInput() {
    shutdown();
}

void FlightYokeInput::close_index(size_t i) {
#ifdef __linux__
    if (i < fds_.size() && fds_[i] >= 0) close(fds_[i]);
#endif
    if (i < devs_.size()) {
        std::printf("[YOKE] Disconnected %s (%s)\n", devs_[i].name.c_str(), devs_[i].path.c_str());
        devs_.erase(devs_.begin() + static_cast<ptrdiff_t>(i));
    }
    if (i < fds_.size()) fds_.erase(fds_.begin() + static_cast<ptrdiff_t>(i));
}

void FlightYokeInput::shutdown() {
#ifdef __linux__
    for (int fd : fds_) if (fd >= 0) close(fd);
#endif
    fds_.clear();
    devs_.clear();
    state_ = {};
}

void FlightYokeInput::calibrate_center() {
    for (auto &d : devs_) d.calibrate_center();
}

void FlightYokeInput::reset_calibration() {
    for (auto &d : devs_) d.reset_calibration();
}

void FlightYokeInput::rescan() {
#ifdef __linux__
    std::error_code ec;
    const std::filesystem::path dir("/dev/input");
    if (!std::filesystem::exists(dir, ec)) return;

    std::vector<std::string> paths;
    for (const auto &entry : std::filesystem::directory_iterator(dir, ec)) {
        const std::string fname = entry.path().filename().string();
        if (fname.rfind("event", 0) == 0) paths.push_back(entry.path().string());
    }
    std::sort(paths.begin(), paths.end());

    static std::vector<std::string> warned_paths;
    for (const std::string &path : paths) {
        if (devs_.size() >= 8) break;
        bool already_open = false;
        for (const auto &d : devs_) if (d.path == path) { already_open = true; break; }
        if (already_open) continue;

        const int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            if (errno == EACCES &&
                std::find(warned_paths.begin(), warned_paths.end(), path) == warned_paths.end()) {
                warned_paths.push_back(path);
            }
            continue;
        }

        uint8_t ev_bits[(EV_MAX + 7) / 8] = {};
        uint8_t abs_bits[(ABS_MAX + 7) / 8] = {};
        uint8_t key_bits[(KEY_MAX + 7) / 8] = {};
        uint8_t prop_bits[(INPUT_PROP_MAX + 7) / 8] = {};
        if (ioctl(fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits) < 0 || !test_bit(ev_bits, EV_ABS)) {
            close(fd);
            continue;
        }
        ioctl(fd, EVIOCGPROP(sizeof(prop_bits)), prop_bits);
        if (test_bit(prop_bits, INPUT_PROP_ACCELEROMETER)) {
            close(fd);
            continue;
        }
        ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits);
        if (test_bit(ev_bits, EV_KEY)) ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits);

        // Exclude mice, touchpads, touchscreens, and styluses cleanly.
        if (test_bit(key_bits, BTN_LEFT) || test_bit(key_bits, BTN_TOOL_FINGER) || test_bit(key_bits, BTN_TOUCH)) {
            close(fd);
            continue;
        }

        char raw_name[128] = "USB Flight Controller";
        if (ioctl(fd, EVIOCGNAME(sizeof(raw_name)), raw_name) < 0) {
            std::snprintf(raw_name, sizeof(raw_name), "USB Flight Controller");
        }
        const std::string name_str(raw_name);
        const std::string low_name = to_lower_ascii(name_str);

        bool has_joy_btns = false;
        for (int b = BTN_JOYSTICK; b < BTN_GAMEPAD; ++b) {
            if (test_bit(key_bits, b)) { has_joy_btns = true; break; }
        }
        for (int b = BTN_TRIGGER_HAPPY; b < BTN_TRIGGER_HAPPY + 16 && !has_joy_btns; ++b) {
            if (test_bit(key_bits, b)) { has_joy_btns = true; break; }
        }
        bool has_pad_btns = false;
        for (int b = BTN_GAMEPAD; b <= BTN_THUMBR; ++b) {
            if (test_bit(key_bits, b)) { has_pad_btns = true; break; }
        }
        const bool has_flight_axes =
            test_bit(abs_bits, ABS_THROTTLE) || test_bit(abs_bits, ABS_RUDDER) ||
            test_bit(abs_bits, ABS_RZ) || test_bit(abs_bits, ABS_Z) ||
            (test_bit(abs_bits, ABS_X) && test_bit(abs_bits, ABS_Y));
        const bool flight_named =
            contains_ci(low_name, "yoke") || contains_ci(low_name, "flight") ||
            contains_ci(low_name, "rudder") || contains_ci(low_name, "pedal") ||
            contains_ci(low_name, "throttle") || contains_ci(low_name, "quadrant") ||
            contains_ci(low_name, "hotas") || contains_ci(low_name, "joystick") ||
            contains_ci(low_name, "honeycomb") || contains_ci(low_name, "saitek") ||
            contains_ci(low_name, "thrustmaster") || contains_ci(low_name, "virpil") ||
            contains_ci(low_name, "vkb");

        if (!has_flight_axes || (!has_joy_btns && !has_pad_btns && !flight_named)) {
            close(fd);
            continue;
        }

        YokeDeviceState dev;
        dev.path = path;
        dev.name = name_str;
        for (int c = 0; c < YokeDeviceState::kMaxAbs; ++c) {
            if (!test_bit(abs_bits, c)) continue;
            input_absinfo info{};
            if (ioctl(fd, EVIOCGABS(c), &info) == 0 && info.maximum > info.minimum) {
                dev.axes[static_cast<size_t>(c)].present = true;
                dev.axes[static_cast<size_t>(c)].min_val = info.minimum;
                dev.axes[static_cast<size_t>(c)].max_val = info.maximum;
                dev.axes[static_cast<size_t>(c)].value = info.value;
            }
        }
        dev.assign_default_mapping(has_joy_btns, has_pad_btns);
        std::printf("[YOKE] Connected %s (%s) role=%d roll=%d pitch=%d rud=%d thr=%d mix=%d brk=%d/%d\n",
                    dev.name.c_str(), dev.path.c_str(), static_cast<int>(dev.role),
                    dev.roll_axis, dev.pitch_axis, dev.rudder_axis,
                    dev.throttle_axis, dev.mixture_axis, dev.brake_left_axis, dev.brake_right_axis);
        devs_.push_back(std::move(dev));
        fds_.push_back(fd);
    }
#endif
}

void FlightYokeInput::poll(const YokeConfig &cfg) {
#ifdef __linux__
    const auto now_s = std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (next_scan_s_ < 0.0 || now_s >= next_scan_s_) {
        rescan();
        next_scan_s_ = now_s + 1.5;
    }

    for (size_t i = 0; i < devs_.size();) {
        bool unplugged = false;
        input_event ev[32];
        while (true) {
            const ssize_t rd = read(fds_[i], ev, sizeof(ev));
            if (rd < 0) {
                if (errno == EAGAIN || errno == EINTR) break;
                unplugged = true;
                break;
            }
            if (rd == 0) {
                unplugged = true;
                break;
            }
            const int n = static_cast<int>(rd / static_cast<ssize_t>(sizeof(input_event)));
            for (int k = 0; k < n; ++k) {
                if (ev[k].type == EV_ABS && ev[k].code < YokeDeviceState::kMaxAbs) {
                    devs_[i].axes[ev[k].code].value = ev[k].value;
                } else if (ev[k].type == EV_KEY) {
                    const int slot = map_evdev_button(ev[k].code);
                    if (slot >= 0 && slot < YokeDeviceState::kMaxBtns) {
                        devs_[i].buttons[static_cast<size_t>(slot)] = (ev[k].value != 0);
                    }
                }
            }
        }
        if (unplugged) close_index(i);
        else ++i;
    }
#endif
    state_ = evaluate_devices(devs_, cfg);
}

bool FlightYokeInput::run_selftest() {
    auto mk_axis = [](int32_t lo, int32_t hi, int32_t val) {
        return YokeAxisCalib{true, lo, hi, val, 0, false};
    };

    // 1. Flight yoke with asymmetric off-center pot tare calibration + trim rocker + throttle/mixture levers.
    YokeDeviceState yoke;
    yoke.name = "Logitech Saitek Pro Flight Yoke";
    yoke.axes[0] = mk_axis(0, 1023, 540);   // ABS_X roll resting off-center at 540
    yoke.axes[1] = mk_axis(0, 1023, 500);   // ABS_Y pitch resting off-center at 500
    yoke.axes[2] = mk_axis(0, 255, 192);    // ABS_Z throttle 75%
    yoke.axes[4] = mk_axis(0, 255, 255);    // ABS_RY mixture full rich
    yoke.assign_default_mapping(true, false);
    if (yoke.role != YokeDeviceRole::YokeOrStick || yoke.roll_axis != 0 ||
        yoke.pitch_axis != 1 || yoke.throttle_axis != 2 || yoke.mixture_axis != 4) {
        return false;
    }
    yoke.calibrate_center();
    YokeConfig cfg{};
    cfg.deadzone = 0.05;
    cfg.curve = 0.35;

    // 2. Separate USB rudder pedals with uncentered toe brakes should NOT clobber yoke roll/pitch.
    YokeDeviceState pedals;
    pedals.name = "Saitek Pro Flight Rudder Pedals";
    pedals.axes[0] = mk_axis(0, 127, 0);    // Left toe brake released
    pedals.axes[1] = mk_axis(0, 127, 64);   // Right toe brake half pressed
    pedals.axes[5] = mk_axis(0, 511, 383);  // ABS_RZ right rudder ~50%
    pedals.assign_default_mapping(false, false);
    if (pedals.role != YokeDeviceRole::RudderPedals || pedals.roll_axis >= 0 || pedals.rudder_axis != 5) {
        return false;
    }

    std::vector<YokeDeviceState> pair{yoke, pedals};
    pair[0].buttons[LBTN_PARK_BRK] = true;
    pair[0].buttons[LBTN_TRIM_UP] = true;
    const YokeState s0 = evaluate_devices(pair, cfg);
    if (!s0.connected || !s0.has_yoke || !s0.has_rudder || !s0.has_throttle || !s0.has_mixture || !s0.has_brakes ||
        std::abs(s0.roll) > 1e-6 || std::abs(s0.pitch) > 1e-6 ||
        s0.rudder <= 0.25 || std::abs(s0.throttle - 192.0 / 255.0) > 0.01 ||
        s0.brake_left > 1e-6 || s0.brake_right < 0.40 ||
        !s0.parking_brake_pressed || s0.trim_rate != 1.0) {
        return false;
    }

    // 3. Asymmetric full-lock check after tare + edge-trigger clear on held button.
    pair[0].axes[0].value = 1023;
    pair[0].axes[1].value = 0;
    const YokeState s1 = evaluate_devices(pair, cfg);
    if (std::abs(s1.roll - 1.0) > 1e-5 || std::abs(s1.pitch - (-1.0)) > 1e-5 || s1.parking_brake_pressed) {
        return false;
    }

    std::printf("[YOKE SELFTEST OK] Evdev device classification, asymmetric tare calibration, pedal brakes & S-curve verified\n");
    return true;
}

} // namespace nspain
