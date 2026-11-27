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
    params.m_perceptual = !fast;
    params.m_try_least_squares = !fast;
    params.m_mode17_partition_estimation_filterbank = true;

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

void AsyncTerrainStreamer::queue_request(
    std::unordered_map<uint64_t, float> &requests, RequestHeap &heap, uint64_t key, float priority
) {
    auto it = requests.find(key);
    if (it == requests.end()) {
        requests.emplace(key, priority);
        heap.push({priority, key});
    } else if (priority > it->second) {
        it->second = priority;
        heap.push({priority, key});
    }
    // Priority updates leave stale heap entries behind; compact occasionally so canceled
    // or repeatedly reprioritized requests cannot grow the queue without bound.
    if (heap.size() > requests.size() * 4 + 64) rebuild_request_heap(requests, heap);
}

void AsyncTerrainStreamer::rebuild_request_heap(
    const std::unordered_map<uint64_t, float> &requests, RequestHeap &heap
) {
    RequestHeap compact;
    for (const auto &[key, priority] : requests) compact.push({priority, key});
    heap.swap(compact);
}

bool AsyncTerrainStreamer::pop_best_request(
    std::unordered_map<uint64_t, float> &requests, RequestHeap &heap,
    uint64_t &out_key, float &out_priority
) {
    while (!heap.empty()) {
        const RequestOrder next = heap.top();
        heap.pop();
        auto it = requests.find(next.key);
        if (it == requests.end() || it->second != next.priority) continue;
        out_key = next.key;
        out_priority = next.priority;
        requests.erase(it);
        return true;
    }
    return false;
}

void AsyncTerrainStreamer::release_cache_slot(int idx) {
    CacheSlot &slot = slots_[idx];
    if (slot.key != UINT64_MAX) free_slots_.push_back(idx);
    slot = CacheSlot{};
}

bool AsyncTerrainStreamer::init(const PackReader *pack, int worker_count, OrthoFormat format, int page_capacity) {
    if (!pack) return false;
    pack_ = pack;
    world_frame_ = pack_->world_frame();
    format_ = format;
    layout_ = make_page_layout(format);

    page_capacity_ = std::clamp(page_capacity, MAX_PAGES, 1024);
    slots_.assign(page_capacity_, CacheSlot{});
    free_slots_.clear();
    free_slots_.reserve(page_capacity_);
    key_to_slot_.clear();
    pending_key_to_slot_.clear();
    for (int i = 0; i < page_capacity_; ++i) free_slots_.push_back(i);
    {
        std::lock_guard<std::mutex> lk(req_mtx_);
        pending_reqs_.clear();
        pending_req_heap_ = RequestHeap{};
        pending_req_frame_.clear();
        pending_reqs_next_.clear();
        pending_req_heap_next_ = RequestHeap{};
        inflight_active_.clear();
        inflight_next_.clear();
    }

    // Allocate the resident terrain arrays.
    glCreateTextures(GL_TEXTURE_2D_ARRAY, 1, &height_arr_);
    glTextureStorage3D(height_arr_, 1, GL_R32F, HEIGHT_N, HEIGHT_N, page_capacity_);
    glTextureParameteri(height_arr_, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(height_arr_, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(height_arr_, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(height_arr_, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glCreateTextures(GL_TEXTURE_2D_ARRAY, 1, &ortho_arr_);
    // BC7 keeps PNOA RGB fidelity while cutting the streamed ortho page upload
    // and resident texture footprint to one byte per texel.
    const GLenum internal_format = is_compressed(format_) ? GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM : GL_SRGB8_ALPHA8;
    glTextureStorage3D(ortho_arr_, ORTHO_MIPS, internal_format, ORTHO_RES, ORTHO_RES, page_capacity_);
    glTextureParameteri(ortho_arr_, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTextureParameteri(ortho_arr_, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(ortho_arr_, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(ortho_arr_, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTextureParameterf(ortho_arr_, GL_TEXTURE_MAX_ANISOTROPY, 4.0f);

    glCreateTextures(GL_TEXTURE_2D_ARRAY, 1, &norm_arr_);
    glTextureStorage3D(norm_arr_, 1, GL_RGBA8, AUX_RES, AUX_RES, page_capacity_);
    glTextureParameteri(norm_arr_, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(norm_arr_, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(norm_arr_, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(norm_arr_, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glCreateTextures(GL_TEXTURE_2D_ARRAY, 1, &surf_arr_);
    glTextureStorage3D(surf_arr_, 1, GL_RGBA8, AUX_RES, AUX_RES, page_capacity_);
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
    int nw = (worker_count > 0) ? std::clamp(worker_count, 1, 10) : 3;
    for (int i = 0; i < nw; i++) {
        workers_.emplace_back(&AsyncTerrainStreamer::worker_loop, this, i);
    }
    return true;
}

void AsyncTerrainStreamer::shutdown() {
    {
        std::lock_guard<std::mutex> lk(req_mtx_);
        stop_.store(true);
    }
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

void AsyncTerrainStreamer::fill_staging_slot(StagingSlot &slot, uint64_t key, const PackReader::WorldFrame &frame) {
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
    pack_->fill_page_data(frame, ce, cn, hs, HEIGHT_N, ORTHO_RES, AUX_RES, sdir,
                          tmp_h, tmp_ortho, tmp_norm, tmp_surf, &slot.imagery);

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

void AsyncTerrainStreamer::limit_workers(bool limited) {
    {
        std::lock_guard lock(req_mtx_);
        if (workers_limited_ == limited) return;
        workers_limited_ = limited;
    }
    req_cv_.notify_all();
}

void AsyncTerrainStreamer::worker_loop(int index) {
    while (!stop_.load(std::memory_order_relaxed)) {
        uint64_t best_key = UINT64_MAX;
        float best_prio = 0.0f;
        uint64_t job_gen = 0;
        std::shared_ptr<const PackReader::WorldFrame> frame;
        int free_slot = -1;

        {
            std::unique_lock<std::mutex> lk(req_mtx_);
            const auto has_work = [&]() {
                return (index == 0 || !workers_limited_) &&
                       (!pending_reqs_.empty() || (pending_gen_.load(std::memory_order_acquire) != 0 && !pending_reqs_next_.empty()));
            };
            req_cv_.wait(lk, [&]() { return stop_.load(std::memory_order_relaxed) || has_work(); });
            // Staging fences are reclaimed by the render thread. Retry only
            // while work is queued, in case a fence wake races this wait.
            req_cv_.wait_for(lk, std::chrono::milliseconds(8), [&]() {
                if (stop_.load(std::memory_order_relaxed) || !has_work()) return true;
                for (int i = 0; i < NUM_STAGING; i++) {
                    if (staging_[i].state.load(std::memory_order_acquire) == StageState::FREE) return true;
                }
                return false;
            });
            if (stop_.load(std::memory_order_relaxed)) break;
            const uint64_t cur_pending = pending_gen_.load(std::memory_order_acquire);
            const bool have_next = (cur_pending != 0) && !pending_reqs_next_.empty();
            if (!has_work()) continue;

            for (int i = 0; i < NUM_STAGING; i++) {
                StageState expect = StageState::FREE;
                if (staging_[i].state.compare_exchange_strong(expect, StageState::FILLING, std::memory_order_acq_rel)) {
                    free_slot = i;
                    break;
                }
            }
            if (free_slot < 0) continue;

            auto &qmap = have_next ? pending_reqs_next_ : pending_reqs_;
            auto &qheap = have_next ? pending_req_heap_next_ : pending_req_heap_;
            if (!pop_best_request(qmap, qheap, best_key, best_prio)) {
                staging_[free_slot].state.store(StageState::FREE, std::memory_order_release);
                continue;
            }
            if (!have_next) pending_req_frame_.erase(best_key);

            if (have_next) {
                job_gen = cur_pending;
                inflight_next_.insert(best_key);
                frame = pending_frame_;
            } else {
                job_gen = active_gen_.load(std::memory_order_acquire);
                inflight_active_.insert(best_key);
                frame = world_frame_;
            }
        }

        auto gen_still_live = [&]() -> bool {
            const uint64_t ag = active_gen_.load(std::memory_order_acquire);
            const uint64_t pg = pending_gen_.load(std::memory_order_acquire);
            return (job_gen == ag) || (pg != 0 && job_gen == pg);
        };

        if (!frame || !gen_still_live()) {
            std::lock_guard<std::mutex> lk(req_mtx_);
            if (job_gen == active_gen_.load(std::memory_order_acquire)) inflight_active_.erase(best_key);
            else if (pending_gen_.load(std::memory_order_acquire) == job_gen) inflight_next_.erase(best_key);
            staging_[free_slot].key = UINT64_MAX;
            staging_[free_slot].generation = 0;
            staging_[free_slot].state.store(StageState::FREE, std::memory_order_release);
            continue;
        }

        staging_[free_slot].generation = job_gen;
        fill_staging_slot(staging_[free_slot], best_key, *frame);
        if (!gen_still_live()) {
            // Re-anchor or transition invalidation happened mid-build: discard cleanly.
            staging_[free_slot].key = UINT64_MAX;
            staging_[free_slot].generation = 0;
            staging_[free_slot].state.store(StageState::FREE, std::memory_order_release);
            continue;
        }
        staging_[free_slot].state.store(StageState::READY, std::memory_order_release);
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
            done = pending_reqs_.empty() && inflight_active_.empty();
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
        pending_req_heap_ = RequestHeap{};
        pending_req_frame_.clear();
        for (const auto &kv : key_to_slot_) {
            int slot_idx = kv.second;
            bool active = slots_[slot_idx].pinned || (cur_frame_ <= 4) || ((cur_frame_ - slots_[slot_idx].last_used_frame) <= 4);
            if (!active) continue;
            if (inflight_active_.find(kv.first) != inflight_active_.end()) continue;
            QuadKey qk = QuadKey::unpack(kv.first);
            float prio = (qk.lod <= 2) ? 6000.0f : (2500.0f - qk.lod * 120.0f);
            queue_request(pending_reqs_, pending_req_heap_, kv.first, prio);
            pending_req_frame_[kv.first] = cur_frame_;
        }
    }
    req_cv_.notify_all();
}

void AsyncTerrainStreamer::invalidate_close_pages() {
    {
        std::lock_guard<std::mutex> lk(req_mtx_);
        pending_reqs_.clear();
        pending_req_heap_ = RequestHeap{};
        pending_req_frame_.clear();
        for (auto it = key_to_slot_.begin(); it != key_to_slot_.end(); ) {
            QuadKey qk = QuadKey::unpack(it->first);
            if (qk.lod < 5) { ++it; continue; }
            int slot_idx = it->second;
            const bool active = (cur_frame_ <= 8) || ((cur_frame_ - slots_[slot_idx].last_used_frame) <= 8);
            if (!active) {
                release_cache_slot(slot_idx);
                it = key_to_slot_.erase(it);
                continue;
            }
            if (inflight_active_.find(it->first) == inflight_active_.end()) {
                queue_request(pending_reqs_, pending_req_heap_, it->first, 1200.0f + qk.lod * 25.0f);
                pending_req_frame_[it->first] = cur_frame_;
            }
            ++it;
        }
    }
    req_cv_.notify_all();
}

void AsyncTerrainStreamer::refine_online_pages(double now_sec) {
    if (now_sec < next_imagery_refine_sec_ || pack_->online_provider() == OnlineOrthoProvider::OFF) return;
    next_imagery_refine_sec_ = now_sec + 0.20;
    const uint64_t revision = pack_->online_revision();
    std::vector<RequestOrder> candidates;
    const auto &frame = *world_frame_;
    for (const auto &[key, index] : key_to_slot_) {
        const CacheSlot &slot = slots_[index];
        if (cur_frame_ - slot.last_used_frame > 8) continue;
        const QuadKey qk = QuadKey::unpack(key);
        if (qk.lod < 4) continue;
        double east, north, half_span;
        quad_bounds_en(qk, east, north, half_span);
        const int zoom = pack_->online_page_zoom(frame.anchor_lat + north / frame.metres_lat,
                                                 frame.anchor_lon + east / frame.metres_lon,
                                                 half_span, ORTHO_RES);
        const double age_sec = now_sec - slot.last_imagery_upload_sec;
        // Compare with the revision sampled before this page was built. An
        // arrival during a build still triggers a refresh after upload.
        const bool retry = slot.imagery.target_tiles_missing > 0 && age_sec >= 1.0 &&
            (slot.imagery.source_revision != revision || age_sec >= 30.0);
        if (zoom > slot.imagery.target_zoom || (zoom == slot.imagery.target_zoom && retry)) {
            const float priority = zoom == 20 ? 10500.0f : 1700.0f;
            candidates.push_back({priority + slot.last_importance, key});
        }
    }
    if (candidates.empty()) return;
    std::sort(candidates.begin(), candidates.end(), [](const RequestOrder &a, const RequestOrder &b) {
        return RequestOrderLess{}(b, a);
    });
    {
        std::lock_guard<std::mutex> lk(req_mtx_);
        int queued = 0;
        for (const auto &request : candidates) {
            if (inflight_active_.contains(request.key)) continue;
            queue_request(pending_reqs_, pending_req_heap_, request.key, request.priority);
            pending_req_frame_[request.key] = cur_frame_;
            if (++queued == 6) break;
        }
    }
    req_cv_.notify_all();
}

void AsyncTerrainStreamer::clear_pending_requests() {
    {
        std::lock_guard<std::mutex> lk(req_mtx_);
        pending_reqs_.clear();
        pending_req_heap_ = RequestHeap{};
        pending_req_frame_.clear();
    }
}

void AsyncTerrainStreamer::invalidate_all_and_prewarm(const Vec3 &sun_dir) {
    {
        std::lock_guard<std::mutex> lk(req_mtx_);
        const uint64_t next_gen = gen_seq_.fetch_add(1, std::memory_order_acq_rel) + 1;
        world_frame_ = pack_->world_frame();
        pending_frame_.reset();
        active_gen_.store(next_gen, std::memory_order_release);
        pending_gen_.store(0, std::memory_order_release);
        pending_reqs_.clear();
        pending_req_heap_ = RequestHeap{};
        pending_req_frame_.clear();
        pending_reqs_next_.clear();
        pending_req_heap_next_ = RequestHeap{};
        inflight_active_.clear();
        inflight_next_.clear();
        pending_required_keys_.clear();
        key_to_slot_.clear();
        pending_key_to_slot_.clear();
        for (auto &s : slots_) {
            s.key = UINT64_MAX;
            s.generation = 0;
            s.last_used_frame = 0;
            s.last_importance = 0.0f;
            s.pinned = false;
            s.imagery = {};
        }
        free_slots_.clear();
        for (int i = 0; i < page_capacity_; ++i) free_slots_.push_back(i);
    }
    prewarm_coarse(sun_dir);
}

void AsyncTerrainStreamer::begin_async_transition(
    std::shared_ptr<const PackReader::WorldFrame> next_frame,
    const Vec3 &sun_dir,
    double lat_deg, double lon_deg,
    double vel_e_mps, double vel_n_mps
) {
    if (!next_frame) return;
    std::lock_guard<std::mutex> lk(req_mtx_);
    const uint64_t old_pending = pending_gen_.load(std::memory_order_acquire);
    if (old_pending != 0) {
        for (const auto &kv : pending_key_to_slot_) {
            int s = kv.second;
            if (s >= 0 && s < page_capacity_ && slots_[s].generation == old_pending) {
                release_cache_slot(s);
            }
        }
    }
    pending_key_to_slot_.clear();
    pending_reqs_next_.clear();
    pending_req_heap_next_ = RequestHeap{};
    inflight_next_.clear();
    pending_required_keys_.clear();

    const uint64_t next_gen = gen_seq_.fetch_add(1, std::memory_order_acq_rel) + 1;
    sun_dir_ = normalize(sun_dir);
    pending_frame_ = std::move(next_frame);
    pending_gen_.store(next_gen, std::memory_order_release);

    static constexpr float kCoarsePrio[3] = {50000.0f, 25000.0f, 12000.0f};
    for (int lod = 0; lod <= 2; lod++) {
        int n = 1 << lod;
        for (int y = 0; y < n; y++) {
            for (int x = 0; x < n; x++) {
                uint64_t k = QuadKey{static_cast<uint8_t>(lod), static_cast<uint16_t>(x), static_cast<uint16_t>(y)}.pack();
                queue_request(pending_reqs_next_, pending_req_heap_next_, k, kCoarsePrio[lod]);
                pending_required_keys_.push_back(k); // first 21 keys = L0..L2
            }
        }
    }

    const double ce = (lon_deg - pending_frame_->anchor_lon) * pending_frame_->metres_lon + vel_e_mps * 2.0;
    const double cn = (lat_deg - pending_frame_->anchor_lat) * pending_frame_->metres_lat + vel_n_mps * 2.0;
    for (int lod = 3; lod <= 8; lod++) {
        const int n = 1 << lod;
        const double tile_sz = WORLD_SPAN_M / static_cast<double>(n);
        const int qx0 = std::clamp(static_cast<int>(std::floor((ce + WORLD_SPAN_M * 0.5) / tile_sz - 0.5)), 0, std::max(0, n - 2));
        const int qy0 = std::clamp(static_cast<int>(std::floor((cn + WORLD_SPAN_M * 0.5) / tile_sz - 0.5)), 0, std::max(0, n - 2));
        const float prio = 9000.0f - static_cast<float>(lod - 3) * 650.0f;
        for (int dy = 0; dy < 2 && (qy0 + dy) < n; ++dy) {
            for (int dx = 0; dx < 2 && (qx0 + dx) < n; ++dx) {
                uint64_t k = QuadKey{static_cast<uint8_t>(lod), static_cast<uint16_t>(qx0 + dx), static_cast<uint16_t>(qy0 + dy)}.pack();
                queue_request(pending_reqs_next_, pending_req_heap_next_, k, prio);
                if (lod <= 6) pending_required_keys_.push_back(k); // next <=16 keys = L3..L6 under aircraft
            }
        }
    }
    req_cv_.notify_all();
}

bool AsyncTerrainStreamer::is_async_transition_ready(double elapsed_sec) const {
    if (pending_gen_.load(std::memory_order_acquire) == 0) return false;
    int coarse_ready = 0;
    int root_l01 = 0;
    int corridor_ready = 0;
    const size_t total = pending_required_keys_.size();
    const size_t coarse_n = std::min<size_t>(21, total);
    for (size_t i = 0; i < total; ++i) {
        if (pending_key_to_slot_.find(pending_required_keys_[i]) != pending_key_to_slot_.end()) {
            if (i < 5) ++root_l01;
            if (i < coarse_n) ++coarse_ready;
            else ++corridor_ready;
        }
    }
    if (coarse_ready >= 21 && (corridor_ready >= 12 || total <= 21)) return true;
    if (elapsed_sec >= 4.5 && root_l01 >= 5 && coarse_ready >= 16) return true;
    return false;
}

void AsyncTerrainStreamer::commit_async_transition() {
    std::lock_guard<std::mutex> lk(req_mtx_);
    const uint64_t next_gen = pending_gen_.load(std::memory_order_acquire);
    if (next_gen == 0) return;
    active_gen_.store(next_gen, std::memory_order_release);
    pending_gen_.store(0, std::memory_order_release);
    world_frame_ = std::move(pending_frame_);
    pending_frame_.reset();
    pending_reqs_.swap(pending_reqs_next_);
    pending_req_heap_.swap(pending_req_heap_next_);
    pending_reqs_next_.clear();
    pending_req_heap_next_ = RequestHeap{};
    pending_req_frame_.clear();
    inflight_active_.swap(inflight_next_);
    inflight_next_.clear();
    pending_required_keys_.clear();
    key_to_slot_.swap(pending_key_to_slot_);
    pending_key_to_slot_.clear();
    for (int i = 0; i < page_capacity_; ++i) {
        if (slots_[i].generation != next_gen) {
            release_cache_slot(i);
        } else {
            slots_[i].last_used_frame = cur_frame_;
        }
    }
    req_cv_.notify_all();
}

void AsyncTerrainStreamer::cancel_async_transition() {
    std::lock_guard<std::mutex> lk(req_mtx_);
    const uint64_t old_pending = pending_gen_.exchange(0, std::memory_order_acq_rel);
    if (old_pending != 0) {
        for (const auto &kv : pending_key_to_slot_) {
            int s = kv.second;
            if (s >= 0 && s < page_capacity_ && slots_[s].generation == old_pending) {
                release_cache_slot(s);
            }
        }
    }
    pending_frame_.reset();
    pending_reqs_next_.clear();
    pending_req_heap_next_ = RequestHeap{};
    inflight_next_.clear();
    pending_required_keys_.clear();
    pending_key_to_slot_.clear();
}

void AsyncTerrainStreamer::begin_frame(uint64_t frame_idx) {
    cur_frame_ = frame_idx;
    uploads_this_frame_ = 0;

    // 1. Proactively free stale off-frustum high-LOD leaves once cache passes 75% capacity
    // so new visible terrain allocates immediately without stalling behind passed tiles.
    const size_t high_water = (page_capacity_ * 3) / 4;
    if (key_to_slot_.size() > high_water && frame_idx > 16) {
        const uint64_t act_gen = active_gen_.load(std::memory_order_acquire);
        auto has_child = [&](uint64_t pk) {
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
        for (int pass = 0; pass < 2 && key_to_slot_.size() > high_water; ++pass) {
            const int min_lod = (pass == 0) ? 6 : 3;
            const uint64_t min_age = (pass == 0) ? 10ULL : 90ULL;
            for (int i = 0; i < page_capacity_ && key_to_slot_.size() > high_water; ++i) {
                CacheSlot &s = slots_[i];
                if (s.key == UINT64_MAX || s.pinned || s.generation != act_gen) continue;
                const uint64_t age = (frame_idx >= s.last_used_frame) ? (frame_idx - s.last_used_frame) : 0ULL;
                if (age <= min_age) continue;
                const QuadKey qk = QuadKey::unpack(s.key);
                if (qk.lod < min_lod || has_child(s.key)) continue;
                key_to_slot_.erase(s.key);
                release_cache_slot(i);
            }
        }
    }

    // 2. Sweep stale off-frustum queue requests (>3 frames unrefreshed) before workers encode them.
    std::lock_guard<std::mutex> lk(req_mtx_);
    for (auto it = pending_reqs_.begin(); it != pending_reqs_.end(); ) {
        const QuadKey qk = QuadKey::unpack(it->first);
        auto fit = pending_req_frame_.find(it->first);
        const uint64_t req_f = (fit != pending_req_frame_.end()) ? fit->second : frame_idx;
        if (qk.lod >= 3 && frame_idx > req_f + 3ULL) {
            if (fit != pending_req_frame_.end()) pending_req_frame_.erase(fit);
            it = pending_reqs_.erase(it);
        } else {
            ++it;
        }
    }
    if (pending_reqs_.size() > 256) {
        std::vector<std::pair<uint64_t, float>> items(pending_reqs_.begin(), pending_reqs_.end());
        std::nth_element(items.begin(), items.begin() + 192, items.end(),
                         [](const auto &a, const auto &b) { return a.second > b.second; });
        std::unordered_map<uint64_t, float> kept;
        kept.reserve(256);
        for (int i = 0; i < 192; i++) kept.emplace(items[i].first, items[i].second);
        for (auto it = pending_req_frame_.begin(); it != pending_req_frame_.end();) {
            if (kept.find(it->first) == kept.end()) it = pending_req_frame_.erase(it);
            else ++it;
        }
        pending_reqs_.swap(kept);
        rebuild_request_heap(pending_reqs_, pending_req_heap_);
    } else if (pending_req_heap_.size() > pending_reqs_.size() * 4 + 64) {
        rebuild_request_heap(pending_reqs_, pending_req_heap_);
    }
}

int AsyncTerrainStreamer::find_resident(uint64_t key, float now_sec, float *out_fade, bool touch_lru, float importance) {
    auto it = key_to_slot_.find(key);
    if (it == key_to_slot_.end()) return -1;
    int idx = it->second;
    if (touch_lru) {
        slots_[idx].last_used_frame = cur_frame_;
        if (importance > 0.0f) slots_[idx].last_importance = importance;
    }
    if (out_fade) {
        float t = std::clamp((now_sec - slots_[idx].ready_time_sec) / std::max(0.05f, slots_[idx].fade_dur_sec), 0.0f, 1.0f);
        *out_fade = t * t * (3.0f - 2.0f * t);
    }
    return idx;
}

void AsyncTerrainStreamer::request_page(uint64_t key, float priority) {
    if (key_to_slot_.find(key) != key_to_slot_.end()) return;
    // Deterministic spatial-hash priority jitter (±12%) breaks up lockstep rectangular grid loading waves!
    uint64_t h = (key ^ (key >> 30)) * 0xbf58476d1ce4e5b9ULL;
    h = (h ^ (h >> 27)) * 0x94d049bb133111ebULL;
    float jitter = 0.88f + static_cast<float>((h >> 32) & 0xFFFFu) * (0.24f / 65535.0f);
    float j_prio = priority * jitter;
    bool limited;
    {
        std::lock_guard<std::mutex> lk(req_mtx_);
        if (inflight_active_.find(key) != inflight_active_.end()) return;
        pending_req_frame_[key] = cur_frame_;
        queue_request(pending_reqs_, pending_req_heap_, key, j_prio);
        limited = workers_limited_;
    }
    if (limited) req_cv_.notify_all();
    else req_cv_.notify_one();
}

int AsyncTerrainStreamer::allocate_cache_slice(
    uint64_t key, float now_sec, uint64_t target_gen,
    std::unordered_map<uint64_t, int> &target_map, bool instant_fade
) {
    auto it = target_map.find(key);
    if (it != target_map.end()) {
        return it->second;
    }

    auto has_resident_child = [&](uint64_t pk, const std::unordered_map<uint64_t, int> &tmap) {
        QuadKey p = QuadKey::unpack(pk);
        if (p.lod >= MAX_LOD) return false;
        for (int ci = 0; ci < 4; ++ci) {
            QuadKey ck{static_cast<uint8_t>(p.lod + 1),
                       static_cast<uint16_t>(p.qx * 2 + (ci & 1)),
                       static_cast<uint16_t>(p.qy * 2 + (ci >> 1))};
            if (tmap.find(ck.pack()) != tmap.end()) return true;
        }
        return false;
    };

    const uint64_t act_gen = active_gen_.load(std::memory_order_acquire);
    const uint64_t pend_gen = pending_gen_.load(std::memory_order_acquire);
    int victim = -1;
    int fallback_victim = -1;
    double best_keep = 1e30;
    double fallback_keep = 1e30;

    if (!free_slots_.empty()) {
        victim = free_slots_.back();
        free_slots_.pop_back();
    } else {
        for (int i = 0; i < page_capacity_; i++) {
            if (slots_[i].key == UINT64_MAX) {
                victim = i;
                break;
            }
            if (slots_[i].pinned) continue;
            // Never overwrite slots already staged for an in-progress async transition.
            if (pend_gen != 0 && slots_[i].generation == pend_gen) continue;

            const bool in_act = (slots_[i].generation == act_gen);
            const auto &owner_map = in_act ? key_to_slot_ : pending_key_to_slot_;
            const QuadKey sq = QuadKey::unpack(slots_[i].key);
            const bool is_leaf = !has_resident_child(slots_[i].key, owner_map);
            const uint64_t age = (cur_frame_ >= slots_[i].last_used_frame) ? (cur_frame_ - slots_[i].last_used_frame) : 0ULL;

            // Bucket 0: stale off-frustum leaves (old high-LOD tiles passed earlier first, keeping L3..L5 horizon).
            // Bucket 1: visible leaves (if saturated, evict lowest-importance / farthest visible leaf).
            // Bucket 2: non-leaf parents (last resort).
            double keep_score = 0.0;
            if (is_leaf && age > 2ULL) {
                keep_score = (sq.lod < 6 ? 1e12 : 0.0) + static_cast<double>(slots_[i].last_used_frame) - static_cast<double>(sq.lod) * 0.1;
            } else if (is_leaf) {
                keep_score = 2e12 + static_cast<double>(slots_[i].last_importance);
            } else {
                keep_score = 4e12 + static_cast<double>(slots_[i].last_used_frame);
            }

            if (is_leaf) {
                if (keep_score < best_keep) {
                    best_keep = keep_score;
                    victim = i;
                }
            }
            if (keep_score < fallback_keep) {
                fallback_keep = keep_score;
                fallback_victim = i;
            }
        }
    }
    if (victim < 0) victim = (fallback_victim >= 0) ? fallback_victim : 0;
    if (slots_[victim].key != UINT64_MAX) {
        if (slots_[victim].generation == act_gen) key_to_slot_.erase(slots_[victim].key);
        else if (pend_gen != 0 && slots_[victim].generation == pend_gen) pending_key_to_slot_.erase(slots_[victim].key);
    }
    QuadKey qk = QuadKey::unpack(key);
    uint32_t fh = static_cast<uint32_t>((key * 2654435761ULL) >> 16) & 0xFFFFu;
    slots_[victim].key = key;
    slots_[victim].generation = target_gen;
    slots_[victim].last_used_frame = cur_frame_;
    slots_[victim].ready_time_sec = instant_fade ? -10.0f : now_sec;
    slots_[victim].fade_dur_sec = 0.18f + static_cast<float>(fh) * (0.12f / 65535.0f);
    slots_[victim].last_importance = 0.0f;
    slots_[victim].pinned = (qk.lod <= 2);
    slots_[victim].imagery = {};
    target_map[key] = victim;
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

    // 2. Frame-time-budgeted DMA uploader (also drains staged async-transition pages cleanly).
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    const uint64_t act_gen = active_gen_.load(std::memory_order_acquire);
    const uint64_t pend_gen = pending_gen_.load(std::memory_order_acquire);
    const int start_idx = static_cast<int>((cur_frame_ * 7u) % NUM_STAGING);
    for (int k = 0; k < NUM_STAGING && uploaded < max_uploads; k++) {
        if (uploaded > 0 && (PlatformWindow::now_sec() - t0) * 1000.0 >= budget_ms) break;
        const int i = (start_idx + k) % NUM_STAGING;
        if (staging_[i].state.load(std::memory_order_acquire) != StageState::READY) continue;

        const uint64_t sgen = staging_[i].generation;
        const bool is_active = (sgen == act_gen);
        const bool is_pending = (pend_gen != 0 && sgen == pend_gen);
        if (!is_active && !is_pending) {
            staging_[i].key = UINT64_MAX;
            staging_[i].generation = 0;
            staging_[i].state.store(StageState::FREE, std::memory_order_release);
            req_cv_.notify_one();
            continue;
        }

        uint64_t key = staging_[i].key;
        int slice = allocate_cache_slice(
            key, now_sec, sgen, is_pending ? pending_key_to_slot_ : key_to_slot_, is_pending);

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

        slots_[slice].imagery = staging_[i].imagery;
        slots_[slice].last_imagery_upload_sec = t0;
        staging_[i].fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        staging_[i].state.store(StageState::UPLOADING, std::memory_order_release);

        {
            std::lock_guard<std::mutex> lk(req_mtx_);
            if (sgen == active_gen_.load(std::memory_order_acquire)) inflight_active_.erase(key);
            else if (pending_gen_.load(std::memory_order_acquire) == sgen) inflight_next_.erase(key);
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
    s.max_pages = page_capacity_;
    for (const auto &kv : key_to_slot_) {
        const uint8_t lod = QuadKey::unpack(kv.first).lod;
        if (lod < s.resident_by_lod.size()) s.resident_by_lod[lod]++;
        const auto &slot = slots_[kv.second];
        if (slot.imagery.target_zoom == 20 && cur_frame_ - slot.last_used_frame <= 8) {
            if (slot.imagery.target_tiles_ready > 0 && slot.imagery.target_tiles_missing == 0) ++s.z20_pages;
            s.z20_tiles_ready += slot.imagery.target_tiles_ready;
            s.z20_tiles_missing += slot.imagery.target_tiles_missing;
        }
    }
    {
        std::lock_guard<std::mutex> lk(req_mtx_);
        s.queued_jobs = static_cast<int>(pending_reqs_.size() + inflight_active_.size() +
                                         pending_reqs_next_.size() + inflight_next_.size());
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

std::vector<QuadKey> AsyncTerrainStreamer::resident_keys_snapshot() const {
    std::vector<QuadKey> out;
    out.reserve(key_to_slot_.size());
    for (const auto &kv : key_to_slot_) {
        out.push_back(QuadKey::unpack(kv.first));
    }
    return out;
}

void AsyncTerrainStreamer::set_anisotropy(int max_aniso) {
    if (!ortho_arr_) return;
    float max_hw = 1.0f;
    glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &max_hw);
    const float a = std::clamp(static_cast<float>(max_aniso), 1.0f, max_hw);
    glTextureParameterf(ortho_arr_, GL_TEXTURE_MAX_ANISOTROPY, a);
}

} // namespace nsim
