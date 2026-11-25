#pragma once

#include <cstdint>
#include <string>

namespace nsim {

enum KeyCode : uint16_t {
    KEY_NONE = 0,
    KEY_W, KEY_A, KEY_S, KEY_D,
    KEY_Q, KEY_E, KEY_R, KEY_T, KEY_F, KEY_V, KEY_C, KEY_L, KEY_K, KEY_O, KEY_B, KEY_P, KEY_M, KEY_Y, KEY_H, KEY_N, KEY_U,
    KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
    KEY_SPACE, KEY_TAB, KEY_ESC, KEY_SHIFT, KEY_ENTER, KEY_X,
    KEY_HOME, KEY_END, KEY_PAGEUP, KEY_PAGEDOWN, KEY_BACKSPACE,
    KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7,
    KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9,
    KEY_COUNT
};

struct PixelRect { int x, y, w, h; };

inline constexpr int GAME_OUTPUT_W = 1920;
inline constexpr int GAME_OUTPUT_H = 1080;

class PlatformWindow {
public:
    PlatformWindow() = default;
    ~PlatformWindow();

    bool create(const char *title, int aspect_w, int aspect_h);
    void destroy();
    void poll_events();
    void set_relative_mouse(bool enabled);
    void swap_buffers();
    void set_vsync(bool enabled);

    bool should_close() const { return should_close_; }
    void request_close() { should_close_ = true; }
    int width() const { return width_; }
    int height() const { return height_; }
    double refresh_hz() const { return refresh_hz_; }

    bool key_down(KeyCode k) const { return k < KEY_COUNT && key_down_[k]; }
    bool key_pressed(KeyCode k) const { return k < KEY_COUNT && key_pressed_[k]; }
    bool mouse_left_down() const { return mouse_left_; }
    bool mouse_right_down() const { return mouse_right_; }
    bool mouse_left_clicked() const { return mouse_left_clicked_; }
    int mouse_x() const { return mouse_x_; }
    int mouse_y() const { return mouse_y_; }
    float mouse_dx() const { return mouse_dx_; }
    float mouse_dy() const { return mouse_dy_; }

    PixelRect presentation_rect(int output_w, int output_h) const;
    static double now_sec();
    static bool save_framebuffer_png(const std::string &path, int width, int height);

private:
    struct Impl;
    Impl *impl_ = nullptr;
    int width_ = 1600;
    int height_ = 900;
    double refresh_hz_ = 60.0;
    bool should_close_ = false;
    bool key_down_[KEY_COUNT] = {};
    bool key_pressed_[KEY_COUNT] = {};
    bool mouse_left_ = false;
    bool mouse_right_ = false;
    bool relative_mouse_ = false;
    bool warp_pending_ = false;
    bool focused_ = false;
    bool mouse_left_clicked_ = false;
    int mouse_x_ = 0;
    int mouse_y_ = 0;
    float mouse_dx_ = 0.0f;
    float mouse_dy_ = 0.0f;
};

} // namespace nsim
