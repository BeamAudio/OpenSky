#include "opentrack_udp.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#if defined(__linux__) || defined(__APPLE__)
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace nspain {

OpenTrackUDP::OpenTrackUDP() = default;

OpenTrackUDP::~OpenTrackUDP() {
    stop_listening();
}

bool OpenTrackUDP::start_listening(int port) {
    stop_listening();
    port_ = port;
#if defined(__linux__) || defined(__APPLE__)
    sock_fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_fd_ < 0) {
        return false;
    }
    int flags = ::fcntl(sock_fd_, F_GETFL, 0);
    if (flags >= 0) {
        ::fcntl(sock_fd_, F_SETFL, flags | O_NONBLOCK);
    }
    int reuse = 1;
    ::setsockopt(sock_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // 127.0.0.1
    if (::bind(sock_fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
        ::close(sock_fd_);
        sock_fd_ = -1;
        return false;
    }
    return true;
#else
    return false;
#endif
}

void OpenTrackUDP::stop_listening() {
#if defined(__linux__) || defined(__APPLE__)
    if (sock_fd_ >= 0) {
        ::close(sock_fd_);
        sock_fd_ = -1;
    }
#endif
}

bool OpenTrackUDP::parse_packet_48(const uint8_t *bytes, size_t len, OpenTrackPose &out_raw) {
    if (bytes == nullptr || len < 48) {
        return false;
    }
    double vals[6];
    std::memcpy(vals, bytes, 48);
    for (int i = 0; i < 6; ++i) {
        if (!std::isfinite(vals[i])) {
            return false;
        }
    }
    out_raw.x_cm = std::clamp(vals[0], -150.0, 150.0);
    out_raw.y_cm = std::clamp(vals[1], -150.0, 150.0);
    out_raw.z_cm = std::clamp(vals[2], -150.0, 150.0);
    out_raw.yaw_deg = std::clamp(vals[3], -175.0, 175.0);
    out_raw.pitch_deg = std::clamp(vals[4], -85.0, 85.0);
    out_raw.roll_deg = std::clamp(vals[5], -85.0, 85.0);
    out_raw.active = true;
    return true;
}

bool OpenTrackUDP::ingest_packet_48(const uint8_t *bytes, size_t len) {
    OpenTrackPose parsed{};
    if (!parse_packet_48(bytes, len, parsed)) {
        return false;
    }
    raw_pose_ = parsed;
    time_since_packet_s_ = 0.0;
    current_pose_.x_cm = std::clamp(raw_pose_.x_cm - tare_pose_.x_cm, -45.0, 45.0);
    current_pose_.y_cm = std::clamp(raw_pose_.y_cm - tare_pose_.y_cm, -35.0, 35.0);
    current_pose_.z_cm = std::clamp(raw_pose_.z_cm - tare_pose_.z_cm, -45.0, 45.0);
    current_pose_.yaw_deg = std::clamp(raw_pose_.yaw_deg - tare_pose_.yaw_deg, -165.0, 165.0);
    current_pose_.pitch_deg = std::clamp(raw_pose_.pitch_deg - tare_pose_.pitch_deg, -75.0, 75.0);
    current_pose_.roll_deg = std::clamp(raw_pose_.roll_deg - tare_pose_.roll_deg, -45.0, 45.0);
    current_pose_.active = true;
    return true;
}

void OpenTrackUDP::poll(double dt) {
#if defined(__linux__) || defined(__APPLE__)
    if (sock_fd_ >= 0) {
        uint8_t buf[128];
        for (int iter = 0; iter < 16; ++iter) {
            ssize_t n = ::recv(sock_fd_, buf, sizeof(buf), 0);
            if (n < 48) {
                break;
            }
            ingest_packet_48(buf, static_cast<size_t>(n));
        }
    }
#endif
    time_since_packet_s_ += std::max(0.0, dt);
    if (time_since_packet_s_ > 1.25) {
        // Smooth timeout decay back to centered neutral pose
        const double decay = std::exp(-std::max(0.0, dt) * 4.5);
        current_pose_.x_cm *= decay;
        current_pose_.y_cm *= decay;
        current_pose_.z_cm *= decay;
        current_pose_.yaw_deg *= decay;
        current_pose_.pitch_deg *= decay;
        current_pose_.roll_deg *= decay;
        if (time_since_packet_s_ > 2.5) {
            current_pose_.active = false;
        }
    }
}

void OpenTrackUDP::recenter() {
    tare_pose_ = raw_pose_;
    current_pose_.x_cm = 0.0;
    current_pose_.y_cm = 0.0;
    current_pose_.z_cm = 0.0;
    current_pose_.yaw_deg = 0.0;
    current_pose_.pitch_deg = 0.0;
    current_pose_.roll_deg = 0.0;
}

double OpenTrackUDP::apply_axis_curve(double raw_val, double deadzone, double cubic_weight, bool invert) {
    double v = std::clamp(raw_val, -1.0, 1.0);
    if (invert) {
        v = -v;
    }
    const double dz = std::clamp(deadzone, 0.0, 0.45);
    const double av = std::abs(v);
    if (av <= dz) {
        return 0.0;
    }
    const double sign = (v >= 0.0) ? 1.0 : -1.0;
    const double norm = (av - dz) / (1.0 - dz);
    const double k = std::clamp(cubic_weight, 0.0, 1.0);
    const double shaped = (1.0 - k) * norm + k * (norm * norm * norm);
    return sign * std::clamp(shaped, 0.0, 1.0);
}

} // namespace nspain
