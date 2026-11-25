#include "platform_window.hpp"
#include "gl_loader.hpp"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../third_party/stb_image_write.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

#ifndef _WIN32
#include <X11/Xlib.h>
#include <X11/XKBlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <dlfcn.h>
#endif

namespace nsim {

#ifndef _WIN32
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

PixelRect fit_rect(PixelRect bounds, int aspect_w, int aspect_h) {
    int w = std::min(bounds.w, static_cast<int>(1LL * bounds.h * aspect_w / aspect_h));
    int h = static_cast<int>(1LL * w * aspect_h / aspect_w);
    return {bounds.x + (bounds.w - w) / 2, bounds.y + (bounds.h - h) / 2, w, h};
}

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

bool PlatformWindow::create(const char *title, int aspect_w, int aspect_h, DisplayMode mode, bool visible) {
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
        const int w = std::min(monitor.w * 9 / 10, work.w);
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
    impl_->swap_ext_fn = reinterpret_cast<PFN_glXSwapIntervalEXT>(get_proc("glXSwapIntervalEXT"));
    impl_->swap_mesa_fn = reinterpret_cast<PFN_glXSwapIntervalMESA>(get_proc("glXSwapIntervalMESA"));

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

void PlatformWindow::set_vsync(bool enabled) {
    if (!impl_ || !impl_->dpy) return;
    int v = enabled ? 1 : 0;
    if (impl_->swap_ext_fn) impl_->swap_ext_fn(impl_->dpy, impl_->win, v);
    else if (impl_->swap_mesa_fn) impl_->swap_mesa_fn(static_cast<unsigned int>(v));
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

PixelRect PlatformWindow::presentation_rect(int output_w, int output_h) const {
    return fit_rect({0, 0, width_, height_}, output_w, output_h);
}

bool PlatformWindow::save_framebuffer_png(const std::string &path, int width, int height) {
    if (width <= 0 || height <= 0) return false;
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
    // Flip vertically (OpenGL bottom-up -> PNG top-down)
    const size_t row_bytes = static_cast<size_t>(width) * 3;
    std::vector<uint8_t> row(row_bytes);
    for (int y = 0; y < height / 2; y++) {
        uint8_t *a = pixels.data() + static_cast<size_t>(y) * row_bytes;
        uint8_t *b = pixels.data() + static_cast<size_t>(height - 1 - y) * row_bytes;
        std::memcpy(row.data(), a, row_bytes);
        std::memcpy(a, b, row_bytes);
        std::memcpy(b, row.data(), row_bytes);
    }
    return stbi_write_png(path.c_str(), width, height, 3, pixels.data(), width * 3) != 0;
}

} // namespace nsim
