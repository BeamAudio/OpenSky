#pragma once

#include <cstdint>

namespace nspain {

constexpr char PACK_MAGIC[4] = {'N', 'S', 'P', 'K'};
constexpr uint32_t PACK_VERSION = 1;

enum PackLayer : uint8_t {
    LAYER_ELEVATION = 1, // 257x257 quantized u16 heightmap (base_m + u16 * step_m)
    LAYER_IMAGERY = 2,   // 256x256 IGN PNOA ortho tile
    LAYER_SURFACE = 3,   // 128x128 RGB mask: R=forest, G=surface_class, B=water
    LAYER_VEGETATION = 4, // PNG: R=normalized vegetation height (0..25m), G=valid
};

enum PackEncoding : uint8_t {
    ENC_RAW_U16_HEIGHT = 1,
    ENC_ZLIB_U16_HEIGHT = 2,
    ENC_JPEG = 3,
    ENC_PNG = 4,
    ENC_WEBP = 5,
};

inline uint64_t make_tile_key(uint8_t layer, uint8_t z, uint32_t x, uint32_t y) {
    return (static_cast<uint64_t>(layer) << 56) |
           (static_cast<uint64_t>(z) << 48) |
           (static_cast<uint64_t>(x & 0xFFFFFFu) << 24) |
           static_cast<uint64_t>(y & 0xFFFFFFu);
}

#pragma pack(push, 1)
struct PackHeader {
    char magic[4];
    uint32_t version;
    uint32_t flags;
    uint32_t reserved0;
    double lon_min;
    double lat_min;
    double lon_max;
    double lat_max;
    uint64_t index_offset;
    uint32_t index_count;
    uint32_t reserved1;
    uint64_t manifest_offset;
    uint64_t manifest_size;
    uint64_t created_unix;
    char region_id[32];
    uint8_t pad[8];
};

struct PackTileEntry {
    uint64_t key;
    uint64_t offset;
    uint32_t size;
    uint16_t width;
    uint16_t height;
    uint8_t encoding;
    uint8_t flags;
    uint16_t reserved;
    uint32_t crc32;
};

struct HeightTileHeader {
    float base_m;
    float step_m;
};
#pragma pack(pop)

static_assert(sizeof(PackHeader) == 128, "PackHeader must be 128 bytes");
static_assert(sizeof(PackTileEntry) == 32, "PackTileEntry must be 32 bytes");
static_assert(sizeof(HeightTileHeader) == 8, "HeightTileHeader must be 8 bytes");

} // namespace nspain
