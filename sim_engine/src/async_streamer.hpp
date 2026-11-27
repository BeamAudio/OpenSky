#pragma once

#include "gl_loader.hpp"
#include "math3d.hpp"
#include "pack_reader.hpp"

#include <atomic>
#include <array>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace nsim {

inline constexpr size_t bc7_square_level_bytes(int n) {
    return static_cast<size_t>((n + 3) / 4) * static_cast<size_t>((n + 3) / 4) * 16u;
}

struct QuadKey {
    uint8_t lod = 0;
    uint16_t qx = 0;
    uint16_t qy = 0;

    uint64_t pack() const {
        return (static_cast<uint64_t>(lod) << 40) | (static_cast<uint64_t>(qx) << 20) | static_cast<uint64_t>(qy);
    }
    static QuadKey unpack(uint64_t k) {
        return {
            static_cast<uint8_t>((k >> 40) & 0xFFu),
            static_cast<uint16_t>((k >> 20) & 0xFFFFFu),
            static_cast<uint16_t>(k & 0xFFFFFu)
        };
    }
    QuadKey parent() const {
        return {static_cast<uint8_t>(lod > 0 ? lod - 1 : 0), static_cast<uint16_t>(qx >> 1), static_cast<uint16_t>(qy >> 1)};
    }
};

struct StreamStats {
    int resident_pages = 0;
    int max_pages = 0;
    int queued_jobs = 0;
    int uploads_this_frame = 0;
    int total_uploads = 0;
    double last_upload_ms = 0.0;
    double avg_worker_ms = 0.0;
    uint64_t finished_pages = 0;
    double worker_build_total_ms = 0.0;
    double worker_encode_total_ms = 0.0;
    size_t gpu_page_bytes = 0;
    std::array<int, 12> resident_by_lod{};
    int z20_pages = 0;
    int z20_tiles_ready = 0;
    int z20_tiles_missing = 0;
};

enum class OrthoFormat { BC7, BC7Fast, RGBA8 };

// Workers build terrain pages and place them in persistent PBO staging slots.
// The render thread uploads completed slots to the resident texture arrays.
class AsyncTerrainStreamer {
public:
    static constexpr double WORLD_SPAN_M = 163840.0; // 163.84 km corridor
    static constexpr int MAX_LOD = 9;                // L0=163.84km .. L9=320m (0.625m/px @ 512 ortho!)
    // 512 resident pages (~240 MB GPU) prevents L6-L9 thrash in full valleys while reserving plenty of VRAM headroom.
    static constexpr int MAX_PAGES = 512;
    static constexpr int HEIGHT_N = 67;              // 65x65 patch verts + 1-texel seamless gutter (4x terrain mesh detail!)
    static constexpr int ORTHO_RES = 512;            // 512x512 ortho per page (4x sharper IGN PNOA!)
    static constexpr int ORTHO_MIPS = 6;             // 512, 256, 128, 64, 32, 16 (pre-built off-thread!)
    static constexpr int AUX_RES = 129;              // 129x129 Normal+AO & Surface/Shadow/Water/PMRF (4x sharper cliffs & PMRF!)

    AsyncTerrainStreamer() = default;
    ~AsyncTerrainStreamer();

    bool init(const PackReader *pack, int worker_count = 0, OrthoFormat format = OrthoFormat::BC7Fast,
              int page_capacity = MAX_PAGES);
    void shutdown();

    // Synchronously pre-warm coarse root pages (lod 0..2 = 21 pages) so frame 1 covers the full 163km horizon!
    void prewarm_coarse(const Vec3 &sun_dir);
    // Synchronously flush high-priority pages around a teleport target (used by --autotest & Tab jump)
    void flush_pending_sync(int max_pages_to_drain = 160, double timeout_sec = 2.5);
    // Re-queue currently active resident pages in-place when user jumps time of day (zero blank flash!)
    void invalidate_shadows();
    // Rebuild active pages and evict stale ones after changing imagery providers.
    void invalidate_close_pages();
    void refine_online_pages(double now_sec);
    // Drop queued pages left behind after a QA teleport; resident pages remain available.
    void clear_pending_requests();
    // Clear all resident pages and prewarm coarse root pages cleanly when jumping across regions!
    void invalidate_all_and_prewarm(const Vec3 &sun_dir);
    void begin_async_transition(std::shared_ptr<const PackReader::WorldFrame> next_frame,
                                const Vec3 &sun_dir,
                                double lat_deg, double lon_deg,
                                double vel_e_mps, double vel_n_mps);
    bool has_async_transition() const { return pending_gen_.load(std::memory_order_acquire) != 0; }
    bool is_async_transition_ready(double elapsed_sec = 0.0) const;
    void commit_async_transition();
    void cancel_async_transition();
    int worker_count() const { return static_cast<int>(workers_.size()); }
    void set_anisotropy(int max_aniso);

    void set_sun_direction(const Vec3 &sun_dir);
    void begin_frame(uint64_t frame_idx);

    // Lookup resident GPU array slice (-1 if not yet streamed).
    // Pass touch_lru=false for off-frustum/prefetch lookups so passed tiles are evicted cleanly.
    int find_resident(uint64_t key, float now_sec, float *out_fade = nullptr,
                      bool touch_lru = true, float importance = 0.0f);
    // Queue a page with velocity-predictive + deterministic stochastic jitter priority (higher = sooner).
    void request_page(uint64_t key, float priority);
    // Frame-time-budgeted randomized GPU DMA uploader (never saturates low-end iGPUs!).
    void pump_gpu_uploads(float now_sec, int max_uploads = 2, double budget_ms = 0.45);

    GLuint height_array_tex() const { return height_arr_; }
    GLuint ortho_array_tex() const { return ortho_arr_; }
    GLuint norm_array_tex() const { return norm_arr_; }
    GLuint surf_array_tex() const { return surf_arr_; }
    StreamStats stats() const;

    static void quad_bounds_en(QuadKey k, double &out_center_e, double &out_center_n, double &out_half_span);

private:
    struct CacheSlot {
        uint64_t key = UINT64_MAX;
        uint64_t generation = 0;
        uint64_t last_used_frame = 0;
        float ready_time_sec = 0.0f;
        float fade_dur_sec = 0.65f; // Per-page randomized arrival duration (0.18..0.30s) for non-grid organic dissolve!
        float last_importance = 0.0f;
        bool pinned = false;        // Coarse root pages (lod <= 2) never evicted
        PageImagery imagery;
        double last_imagery_upload_sec = 0.0;
    };

    enum class StageState : uint8_t { FREE = 0, FILLING = 1, READY = 2, UPLOADING = 3 };

    struct StagingSlot {
        GLuint pbo = 0;
        uint8_t *mapped = nullptr;
        GLsync fence = nullptr;
        uint64_t key = UINT64_MAX;
        uint64_t generation = 0;
        float worker_ms = 0.0f;
        PageImagery imagery;
        std::atomic<StageState> state{StageState::FREE};
    };

    struct RequestOrder {
        float priority = 0.0f;
        uint64_t key = UINT64_MAX;
    };
    struct RequestOrderLess {
        bool operator()(const RequestOrder &a, const RequestOrder &b) const {
            if (a.priority != b.priority) return a.priority < b.priority;
            return a.key > b.key;
        }
    };
    using RequestHeap = std::priority_queue<RequestOrder, std::vector<RequestOrder>, RequestOrderLess>;

    static constexpr int NUM_STAGING = 24;
    double next_imagery_refine_sec_ = 0.0;
    static constexpr size_t SZ_HEIGHT = static_cast<size_t>(HEIGHT_N) * HEIGHT_N * sizeof(float);
    static constexpr size_t SZ_AUX = static_cast<size_t>(AUX_RES) * AUX_RES * 4;

    struct PageLayout {
        size_t height = 0, normal = 0, surface = 0;
        std::array<size_t, ORTHO_MIPS> ortho{};
        size_t ortho_bytes = 0, staging_bytes = 0;
    };
    static constexpr bool is_compressed(OrthoFormat format) { return format != OrthoFormat::RGBA8; }
    static PageLayout make_page_layout(OrthoFormat format);

    void worker_loop();
    bool coarse_root_ready() const;
    static void queue_request(std::unordered_map<uint64_t, float> &requests, RequestHeap &heap,
                              uint64_t key, float priority);
    static void rebuild_request_heap(const std::unordered_map<uint64_t, float> &requests, RequestHeap &heap);
    static bool pop_best_request(std::unordered_map<uint64_t, float> &requests, RequestHeap &heap,
                                 uint64_t &out_key, float &out_priority);
    void release_cache_slot(int idx);
    void fill_staging_slot(StagingSlot &slot, uint64_t key, const PackReader::WorldFrame &frame);
    int allocate_cache_slice(uint64_t key, float now_sec, uint64_t target_gen,
                             std::unordered_map<uint64_t, int> &target_map, bool instant_fade);
    static void downsample_rgba_box2(const uint8_t *src, int sw, int sh, uint8_t *dst);

    const PackReader *pack_ = nullptr;
    OrthoFormat format_ = OrthoFormat::BC7;
    PageLayout layout_{};
    GLuint height_arr_ = 0;
    GLuint ortho_arr_ = 0;
    GLuint norm_arr_ = 0;
    GLuint surf_arr_ = 0;

    std::vector<CacheSlot> slots_;
    int page_capacity_ = 0;
    std::vector<int> free_slots_;
    std::unordered_map<uint64_t, int> key_to_slot_;
    std::unordered_map<uint64_t, int> pending_key_to_slot_;
    std::vector<uint64_t> pending_required_keys_;
    uint64_t cur_frame_ = 1;

    StagingSlot staging_[NUM_STAGING]{};

    mutable std::mutex req_mtx_;
    std::condition_variable req_cv_;
    std::unordered_map<uint64_t, float> pending_reqs_;
    RequestHeap pending_req_heap_;
    std::unordered_map<uint64_t, uint64_t> pending_req_frame_;
    std::unordered_map<uint64_t, float> pending_reqs_next_;
    RequestHeap pending_req_heap_next_;
    std::unordered_set<uint64_t> inflight_active_;
    std::unordered_set<uint64_t> inflight_next_;
    std::atomic<uint64_t> active_gen_{1};
    std::atomic<uint64_t> pending_gen_{0};
    std::atomic<uint64_t> gen_seq_{1};
    std::shared_ptr<const PackReader::WorldFrame> world_frame_; // guarded by req_mtx_
    std::shared_ptr<const PackReader::WorldFrame> pending_frame_; // guarded by req_mtx_
    Vec3 sun_dir_{0.86f, 0.32f, 0.38f};

    std::atomic<bool> stop_{false};
    std::atomic<bool> sync_flushing_{false};
    std::vector<std::thread> workers_;

    int uploads_this_frame_ = 0;
    int total_uploads_ = 0;
    double last_upload_ms_ = 0.0;
    std::atomic<double> avg_worker_ms_{0.0};
    std::atomic<uint64_t> finished_pages_{0};
    std::atomic<uint64_t> worker_build_us_{0};
    std::atomic<uint64_t> worker_encode_us_{0};
};

} // namespace nsim
