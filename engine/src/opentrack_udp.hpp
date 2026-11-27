#pragma once

#include <cstddef>
#include <cstdint>

namespace nspain {

struct OpenTrackPose {
    double x_cm = 0.0;
    double y_cm = 0.0;
    double z_cm = 0.0;
    double yaw_deg = 0.0;
    double pitch_deg = 0.0;
    double roll_deg = 0.0;
    bool active = false;
};

class OpenTrackUDP {
public:
    OpenTrackUDP();
    ~OpenTrackUDP();

    // Start non-blocking UDP socket listener on 127.0.0.1:port (default 4242)
    bool start_listening(int port = 4242);
    void stop_listening();
    bool is_listening() const { return sock_fd_ >= 0; }

    // Poll non-blocking socket and advance timeout smoothing
    void poll(double dt);

    // Parse raw 48-byte OpenTrack UDP payload (6 x float64 little-endian: x, y, z, yaw, pitch, roll)
    static bool parse_packet_48(const uint8_t *bytes, size_t len, OpenTrackPose &out_raw);
    bool ingest_packet_48(const uint8_t *bytes, size_t len);

    // Recenter current head pose offset ('R' key)
    void recenter();

    // Multi-device HOTAS/Yoke/Pedal axis deadzone + cubic S-curve sensitivity shaper
    static double apply_axis_curve(double raw_val, double deadzone, double cubic_weight, bool invert = false);

    const OpenTrackPose &get_pose() const { return current_pose_; }
    bool is_tracking_active() const { return current_pose_.active; }
    double get_yaw_deg() const { return current_pose_.yaw_deg; }
    double get_pitch_deg() const { return current_pose_.pitch_deg; }
    double get_roll_deg() const { return current_pose_.roll_deg; }

private:
    intptr_t sock_fd_ = -1;
    int port_ = 4242;
    OpenTrackPose raw_pose_{};
    OpenTrackPose tare_pose_{};
    OpenTrackPose current_pose_{};
    double time_since_packet_s_ = 999.0;
};

} // namespace nspain
