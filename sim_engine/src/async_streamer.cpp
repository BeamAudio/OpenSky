#include "async_streamer.hpp"
#include "platform_window.hpp"
#include "bc7enc.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

namespace nsim {

namespace {

void compress_bc7_rgba_page(const uint8_t *rgba, int width, uint8_t *bc7, bool fast) {
    static std::once_flag encoder_init;
    std::call_once(encoder_init, []() { bc7enc_compress_block_init(); });

    bc7enc_compress_block_params params;
    bc7enc_compress_block_params_init(&params);
    params.m_mode_mask = (1u << 6) | (fast ? 0u : (1u << 1));
    params.m_max_partitions = fast ? 0 : 16;
    params.m_uber_level = 0;
    params.m_perceptual = true;
    params.m_try_least_squares = true;

    uint8_t block_rgba[4 * 4 * 4];
    // All supported terrain mip widths are multiples of the BC7 block width.
    const int blocks = width / 4;
    for (int by = 0; by < blocks; ++by) {
        for (int bx = 0; bx < blocks; ++bx) {
            for (int row = 0; row < 4; ++row) {
                const uint8_t *src = rgba + (static_cast<size_t>(by * 4 + row) * width + bx * 4) * 4;
                std::memcpy(block_rgba + row * 16, src, 16);
            }
            const size_t block_i = static_cast<size_t>(by) * blocks + bx;
            bc7enc_compress_block(bc7 + block_i * 16u, block_rgba, &params);
        }
    }
}

} // namespace

AsyncTerrainStreamer::~AsyncTerrainStreamer() {
    shutdown();
}

void AsyncTerrainStreamer::quad_bounds_en(QuadKey k, double &out_center_e, double &out_center_n, double &out_half_span) {
    double size = WORLD_SPAN_M / static_cast<double>(1u << k.lod);
    out_half_span = size * 0.5;
    out_center_e = -WORLD_SPAN_M * 0.5 + (static_cast<double>(k.qx) + 0.5) * size;
    out_center_n = -WORLD_SPAN_M * 0.5 + (static_cast<double>(k.qy) + 0.5) * size;
}

AsyncTerrainStreamer::PageLayout AsyncTerrainStreamer::make_page_layout(OrthoFormat format) {
    auto aligned = [](size_t n) { return (n + 255u) & ~size_t{255}; };
    PageLayout out;
    size_t offset = aligned(SZ_HEIGHT);
    const size_t base = offset;
    for (int mip = 0; mip < ORTHO_MIPS; ++mip) {
        const int n = ORTHO_RES >> mip;
        out.ortho[mip] = offset;
        offset += is_compressed(format) ? bc7_square_level_bytes(n) : static_cast<size_t>(n) * n * 4;
    }
    out.ortho_bytes = offset - base;
    out.normal = aligned(offset);
    out.surface = aligned(out.normal + SZ_AUX);
    out.staging_bytes = aligned(out.surface + SZ_AUX);
    return out;
}

bool AsyncTerrainStreamer::init(const PackReader *pack, int worker_count, OrthoFormat format) {
    pack_ = pack;
    format_ = format;
    layout_ = make_page_layout(format);
    if (!pack_) return false;

    // Allocate the resident terrain arrays.
    glCreateTextures(GL_TEXTURE_2D_ARRAY, 1, &height_arr_);
    glTextureStorage3D(height_arr_, 1, GL_R32F, HEIGHT_N, HEIGHT_N, MAX_PAGES);
    glTextureParameteri(height_arr_, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(height_arr_, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(height_arr_, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(height_arr_, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glCreateTextures(GL_TEXTURE_2D_ARRAY, 1, &ortho_arr_);
    // BC7 keeps PNOA RGB fidelity while cutting the streamed ortho page upload
    // and resident texture footprint to one byte per texel.
    const GLenum internal_format = is_compressed(format_) ? GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM : GL_SRGB8_ALPHA8;
    glTextureStorage3D(ortho_arr_, ORTHO_MIPS, internal_format, ORTHO_RES, ORTHO_RES, MAX_PAGES);
    glTextureParameteri(ortho_arr_, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTextureParameteri(ortho_arr_, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(ortho_arr_, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(ortho_arr_, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTextureParameterf(ortho_arr_, GL_TEXTURE_MAX_ANISOTROPY, 8.0f);

    glCreateTextures(GL_TEXTURE_2D_ARRAY, 1, &norm_arr_);
    glTextureStorage3D(norm_arr_, 1, GL_RGBA8, AUX_RES, AUX_RES, MAX_PAGES);
    glTextureParameteri(norm_arr_, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(norm_arr_, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(norm_arr_, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(norm_arr_, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glCreateTextures(GL_TEXTURE_2D_ARRAY, 1, &surf_arr_);
    glTextureStorage3D(surf_arr_, 1, GL_RGBA8, AUX_RES, AUX_RES, MAX_PAGES);
    glTextureParameteri(surf_arr_, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(surf_arr_, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(surf_arr_, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(surf_arr_, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // 2. Allocate Persistent-Mapped Coherent PBO Staging Ring (zero main-thread allocation/copy stalls!)
    const GLbitfield flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
    for (int i = 0; i < NUM_STAGING; i++) {
        glCreateBuffers(1, &staging_[i].pbo);
        glNamedBufferStorage(staging_[i].pbo, static_cast<GLsizeiptr>(layout_.staging_bytes), nullptr, flags);
        staging_[i].mapped = static_cast<uint8_t *>(
            glMapNamedBufferRange(staging_[i].pbo, 0, static_cast<GLsizeiptr>(layout_.staging_bytes), flags)
        );
        if (!staging_[i].mapped) {
            std::fprintf(stderr, "[STREAM] Persistent PBO map failed for slot %d\n", i);
            return false;
        }
        staging_[i].state.store(StageState::FREE, std::memory_order_relaxed);
    }

    stop_.store(false);
    // Keep background terrain streaming to 2 workers by default so CPU package
    // temperatures stay below 80 C on thin-and-light Renoir iGPU laptops.
    int nw = (worker_count > 0) ? std::clamp(worker_count, 1, 10) : 2;
    for (int i = 0; i < nw; i++) {
        workers_.emplace_back(&AsyncTerrainStreamer::worker_loop, this);
    }
    return true;
}

void AsyncTerrainStreamer::shutdown() {
    stop_.store(true);
    req_cv_.notify_all();
    for (auto &t : workers_) {
        if (t.joinable()) t.join();
    }
    workers_.clear();

    for (int i = 0; i < NUM_STAGING; i++) {
        if (staging_[i].fence) {
            glDeleteSync(staging_[i].fence);
            staging_[i].fence = nullptr;
        }
        if (staging_[i].pbo) {
            glUnmapNamedBuffer(staging_[i].pbo);
            glDeleteBuffers(1, &staging_[i].pbo);
            staging_[i].pbo = 0;
            staging_[i].mapped = nullptr;
        }
    }
    if (height_arr_) { glDeleteTextures(1, &height_arr_); height_arr_ = 0; }
    if (ortho_arr_)  { glDeleteTextures(1, &ortho_arr_);  ortho_arr_ = 0; }
    if (norm_arr_)   { glDeleteTextures(1, &norm_arr_);   norm_arr_ = 0; }
    if (surf_arr_)   { glDeleteTextures(1, &surf_arr_);   surf_arr_ = 0; }
}

void AsyncTerrainStreamer::downsample_rgba_box2(const uint8_t *src, int sw, int sh, uint8_t *dst) {
    int dw = sw / 2, dh = sh / 2;
    for (int y = 0; y < dh; y++) {
        const uint8_t *r0 = src + static_cast<size_t>(y * 2) * sw * 4;
        const uint8_t *r1 = src + static_cast<size_t>(y * 2 + 1) * sw * 4;
        uint8_t *out = dst + static_cast<size_t>(y) * dw * 4;
        for (int x = 0; x < dw; x++) {
            int sx = x * 8;
            for (int c = 0; c < 4; c++) {
                int s = r0[sx + c] + r0[sx + 4 + c] + r1[sx + c] + r1[sx + 4 + c];
                out[x * 4 + c] = static_cast<uint8_t>((s + 2) >> 2);
            }
        }
    }
}

void AsyncTerrainStreamer::fill_staging_slot(StagingSlot &slot, uint64_t key) {
    double t0 = PlatformWindow::now_sec();
    QuadKey qk = QuadKey::unpack(key);
    double ce = 0.0, cn = 0.0, hs = 1000.0;
    quad_bounds_en(qk, ce, cn, hs);

    Vec3 sdir;
    {
        std::lock_guard<std::mutex> lk(req_mtx_);
        sdir = sun_dir_;
    }

    thread_local std::vector<float> tmp_h;
    thread_local std::vector<uint8_t> tmp_ortho, tmp_norm, tmp_surf;
    pack_->fill_page_data(ce, cn, hs, HEIGHT_N, ORTHO_RES, AUX_RES, sdir, tmp_h, tmp_ortho, tmp_norm, tmp_surf);

    // PBO storage is mapped write-only and may be write-combined. Build and
    // encode in cached system RAM, then make one sequential copy to staging.
    static const PageLayout rgba_layout = make_page_layout(OrthoFormat::RGBA8);
    thread_local std::vector<uint8_t> rgba_mips, encoded;
    rgba_mips.resize(rgba_layout.ortho_bytes);
    const size_t rgba_base = rgba_layout.ortho.front();
    auto rgba_level = [&](int mip) { return rgba_mips.data() + rgba_layout.ortho[mip] - rgba_base; };
    std::memcpy(rgba_level(0), tmp_ortho.data(), static_cast<size_t>(ORTHO_RES) * ORTHO_RES * 4);
    for (int mip = 1; mip < ORTHO_MIPS; ++mip) {
        const int n = ORTHO_RES >> (mip - 1);
        downsample_rgba_box2(rgba_level(mip - 1), n, n, rgba_level(mip));
    }

    const uint8_t *ortho = rgba_mips.data();
    double encode_sec = 0.0;
    if (is_compressed(format_)) {
        const double start = PlatformWindow::now_sec();
        encoded.resize(layout_.ortho_bytes);
        for (int mip = 0; mip < ORTHO_MIPS; ++mip)
            compress_bc7_rgba_page(rgba_level(mip), ORTHO_RES >> mip,
                encoded.data() + layout_.ortho[mip] - layout_.ortho.front(), format_ == OrthoFormat::BC7Fast);
        encode_sec = PlatformWindow::now_sec() - start;
        ortho = encoded.data();
    }
    std::memcpy(slot.mapped + layout_.height, tmp_h.data(), SZ_HEIGHT);
    std::memcpy(slot.mapped + layout_.ortho.front(), ortho, layout_.ortho_bytes);
    std::memcpy(slot.mapped + layout_.normal, tmp_norm.data(), SZ_AUX);
    std::memcpy(slot.mapped + layout_.surface, tmp_surf.data(), SZ_AUX);

    slot.key = key;
    slot.worker_ms = static_cast<float>((PlatformWindow::now_sec() - t0) * 1000.0);
    const double build_ms = std::max(0.0, static_cast<double>(slot.worker_ms) - encode_sec * 1000.0);
    worker_build_us_.fetch_add(static_cast<uint64_t>(build_ms * 1000.0), std::memory_order_relaxed);
    worker_encode_us_.fetch_add(static_cast<uint64_t>(encode_sec * 1.0e6), std::memory_order_relaxed);
    finished_pages_.fetch_add(1, std::memory_order_relaxed);
    double prev = avg_worker_ms_.load(std::memory_order_relaxed);
    avg_worker_ms_.store(prev <= 0.0 ? slot.worker_ms : (prev * 0.9 + slot.worker_ms * 0.1), std::memory_order_relaxed);
}

void AsyncTerrainStreamer::worker_loop() {
    while (!stop_.load(std::memory_order_relaxed)) {
        uint64_t best_key = UINT64_MAX;
        int free_slot = -1;

        {
            std::unique_lock<std::mutex> lk(req_mtx_);
            req_cv_.wait_for(lk, std::chrono::milliseconds(8), [&]() {
                if (stop_.load(std::memory_order_relaxed)) return true;
                if (pending_reqs_.empty()) return false;
                for (int i = 0; i < NUM_STAGING; i++) {
                    if (staging_[i].state.load(std::memory_order_acquire) == StageState::FREE) return true;
                }
                return false;
            });
            if (stop_.load(std::memory_order_relaxed)) break;
            if (pending_reqs_.empty()) continue;

            for (int i = 0; i < NUM_STAGING; i++) {
                StageState expect = StageState::FREE;
                if (staging_[i].state.compare_exchange_strong(expect, StageState::FILLING, std::memory_order_acq_rel)) {
                    free_slot = i;
                    break;
                }
            }
            if (free_slot < 0) continue;

            // Pop highest velocity-predictive priority page
            auto best_it = pending_reqs_.begin();
            for (auto it = pending_reqs_.begin(); it != pending_reqs_.end(); ++it) {
                if (it->second > best_it->second) best_it = it;
            }
            best_key = best_it->first;
            float best_prio = best_it->second;
            pending_reqs_.erase(best_it);
            const uint64_t job_gen = generation_.load(std::memory_order_acquire);
            inflight_keys_[best_key] = job_gen;
            lk.unlock();

            if (job_gen != generation_.load(std::memory_order_acquire)) {
                staging_[free_slot].state.store(StageState::FREE, std::memory_order_release);
                continue;
            }
            staging_[free_slot].generation = job_gen;
            fill_staging_slot(staging_[free_slot], best_key);
            if (job_gen != generation_.load(std::memory_order_acquire)) {
                // Re-anchor happened mid-build: discard cleanly without entering READY upload queue.
                staging_[free_slot].key = UINT64_MAX;
                staging_[free_slot].generation = 0;
                staging_[free_slot].state.store(StageState::FREE, std::memory_order_release);
                continue;
            }
            staging_[free_slot].state.store(StageState::READY, std::memory_order_release);
            // Apply thermal breather across all non-sync pages (halved for high-prio approach tiles)
            // so BC7 workers never peg CPU turbo boost past 80 C on thin-and-light laptops.
            int tms = thermal_sleep_ms_.load(std::memory_order_relaxed);
            if (tms > 0 && !sync_flushing_.load(std::memory_order_relaxed)) {
                int eff_ms = (best_prio >= 4500.0f) ? (tms / 2) : tms;
                if (eff_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(eff_ms));
            }
            continue;
        }
    }
}

bool AsyncTerrainStreamer::coarse_root_ready() const {
    static const QuadKey kRoot[5] = {
        {0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {1, 0, 1}, {1, 1, 1}
    };
    for (const QuadKey &qk : kRoot) {
        if (key_to_slot_.find(qk.pack()) == key_to_slot_.end()) return false;
    }
    return true;
}

void AsyncTerrainStreamer::prewarm_coarse(const Vec3 &sun_dir) {
    set_sun_direction(sun_dir);
    static constexpr float kCoarsePrio[3] = {50000.0f, 20000.0f, 5000.0f};
    for (int lod = 0; lod <= 2; lod++) {
        int n = 1 << lod;
        for (int y = 0; y < n; y++) {
            for (int x = 0; x < n; x++) {
                QuadKey qk{static_cast<uint8_t>(lod), static_cast<uint16_t>(x), static_cast<uint16_t>(y)};
                request_page(qk.pack(), kCoarsePrio[lod]);
            }
        }
    }
    // Wait synchronously only for L0+L1 (5 pages covering the entire 163.84 km world, ~200 ms)
    // so startup and sector transitions never freeze; L2 pages stream immediately behind asynchronously.
    sync_flushing_.store(true, std::memory_order_relaxed);
    double deadline = PlatformWindow::now_sec() + 3.0;
    while (!coarse_root_ready() && PlatformWindow::now_sec() < deadline) {
        pump_gpu_uploads(-10.0f, NUM_STAGING, 20.0);
        if (coarse_root_ready()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
    pump_gpu_uploads(-10.0f, NUM_STAGING, 20.0);
    glFlush();
    sync_flushing_.store(false, std::memory_order_relaxed);
}

void AsyncTerrainStreamer::flush_pending_sync(int max_pages_to_drain, double timeout_sec) {
    sync_flushing_.store(true, std::memory_order_relaxed);
    int drained = 0;
    double deadline = PlatformWindow::now_sec() + timeout_sec;
    while (drained < max_pages_to_drain && PlatformWindow::now_sec() < deadline) {
        pump_gpu_uploads(-10.0f, NUM_STAGING, 20.0); // negative ready_time => instant 1.0 fade on warmup!
        drained += uploads_this_frame_;
        bool done = false;
        {
            std::lock_guard<std::mutex> lk(req_mtx_);
            done = pending_reqs_.empty() && inflight_keys_.empty();
        }
        if (done) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
    pump_gpu_uploads(-10.0f, NUM_STAGING, 20.0);
    glFlush();
    sync_flushing_.store(false, std::memory_order_relaxed);
}

void AsyncTerrainStreamer::set_sun_direction(const Vec3 &sun_dir) {
    std::lock_guard<std::mutex> lk(req_mtx_);
    sun_dir_ = normalize(sun_dir);
}

void AsyncTerrainStreamer::invalidate_shadows() {
    {
        std::lock_guard<std::mutex> lk(req_mtx_);
        pending_reqs_.clear();
        for (const auto &kv : key_to_slot_) {
            int slot_idx = kv.second;
            bool active = slots_[slot_idx].pinned || (cur_frame_ <= 4) || ((cur_frame_ - slots_[slot_idx].last_used_frame) <= 4);
            if (!active) continue;
            if (inflight_keys_.find(kv.first) != inflight_keys_.end()) continue;
            QuadKey qk = QuadKey::unpack(kv.first);
            float prio = (qk.lod <= 2) ? 6000.0f : (2500.0f - qk.lod * 120.0f);
            pending_reqs_[kv.first] = std::max(pending_reqs_[kv.first], prio);
        }
    }
    req_cv_.notify_all();
}

void AsyncTerrainStreamer::invalidate_close_pages(bool evict_inactive) {
    {
        std::lock_guard<std::mutex> lk(req_mtx_);
        if (evict_inactive) pending_reqs_.clear();
        for (auto it = key_to_slot_.begin(); it != key_to_slot_.end(); ) {
            QuadKey qk = QuadKey::unpack(it->first);
            if (qk.lod < 5) { ++it; continue; }
            int slot_idx = it->second;
            const bool active = (cur_frame_ <= 8) || ((cur_frame_ - slots_[slot_idx].last_used_frame) <= 8);
            if (evict_inactive && !active) {
                // Only drop off-screen/stale close pages; keep visible L5-L9 pages resident and refresh
                // them in place so the terrain never collapses to coarse L4 while streaming!
                slots_[slot_idx].key = UINT64_MAX;
                slots_[slot_idx].last_used_frame = 0;
                it = key_to_slot_.erase(it);
                continue;
            }
            if (!active) {
                ++it;
                continue;
            }
            if (inflight_keys_.find(it->first) == inflight_keys_.end()) {
                // Keep in-place refreshes below brand-new approaching quads so new L7-L9 tiles never starve.
                float prio = (evict_inactive ? 1200.0f : 180.0f) + qk.lod * 25.0f;
                pending_reqs_[it->first] = std::max(pending_reqs_[it->first], prio);
            }
            ++it;
        }
    }
    req_cv_.notify_all();
}

void AsyncTerrainStreamer::clear_pending_requests() {
    {
        std::lock_guard<std::mutex> lk(req_mtx_);
        pending_reqs_.clear();
    }
}

void AsyncTerrainStreamer::invalidate_all_and_prewarm(const Vec3 &sun_dir) {
    // Advance generation counter immediately so any in-flight old-anchor jobs self-discard cleanly with 0 ms block.
    generation_.fetch_add(1, std::memory_order_acq_rel);
    {
        std::lock_guard<std::mutex> lk(req_mtx_);
        pending_reqs_.clear();
        inflight_keys_.clear();
        key_to_slot_.clear();
        for (auto &s : slots_) {
            s.key = UINT64_MAX;
            s.last_used_frame = 0;
            s.pinned = false;
        }
    }
    prewarm_coarse(sun_dir);
}

void AsyncTerrainStreamer::begin_frame(uint64_t frame_idx) {
    cur_frame_ = frame_idx;
    uploads_this_frame_ = 0;
    // Keep highest-priority velocity-predictive requests; only trim stale lowest-priority tail if > 256
    std::lock_guard<std::mutex> lk(req_mtx_);
    if (pending_reqs_.size() > 256) {
        std::vector<std::pair<uint64_t, float>> items(pending_reqs_.begin(), pending_reqs_.end());
        std::nth_element(items.begin(), items.begin() + 140, items.end(),
                         [](const auto &a, const auto &b) { return a.second > b.second; });
        pending_reqs_.clear();
        for (int i = 0; i < 140; i++) pending_reqs_[items[i].first] = items[i].second;
    }
}

int AsyncTerrainStreamer::find_resident(uint64_t key, float now_sec, float *out_fade) {
    auto it = key_to_slot_.find(key);
    if (it == key_to_slot_.end()) return -1;
    int idx = it->second;
    slots_[idx].last_used_frame = cur_frame_;
    if (out_fade) {
        float t = std::clamp((now_sec - slots_[idx].ready_time_sec) / std::max(0.05f, slots_[idx].fade_dur_sec), 0.0f, 1.0f);
        // Smooth cubic Hermite arrival curve -> zero sharp velocity kink at start/end of inclusion!
        *out_fade = t * t * (3.0f - 2.0f * t);
    }
    return idx;
}

void AsyncTerrainStreamer::request_page(uint64_t key, float priority) {
    if (key_to_slot_.find(key) != key_to_slot_.end()) return;
    // Deterministic spatial-hash priority jitter (±16%) breaks up lockstep rectangular grid loading waves!
    uint64_t h = (key ^ (key >> 30)) * 0xbf58476d1ce4e5b9ULL;
    h = (h ^ (h >> 27)) * 0x94d049bb133111ebULL;
    float jitter = 0.84f + static_cast<float>((h >> 32) & 0xFFFFu) * (0.32f / 65535.0f);
    float j_prio = priority * jitter;
    {
        std::lock_guard<std::mutex> lk(req_mtx_);
        const uint64_t cur_gen = generation_.load(std::memory_order_acquire);
        auto ii = inflight_keys_.find(key);
        if (ii != inflight_keys_.end() && ii->second == cur_gen) return;
        auto it = pending_reqs_.find(key);
        if (it == pending_reqs_.end() || j_prio > it->second) {
            pending_reqs_[key] = j_prio;
        }
    }
    req_cv_.notify_one();
}

int AsyncTerrainStreamer::allocate_cache_slice(uint64_t key, float now_sec) {
    auto it = key_to_slot_.find(key);
    if (it != key_to_slot_.end()) {
        slots_[it->second].last_used_frame = cur_frame_;
        // The old page is already visible. Refresh its contents without
        // restarting the coarse-parent fade or allocating another slice.
        return it->second;
    }

    auto has_resident_child = [&](uint64_t pk) {
        QuadKey p = QuadKey::unpack(pk);
        if (p.lod >= MAX_LOD) return false;
        for (int ci = 0; ci < 4; ++ci) {
            QuadKey ck{static_cast<uint8_t>(p.lod + 1),
                       static_cast<uint16_t>(p.qx * 2 + (ci & 1)),
                       static_cast<uint16_t>(p.qy * 2 + (ci >> 1))};
            if (key_to_slot_.find(ck.pack()) != key_to_slot_.end()) return true;
        }
        return false;
    };

    int victim = -1;
    int fallback_victim = -1;
    uint64_t oldest_leaf = UINT64_MAX;
    uint64_t oldest_any = UINT64_MAX;
    for (int i = 0; i < MAX_PAGES; i++) {
        if (slots_[i].key == UINT64_MAX) {
            victim = i;
            break;
        }
        if (slots_[i].pinned) continue;
        // Prefer evicting stale leaf pages (never orphaning a resident L5-L9 subtree by evicting its L3-L5 parent!).
        if (!has_resident_child(slots_[i].key)) {
            QuadKey sq = QuadKey::unpack(slots_[i].key);
            // Bias slightly toward evicting stale finer leaves over reusable mid-LOD parents.
            uint64_t score = slots_[i].last_used_frame + (sq.lod < 5 ? 24ULL : 0ULL);
            if (score < oldest_leaf) {
                oldest_leaf = score;
                victim = i;
            }
        }
        if (slots_[i].last_used_frame < oldest_any) {
            oldest_any = slots_[i].last_used_frame;
            fallback_victim = i;
        }
    }
    if (victim < 0) victim = (fallback_victim >= 0) ? fallback_victim : 0;
    if (slots_[victim].key != UINT64_MAX) {
        key_to_slot_.erase(slots_[victim].key);
    }
    QuadKey qk = QuadKey::unpack(key);
    // Snappy randomized per-page fade (0.18..0.30s) so newly streamed detail appears crisply without popping.
    uint32_t fh = static_cast<uint32_t>((key * 2654435761ULL) >> 16) & 0xFFFFu;
    slots_[victim].key = key;
    slots_[victim].last_used_frame = cur_frame_;
    slots_[victim].ready_time_sec = now_sec;
    slots_[victim].fade_dur_sec = 0.18f + static_cast<float>(fh) * (0.12f / 65535.0f);
    slots_[victim].pinned = (qk.lod <= 2);
    key_to_slot_[key] = victim;
    return victim;
}

void AsyncTerrainStreamer::pump_gpu_uploads(float now_sec, int max_uploads, double budget_ms) {
    double t0 = PlatformWindow::now_sec();
    int uploaded = 0;

    // 1. Reclaim any PBO slots whose GPU DMA fence has signaled
    for (int i = 0; i < NUM_STAGING; i++) {
        if (staging_[i].state.load(std::memory_order_acquire) == StageState::UPLOADING) {
            if (!staging_[i].fence || glClientWaitSync(staging_[i].fence, 0, 0) != GL_TIMEOUT_EXPIRED) {
                if (staging_[i].fence) {
                    glDeleteSync(staging_[i].fence);
                    staging_[i].fence = nullptr;
                }
                staging_[i].state.store(StageState::FREE, std::memory_order_release);
                req_cv_.notify_one();
            }
        }
    }

    // 2. Frame-time-budgeted randomized-start DMA uploader -> NEVER saturates low-end iGPUs or causes frame bursts!
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    const uint64_t active_gen = generation_.load(std::memory_order_acquire);
    const int start_idx = static_cast<int>((cur_frame_ * 7u) % NUM_STAGING);
    for (int k = 0; k < NUM_STAGING && uploaded < max_uploads; k++) {
        if (uploaded > 0 && (PlatformWindow::now_sec() - t0) * 1000.0 >= budget_ms) break;
        const int i = (start_idx + k) % NUM_STAGING;
        if (staging_[i].state.load(std::memory_order_acquire) != StageState::READY) continue;

        if (staging_[i].generation != active_gen) {
            staging_[i].key = UINT64_MAX;
            staging_[i].generation = 0;
            staging_[i].state.store(StageState::FREE, std::memory_order_release);
            req_cv_.notify_one();
            continue;
        }

        uint64_t key = staging_[i].key;
        int slice = allocate_cache_slice(key, now_sec);

        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, staging_[i].pbo);
        glTextureSubImage3D(height_arr_, 0, 0, 0, slice, HEIGHT_N, HEIGHT_N, 1, GL_RED, GL_FLOAT, reinterpret_cast<const void *>(layout_.height));
        for (int mip = 0; mip < ORTHO_MIPS; ++mip) {
            const int n = ORTHO_RES >> mip;
            if (is_compressed(format_)) {
                glCompressedTextureSubImage3D(ortho_arr_, mip, 0, 0, slice, n, n, 1,
                    GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM, static_cast<GLsizei>(bc7_square_level_bytes(n)),
                    reinterpret_cast<const void *>(layout_.ortho[mip]));
            } else {
                glTextureSubImage3D(ortho_arr_, mip, 0, 0, slice, n, n, 1, GL_RGBA, GL_UNSIGNED_BYTE,
                    reinterpret_cast<const void *>(layout_.ortho[mip]));
            }
        }
        glTextureSubImage3D(norm_arr_, 0, 0, 0, slice, AUX_RES, AUX_RES, 1, GL_RGBA, GL_UNSIGNED_BYTE, reinterpret_cast<const void *>(layout_.normal));
        glTextureSubImage3D(surf_arr_, 0, 0, 0, slice, AUX_RES, AUX_RES, 1, GL_RGBA, GL_UNSIGNED_BYTE, reinterpret_cast<const void *>(layout_.surface));
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

        staging_[i].fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        staging_[i].state.store(StageState::UPLOADING, std::memory_order_release);

        {
            std::lock_guard<std::mutex> lk(req_mtx_);
            auto it = inflight_keys_.find(key);
            if (it != inflight_keys_.end() && it->second == active_gen) inflight_keys_.erase(it);
        }
        uploaded++;
        total_uploads_++;
    }

    uploads_this_frame_ = uploaded;
    last_upload_ms_ = (PlatformWindow::now_sec() - t0) * 1000.0;
}

StreamStats AsyncTerrainStreamer::stats() const {
    StreamStats s{};
    s.resident_pages = static_cast<int>(key_to_slot_.size());
    s.max_pages = MAX_PAGES;
    for (const auto &kv : key_to_slot_) {
        const uint8_t lod = QuadKey::unpack(kv.first).lod;
        if (lod < s.resident_by_lod.size()) s.resident_by_lod[lod]++;
    }
    {
        std::lock_guard<std::mutex> lk(req_mtx_);
        s.queued_jobs = static_cast<int>(pending_reqs_.size() + inflight_keys_.size());
    }
    s.uploads_this_frame = uploads_this_frame_;
    s.total_uploads = total_uploads_;
    s.last_upload_ms = last_upload_ms_;
    s.avg_worker_ms = avg_worker_ms_.load(std::memory_order_relaxed);
    s.finished_pages = finished_pages_.load(std::memory_order_relaxed);
    s.worker_build_total_ms = worker_build_us_.load(std::memory_order_relaxed) / 1000.0;
    s.worker_encode_total_ms = worker_encode_us_.load(std::memory_order_relaxed) / 1000.0;
    s.gpu_page_bytes = layout_.ortho_bytes + SZ_HEIGHT + 2 * SZ_AUX;
    return s;
}

} // namespace nsim
