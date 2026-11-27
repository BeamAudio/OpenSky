#include "flight_profiler.hpp"
#include "platform_window.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numeric>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#endif

namespace nsim {
namespace {

#ifdef _WIN32
double filetime_seconds(const FILETIME &ft) {
    ULARGE_INTEGER ui{};
    ui.LowPart = ft.dwLowDateTime;
    ui.HighPart = ft.dwHighDateTime;
    return static_cast<double>(ui.QuadPart) * 1.0e-7;
}

double process_cpu_seconds() {
    FILETIME c{}, e{}, k{}, u{};
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return -1.0;
    return filetime_seconds(k) + filetime_seconds(u);
}

double thread_cpu_seconds() {
    FILETIME c{}, e{}, k{}, u{};
    if (!GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u)) return -1.0;
    return filetime_seconds(k) + filetime_seconds(u);
}

void sample_thermals(int64_t &cpu_temp_millic, int64_t &gpu_temp_millic, int64_t &) {
    // Dynamically query Windows Performance Data Helper (pdh.dll) ACPI Thermal Zone temperature
    // without requiring admin privileges or external libraries for live thermal monitoring.
    using PdhHandle = void *;
    struct PdhFmtVal { DWORD status; union { LONG lval; double dval; LONGLONG llval; }; };
    struct PdhFmtItem { const wchar_t *name; PdhFmtVal val; };
    using FnOpenQueryW = LONG(WINAPI *)(const wchar_t *, DWORD_PTR, PdhHandle *);
    using FnAddCounterW = LONG(WINAPI *)(PdhHandle, const wchar_t *, DWORD_PTR, PdhHandle *);
    using FnCollect = LONG(WINAPI *)(PdhHandle);
    using FnGetArrayW = LONG(WINAPI *)(PdhHandle, DWORD, DWORD *, DWORD *, PdhFmtItem *);

    struct WinThermalPdh {
        bool init_done = false;
        bool ready = false;
        PdhHandle query = nullptr;
        PdhHandle counter = nullptr;
        FnCollect collect = nullptr;
        FnGetArrayW get_array = nullptr;
        double last_sample_s = -10.0;
        int64_t cached_mc = -1;
    };
    static WinThermalPdh s;

    const double now_s = PlatformWindow::now_sec();
    if (s.init_done && (now_s - s.last_sample_s) < 0.45) {
        if (s.cached_mc > 0) {
            cpu_temp_millic = std::max(cpu_temp_millic, s.cached_mc);
            gpu_temp_millic = std::max(gpu_temp_millic, s.cached_mc);
        }
        return;
    }
    s.last_sample_s = now_s;

    if (!s.init_done) {
        s.init_done = true;
        HMODULE mod = LoadLibraryW(L"pdh.dll");
        if (mod) {
            auto open_q = reinterpret_cast<FnOpenQueryW>(reinterpret_cast<void *>(GetProcAddress(mod, "PdhOpenQueryW")));
            auto add_c = reinterpret_cast<FnAddCounterW>(reinterpret_cast<void *>(GetProcAddress(mod, "PdhAddEnglishCounterW")));
            if (!add_c) add_c = reinterpret_cast<FnAddCounterW>(reinterpret_cast<void *>(GetProcAddress(mod, "PdhAddCounterW")));
            s.collect = reinterpret_cast<FnCollect>(reinterpret_cast<void *>(GetProcAddress(mod, "PdhCollectQueryData")));
            s.get_array = reinterpret_cast<FnGetArrayW>(reinterpret_cast<void *>(GetProcAddress(mod, "PdhGetFormattedCounterArrayW")));
            if (open_q && add_c && s.collect && s.get_array &&
                open_q(nullptr, 0, &s.query) == 0 &&
                add_c(s.query, L"\\Thermal Zone Information(*)\\Temperature", 0, &s.counter) == 0) {
                s.ready = true;
            }
        }
    }
    if (!s.ready) return;

    if (s.collect(s.query) != 0) return;
    constexpr DWORD kPdhFmtDouble = 0x00000200;
    DWORD buf_sz = 0, item_cnt = 0;
    s.get_array(s.counter, kPdhFmtDouble, &buf_sz, &item_cnt, nullptr);
    if (buf_sz == 0 || buf_sz > 65536) return;
    std::vector<uint8_t> buf(buf_sz);
    if (s.get_array(s.counter, kPdhFmtDouble, &buf_sz, &item_cnt,
                    reinterpret_cast<PdhFmtItem *>(buf.data())) != 0) return;

    const auto *items = reinterpret_cast<const PdhFmtItem *>(buf.data());
    int64_t peak_mc = -1;
    for (DWORD i = 0; i < item_cnt; ++i) {
        if (items[i].val.status != 0) continue;
        const double raw = items[i].val.dval;
        // ACPI thermal zone counters report Kelvin (~270..410 K) or tenths of Kelvin (~2700..4100).
        double c = -1.0;
        if (raw >= 270.0 && raw <= 410.0) c = raw - 273.15;
        else if (raw >= 2700.0 && raw <= 4100.0) c = raw * 0.1 - 273.15;
        if (c > 15.0 && c < 125.0) {
            peak_mc = std::max(peak_mc, static_cast<int64_t>(std::llround(c * 1000.0)));
        }
    }
    s.cached_mc = peak_mc;
    if (peak_mc > 0) {
        cpu_temp_millic = std::max(cpu_temp_millic, peak_mc);
        gpu_temp_millic = std::max(gpu_temp_millic, peak_mc);
    }
}
std::filesystem::path render_device() { return {}; }
#else
int64_t read_number(const std::filesystem::path &path) {
    std::ifstream in(path);
    int64_t value;
    return (in >> value) ? value : -1;
}

void sample_thermals(int64_t &cpu_temp_millic, int64_t &gpu_temp_millic, int64_t &gpu_power_microwatts) {
    std::error_code ec;
    const std::filesystem::path root("/sys/class/hwmon");
    for (const auto &entry : std::filesystem::directory_iterator(root, ec)) {
        if (ec) break;
        std::ifstream name_file(entry.path() / "name");
        std::string name;
        name_file >> name;
        const bool cpu = name == "k10temp" || name == "coretemp";
        const bool gpu = name == "amdgpu" || name.find("gpu") != std::string::npos;
        if (!cpu && !gpu) continue;

        for (const auto &sensor : std::filesystem::directory_iterator(entry.path(), ec)) {
            if (ec) break;
            const std::string file = sensor.path().filename().string();
            if (file.starts_with("temp") && file.ends_with("_input")) {
                int64_t value = read_number(sensor.path());
                if (value < 0) continue;
                int64_t &target = cpu ? cpu_temp_millic : gpu_temp_millic;
                target = std::max(target, value);
            } else if (gpu && (file == "power1_input" || file == "power1_average")) {
                int64_t value = read_number(sensor.path());
                if (value >= 0) gpu_power_microwatts = std::max(gpu_power_microwatts, value);
            }
        }
    }
}

void sample_memory_file(const char *path, const char *key_a, int64_t &a, const char *key_b, int64_t &b) {
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (line.starts_with(key_a)) a = std::strtoll(line.c_str() + std::char_traits<char>::length(key_a), nullptr, 10);
        if (line.starts_with(key_b)) b = std::strtoll(line.c_str() + std::char_traits<char>::length(key_b), nullptr, 10);
    }
}

std::filesystem::path render_device() {
    // GLX may offload rendering. Select the DRM node the process actually opened.
    std::error_code ec;
    std::filesystem::path device;
    for (const auto &fd : std::filesystem::directory_iterator("/proc/self/fd", ec)) {
        auto path = std::filesystem::read_symlink(fd.path(), ec);
        if (ec || path.parent_path() != "/dev/dri" || !path.filename().string().starts_with("renderD")) continue;
        auto candidate = std::filesystem::path("/sys/class/drm") / path.filename() / "device";
        if (!device.empty() && candidate != device) return {};
        device = candidate;
    }
    return device;
}

double cpu_seconds(int who) {
    rusage ru{};
    if (getrusage(who, &ru) != 0) return -1.0;
    return ru.ru_utime.tv_sec + ru.ru_stime.tv_sec + (ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) * 1.0e-6;
}
#endif

} // namespace

FlightProfiler::FlightProfiler() {
    start_sec_ = stage_sec_ = PlatformWindow::now_sec();
    latest_gpu_ms_.fill(-1.0);
}

FlightProfiler::~FlightProfiler() {
    if (gpu_initialized_) {
        for (auto &entry : queries_) glDeleteQueries(PassCount * 2, entry.ids.data());
    }
}

bool FlightProfiler::open(const std::filesystem::path &directory) {
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) return false;
    startup_.open(directory / "startup.csv");
    frames_.open(directory / "frames.csv");
    gpu_.open(directory / "gpu_pass_times.csv");
    resources_.open(directory / "resources.csv");
    metadata_.open(directory / "metadata.txt");
    if (!startup_ || !frames_ || !gpu_ || !resources_ || !metadata_) return false;
    startup_ << "stage,stage_ms,since_start_ms\n";
    frames_ << "scene,frame,quality,since_start_ms,frame_ms,input_physics_ms,weather_main_ms,"
               "prepare_ms,present_ms,scene_w,scene_h,pages_resident,pages_queued,pages_uploaded,"
               "pages_uploaded_total,pages_built_total,worker_build_total_ms,worker_bc7_total_ms,"
               "upload_cpu_ms,logical_upload_bytes,max_draw_lod,pages_l7,pages_l8,pages_l9,"
               "weather_steps_total,weather_uploads_total,weather_solve_total_ms,weather_publish_total_ms,"
               "ortho_provider,online_cache_uses_total,online_downloads_total\n";
    gpu_ << "scene,frame,quality";
    for (auto *name : pass_names) gpu_ << ',' << name << "_gpu_ms," << name << "_cpu_submit_ms";
    gpu_ << '\n';
    resources_ << "since_start_ms,process_cpu_cores,main_cpu_pct,rss_kib,peak_rss_kib,host_available_kib,host_total_kib,"
                  "device_gpu_busy_pct,device_vram_used_bytes,device_vram_total_bytes,device_gtt_used_bytes,"
                  "cpu_temp_millic,gpu_temp_millic,gpu_power_microwatts\n";
    return true;
}

void FlightProfiler::startup_stage(const char *stage) {
    if (!startup_.is_open()) return;
    const double now = PlatformWindow::now_sec();
    startup_ << stage << ',' << (now - stage_sec_) * 1000 << ',' << (now - start_sec_) * 1000 << std::endl;
    stage_sec_ = now;
}

void FlightProfiler::metadata(int width, int height, const char *format, const char *provider,
                             int terrain_workers, size_t terrain_storage_bytes) {
    if (!metadata_.is_open()) return;
    metadata_ << "renderer=" << glGetString(GL_RENDERER) << '\n'
              << "opengl=" << glGetString(GL_VERSION) << '\n'
              << "output=" << width << 'x' << height << '\n'
              << "ortho=" << format << '\n' << "ortho_provider=" << provider << '\n'
              << "terrain_workers=" << terrain_workers << '\n'
              << "terrain_gpu_logical_storage_bytes=" << terrain_storage_bytes << '\n'
              << "drm_device=" << drm_device_.string() << '\n';
    metadata_.flush();
}

void FlightProfiler::init_gpu() {
    drm_device_ = render_device();
    for (auto &entry : queries_) glGenQueries(PassCount * 2, entry.ids.data());
    gpu_initialized_ = true;
}

void FlightProfiler::set_scene(const std::string &name) {
    scene_ = name;
    reset_frame_history();
}

void FlightProfiler::reset_frame_history() {
    last_frame_sec_ = 0.0;
    history_pos_ = history_count_ = 0;
}

void FlightProfiler::start_frame(uint64_t frame, int quality) {
    active_ = nullptr;
    if (!enabled()) return;
    frame_ = frame;
    quality_ = quality;
    collect_gpu();
    if (frame % 4 != 0) return;
    auto &entry = queries_[(frame / 4) % queries_.size()];
    if (entry.pending) {
        ++dropped_queries_;
        return;
    }
    entry.pending = true;
    entry.frame = frame;
    entry.quality = quality;
    entry.scene = scene_;
    active_ = &entry;
}

void FlightProfiler::begin(Pass pass) {
    if (!active_) return;
    query_begin_sec_ = PlatformWindow::now_sec();
    glQueryCounter(active_->ids[2 * pass], GL_TIMESTAMP);
}

void FlightProfiler::end(Pass pass) {
    if (!active_) return;
    glQueryCounter(active_->ids[2 * pass + 1], GL_TIMESTAMP);
    active_->submit_ms[pass] = (PlatformWindow::now_sec() - query_begin_sec_) * 1000.0;
}

double FlightProfiler::cpu_start() const {
    return enabled() ? PlatformWindow::now_sec() : 0.0;
}

void FlightProfiler::cpu_end(CpuPhase phase, double start) {
    if (start > 0.0) cpu_phases_ms_[phase] += (PlatformWindow::now_sec() - start) * 1000.0;
}

void FlightProfiler::collect_gpu() {
    for (auto &entry : queries_) {
        if (!entry.pending || entry.frame == frame_) continue;
        GLint available = GL_FALSE;
        glGetQueryObjectiv(entry.ids.back(), GL_QUERY_RESULT_AVAILABLE, &available);
        if (!available) continue;
        std::array<double, PassCount> durations{};
        for (int pass = 0; pass < PassCount; ++pass) {
            GLuint64 first_ns = 0, last_ns = 0;
            glGetQueryObjectui64v(entry.ids[2 * pass], GL_QUERY_RESULT, &first_ns);
            glGetQueryObjectui64v(entry.ids[2 * pass + 1], GL_QUERY_RESULT, &last_ns);
            durations[pass] = last_ns >= first_ns ? (last_ns - first_ns) / 1.0e6 : -1.0;
        }
        if (entry.frame >= latest_gpu_frame_) {
            latest_gpu_frame_ = entry.frame;
            latest_gpu_ms_ = durations;
            latest_cpu_ms_ = entry.submit_ms;
        }
        if (gpu_.is_open()) {
            gpu_ << entry.scene << ',' << entry.frame << ',' << entry.quality;
            for (int pass = 0; pass < PassCount; ++pass)
                gpu_ << ',' << durations[pass] << ',' << entry.submit_ms[pass];
            gpu_ << '\n';
        }
        entry.pending = false;
    }
}

void FlightProfiler::finish() {
    if (!gpu_.is_open()) return;
    glFinish(); // Drain only after the final measured frame.
    frame_ = 0;
    collect_gpu();
    metadata_ << "gpu_samples_dropped=" << dropped_queries_ << '\n';
}

void FlightProfiler::sample_host(double now) {
    if (now - last_host_sec_ < 1.0) return;
#ifdef _WIN32
    const double proc = process_cpu_seconds(), thread = thread_cpu_seconds();
#else
    const double proc = cpu_seconds(RUSAGE_SELF), thread = cpu_seconds(RUSAGE_THREAD);
#endif
    const double dt = now - last_host_sec_;
    if (last_host_sec_ > 0.0 && std::min(proc, proc_cpu_sec_) >= 0.0) host_.cpu_cores = (proc - proc_cpu_sec_) / dt;
    if (last_host_sec_ > 0.0 && std::min(thread, thread_cpu_sec_) >= 0.0) host_.main_cpu_percent = (thread - thread_cpu_sec_) / dt * 100.0;
    proc_cpu_sec_ = proc;
    thread_cpu_sec_ = thread;
    last_host_sec_ = now;
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        host_.rss_kib = static_cast<int64_t>(pmc.WorkingSetSize / 1024u);
        host_.peak_rss_kib = static_cast<int64_t>(pmc.PeakWorkingSetSize / 1024u);
    }
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        host_.available_kib = static_cast<int64_t>(ms.ullAvailPhys / 1024u);
        host_.total_kib = static_cast<int64_t>(ms.ullTotalPhys / 1024u);
    }
#else
    sample_memory_file("/proc/self/status", "VmRSS:", host_.rss_kib, "VmHWM:", host_.peak_rss_kib);
    sample_memory_file("/proc/meminfo", "MemAvailable:", host_.available_kib, "MemTotal:", host_.total_kib);
    if (!drm_device_.empty()) {
        host_.gpu_busy_pct = read_number(drm_device_ / "gpu_busy_percent");
        host_.vram_used_bytes = read_number(drm_device_ / "mem_info_vram_used");
        host_.vram_total_bytes = read_number(drm_device_ / "mem_info_vram_total");
        host_.gtt_used_bytes = read_number(drm_device_ / "mem_info_gtt_used");
    }
#endif
    sample_thermals(host_.cpu_temp_millic, host_.gpu_temp_millic, host_.gpu_power_microwatts);
    if (resources_.is_open()) {
        resources_ << (now - start_sec_) * 1000 << ',' << host_.cpu_cores << ',' << host_.main_cpu_percent
                   << ',' << host_.rss_kib << ',' << host_.peak_rss_kib << ',' << host_.available_kib << ',' << host_.total_kib
                   << ',' << host_.gpu_busy_pct << ',' << host_.vram_used_bytes << ',' << host_.vram_total_bytes
                   << ',' << host_.gtt_used_bytes << ',' << host_.cpu_temp_millic << ',' << host_.gpu_temp_millic
                   << ',' << host_.gpu_power_microwatts << '\n';
    }
}

void FlightProfiler::update_percentiles(double now) {
    if (history_count_ == 0 || now - last_stats_sec_ < 0.25) return;
    last_stats_sec_ = now;
    auto sorted = frame_history_;
    std::sort(sorted.begin(), sorted.begin() + history_count_);
    frame_avg_ms_ = std::accumulate(sorted.begin(), sorted.begin() + history_count_, 0.0) / history_count_;
    frame_p95_ms_ = sorted[static_cast<size_t>(std::ceil(history_count_ * 0.95)) - 1];
    frame_p99_ms_ = sorted[static_cast<size_t>(std::ceil(history_count_ * 0.99)) - 1];
}

void FlightProfiler::end_frame(const StreamStats &stream, const WeatherWorkStats &weather,
                               const PackReader &pack, int max_lod, int scene_w, int scene_h) {
    if (!enabled()) {
        last_frame_sec_ = 0.0;
        return;
    }
    const double now = PlatformWindow::now_sec();
    const double elapsed_ms = last_frame_sec_ == 0.0 ? -1.0 : (now - last_frame_sec_) * 1000.0;
    last_frame_sec_ = now;
    if (elapsed_ms >= 0.0) {
        frame_history_[history_pos_] = elapsed_ms;
        history_pos_ = (history_pos_ + 1) % frame_history_.size();
        history_count_ = std::min(history_count_ + 1, frame_history_.size());
    }
    latest_stream_ = stream;
    latest_phases_ms_ = cpu_phases_ms_;
    cpu_phases_ms_.fill(0.0);
    update_percentiles(now);
    sample_host(now);
    if (!frames_.is_open()) return;
    frames_ << scene_ << ',' << frame_ << ',' << quality_ << ',' << (now - start_sec_) * 1000 << ',' << elapsed_ms;
    for (double ms : latest_phases_ms_) frames_ << ',' << ms;
    frames_ << ',' << scene_w << ',' << scene_h << ',' << stream.resident_pages << ',' << stream.queued_jobs
            << ',' << stream.uploads_this_frame << ',' << stream.total_uploads << ',' << stream.finished_pages
            << ',' << stream.worker_build_total_ms << ',' << stream.worker_encode_total_ms << ',' << stream.last_upload_ms
            << ',' << stream.gpu_page_bytes * stream.uploads_this_frame << ',' << max_lod << ',' << stream.resident_by_lod[7]
            << ',' << stream.resident_by_lod[8] << ',' << stream.resident_by_lod[9] << ','
            << weather.steps << ',' << weather.uploads << ',' << weather.solve_ms << ',' << weather.publish_ms << ','
            << pack.online_provider_name() << ',' << pack.online_tiles_used() << ',' << pack.online_tiles_fetched() << '\n';
}

double FlightProfiler::encode_ms_per_page() const {
    return latest_stream_.finished_pages ? latest_stream_.worker_encode_total_ms / latest_stream_.finished_pages : 0.0;
}

double FlightProfiler::build_ms_per_page() const {
    return latest_stream_.finished_pages ? latest_stream_.worker_build_total_ms / latest_stream_.finished_pages : 0.0;
}

double FlightProfiler::peak_package_temp_c() {
    int64_t cpu_mc = -1, gpu_mc = -1, gpu_uw = -1;
    sample_thermals(cpu_mc, gpu_mc, gpu_uw);
    int64_t peak = std::max(cpu_mc, gpu_mc);
    return (peak > 0) ? (static_cast<double>(peak) * 0.001) : -1.0;
}

} // namespace nsim
