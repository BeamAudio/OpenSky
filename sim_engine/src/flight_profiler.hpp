#pragma once

#include "async_streamer.hpp"
#include "regional_atmosphere.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <string>

namespace nsim {

class FlightProfiler {
public:
    enum Pass { Upload, Sky, Terrain, CloudDepth, World, Aircraft, CloudsPost, NativeCockpit, Hud, PassCount };
    enum CpuPhase { InputPhysics, Weather, Prepare, Present, CpuPhaseCount };
    static constexpr std::array<const char *, PassCount> pass_names = {
        "upload", "sky", "terrain", "cloud_depth", "world", "aircraft", "clouds_post", "native_cockpit", "hud"
    };

    struct HostSample {
        double cpu_cores = -1.0;
        double main_cpu_percent = -1.0;
        int64_t rss_kib = -1, peak_rss_kib = -1, available_kib = -1, total_kib = -1;
        int64_t gpu_busy_pct = -1, vram_used_bytes = -1, vram_total_bytes = -1, gtt_used_bytes = -1;
    };

    FlightProfiler();
    ~FlightProfiler();
    bool open(const std::filesystem::path &directory);
    void startup_stage(const char *stage);
    void metadata(int width, int height, const char *format, const char *provider, int terrain_workers, size_t terrain_storage_bytes);
    void init_gpu();
    void finish();
    void set_overlay(bool visible) { overlay_ = visible; }
    bool overlay() const { return overlay_; }
    bool enabled() const { return overlay_ || frames_.is_open(); }
    void set_scene(const std::string &name);

    void start_frame(uint64_t frame, int quality);
    void begin(Pass pass);
    void end(Pass pass);
    double cpu_start() const;
    void cpu_end(CpuPhase phase, double start);
    void end_frame(const StreamStats &stream, const WeatherWorkStats &weather,
                   const PackReader &pack, int max_lod, int scene_w, int scene_h,
                   bool budget_limited = false);

    const HostSample &host() const { return host_; }
    const auto &latest_gpu_ms() const { return latest_gpu_ms_; }
    const auto &latest_cpu_ms() const { return latest_cpu_ms_; }
    const auto &latest_phases_ms() const { return latest_phases_ms_; }
    double frame_avg_ms() const { return frame_avg_ms_; }
    double frame_p95_ms() const { return frame_p95_ms_; }
    double frame_p99_ms() const { return frame_p99_ms_; }
    double encode_ms_per_page() const;
    double build_ms_per_page() const;

private:
    struct QueryFrame {
        std::array<GLuint, PassCount * 2> ids{};
        std::array<double, PassCount> submit_ms{};
        uint64_t frame = 0;
        int quality = 0;
        std::string scene;
        bool pending = false;
    };

    void collect_gpu();
    void sample_host(double now);
    void update_percentiles(double now);
    void reset_frame_history();

    std::ofstream startup_, frames_, gpu_, resources_, metadata_;
    std::filesystem::path drm_device_;
    std::array<QueryFrame, 8> queries_{};
    QueryFrame *active_ = nullptr;
    std::array<double, PassCount> latest_gpu_ms_{}, latest_cpu_ms_{};
    std::array<double, CpuPhaseCount> cpu_phases_ms_{}, latest_phases_ms_{};
    HostSample host_{};
    StreamStats latest_stream_{};
    std::array<double, 240> frame_history_{};
    size_t history_pos_ = 0, history_count_ = 0;
    uint64_t frame_ = 0, latest_gpu_frame_ = 0, dropped_queries_ = 0;
    int quality_ = 0;
    std::string scene_ = "flight";
    double start_sec_ = 0.0, stage_sec_ = 0.0;
    double query_begin_sec_ = 0.0;
    double last_frame_sec_ = 0.0, last_host_sec_ = 0.0, last_stats_sec_ = 0.0;
    double proc_cpu_sec_ = 0.0, thread_cpu_sec_ = 0.0;
    double frame_avg_ms_ = 0.0, frame_p95_ms_ = 0.0, frame_p99_ms_ = 0.0;
    bool overlay_ = false, gpu_initialized_ = false;
};

} // namespace nsim
