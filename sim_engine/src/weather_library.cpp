#include "weather_library.hpp"
#include "../third_party/miniz_tinfl.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace nsim {

namespace {

const char *kSeasonNames[nspain::NSWX_SEASONS] = {"WINTER", "SPRING", "SUMMER", "AUTUMN"};
const char *kSlotNames[nspain::NSWX_SLOTS] = {
    "07:15 DAWN", "08:30 EARLY AM", "09:45 FAIR VFR", "11:00 LATE AM", "12:15 PRE-NOON",
    "13:30 NOON", "14:45 EARLY PM", "16:00 MID PM", "17:15 GOLDEN", "18:30 SUNSET", "SHORT NIGHT",
};
const char *kTypeNames[nspain::NSWX_TYPES] = {
    "PYRENEAN THERMAL", "EL CIERZO NW", "FOEHN SUR", "KATABATIC DAWN", "CONVECTIVE STORM",
};

inline float smooth01(float a, float b, float x) {
    float t = std::clamp((x - a) / std::max(1e-6f, b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

} // namespace

WeatherLibrary::~WeatherLibrary() {
    shutdown();
}

void WeatherLibrary::shutdown() {
    close_file(fd_);
    cache_.clear();
    lru_.clear();
    valid_ = false;
}

bool WeatherLibrary::load(const std::string &path) {
    shutdown();
    fd_ = open_read_only(path);
    if (fd_ == kInvalidFileHandle) {
        std::fprintf(stderr, "[WX] Missing %s — run `python3 builder/bake_weather.py`\n", path.c_str());
        return false;
    }
    if (!read_at_offset(fd_, &hdr_, sizeof(hdr_), 0) ||
        std::memcmp(hdr_.magic, nspain::NSWX_MAGIC, 4) != 0 || hdr_.version != nspain::NSWX_VERSION ||
        hdr_.state_count != nspain::NSWX_STATE_COUNT ||
        hdr_.grid_w != nspain::NSWX_GRID_W || hdr_.grid_h != nspain::NSWX_GRID_H ||
        hdr_.levels != nspain::NSWX_LEVELS) {
        std::fprintf(stderr, "[WX] Invalid .nswx header in %s\n", path.c_str());
        shutdown();
        return false;
    }
    entries_.resize(hdr_.state_count);
    if (!read_at_offset(fd_, entries_.data(), entries_.size() * sizeof(nspain::NswxStateEntry), sizeof(hdr_))) {
        shutdown();
        return false;
    }
    for (int i = 0; i < static_cast<int>(entries_.size()); i++) {
        int expect = nspain::nswx_state_index(entries_[i].season, entries_[i].slot, entries_[i].wtype);
        if (expect != i || entries_[i].comp_size == 0) {
            std::fprintf(stderr, "[WX] Corrupt state index at %d\n", i);
            shutdown();
            return false;
        }
    }
    min_e_ = hdr_.min_e_m;
    min_n_ = hdr_.min_n_m;
    span_e_ = std::max(1.0, hdr_.span_e_m);
    span_n_ = std::max(1.0, hdr_.span_n_m);
    field_rgba_.assign(static_cast<size_t>(hdr_.grid_w) * hdr_.grid_h * 4, 0.0f);
    valid_ = true;
    select(1, 2.0f, 0, 0.0f);
    return true;
}

const WeatherLibrary::DecodedState *WeatherLibrary::state(int idx) const {
    auto it = cache_.find(idx);
    if (it != cache_.end()) {
        auto p = std::find(lru_.begin(), lru_.end(), idx);
        if (p != lru_.end()) { lru_.erase(p); lru_.push_back(idx); }
        return &it->second;
    }
    const size_t raw_sz = static_cast<size_t>(hdr_.levels) * hdr_.grid_w * hdr_.grid_h * 4;
    std::vector<uint8_t> comp(entries_[idx].comp_size);
    std::vector<uint8_t> raw(raw_sz);
    if (!read_at_offset(fd_, comp.data(), comp.size(), entries_[idx].offset)) return nullptr;
    if (tinfl_decompress_mem_to_mem(raw.data(), raw_sz, comp.data(), comp.size(),
                                   TINFL_FLAG_PARSE_ZLIB_HEADER) != raw_sz) return nullptr;

    DecodedState st;
    st.f.resize(raw_sz);
    for (size_t i = 0; i < raw_sz; i++) {
        int ch = static_cast<int>(i & 3u);
        st.f[i] = hdr_.chan_lo[ch] + (static_cast<float>(raw[i]) / 255.0f) *
                  (hdr_.chan_hi[ch] - hdr_.chan_lo[ch]);
    }
    while (cache_.size() >= 8) {
        int victim = lru_.front();
        lru_.erase(lru_.begin());
        cache_.erase(victim);
    }
    auto res = cache_.emplace(idx, std::move(st));
    lru_.push_back(idx);
    return &res.first->second;
}

void WeatherLibrary::select(int season, float slot_f, int wtype, float severity) {
    if (!valid_) return;
    season = std::clamp(season, 0, nspain::NSWX_SEASONS - 1);
    wtype = std::clamp(wtype, 0, nspain::NSWX_TYPES - 1);
    slot_f = std::clamp(slot_f, 0.0f, static_cast<float>(nspain::NSWX_SLOTS - 1));
    int s0 = static_cast<int>(slot_f);
    int s1 = std::min(nspain::NSWX_SLOTS - 1, s0 + 1);
    float a = slot_f - s0;
    float sev = smooth01(0.32f, 0.95f, std::clamp(severity, 0.0f, 1.0f));

    blend_n_ = 0;
    auto push = [&](int slot, int type, float w) {
        if (w < 0.004f) return;
        int idx = nspain::nswx_state_index(season, slot, type);
        for (int k = 0; k < blend_n_; k++) {
            if (blend_[k].idx == idx) { blend_[k].w += w; return; }
        }
        blend_[blend_n_++] = {idx, w};
    };
    push(s0, wtype, (1.0f - a) * (1.0f - sev));
    push(s1, wtype, a * (1.0f - sev));
    push(s0, nspain::NSWX_STORM_TYPE, (1.0f - a) * sev);
    push(s1, nspain::NSWX_STORM_TYPE, a * sev);
    if (blend_n_ == 0) blend_[blend_n_++] = {nspain::nswx_state_index(season, s0, wtype), 1.0f};

    float wsum = 0.0f;
    for (int k = 0; k < blend_n_; k++) wsum += blend_[k].w;
    for (int k = 0; k < blend_n_; k++) blend_[k].w /= wsum;

    float sc[16] = {};
    for (int k = 0; k < blend_n_; k++) {
        const auto &e = entries_[blend_[k].idx];
        for (int c = 0; c < 16; c++) sc[c] += e.scalars[c] * blend_[k].w;
    }
    snapshot_.cloud_cov = std::clamp(sc[nspain::S_CLOUD_COV], 0.02f, 0.98f);
    snapshot_.precip = std::clamp(sc[nspain::S_PRECIP], 0.0f, 1.0f);
    snapshot_.precip_mm_hr = snapshot_.precip * 5.0f; // Explicit proxy for baked normalized severity.
    snapshot_.cloud_base_m = sc[nspain::S_CLOUD_BASE_M];
    snapshot_.cloud_thick_m = sc[nspain::S_CLOUD_THICK_M];
    snapshot_.oat_sl_c = sc[nspain::S_OAT_SL_C];
    snapshot_.dewpoint_sl_c = sc[nspain::S_DEWPOINT_SL_C];
    snapshot_.qnh_hpa = sc[nspain::S_QNH_HPA];
    // Older baked libraries may contain pressure values in the wrong units.
    if (!std::isfinite(snapshot_.qnh_hpa) || snapshot_.qnh_hpa < 850.0f || snapshot_.qnh_hpa > 1080.0f)
        snapshot_.qnh_hpa = 1013.25f;
    snapshot_.haze_mult = sc[nspain::S_HAZE_MULT];
    snapshot_.turb_scale = sc[nspain::S_TURB_SCALE];
    snapshot_.visibility_m = sc[nspain::S_VISIBILITY_M];
    snapshot_.wind_dir_from_deg = sc[nspain::S_WIND_DIR_DEG];
    snapshot_.wind_speed_10m_mps = sc[nspain::S_WIND_SPEED_MPS];
    snapshot_.storm_int = sc[nspain::S_STORM_INT];
    snapshot_.wave_amp = sc[nspain::S_WAVE_AMP];
    snapshot_.snowline_m = sc[nspain::S_SNOWLINE_M];
    snapshot_.season = season;
    snapshot_.slot = s0;
    snapshot_.wtype = wtype;
    active_state_ = blend_[0].idx;
    snapshot_.label = std::string(kSeasonNames[season]) + " / " + kSlotNames[s0] + " / " + kTypeNames[wtype];

    // Composite cloud-level field (levels 1..2) rebuilt only on state change.
    const int W = hdr_.grid_w, H = hdr_.grid_h, L = hdr_.levels;
    std::fill(field_rgba_.begin(), field_rgba_.end(), 0.0f);
    for (int k = 0; k < blend_n_; k++) {
        const DecodedState *st = state(blend_[k].idx);
        if (!st) continue;
        for (int lev = 1; lev < std::min(3, L); lev++) {
            const float *src = st->f.data() + static_cast<size_t>(lev) * H * W * 4;
            for (size_t i = 0; i < field_rgba_.size(); i++) field_rgba_[i] += src[i] * blend_[k].w * 0.5f;
        }
    }

    // Human-readable regional summary; this is not an airport observation.
    int wd = static_cast<int>(std::lround(snapshot_.wind_dir_from_deg)) % 360;
    int ws = static_cast<int>(std::lround(snapshot_.wind_speed_10m_mps * 1.94384f));
    int vis_km = static_cast<int>(std::lround(snapshot_.visibility_m / 1000.0f));
    int t = static_cast<int>(std::lround(snapshot_.oat_sl_c - 0.0065f * 540.0f));
    int td = static_cast<int>(std::lround(snapshot_.dewpoint_sl_c - 0.0018f * 540.0f));
    const char *sky = snapshot_.cloud_cov > 0.85f ? "OVC" : snapshot_.cloud_cov > 0.62f ? "BKN"
                    : snapshot_.cloud_cov > 0.40f ? "SCT" : snapshot_.cloud_cov > 0.18f ? "FEW" : "SKC";
    char buf[160];
    std::snprintf(buf, sizeof(buf), "ERA5-BASED CLIMATOLOGY | W %03d/%d kt | %s base %dm | vis %dkm | %d/%d C | QNH %d hPa",
                  wd, ws, sky, static_cast<int>(std::lround(snapshot_.cloud_base_m)),
                  vis_km, t, td, static_cast<int>(std::lround(snapshot_.qnh_hpa)));
    snapshot_.summary = buf;
}

WeatherCell WeatherLibrary::sample(double east_m, double north_m, double agl_m) const {
    WeatherCell out{};
    if (!valid_) return out;
    const int W = hdr_.grid_w, H = hdr_.grid_h, L = hdr_.levels;
    double fx = std::clamp((east_m - min_e_) / span_e_ * (W - 1), 0.0, static_cast<double>(W - 1));
    double fy = std::clamp((north_m - min_n_) / span_n_ * (H - 1), 0.0, static_cast<double>(H - 1));
    int x0 = std::min(W - 2, static_cast<int>(fx));
    int y0 = std::min(H - 2, static_cast<int>(fy));
    float dx = static_cast<float>(fx - x0), dy = static_cast<float>(fy - y0);

    int l0 = 0;
    while (l0 < L - 2 && agl_m > nspain::NSWX_LEVEL_AGL_M[l0 + 1]) l0++;
    int l1 = std::min(L - 1, l0 + 1);
    float lz = static_cast<float>(std::clamp(
        (agl_m - nspain::NSWX_LEVEL_AGL_M[l0]) /
        std::max(1.0, static_cast<double>(nspain::NSWX_LEVEL_AGL_M[l1] - nspain::NSWX_LEVEL_AGL_M[l0])),
        0.0, 1.0));

    for (int k = 0; k < blend_n_; k++) {
        const DecodedState *st = state(blend_[k].idx);
        if (!st) continue;
        const float *f = st->f.data();
        const size_t plane = static_cast<size_t>(H) * W * 4;
        auto at = [&](int lev, int y, int x, int c) -> float {
            return f[static_cast<size_t>(lev) * plane + (static_cast<size_t>(y) * W + x) * 4 + c];
        };
        for (int c = 0; c < 4; c++) {
            float a0 = at(l0, y0, x0, c) * (1 - dx) + at(l0, y0, x0 + 1, c) * dx;
            float b0 = at(l0, y0 + 1, x0, c) * (1 - dx) + at(l0, y0 + 1, x0 + 1, c) * dx;
            float a1 = at(l1, y0, x0, c) * (1 - dx) + at(l1, y0, x0 + 1, c) * dx;
            float b1 = at(l1, y0 + 1, x0, c) * (1 - dx) + at(l1, y0 + 1, x0 + 1, c) * dx;
            float val = ((a0 * (1 - dy) + b0 * dy) * (1 - lz) + (a1 * (1 - dy) + b1 * dy) * lz) * blend_[k].w;
            if (c == 0) out.u += val;
            else if (c == 1) out.v += val;
            else if (c == 2) out.w += val;
            else out.cloud += val;
        }
        float wu = at(std::min(2, L - 1), y0, x0, 2) * (1 - dx) + at(std::min(2, L - 1), y0, x0 + 1, 2) * dx;
        out.w_upper += wu * blend_[k].w;
    }
    return out;
}

const char *WeatherLibrary::season_name(int s) { return kSeasonNames[std::clamp(s, 0, 3)]; }
const char *WeatherLibrary::slot_name(int s) { return kSlotNames[std::clamp(s, 0, nspain::NSWX_SLOTS - 1)]; }
const char *WeatherLibrary::type_name(int t) { return kTypeNames[std::clamp(t, 0, nspain::NSWX_TYPES - 1)]; }

int WeatherLibrary::season_from_month(int month) {
    if (month == 12 || month == 1 || month == 2) return 0;
    if (month >= 3 && month <= 5) return 1;
    if (month >= 6 && month <= 8) return 2;
    return 3;
}

} // namespace nsim
