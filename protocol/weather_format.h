#pragma once

#include <cstdint>

namespace nspain {

constexpr char NSWX_MAGIC[4] = {'N', 'S', 'W', 'X'};
constexpr uint32_t NSWX_VERSION = 1;

constexpr int NSWX_GRID_W = 128;
constexpr int NSWX_GRID_H = 96;
constexpr int NSWX_LEVELS = 4;
constexpr float NSWX_LEVEL_AGL_M[NSWX_LEVELS] = {10.0f, 550.0f, 1800.0f, 4500.0f};

constexpr int NSWX_SEASONS = 4;
constexpr int NSWX_SLOTS = 11; // 0..9 = pre-baked solar TODs, 10 = compressed short night
constexpr int NSWX_NIGHT_SLOT = 10;
constexpr int NSWX_TYPES = 5;
constexpr int NSWX_STORM_TYPE = 4;
constexpr int NSWX_STATE_COUNT = NSWX_SEASONS * NSWX_SLOTS * NSWX_TYPES;

constexpr float NSWX_CHAN_LO[4] = {-45.0f, -45.0f, -14.0f, -1.0f};
constexpr float NSWX_CHAN_HI[4] = {45.0f, 45.0f, 14.0f, 1.0f};

#pragma pack(push, 1)
struct NswxHeader {
    char magic[4];
    uint32_t version;
    uint32_t state_count;
    uint32_t grid_w;
    uint32_t grid_h;
    uint32_t levels;
    double min_e_m;
    double min_n_m;
    double span_e_m;
    double span_n_m;
    float level_agl_m[4];
    float chan_lo[4];
    float chan_hi[4];
    char provenance[48];
};

struct NswxStateEntry {
    uint32_t season;
    uint32_t slot;
    uint32_t wtype;
    uint32_t comp_size;
    uint64_t offset;
    float scalars[16];
    char label[32];
};
#pragma pack(pop)

static_assert(sizeof(NswxHeader) == 152, "NswxHeader must match weather_format.py");
static_assert(sizeof(NswxStateEntry) == 120, "NswxStateEntry must match weather_format.py");

enum NswxScalar : int {
    S_CLOUD_COV = 0,
    S_PRECIP = 1,
    S_CLOUD_BASE_M = 2,
    S_CLOUD_THICK_M = 3,
    S_OAT_SL_C = 4,
    S_DEWPOINT_SL_C = 5,
    S_QNH_HPA = 6,
    S_WIND_DIR_DEG = 7,
    S_WIND_SPEED_MPS = 8,
    S_HAZE_MULT = 9,
    S_TURB_SCALE = 10,
    S_STORM_INT = 11,
    S_VISIBILITY_M = 12,
    S_WAVE_AMP = 13,
    S_SNOWLINE_M = 14,
};

inline int nswx_state_index(int season, int slot, int wtype) {
    return (season * NSWX_SLOTS + slot) * NSWX_TYPES + wtype;
}

} // namespace nspain
