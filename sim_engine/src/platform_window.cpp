#include "platform_window.hpp"
#include "gl_loader.hpp"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../third_party/stb_image_write.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#include <mmsystem.h>
#else
#include <X11/Xlib.h>
#include <X11/XKBlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <dlfcn.h>
#endif

namespace nsim {

namespace {

PixelRect fit_rect(PixelRect bounds, int aspect_w, int aspect_h) {
    int w = std::min(bounds.w, static_cast<int>(1LL * bounds.h * aspect_w / aspect_h));
    int h = static_cast<int>(1LL * w * aspect_h / aspect_w);
    return {bounds.x + (bounds.w - w) / 2, bounds.y + (bounds.h - h) / 2, w, h};
}

} // namespace

#ifdef _WIN32
namespace {

constexpr int kWGL_DRAW_TO_WINDOW_ARB = 0x2001;
constexpr int kWGL_SUPPORT_OPENGL_ARB = 0x2010;
constexpr int kWGL_DOUBLE_BUFFER_ARB = 0x2011;
constexpr int kWGL_PIXEL_TYPE_ARB = 0x2013;
constexpr int kWGL_COLOR_BITS_ARB = 0x2014;
constexpr int kWGL_RED_BITS_ARB = 0x2015;
constexpr int kWGL_GREEN_BITS_ARB = 0x2017;
constexpr int kWGL_BLUE_BITS_ARB = 0x2019;
constexpr int kWGL_ALPHA_BITS_ARB = 0x201B;
constexpr int kWGL_DEPTH_BITS_ARB = 0x2022;
constexpr int kWGL_TYPE_RGBA_ARB = 0x202B;
constexpr int kWGL_CONTEXT_MAJOR_VERSION_ARB = 0x2091;
constexpr int kWGL_CONTEXT_MINOR_VERSION_ARB = 0x2092;
constexpr int kWGL_CONTEXT_PROFILE_MASK_ARB = 0x9126;
constexpr int kWGL_CONTEXT_CORE_PROFILE_BIT_ARB = 0x00000001;

using PFN_wglChoosePixelFormatARB = BOOL (WINAPI *)(HDC, const int *, const FLOAT *, UINT, int *, UINT *);
using PFN_wglCreateContextAttribsARB = HGLRC (WINAPI *)(HDC, HGLRC, const int *);
using PFN_wglSwapIntervalEXT = BOOL (WINAPI *)(int);

std::wstring utf8_to_wide_win(const char *s) {
    if (!s || !*s) return L"OpenSky VFR Simulator";
    const int len = static_cast<int>(std::strlen(s));
    const int needed = MultiByteToWideChar(CP_UTF8, 0, s, len, nullptr, 0);
    if (needed <= 0) return L"OpenSky VFR Simulator";
    std::wstring out(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, len, out.data(), needed);
    return out;
}

void enable_per_monitor_dpi() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) return;
    using SetDpiCtxFn = BOOL (WINAPI *)(HANDLE);
    auto set_ctx = reinterpret_cast<SetDpiCtxFn>(reinterpret_cast<void *>(
        GetProcAddress(user32, "SetProcessDpiAwarenessContext")));
    if (set_ctx) {
        // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == (HANDLE)-4
        if (set_ctx(reinterpret_cast<HANDLE>(static_cast<intptr_t>(-4)))) return;
    }
    using SetDpiAwareFn = BOOL (WINAPI *)(void);
    auto set_aware = reinterpret_cast<SetDpiAwareFn>(reinterpret_cast<void *>(
        GetProcAddress(user32, "SetProcessDPIAware")));
    if (set_aware) set_aware();
}

KeyCode map_vk(WPARAM vk) {
    switch (vk) {
        case 'W': return KEY_W;
        case 'A': return KEY_A;
        case 'S': return KEY_S;
        case 'D': return KEY_D;
        case 'Q': return KEY_Q;
        case 'E': return KEY_E;
        case 'R': return KEY_R;
        case 'T': return KEY_T;
        case 'F': return KEY_F;
        case 'V': return KEY_V;
        case 'C': return KEY_C;
        case 'L': return KEY_L;
        case 'K': return KEY_K;
        case 'O': return KEY_O;
        case 'B': return KEY_B;
        case 'P': return KEY_P;
        case 'M': return KEY_M;
        case 'Y': return KEY_Y;
        case 'H': return KEY_H;
        case 'N': return KEY_N;
        case 'U': return KEY_U;
        case 'J': return KEY_J;
        case 'X': return KEY_X;
        case VK_RETURN: return KEY_ENTER;
        case VK_HOME: return KEY_HOME;
        case VK_END: return KEY_END;
        case VK_PRIOR: return KEY_PAGEUP;
        case VK_NEXT: return KEY_PAGEDOWN;
        case VK_BACK: return KEY_BACKSPACE;
        case VK_UP: return KEY_UP;
        case VK_DOWN: return KEY_DOWN;
        case VK_LEFT: return KEY_LEFT;
        case VK_RIGHT: return KEY_RIGHT;
        case VK_SPACE: return KEY_SPACE;
        case VK_TAB: return KEY_TAB;
        case VK_ESCAPE: return KEY_ESC;
        case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: return KEY_SHIFT;
        case VK_F1: return KEY_F1;
        case VK_F2: return KEY_F2;
        case VK_F3: return KEY_F3;
        case VK_F4: return KEY_F4;
        case VK_F5: return KEY_F5;
        case VK_F6: return KEY_F6;
        case VK_F7: return KEY_F7;
        case '1': case VK_NUMPAD1: return KEY_1;
        case '2': case VK_NUMPAD2: return KEY_2;
        case '3': case VK_NUMPAD3: return KEY_3;
        case '4': case VK_NUMPAD4: return KEY_4;
        case '5': case VK_NUMPAD5: return KEY_5;
        case '6': case VK_NUMPAD6: return KEY_6;
        case '7': case VK_NUMPAD7: return KEY_7;
        case '8': case VK_NUMPAD8: return KEY_8;
        case '9': case VK_NUMPAD9: return KEY_9;
        default: return KEY_NONE;
    }
}

void detect_monitor_win32(PixelRect &out_monitor, PixelRect &out_work, double &out_hz) {
    POINT pt{0, 0};
    GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(mon, &mi)) {
        out_monitor = {
            mi.rcMonitor.left, mi.rcMonitor.top,
            mi.rcMonitor.right - mi.rcMonitor.left,
            mi.rcMonitor.bottom - mi.rcMonitor.top
        };
        out_work = {
            mi.rcWork.left, mi.rcWork.top,
            mi.rcWork.right - mi.rcWork.left,
            mi.rcWork.bottom - mi.rcWork.top
        };
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) &&
            dm.dmDisplayFrequency >= 24 && dm.dmDisplayFrequency <= 360) {
            out_hz = static_cast<double>(dm.dmDisplayFrequency);
        }
    } else {
        const int sw = std::max(640, GetSystemMetrics(SM_CXSCREEN));
        const int sh = std::max(480, GetSystemMetrics(SM_CYSCREEN));
        out_monitor = {0, 0, sw, sh};
        out_work = out_monitor;
    }
}

} // namespace

struct PlatformWindow::Impl {
    HINSTANCE hinst = nullptr;
    HWND hwnd = nullptr;
    HDC hdc = nullptr;
    HGLRC hglrc = nullptr;
    PFN_wglSwapIntervalEXT swap_interval_fn = nullptr;
    PlatformWindow *owner = nullptr;
    bool timer_period_set = false;

    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
        if (msg == WM_NCCREATE) {
            auto *cs = reinterpret_cast<CREATESTRUCTW *>(lparam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            return DefWindowProcW(hwnd, msg, wparam, lparam);
        }
        auto *self = reinterpret_cast<PlatformWindow *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (!self) return DefWindowProcW(hwnd, msg, wparam, lparam);
        return self->impl_->handle_msg(hwnd, msg, wparam, lparam);
    }

    LRESULT handle_msg(HWND wnd, UINT msg, WPARAM wparam, LPARAM lparam) {
        switch (msg) {
            case WM_CLOSE:
                owner->should_close_ = true;
                return 0;
            case WM_DESTROY:
                owner->should_close_ = true;
                return 0;
            case WM_SETFOCUS:
                owner->focused_ = true;
                return 0;
            case WM_KILLFOCUS:
                owner->focused_ = false;
                owner->mouse_left_ = owner->mouse_right_ = false;
                std::memset(owner->key_down_, 0, sizeof(owner->key_down_));
                ReleaseCapture();
                return 0;
            case WM_SIZE:
                if (wparam != SIZE_MINIMIZED) {
                    const int nw = LOWORD(lparam);
                    const int nh = HIWORD(lparam);
                    if (nw > 0 && nh > 0) {
                        owner->width_ = nw;
                        owner->height_ = nh;
                    }
                }
                return 0;
            case WM_KEYDOWN:
            case WM_SYSKEYDOWN: {
                if (msg == WM_SYSKEYDOWN && (lparam & (1 << 29)) != 0 && wparam == VK_F4) {
                    return DefWindowProcW(wnd, msg, wparam, lparam);
                }
                const KeyCode k = map_vk(wparam);
                if (k != KEY_NONE) {
                    if (!owner->key_down_[k]) owner->key_pressed_[k] = true;
                    owner->key_down_[k] = true;
                    return 0;
                }
                break;
            }
            case WM_KEYUP:
            case WM_SYSKEYUP: {
                const KeyCode k = map_vk(wparam);
                if (k == KEY_SHIFT) {
                    if ((GetKeyState(VK_SHIFT) & 0x8000) == 0) owner->key_down_[KEY_SHIFT] = false;
                    return 0;
                }
                if (k != KEY_NONE) {
                    owner->key_down_[k] = false;
                    return 0;
                }
                break;
            }
            case WM_LBUTTONDOWN:
                owner->mouse_left_ = true;
                owner->mouse_left_clicked_ = true;
                owner->mouse_x_ = GET_X_LPARAM(lparam);
                owner->mouse_y_ = GET_Y_LPARAM(lparam);
                SetCapture(wnd);
                return 0;
            case WM_LBUTTONUP:
                owner->mouse_left_ = false;
                if (!owner->mouse_right_) ReleaseCapture();
                return 0;
            case WM_RBUTTONDOWN:
                owner->mouse_right_ = true;
                owner->mouse_x_ = GET_X_LPARAM(lparam);
                owner->mouse_y_ = GET_Y_LPARAM(lparam);
                SetCapture(wnd);
                return 0;
            case WM_RBUTTONUP:
                owner->mouse_right_ = false;
                if (!owner->mouse_left_) ReleaseCapture();
                return 0;
            case WM_MOUSEMOVE: {
                const int nx = GET_X_LPARAM(lparam);
                const int ny = GET_Y_LPARAM(lparam);
                if (owner->warp_pending_ && nx == owner->width_ / 2 && ny == owner->height_ / 2) {
                    owner->warp_pending_ = false;
                    owner->mouse_x_ = nx;
                    owner->mouse_y_ = ny;
                    return 0;
                }
                if (owner->mouse_left_ || owner->mouse_right_ || (owner->relative_mouse_ && owner->focused_)) {
                    owner->mouse_dx_ += static_cast<float>(nx - owner->mouse_x_);
                    owner->mouse_dy_ += static_cast<float>(ny - owner->mouse_y_);
                }
                owner->mouse_x_ = nx;
                owner->mouse_y_ = ny;
                return 0;
            }
            default:
                break;
        }
        return DefWindowProcW(wnd, msg, wparam, lparam);
    }
};

PlatformWindow::~PlatformWindow() {
    destroy();
}

bool PlatformWindow::create(const char *title, int aspect_w, int aspect_h, DisplayMode mode, bool visible, bool compact_preview) {
    destroy();
    enable_per_monitor_dpi();

    impl_ = new Impl();
    impl_->owner = this;
    impl_->hinst = GetModuleHandleW(nullptr);
    impl_->timer_period_set = (timeBeginPeriod(1) == TIMERR_NOERROR);

    constexpr wchar_t kClassName[] = L"OpenSkyVFRWindow";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_OWNDC | CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = Impl::wnd_proc;
    wc.hInstance = impl_->hinst;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        std::fprintf(stderr, "[WIN] RegisterClassExW failed\n");
        destroy();
        return false;
    }

    // Bootstrap WGL extension pointers through a hidden dummy window.
    HWND dummy_wnd = CreateWindowExW(0, kClassName, L"", WS_OVERLAPPED, 0, 0, 16, 16,
                                     nullptr, nullptr, impl_->hinst, nullptr);
    if (!dummy_wnd) {
        std::fprintf(stderr, "[WIN] Failed to create bootstrap WGL window\n");
        destroy();
        return false;
    }
    HDC dummy_dc = GetDC(dummy_wnd);
    PIXELFORMATDESCRIPTOR dummy_pfd{};
    dummy_pfd.nSize = sizeof(dummy_pfd);
    dummy_pfd.nVersion = 1;
    dummy_pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    dummy_pfd.iPixelType = PFD_TYPE_RGBA;
    dummy_pfd.cColorBits = 32;
    dummy_pfd.cDepthBits = 24;
    const int dummy_pf = ChoosePixelFormat(dummy_dc, &dummy_pfd);
    if (!dummy_pf || !SetPixelFormat(dummy_dc, dummy_pf, &dummy_pfd)) {
        ReleaseDC(dummy_wnd, dummy_dc);
        DestroyWindow(dummy_wnd);
        std::fprintf(stderr, "[WIN] Bootstrap SetPixelFormat failed\n");
        destroy();
        return false;
    }
    HGLRC dummy_gl = wglCreateContext(dummy_dc);
    if (!dummy_gl || !wglMakeCurrent(dummy_dc, dummy_gl)) {
        if (dummy_gl) wglDeleteContext(dummy_gl);
        ReleaseDC(dummy_wnd, dummy_dc);
        DestroyWindow(dummy_wnd);
        std::fprintf(stderr, "[WIN] Bootstrap wglCreateContext failed\n");
        destroy();
        return false;
    }

    auto choose_pf_arb = reinterpret_cast<PFN_wglChoosePixelFormatARB>(reinterpret_cast<void *>(
        wglGetProcAddress("wglChoosePixelFormatARB")));
    auto create_ctx_arb = reinterpret_cast<PFN_wglCreateContextAttribsARB>(reinterpret_cast<void *>(
        wglGetProcAddress("wglCreateContextAttribsARB")));
    impl_->swap_interval_fn = reinterpret_cast<PFN_wglSwapIntervalEXT>(reinterpret_cast<void *>(
        wglGetProcAddress("wglSwapIntervalEXT")));

    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(dummy_gl);
    ReleaseDC(dummy_wnd, dummy_dc);
    DestroyWindow(dummy_wnd);

    if (!create_ctx_arb) {
        std::fprintf(stderr, "[WIN] Missing wglCreateContextAttribsARB (OpenGL 4.5+ driver required)\n");
        destroy();
        return false;
    }

    PixelRect monitor{0, 0, 1600, 900}, work{0, 0, 1600, 900};
    refresh_hz_ = 60.0;
    detect_monitor_win32(monitor, work, refresh_hz_);

    PixelRect bounds{};
    if (mode == DisplayMode::Fullscreen || mode == DisplayMode::FullscreenWindowed) {
        bounds = monitor;
    } else {
        const int w = std::min(compact_preview ? 1280 : monitor.w * 9 / 10, work.w);
        const int h = std::min(monitor.h * 9 / 10, std::max(1, work.h - 48));
        bounds = fit_rect({work.x + (work.w - w) / 2, work.y + (work.h - h) / 2, w, h}, aspect_w, aspect_h);
    }
    width_ = bounds.w;
    height_ = bounds.h;

    DWORD style = WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
    DWORD ex_style = WS_EX_APPWINDOW;
    int win_x = bounds.x;
    int win_y = bounds.y;
    int win_w = width_;
    int win_h = height_;
    if (mode == DisplayMode::Fullscreen || mode == DisplayMode::FullscreenWindowed) {
        style |= WS_POPUP;
    } else {
        style |= WS_OVERLAPPEDWINDOW;
        RECT wr{bounds.x, bounds.y, bounds.x + width_, bounds.y + height_};
        AdjustWindowRectEx(&wr, style, FALSE, ex_style);
        win_x = std::max(work.x, static_cast<int>(wr.left));
        win_y = std::max(work.y, static_cast<int>(wr.top));
        win_w = wr.right - wr.left;
        win_h = wr.bottom - wr.top;
    }

    const std::wstring wtitle = utf8_to_wide_win(title);
    impl_->hwnd = CreateWindowExW(ex_style, kClassName, wtitle.c_str(), style,
                                  win_x, win_y, win_w, win_h,
                                  nullptr, nullptr, impl_->hinst, this);
    if (!impl_->hwnd) {
        std::fprintf(stderr, "[WIN] CreateWindowExW failed\n");
        destroy();
        return false;
    }
    impl_->hdc = GetDC(impl_->hwnd);
    if (!impl_->hdc) {
        std::fprintf(stderr, "[WIN] GetDC failed\n");
        destroy();
        return false;
    }

    int pixel_format = 0;
    if (choose_pf_arb) {
        const int pf_attrs[] = {
            kWGL_DRAW_TO_WINDOW_ARB, TRUE,
            kWGL_SUPPORT_OPENGL_ARB, TRUE,
            kWGL_DOUBLE_BUFFER_ARB, TRUE,
            kWGL_PIXEL_TYPE_ARB, kWGL_TYPE_RGBA_ARB,
            kWGL_COLOR_BITS_ARB, 32,
            kWGL_RED_BITS_ARB, 8,
            kWGL_GREEN_BITS_ARB, 8,
            kWGL_BLUE_BITS_ARB, 8,
            kWGL_ALPHA_BITS_ARB, 8,
            kWGL_DEPTH_BITS_ARB, 24,
            0
        };
        UINT num_formats = 0;
        if (!choose_pf_arb(impl_->hdc, pf_attrs, nullptr, 1, &pixel_format, &num_formats) || num_formats == 0) {
            pixel_format = 0;
        }
    }
    PIXELFORMATDESCRIPTOR pfd{};
    if (pixel_format == 0) {
        pfd.nSize = sizeof(pfd);
        pfd.nVersion = 1;
        pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
        pfd.iPixelType = PFD_TYPE_RGBA;
        pfd.cColorBits = 32;
        pfd.cDepthBits = 24;
        pixel_format = ChoosePixelFormat(impl_->hdc, &pfd);
    } else {
        DescribePixelFormat(impl_->hdc, pixel_format, sizeof(pfd), &pfd);
    }
    if (!pixel_format || !SetPixelFormat(impl_->hdc, pixel_format, &pfd)) {
        std::fprintf(stderr, "[WIN] SetPixelFormat failed\n");
        destroy();
        return false;
    }

    const int ctx_46[] = {
        kWGL_CONTEXT_MAJOR_VERSION_ARB, 4,
        kWGL_CONTEXT_MINOR_VERSION_ARB, 6,
        kWGL_CONTEXT_PROFILE_MASK_ARB, kWGL_CONTEXT_CORE_PROFILE_BIT_ARB,
        0
    };
    impl_->hglrc = create_ctx_arb(impl_->hdc, nullptr, ctx_46);
    if (!impl_->hglrc) {
        const int ctx_45[] = {
            kWGL_CONTEXT_MAJOR_VERSION_ARB, 4,
            kWGL_CONTEXT_MINOR_VERSION_ARB, 5,
            kWGL_CONTEXT_PROFILE_MASK_ARB, kWGL_CONTEXT_CORE_PROFILE_BIT_ARB,
            0
        };
        impl_->hglrc = create_ctx_arb(impl_->hdc, nullptr, ctx_45);
    }
    if (!impl_->hglrc || !wglMakeCurrent(impl_->hdc, impl_->hglrc)) {
        std::fprintf(stderr, "[WIN] Failed to create OpenGL 4.5/4.6 Core context\n");
        destroy();
        return false;
    }

    if (visible) {
        ShowWindow(impl_->hwnd, compact_preview ? SW_SHOWNOACTIVATE : SW_SHOW);
        UpdateWindow(impl_->hwnd);
        if (!compact_preview) SetForegroundWindow(impl_->hwnd);
        focused_ = (GetForegroundWindow() == impl_->hwnd);
    }

    if (!load_gl_functions()) {
        destroy();
        return false;
    }
    set_vsync(false);
    return true;
}

void PlatformWindow::destroy() {
    if (!impl_) return;
    if (impl_->hglrc) {
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(impl_->hglrc);
        impl_->hglrc = nullptr;
    }
    if (impl_->hdc && impl_->hwnd) {
        ReleaseDC(impl_->hwnd, impl_->hdc);
        impl_->hdc = nullptr;
    }
    if (impl_->hwnd) {
        DestroyWindow(impl_->hwnd);
        impl_->hwnd = nullptr;
    }
    if (impl_->timer_period_set) {
        timeEndPeriod(1);
        impl_->timer_period_set = false;
    }
    delete impl_;
    impl_ = nullptr;
}

bool PlatformWindow::set_vsync(bool enabled) {
    return impl_ && impl_->swap_interval_fn && impl_->swap_interval_fn(enabled ? 1 : 0);
}

void PlatformWindow::poll_events() {
    std::memset(key_pressed_, 0, sizeof(key_pressed_));
    mouse_left_clicked_ = false;
    mouse_dx_ = 0.0f;
    mouse_dy_ = 0.0f;
    if (!impl_ || !impl_->hwnd) return;

    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (relative_mouse_ && focused_ && (mouse_x_ != width_ / 2 || mouse_y_ != height_ / 2)) {
        mouse_x_ = width_ / 2;
        mouse_y_ = height_ / 2;
        warp_pending_ = true;
        POINT pt{mouse_x_, mouse_y_};
        ClientToScreen(impl_->hwnd, &pt);
        SetCursorPos(pt.x, pt.y);
    }
}

void PlatformWindow::set_relative_mouse(bool enabled) {
    if (relative_mouse_ == enabled) return;
    relative_mouse_ = enabled;
    mouse_dx_ = mouse_dy_ = 0.0f;
    if (enabled && focused_ && impl_ && impl_->hwnd) {
        mouse_x_ = width_ / 2;
        mouse_y_ = height_ / 2;
        warp_pending_ = true;
        POINT pt{mouse_x_, mouse_y_};
        ClientToScreen(impl_->hwnd, &pt);
        SetCursorPos(pt.x, pt.y);
    } else {
        warp_pending_ = false;
    }
}

void PlatformWindow::swap_buffers() {
    if (impl_ && impl_->hdc) {
        SwapBuffers(impl_->hdc);
    }
}
#else
namespace {

using GLXFBConfig = void *;
using GLXContext = void *;
using GLXDrawable = XID;

constexpr int kGLX_X_RENDERABLE = 0x8012;
constexpr int kGLX_DRAWABLE_TYPE = 0x8010;
constexpr int kGLX_RENDER_TYPE = 0x8011;
constexpr int kGLX_WINDOW_BIT = 0x0001;
constexpr int kGLX_RGBA_BIT = 0x0001;
constexpr int kGLX_DOUBLEBUFFER = 5;
constexpr int kGLX_RED_SIZE = 8;
constexpr int kGLX_GREEN_SIZE = 9;
constexpr int kGLX_BLUE_SIZE = 10;
constexpr int kGLX_ALPHA_SIZE = 11;
constexpr int kGLX_DEPTH_SIZE = 12;
constexpr int kGLX_CONTEXT_MAJOR_VERSION_ARB = 0x2091;
constexpr int kGLX_CONTEXT_MINOR_VERSION_ARB = 0x2092;
constexpr int kGLX_CONTEXT_PROFILE_MASK_ARB = 0x9126;
constexpr int kGLX_CONTEXT_CORE_PROFILE_BIT_ARB = 0x00000001;

using PFN_glXChooseFBConfig = GLXFBConfig *(*)(Display *, int, const int *, int *);
using PFN_glXGetVisualFromFBConfig = XVisualInfo *(*)(Display *, GLXFBConfig);
using PFN_glXGetProcAddressARB = void *(*)(const char *);
using PFN_glXCreateContextAttribsARB = GLXContext (*)(Display *, GLXFBConfig, GLXContext, Bool, const int *);
using PFN_glXMakeCurrent = Bool (*)(Display *, GLXDrawable, GLXContext);
using PFN_glXDestroyContext = void (*)(Display *, GLXContext);
using PFN_glXSwapBuffers = void (*)(Display *, GLXDrawable);
using PFN_glXQueryExtensionsString = const char *(*)(Display *, int);
using PFN_glXSwapIntervalEXT = void (*)(Display *, GLXDrawable, int);
using PFN_glXSwapIntervalMESA = int (*)(unsigned int);

KeyCode map_keysym(KeySym ks) {
    switch (ks) {
        case XK_w: case XK_W: return KEY_W;
        case XK_a: case XK_A: return KEY_A;
        case XK_s: case XK_S: return KEY_S;
        case XK_d: case XK_D: return KEY_D;
        case XK_q: case XK_Q: return KEY_Q;
        case XK_e: case XK_E: return KEY_E;
        case XK_r: case XK_R: return KEY_R;
        case XK_t: case XK_T: return KEY_T;
        case XK_f: case XK_F: return KEY_F;
        case XK_v: case XK_V: return KEY_V;
        case XK_c: case XK_C: return KEY_C;
        case XK_l: case XK_L: return KEY_L;
        case XK_k: case XK_K: return KEY_K;
        case XK_o: case XK_O: return KEY_O;
        case XK_b: case XK_B: return KEY_B;
        case XK_p: case XK_P: return KEY_P;
        case XK_m: case XK_M: return KEY_M;
        case XK_y: case XK_Y: return KEY_Y;
        case XK_h: case XK_H: return KEY_H;
        case XK_n: case XK_N: return KEY_N;
        case XK_u: case XK_U: return KEY_U;
        case XK_j: case XK_J: return KEY_J;
        case XK_x: case XK_X: return KEY_X;
        case XK_Return: case XK_KP_Enter: return KEY_ENTER;
        case XK_Home: case XK_KP_Home: return KEY_HOME;
        case XK_End: case XK_KP_End: return KEY_END;
        case XK_Page_Up: case XK_KP_Page_Up: return KEY_PAGEUP;
        case XK_Page_Down: case XK_KP_Page_Down: return KEY_PAGEDOWN;
        case XK_BackSpace: return KEY_BACKSPACE;
        case XK_Up: return KEY_UP;
        case XK_Down: return KEY_DOWN;
        case XK_Left: return KEY_LEFT;
        case XK_Right: return KEY_RIGHT;
        case XK_space: return KEY_SPACE;
        case XK_Tab: return KEY_TAB;
        case XK_Escape: return KEY_ESC;
        case XK_Shift_L: case XK_Shift_R: return KEY_SHIFT;
        case XK_F1: return KEY_F1;
        case XK_F2: return KEY_F2;
        case XK_F3: return KEY_F3;
        case XK_F4: return KEY_F4;
        case XK_F5: return KEY_F5;
        case XK_F6: return KEY_F6;
        case XK_F7: return KEY_F7;
        case XK_1: return KEY_1;
        case XK_2: return KEY_2;
        case XK_3: return KEY_3;
        case XK_4: return KEY_4;
        case XK_5: return KEY_5;
        case XK_6: return KEY_6;
        case XK_7: return KEY_7;
        case XK_8: return KEY_8;
        case XK_9: return KEY_9;
        default: return KEY_NONE;
    }
}

} // namespace

namespace {

// XRandR 1.5 monitor information. Load the extension at runtime so X11
// sessions without the extension still work with the default screen bounds.
struct MonitorInfo {
    Atom name;
    Bool primary;
    Bool automatic;
    int noutput, x, y, width, height, mwidth, mheight;
    XID *outputs;
};

PixelRect current_monitor(Display *display, Window root, PixelRect fallback) {
    static void *randr = dlopen("libXrandr.so.2", RTLD_LAZY | RTLD_LOCAL);
    if (!randr) return fallback;
    auto get = reinterpret_cast<MonitorInfo *(*)(Display *, Window, Bool, int *)>(dlsym(randr, "XRRGetMonitors"));
    auto free_monitors = reinterpret_cast<void (*)(MonitorInfo *)>(dlsym(randr, "XRRFreeMonitors"));
    auto version = reinterpret_cast<Status (*)(Display *, int *, int *)>(dlsym(randr, "XRRQueryVersion"));
    int major = 0, minor = 0, count = 0;
    if (get && free_monitors && version && version(display, &major, &minor) &&
        (major > 1 || (major == 1 && minor >= 5))) {
        if (MonitorInfo *monitors = get(display, root, True, &count)) {
            Window root_return, child;
            int x = 0, y = 0, local_x, local_y;
            unsigned mask;
            const bool have_pointer = XQueryPointer(display, root, &root_return, &child,
                                                    &x, &y, &local_x, &local_y, &mask);
            for (int i = 0; i < count; ++i) {
                const auto &m = monitors[i];
                if (m.primary) fallback = {m.x, m.y, m.width, m.height};
                if (have_pointer && x >= m.x && y >= m.y &&
                    x < m.x + m.width && y < m.y + m.height) {
                    fallback = {m.x, m.y, m.width, m.height};
                    break;
                }
            }
            free_monitors(monitors);
        }
    }
    return fallback;
}

double detect_refresh_hz(Display *display, Window root) {
    static void *randr = dlopen("libXrandr.so.2", RTLD_LAZY | RTLD_LOCAL);
    if (!randr) return 60.0;
    struct ScreenConf;
    auto get_info = reinterpret_cast<ScreenConf *(*)(Display *, Drawable)>(dlsym(randr, "XRRGetScreenInfo"));
    auto cur_rate = reinterpret_cast<short (*)(ScreenConf *)>(dlsym(randr, "XRRConfigCurrentRate"));
    auto free_info = reinterpret_cast<void (*)(ScreenConf *)>(dlsym(randr, "XRRFreeScreenConfigInfo"));
    if (!get_info || !cur_rate || !free_info) return 60.0;
    ScreenConf *conf = get_info(display, root);
    if (!conf) return 60.0;
    short hz = cur_rate(conf);
    free_info(conf);
    return (hz >= 24 && hz <= 360) ? static_cast<double>(hz) : 60.0;
}

PixelRect window_bounds(Display *display, Window root, PixelRect monitor) {
    unsigned long desktop = 0;
    auto read_property = [&](const char *name, long offset, long items, unsigned long *out) {
        unsigned char *data = nullptr;
        Atom type;
        int format;
        unsigned long count, remaining;
        const bool ok = XGetWindowProperty(display, root, XInternAtom(display, name, False),
            offset, items, False, XA_CARDINAL, &type, &format, &count, &remaining, &data) == Success &&
            type == XA_CARDINAL && format == 32 && count >= static_cast<unsigned long>(items) && data;
        if (ok) std::memcpy(out, data, items * sizeof(unsigned long));
        if (data) XFree(data);
        return ok;
    };
    read_property("_NET_CURRENT_DESKTOP", 0, 1, &desktop);
    unsigned long area[4];
    if (!read_property("_NET_WORKAREA", 4 * desktop, 4, area)) return monitor;
    const int x = static_cast<int>(area[0]), y = static_cast<int>(area[1]);
    const int left = std::max(monitor.x, x), top = std::max(monitor.y, y);
    const int right = std::min(monitor.x + monitor.w, x + static_cast<int>(area[2]));
    const int bottom = std::min(monitor.y + monitor.h, y + static_cast<int>(area[3]));
    return right > left && bottom > top ? PixelRect{left, top, right - left, bottom - top} : monitor;
}

} // namespace

struct PlatformWindow::Impl {
    void *gl_lib = nullptr;
    Display *dpy = nullptr;
    Window win = 0;
    Colormap cmap = 0;
    Atom wm_delete = 0;
    bool detectable_repeat = false;
    GLXContext ctx = nullptr;
    PFN_glXSwapBuffers swap_fn = nullptr;
    PFN_glXMakeCurrent make_cur_fn = nullptr;
    PFN_glXDestroyContext destroy_ctx_fn = nullptr;
    PFN_glXSwapIntervalEXT swap_ext_fn = nullptr;
    PFN_glXSwapIntervalMESA swap_mesa_fn = nullptr;
};

PlatformWindow::~PlatformWindow() {
    destroy();
}

bool PlatformWindow::create(const char *title, int aspect_w, int aspect_h, DisplayMode mode, bool visible, bool compact_preview) {
    impl_ = new Impl();
    impl_->gl_lib = dlopen("libGL.so.1", RTLD_LAZY | RTLD_LOCAL);
    if (!impl_->gl_lib) {
        std::fprintf(stderr, "[WIN] dlopen(libGL.so.1) failed\n");
        return false;
    }

    auto choose_fbc = reinterpret_cast<PFN_glXChooseFBConfig>(dlsym(impl_->gl_lib, "glXChooseFBConfig"));
    auto get_vis = reinterpret_cast<PFN_glXGetVisualFromFBConfig>(dlsym(impl_->gl_lib, "glXGetVisualFromFBConfig"));
    auto get_proc = reinterpret_cast<PFN_glXGetProcAddressARB>(dlsym(impl_->gl_lib, "glXGetProcAddressARB"));
    impl_->make_cur_fn = reinterpret_cast<PFN_glXMakeCurrent>(dlsym(impl_->gl_lib, "glXMakeCurrent"));
    impl_->destroy_ctx_fn = reinterpret_cast<PFN_glXDestroyContext>(dlsym(impl_->gl_lib, "glXDestroyContext"));
    impl_->swap_fn = reinterpret_cast<PFN_glXSwapBuffers>(dlsym(impl_->gl_lib, "glXSwapBuffers"));
    if (!choose_fbc || !get_vis || !get_proc || !impl_->make_cur_fn || !impl_->swap_fn) {
        std::fprintf(stderr, "[WIN] Missing GLX entry points\n");
        return false;
    }

    impl_->dpy = XOpenDisplay(nullptr);
    if (!impl_->dpy) {
        std::fprintf(stderr, "[WIN] XOpenDisplay failed (is DISPLAY set?)\n");
        return false;
    }
    Bool repeat_supported = False;
    impl_->detectable_repeat = XkbSetDetectableAutoRepeat(impl_->dpy, True, &repeat_supported)
                              && repeat_supported;
    int scr = DefaultScreen(impl_->dpy);
    Window root = RootWindow(impl_->dpy, scr);
    const PixelRect monitor = current_monitor(impl_->dpy, root,
        {0, 0, DisplayWidth(impl_->dpy, scr), DisplayHeight(impl_->dpy, scr)});
    refresh_hz_ = detect_refresh_hz(impl_->dpy, root);
    const PixelRect work = window_bounds(impl_->dpy, root, monitor);
    PixelRect bounds{};
    if (mode == DisplayMode::Fullscreen || mode == DisplayMode::FullscreenWindowed) {
        bounds = monitor;
    } else {
        const int w = std::min(compact_preview ? 1280 : monitor.w * 9 / 10, work.w);
        const int h = std::min(monitor.h * 9 / 10, std::max(1, work.h - 48));
        bounds = fit_rect({work.x + (work.w - w) / 2, work.y + (work.h - h) / 2, w, h},
                          aspect_w, aspect_h);
    }
    width_ = bounds.w;
    height_ = bounds.h;

    const int fbc_attrs[] = {
        kGLX_X_RENDERABLE, True,
        kGLX_DRAWABLE_TYPE, kGLX_WINDOW_BIT,
        kGLX_RENDER_TYPE, kGLX_RGBA_BIT,
        kGLX_DOUBLEBUFFER, True,
        kGLX_RED_SIZE, 8,
        kGLX_GREEN_SIZE, 8,
        kGLX_BLUE_SIZE, 8,
        kGLX_ALPHA_SIZE, 8,
        kGLX_DEPTH_SIZE, 24,
        None
    };
    int fb_count = 0;
    GLXFBConfig *fbcs = choose_fbc(impl_->dpy, scr, fbc_attrs, &fb_count);
    if (!fbcs || fb_count == 0) {
        std::fprintf(stderr, "[WIN] glXChooseFBConfig failed\n");
        return false;
    }
    GLXFBConfig fbc = fbcs[0];
    XFree(fbcs);

    XVisualInfo *vi = get_vis(impl_->dpy, fbc);
    if (!vi) {
        std::fprintf(stderr, "[WIN] glXGetVisualFromFBConfig failed\n");
        return false;
    }

    impl_->cmap = XCreateColormap(impl_->dpy, root, vi->visual, AllocNone);
    XSetWindowAttributes swa{};
    swa.colormap = impl_->cmap;
    swa.event_mask = ExposureMask | KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask |
                     PointerMotionMask | StructureNotifyMask | FocusChangeMask;

    impl_->win = XCreateWindow(
        impl_->dpy, root,
        bounds.x, bounds.y,
        static_cast<unsigned int>(width_), static_cast<unsigned int>(height_),
        0, vi->depth, InputOutput, vi->visual,
        CWColormap | CWEventMask, &swa
    );
    XFree(vi);

    // Keep the fullscreen-windowed mode managed by the desktop compositor,
    // but ask the window manager to remove its title bar and frame.
    if (mode == DisplayMode::FullscreenWindowed) {
        struct MotifWmHints {
            unsigned long flags;
            unsigned long functions;
            unsigned long decorations;
            long input_mode;
            unsigned long status;
        } motif_hints{2, 0, 0, 0, 0}; // MWM_HINTS_DECORATIONS, no decorations
        const Atom motif_atom = XInternAtom(impl_->dpy, "_MOTIF_WM_HINTS", False);
        XChangeProperty(impl_->dpy, impl_->win, motif_atom, motif_atom, 32,
                        PropModeReplace, reinterpret_cast<const unsigned char *>(&motif_hints), 5);
    }

    XStoreName(impl_->dpy, impl_->win, title);
    XSizeHints hints{};
    hints.flags = PPosition | PSize;
    hints.x = bounds.x;
    hints.y = bounds.y;
    hints.width = width_;
    hints.height = height_;
    XSetWMNormalHints(impl_->dpy, impl_->win, &hints);
    impl_->wm_delete = XInternAtom(impl_->dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(impl_->dpy, impl_->win, &impl_->wm_delete, 1);
    if (visible) XMapWindow(impl_->dpy, impl_->win);
    if (visible && mode == DisplayMode::Fullscreen) {
        const Atom state_atom = XInternAtom(impl_->dpy, "_NET_WM_STATE", False);
        const Atom fullscreen_atom = XInternAtom(impl_->dpy, "_NET_WM_STATE_FULLSCREEN", False);
        XEvent request{};
        request.xclient.type = ClientMessage;
        request.xclient.window = impl_->win;
        request.xclient.message_type = state_atom;
        request.xclient.format = 32;
        request.xclient.data.l[0] = 1; // _NET_WM_STATE_ADD
        request.xclient.data.l[1] = static_cast<long>(fullscreen_atom);
        request.xclient.data.l[2] = 0;
        request.xclient.data.l[3] = 1; // application source
        XSendEvent(impl_->dpy, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &request);
    }
    XFlush(impl_->dpy);

    auto create_ctx_attribs = reinterpret_cast<PFN_glXCreateContextAttribsARB>(get_proc("glXCreateContextAttribsARB"));
    if (!create_ctx_attribs) {
        std::fprintf(stderr, "[WIN] Missing glXCreateContextAttribsARB\n");
        return false;
    }

    const int ctx_attrs_46[] = {
        kGLX_CONTEXT_MAJOR_VERSION_ARB, 4,
        kGLX_CONTEXT_MINOR_VERSION_ARB, 6,
        kGLX_CONTEXT_PROFILE_MASK_ARB, kGLX_CONTEXT_CORE_PROFILE_BIT_ARB,
        None
    };
    impl_->ctx = create_ctx_attribs(impl_->dpy, fbc, nullptr, True, ctx_attrs_46);
    if (!impl_->ctx) {
        const int ctx_attrs_45[] = {
            kGLX_CONTEXT_MAJOR_VERSION_ARB, 4,
            kGLX_CONTEXT_MINOR_VERSION_ARB, 5,
            kGLX_CONTEXT_PROFILE_MASK_ARB, kGLX_CONTEXT_CORE_PROFILE_BIT_ARB,
            None
        };
        impl_->ctx = create_ctx_attribs(impl_->dpy, fbc, nullptr, True, ctx_attrs_45);
    }
    if (!impl_->ctx) {
        std::fprintf(stderr, "[WIN] Failed to create OpenGL 4.5/4.6 Core context\n");
        return false;
    }

    impl_->make_cur_fn(impl_->dpy, impl_->win, impl_->ctx);
    auto query_extensions = reinterpret_cast<PFN_glXQueryExtensionsString>(dlsym(impl_->gl_lib, "glXQueryExtensionsString"));
    const char *extension_string = query_extensions ? query_extensions(impl_->dpy, scr) : nullptr;
    std::string_view extensions = extension_string ? extension_string : "";
    while (!extensions.empty()) {
        const size_t end = extensions.find(' ');
        const auto name = extensions.substr(0, end);
        if (name == "GLX_EXT_swap_control")
            impl_->swap_ext_fn = reinterpret_cast<PFN_glXSwapIntervalEXT>(get_proc("glXSwapIntervalEXT"));
        else if (name == "GLX_MESA_swap_control")
            impl_->swap_mesa_fn = reinterpret_cast<PFN_glXSwapIntervalMESA>(get_proc("glXSwapIntervalMESA"));
        if (end == std::string_view::npos) break;
        extensions.remove_prefix(end + 1);
    }

    if (!load_gl_functions()) {
        return false;
    }
    set_vsync(false);
    return true;
}

void PlatformWindow::destroy() {
    if (!impl_) return;
    if (impl_->dpy) {
        if (impl_->ctx && impl_->make_cur_fn && impl_->destroy_ctx_fn) {
            impl_->make_cur_fn(impl_->dpy, None, nullptr);
            impl_->destroy_ctx_fn(impl_->dpy, impl_->ctx);
        }
        if (impl_->win) XDestroyWindow(impl_->dpy, impl_->win);
        if (impl_->cmap) XFreeColormap(impl_->dpy, impl_->cmap);
        XCloseDisplay(impl_->dpy);
    }
    delete impl_;
    impl_ = nullptr;
}

bool PlatformWindow::set_vsync(bool enabled) {
    if (!impl_ || !impl_->dpy) return false;
    int v = enabled ? 1 : 0;
    if (impl_->swap_ext_fn) {
        impl_->swap_ext_fn(impl_->dpy, impl_->win, v);
        return true;
    }
    return impl_->swap_mesa_fn && impl_->swap_mesa_fn(static_cast<unsigned int>(v)) == 0;
}

void PlatformWindow::poll_events() {
    std::memset(key_pressed_, 0, sizeof(key_pressed_));
    mouse_left_clicked_ = false;
    mouse_dx_ = 0.0f;
    mouse_dy_ = 0.0f;
    if (!impl_ || !impl_->dpy) return;

    while (XPending(impl_->dpy) > 0) {
        XEvent ev{};
        XNextEvent(impl_->dpy, &ev);
        switch (ev.type) {
            case FocusIn: focused_ = true; break;
            case FocusOut:
                focused_ = false;
                mouse_left_ = mouse_right_ = false;
                std::memset(key_down_, 0, sizeof(key_down_));
                break;
            case ClientMessage:
                if (static_cast<Atom>(ev.xclient.data.l[0]) == impl_->wm_delete) {
                    should_close_ = true;
                }
                break;
            case ConfigureNotify:
                if (ev.xconfigure.width > 0 && ev.xconfigure.height > 0) {
                    width_ = ev.xconfigure.width;
                    height_ = ev.xconfigure.height;
                }
                break;
            case KeyPress: {
                KeyCode k = map_keysym(XLookupKeysym(&ev.xkey, 0));
                if (k != KEY_NONE) {
                    if (!key_down_[k]) key_pressed_[k] = true;
                    key_down_[k] = true;
                }
                break;
            }
            case KeyRelease: {
                // Filter X11 auto-repeat pairs
                if (!impl_->detectable_repeat && XEventsQueued(impl_->dpy, QueuedAfterReading) > 0) {
                    XEvent next{};
                    XPeekEvent(impl_->dpy, &next);
                    if (next.type == KeyPress && next.xkey.time == ev.xkey.time && next.xkey.keycode == ev.xkey.keycode) {
                        break;
                    }
                }
                KeyCode k = map_keysym(XLookupKeysym(&ev.xkey, 0));
                if (k != KEY_NONE) key_down_[k] = false;
                break;
            }
            case ButtonPress:
                if (ev.xbutton.button == Button1) {
                    mouse_left_ = true;
                    mouse_left_clicked_ = true;
                    mouse_x_ = ev.xbutton.x;
                    mouse_y_ = ev.xbutton.y;
                } else if (ev.xbutton.button == Button3) {
                    mouse_right_ = true;
                    mouse_x_ = ev.xbutton.x;
                    mouse_y_ = ev.xbutton.y;
                }
                break;
            case ButtonRelease:
                if (ev.xbutton.button == Button1) mouse_left_ = false;
                if (ev.xbutton.button == Button3) mouse_right_ = false;
                break;
            case MotionNotify: {
                int nx = ev.xmotion.x;
                int ny = ev.xmotion.y;
                if (warp_pending_ && nx == width_ / 2 && ny == height_ / 2) {
                    // Swallow synthetic XWarpPointer motion event so mouse yoke never jumps!
                    warp_pending_ = false;
                    mouse_x_ = nx;
                    mouse_y_ = ny;
                    break;
                }
                if (mouse_left_ || mouse_right_ || (relative_mouse_ && focused_)) {
                    mouse_dx_ += static_cast<float>(nx - mouse_x_);
                    mouse_dy_ += static_cast<float>(ny - mouse_y_);
                }
                mouse_x_ = nx;
                mouse_y_ = ny;
                break;
            }
            default:
                break;
        }
    }
    if (relative_mouse_ && focused_ && (mouse_x_ != width_ / 2 || mouse_y_ != height_ / 2)) {
        mouse_x_ = width_ / 2;
        mouse_y_ = height_ / 2;
        warp_pending_ = true;
        XWarpPointer(impl_->dpy, None, impl_->win, 0, 0, 0, 0, mouse_x_, mouse_y_);
        XFlush(impl_->dpy);
    }
}

void PlatformWindow::set_relative_mouse(bool enabled) {
    if (relative_mouse_ == enabled) return;
    relative_mouse_ = enabled;
    mouse_dx_ = mouse_dy_ = 0.0f;
    if (enabled && focused_ && impl_ && impl_->dpy && impl_->win) {
        mouse_x_ = width_ / 2;
        mouse_y_ = height_ / 2;
        warp_pending_ = true;
        XWarpPointer(impl_->dpy, None, impl_->win, 0, 0, 0, 0, mouse_x_, mouse_y_);
        XFlush(impl_->dpy);
    } else {
        warp_pending_ = false;
    }
}

void PlatformWindow::swap_buffers() {
    if (impl_ && impl_->dpy && impl_->swap_fn) {
        impl_->swap_fn(impl_->dpy, impl_->win);
    }
}
#endif

double PlatformWindow::now_sec() {
    using clock = std::chrono::steady_clock;
    static const auto t0 = clock::now();
    return std::chrono::duration<double>(clock::now() - t0).count();
}

void PlatformWindow::sleep_sec(double seconds) {
    if (seconds <= 0.0) return;
#ifdef _WIN32
    const double deadline = now_sec() + seconds;
    // Use Windows 10+ high-resolution waitable timer + short spin tail to avoid
    // Windows' default 15.625 ms timer tick stalling 60 FPS pacing into 30-40 FPS.
    struct HiResTimer {
        HANDLE h = nullptr;
        HiResTimer() {
            using CreateExFn = HANDLE (WINAPI *)(LPSECURITY_ATTRIBUTES, LPCWSTR, DWORD, DWORD);
            HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
            auto create_ex = k32 ? reinterpret_cast<CreateExFn>(reinterpret_cast<void *>(
                                       GetProcAddress(k32, "CreateWaitableTimerExW")))
                                 : nullptr;
            if (create_ex) {
                constexpr DWORD kHighRes = 0x00000002u;
                h = create_ex(nullptr, nullptr, kHighRes, TIMER_MODIFY_STATE | SYNCHRONIZE);
            }
            if (!h) h = CreateWaitableTimerW(nullptr, TRUE, nullptr);
        }
        ~HiResTimer() {
            if (h) CloseHandle(h);
        }
    };
    thread_local HiResTimer timer;
    for (;;) {
        const double rem = deadline - now_sec();
        if (rem <= 0.0) break;
        if (rem > 0.00085) {
            const double wait_s = rem - 0.00045;
            if (timer.h) {
                LARGE_INTEGER due{};
                due.QuadPart = -static_cast<LONGLONG>(wait_s * 1.0e7);
                if (due.QuadPart < 0 && SetWaitableTimer(timer.h, &due, 0, nullptr, nullptr, FALSE)) {
                    WaitForSingleObject(timer.h, static_cast<DWORD>(wait_s * 1000.0) + 2u);
                    continue;
                }
            }
            const DWORD ms = static_cast<DWORD>(wait_s * 1000.0);
            if (ms > 0) Sleep(ms);
            else SwitchToThread();
        } else if (rem > 0.00018) {
            SwitchToThread();
        } else {
            YieldProcessor();
        }
    }
#else
    std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
#endif
}

PixelRect PlatformWindow::presentation_rect(int output_w, int output_h) const {
    return fit_rect({0, 0, width_, height_}, output_w, output_h);
}

bool PlatformWindow::save_framebuffer_png(const std::string &path, int width, int height) {
    if (width <= 0 || height <= 0) return false;
    thread_local std::vector<uint8_t> pixels;
    pixels.resize(static_cast<size_t>(width) * height * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
    // Flip vertically (OpenGL bottom-up -> PNG/PPM top-down)
    const size_t row_bytes = static_cast<size_t>(width) * 3;
    thread_local std::vector<uint8_t> row;
    row.resize(row_bytes);
    for (int y = 0; y < height / 2; y++) {
        uint8_t *a = pixels.data() + static_cast<size_t>(y) * row_bytes;
        uint8_t *b = pixels.data() + static_cast<size_t>(height - 1 - y) * row_bytes;
        std::memcpy(row.data(), a, row_bytes);
        std::memcpy(a, b, row_bytes);
        std::memcpy(b, row.data(), row_bytes);
    }
    if (path.size() >= 4 && path.compare(path.size() - 4, 4, ".ppm") == 0) {
        FILE *fp = std::fopen(path.c_str(), "wb");
        if (!fp) return false;
        std::fprintf(fp, "P6\n%d %d\n255\n", width, height);
        const bool ok = std::fwrite(pixels.data(), 1, pixels.size(), fp) == pixels.size();
        std::fclose(fp);
        return ok;
    }
    return stbi_write_png(path.c_str(), width, height, 3, pixels.data(), width * 3) != 0;
}

} // namespace nsim
