#include "world_objects.hpp"
#include "platform_window.hpp"

#define CGLTF_IMPLEMENTATION
#include "../third_party/cgltf.h"
#include "../third_party/stb_image.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>

namespace nsim {

namespace {

inline int64_t pack_tc(int32_t cx, int32_t cy) {
    return (static_cast<int64_t>(cx) << 32) | static_cast<uint32_t>(cy);
}

// Local right-handed OpenGL builder (X=East, Y=Up, Z=South) mapped into anchor-relative (East, Elev_m, North)
// so landmark.vert's Z-negation (-dn) preserves exact outward CCW winding & world-space lighting normals!
struct LmBuilder {
    std::vector<float> &out; // 12 floats/vert: pos(3), nrm(3), col(4: rgb+rough), meta(2: metal+emiss)
    double ce = 0.0, cn = 0.0, gy = 0.0;
    float c = 1.0f, s = 0.0f;

    LmBuilder(std::vector<float> &v, const PackReader *pack, const PackReader::WorldFrame &frame,
              double lat, double lon, float yaw_deg, double y_offset = 0.0)
        : out(v) {
        ce = (lon - frame.anchor_lon) * frame.metres_lon;
        cn = (lat - frame.anchor_lat) * frame.metres_lat;
        gy = pack->sample_elevation(lat, lon) + y_offset;
        float yr = yaw_deg * DEG2RAD_F;
        c = std::cos(yr);
        s = std::sin(yr);
    }

    void push_v(float lx, float ly, float lz, float nx, float ny, float nz,
                const Vec3 &rgb, float rough, float metal, float emiss) {
        float rx = lx * c + lz * s;
        float rz = -lx * s + lz * c;
        float rnx = nx * c + nz * s;
        float rnz = -nx * s + nz * c;
        out.insert(out.end(), {
            static_cast<float>(ce) + rx, static_cast<float>(gy) + ly, static_cast<float>(cn) - rz,
            rnx, ny, rnz,
            rgb.x, rgb.y, rgb.z, rough, metal, emiss
        });
    }

    void add_quad(Vec3 a, Vec3 b, Vec3 d, Vec3 e, Vec3 n, const Vec3 &rgb, float rough, float metal, float emiss) {
        push_v(a.x, a.y, a.z, n.x, n.y, n.z, rgb, rough, metal, emiss);
        push_v(b.x, b.y, b.z, n.x, n.y, n.z, rgb, rough, metal, emiss);
        push_v(d.x, d.y, d.z, n.x, n.y, n.z, rgb, rough, metal, emiss);
        push_v(a.x, a.y, a.z, n.x, n.y, n.z, rgb, rough, metal, emiss);
        push_v(d.x, d.y, d.z, n.x, n.y, n.z, rgb, rough, metal, emiss);
        push_v(e.x, e.y, e.z, n.x, n.y, n.z, rgb, rough, metal, emiss);
    }

    void add_box(float cx, float y0, float cz, float sx, float sy, float sz,
                 const Vec3 &rgb, float rough, float metal = 0.0f, float emiss = 0.0f) {
        float x0 = cx - sx * 0.5f, x1 = cx + sx * 0.5f;
        float y1 = y0 + sy;
        float z0 = cz - sz * 0.5f, z1 = cz + sz * 0.5f;
        add_quad({x0, y0, z1}, {x1, y0, z1}, {x1, y1, z1}, {x0, y1, z1}, { 0,  0,  1}, rgb, rough, metal, emiss);
        add_quad({x1, y0, z0}, {x0, y0, z0}, {x0, y1, z0}, {x1, y1, z0}, { 0,  0, -1}, rgb, rough, metal, emiss);
        add_quad({x1, y0, z1}, {x1, y0, z0}, {x1, y1, z0}, {x1, y1, z1}, { 1,  0,  0}, rgb, rough, metal, emiss);
        add_quad({x0, y0, z0}, {x0, y0, z1}, {x0, y1, z1}, {x0, y1, z0}, {-1,  0,  0}, rgb, rough, metal, emiss);
        add_quad({x0, y1, z1}, {x1, y1, z1}, {x1, y1, z0}, {x0, y1, z0}, { 0,  1,  0}, rgb, rough, metal, emiss);
    }

    void add_cyl(float cx, float y0, float cz, float r_bot, float r_top, float h, int segs,
                 const Vec3 &rgb, float rough, float metal = 0.0f, float emiss = 0.0f, bool cap_top = true) {
        float y1 = y0 + h;
        float slope = (r_bot - r_top) / std::max(0.01f, h);
        for (int i = 0; i < segs; i++) {
            float a0 = (2.0f * PI_F * i) / segs;
            float a1 = (2.0f * PI_F * (i + 1)) / segs;
            float c0 = std::cos(a0), s0 = std::sin(a0);
            float c1 = std::cos(a1), s1 = std::sin(a1);
            Vec3 n0 = normalize({c0, slope, s0});
            Vec3 n1 = normalize({c1, slope, s1});
            Vec3 b0{cx + r_bot * c0, y0, cz + r_bot * s0};
            Vec3 b1{cx + r_bot * c1, y0, cz + r_bot * s1};
            Vec3 t1{cx + r_top * c1, y1, cz + r_top * s1};
            Vec3 t0{cx + r_top * c0, y1, cz + r_top * s0};
            // Outward CCW when seen from outside (+r)
            push_v(b0.x, b0.y, b0.z, n0.x, n0.y, n0.z, rgb, rough, metal, emiss);
            push_v(t0.x, t0.y, t0.z, n0.x, n0.y, n0.z, rgb, rough, metal, emiss);
            push_v(t1.x, t1.y, t1.z, n1.x, n1.y, n1.z, rgb, rough, metal, emiss);
            push_v(b0.x, b0.y, b0.z, n0.x, n0.y, n0.z, rgb, rough, metal, emiss);
            push_v(t1.x, t1.y, t1.z, n1.x, n1.y, n1.z, rgb, rough, metal, emiss);
            push_v(b1.x, b1.y, b1.z, n1.x, n1.y, n1.z, rgb, rough, metal, emiss);
            if (cap_top && r_top > 0.05f) {
                push_v(cx, y1, cz, 0, 1, 0, rgb, rough, metal, emiss);
                push_v(t1.x, t1.y, t1.z, 0, 1, 0, rgb, rough, metal, emiss);
                push_v(t0.x, t0.y, t0.z, 0, 1, 0, rgb, rough, metal, emiss);
            }
        }
    }

    void add_dome(float cx, float y0, float cz, float rx, float ry, float rz, int rings, int segs,
                  const Vec3 &rgb, float rough, float metal = 0.0f, float emiss = 0.0f) {
        for (int r = 0; r < rings; r++) {
            float p0 = (PI_F * 0.5f * r) / rings;
            float p1 = (PI_F * 0.5f * (r + 1)) / rings;
            float cp0 = std::cos(p0), sp0 = std::sin(p0);
            float cp1 = std::cos(p1), sp1 = std::sin(p1);
            for (int i = 0; i < segs; i++) {
                float a0 = (2.0f * PI_F * i) / segs;
                float a1 = (2.0f * PI_F * (i + 1)) / segs;
                float c0 = std::cos(a0), s0 = std::sin(a0);
                float c1 = std::cos(a1), s1 = std::sin(a1);
                auto pt = [&](float cp, float sp, float ca, float sa) -> Vec3 {
                    return {cx + rx * cp * ca, y0 + ry * sp, cz + rz * cp * sa};
                };
                auto nm = [&](float cp, float sp, float ca, float sa) -> Vec3 {
                    return normalize({cp * ca / std::max(0.1f, rx), sp / std::max(0.1f, ry), cp * sa / std::max(0.1f, rz)});
                };
                Vec3 v00 = pt(cp0, sp0, c0, s0), n00 = nm(cp0, sp0, c0, s0);
                Vec3 v10 = pt(cp0, sp0, c1, s1), n10 = nm(cp0, sp0, c1, s1);
                Vec3 v11 = pt(cp1, sp1, c1, s1), n11 = nm(cp1, sp1, c1, s1);
                Vec3 v01 = pt(cp1, sp1, c0, s0), n01 = nm(cp1, sp1, c0, s0);
                push_v(v00.x, v00.y, v00.z, n00.x, n00.y, n00.z, rgb, rough, metal, emiss);
                push_v(v01.x, v01.y, v01.z, n01.x, n01.y, n01.z, rgb, rough, metal, emiss);
                push_v(v11.x, v11.y, v11.z, n11.x, n11.y, n11.z, rgb, rough, metal, emiss);
                push_v(v00.x, v00.y, v00.z, n00.x, n00.y, n00.z, rgb, rough, metal, emiss);
                push_v(v11.x, v11.y, v11.z, n11.x, n11.y, n11.z, rgb, rough, metal, emiss);
                push_v(v10.x, v10.y, v10.z, n10.x, n10.y, n10.z, rgb, rough, metal, emiss);
            }
        }
    }
};

} // namespace

WorldObjects::~WorldObjects() {
    shutdown();
}

GLuint WorldObjects::load_tex2d(const std::string &path, bool srgb, bool repeat) {
    int w = 0, h = 0, ch = 0;
    unsigned char *pix = stbi_load(path.c_str(), &w, &h, &ch, 4);
    if (!pix || w <= 0 || h <= 0) {
        if (pix) stbi_image_free(pix);
        return 0;
    }
    int levels = 1;
    for (int m = std::max(w, h); m > 1; m >>= 1) levels++;
    GLuint tex = 0;
    glCreateTextures(GL_TEXTURE_2D, 1, &tex);
    glTextureStorage2D(tex, levels, srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8, w, h);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTextureSubImage2D(tex, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pix);
    glGenerateTextureMipmap(tex);
    glTextureParameteri(tex, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTextureParameteri(tex, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    GLint wrap = repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE;
    glTextureParameteri(tex, GL_TEXTURE_WRAP_S, wrap);
    glTextureParameteri(tex, GL_TEXTURE_WRAP_T, wrap);
    glTextureParameterf(tex, GL_TEXTURE_MAX_ANISOTROPY, 4.0f);
    stbi_image_free(pix);
    return tex;
}

bool WorldObjects::is_dense_urban_en(double e, double n) const {
    for (const CityRegionGpu &cr : city_regions_) {
        if (cr.dense_cells.empty()) continue;
        double le = (e - cr.off_e) / cr.scl_e;
        double ln = (n - cr.off_n) / cr.scl_n;
        int32_t cx = static_cast<int32_t>(std::floor(le / 500.0));
        int32_t cy = static_cast<int32_t>(std::floor(ln / 500.0));
        if (cr.dense_cells.find(pack_tc(cx, cy)) != cr.dense_cells.end()) return true;
    }
    return false;
}

bool WorldObjects::load_city_region_meta(CityRegionGpu &cr) {
    if (cr.meta_loaded || cr.city_path.empty()) return cr.meta_loaded;
    std::ifstream ifs(cr.city_path, std::ios::binary);
    if (!ifs.is_open()) return false;

#pragma pack(push, 1)
    struct RawCityHdr {
        char magic[4];
        uint32_t version;
        uint32_t total_bldgs;
        uint32_t chunk_count;
        uint32_t vert_count;
        uint32_t idx_count;
    };
    struct RawChunkMeta {
        uint32_t is_urban, first_index, index_count;
        int32_t base_vertex;
        float ce, cy, cn, rad;
    };
#pragma pack(pop)
    static_assert(sizeof(RawChunkMeta) == 32, "RawChunkMeta layout mismatch");

    RawCityHdr hdr{};
    ifs.read(reinterpret_cast<char *>(&hdr), sizeof(hdr));
    if (!ifs || std::memcmp(hdr.magic, "NSCT", 4) != 0 || hdr.version != 3 ||
        hdr.chunk_count == 0 || hdr.vert_count == 0 || hdr.idx_count == 0) {
        return false;
    }

    std::vector<RawChunkMeta> rchunks(hdr.chunk_count);
    ifs.read(reinterpret_cast<char *>(rchunks.data()), static_cast<std::streamsize>(rchunks.size() * sizeof(RawChunkMeta)));
    if (!ifs) return false;

    cr.vert_file_off = sizeof(RawCityHdr) + static_cast<uint64_t>(hdr.chunk_count) * sizeof(RawChunkMeta);
    cr.idx_file_off  = cr.vert_file_off + static_cast<uint64_t>(hdr.vert_count) * 32ULL;
    cr.disk_chunks.clear();
    cr.disk_chunks.reserve(hdr.chunk_count);
    cr.dense_cells.clear();
    for (uint32_t i = 0; i < hdr.chunk_count; i++) {
        const RawChunkMeta &rc = rchunks[i];
        const uint32_t v0 = static_cast<uint32_t>(std::max(0, rc.base_vertex));
        const uint32_t v1 = (i + 1 < hdr.chunk_count)
            ? static_cast<uint32_t>(std::max(0, rchunks[i + 1].base_vertex))
            : hdr.vert_count;
        const uint32_t vcnt = (v1 >= v0) ? (v1 - v0) : 0u;
        cr.disk_chunks.push_back({rc.is_urban, rc.first_index, rc.index_count, v0, vcnt, rc.ce, rc.cy, rc.cn, rc.rad});
        if (rc.is_urban) {
            int32_t cx = static_cast<int32_t>(std::floor(rc.ce / 500.0f));
            int32_t cy = static_cast<int32_t>(std::floor(rc.cn / 500.0f));
            cr.dense_cells[pack_tc(cx, cy)] = 1;
        }
    }
    cr.bldgs = static_cast<int>(hdr.total_bldgs);
    cr.meta_loaded = true;
    std::printf("[CITY-BAKE] Indexed %s (local 28km GPU streaming): %u bldgs | %u chunks (%zu dense urban)\n",
                cr.city_path.c_str(), hdr.total_bldgs, hdr.chunk_count, cr.dense_cells.size());
    return true;
}

namespace {
constexpr size_t kCityVertexBytes = 32;
}

std::optional<WorldObjects::CityRegionData> WorldObjects::read_city_region(
    const CityRegionGpu &cr, double cam_le, double cam_ln) {

    // Stream chunks within 22 km of camera, prioritizing nearest chunks under an iGPU-safe
    // vertex cap (~3.6M verts / ~130 MB GPU) so Barcelona keeps full forward skyline within VRAM.
    static constexpr double kStreamRadiusM = 22000.0;
    static constexpr size_t kMaxStreamVerts = 3600000;
    const double r2_max = kStreamRadiusM * kStreamRadiusM;

    struct Cand { uint32_t idx; double d2; };
    struct Run { uint32_t first_ch, last_ch; };
    std::vector<Cand> cands;
    cands.reserve(1500);
    for (uint32_t i = 0; i < static_cast<uint32_t>(cr.disk_chunks.size()); ++i) {
        const auto &dc = cr.disk_chunks[i];
        const double de = dc.ce - cam_le;
        const double dn = dc.cn - cam_ln;
        const double d2 = de * de + dn * dn;
        if (d2 <= r2_max && dc.vert_count > 0 && dc.index_count > 0) {
            cands.push_back({i, d2});
        }
    }
    std::sort(cands.begin(), cands.end(), [](const Cand &a, const Cand &b) { return a.d2 < b.d2; });

    std::vector<uint32_t> selected;
    selected.reserve(cands.size());
    size_t tot_verts = 0, tot_idx = 0;
    for (const Cand &c : cands) {
        const auto &dc = cr.disk_chunks[c.idx];
        if (tot_verts > 0 && tot_verts + dc.vert_count > kMaxStreamVerts) break;
        selected.push_back(c.idx);
        tot_verts += dc.vert_count;
        tot_idx += dc.index_count;
    }
    // Re-sort in file order so contiguous NVMe read coalescing stays fast.
    std::sort(selected.begin(), selected.end());

    CityRegionData data;
    if (selected.empty()) return data;

    std::ifstream ifs(cr.city_path, std::ios::binary);
    if (!ifs.is_open()) return std::nullopt;

    // Merge adjacent selected chunks into contiguous file reads for fast NVMe I/O
    std::vector<Run> runs;
    runs.reserve(256);
    for (uint32_t idx : selected) {
        if (!runs.empty() && runs.back().last_ch + 1 == idx) runs.back().last_ch = idx;
        else runs.push_back({idx, idx});
    }

    data.vertices.resize(tot_verts * kCityVertexBytes);
    data.indices.resize(tot_idx);
    data.chunks.reserve(selected.size());
    data.macro_grid.reserve(512);

    size_t v_cur = 0, i_cur = 0;
    for (const Run &run : runs) {
        const auto &first_dc = cr.disk_chunks[run.first_ch];
        const auto &last_dc  = cr.disk_chunks[run.last_ch];
        const size_t run_v_cnt = (last_dc.base_vertex + last_dc.vert_count) - first_dc.base_vertex;
        const size_t run_i_cnt = (last_dc.first_index + last_dc.index_count) - first_dc.first_index;

        if (run_v_cnt > tot_verts - v_cur || run_i_cnt > tot_idx - i_cur) return std::nullopt;
        ifs.seekg(static_cast<std::streamoff>(cr.vert_file_off + static_cast<uint64_t>(first_dc.base_vertex) * kCityVertexBytes));
        ifs.read(reinterpret_cast<char *>(data.vertices.data() + v_cur * kCityVertexBytes), static_cast<std::streamsize>(run_v_cnt * kCityVertexBytes));
        ifs.seekg(static_cast<std::streamoff>(cr.idx_file_off + static_cast<uint64_t>(first_dc.first_index) * sizeof(uint32_t)));
        ifs.read(reinterpret_cast<char *>(data.indices.data() + i_cur), static_cast<std::streamsize>(run_i_cnt * sizeof(uint32_t)));
        if (!ifs) return std::nullopt;

        for (uint32_t k = run.first_ch; k <= run.last_ch; ++k) {
            const auto &dc = cr.disk_chunks[k];
            const uint32_t local_first_idx = static_cast<uint32_t>(i_cur + (dc.first_index - first_dc.first_index));
            const int32_t local_base_v = static_cast<int32_t>(v_cur + (dc.base_vertex - first_dc.base_vertex));
            const uint32_t out_i = static_cast<uint32_t>(data.chunks.size());
            data.chunks.push_back({dc.is_urban, local_first_idx, dc.index_count, local_base_v, dc.ce, dc.cy, dc.cn, dc.rad});
            int32_t mx = static_cast<int32_t>(std::floor(dc.ce / 4000.0f));
            int32_t my = static_cast<int32_t>(std::floor(dc.cn / 4000.0f));
            data.macro_grid[pack_tc(mx, my)].push_back(out_i);
        }
        v_cur += run_v_cnt;
        i_cur += run_i_cnt;
    }

    return data;
}

void WorldObjects::publish_city_region(CityRegionGpu &cr, CityRegionData &&data,
                                       double le, double ln, GLuint vbo, GLuint ebo) {
    unload_city_region_gpu(cr);
    cr.stream_le = le;
    cr.stream_ln = ln;
    cr.gpu_streamed = true;
    if (data.vertices.empty()) return;
    cr.chunks = std::move(data.chunks);
    cr.macro_grid = std::move(data.macro_grid);
    cr.vbo = vbo;
    cr.ebo = ebo;
    glCreateVertexArrays(1, &cr.vao);
    glVertexArrayVertexBuffer(cr.vao, 0, cr.vbo, 0, kCityVertexBytes);
    glVertexArrayElementBuffer(cr.vao, cr.ebo);
    glEnableVertexArrayAttrib(cr.vao, 0); glVertexArrayAttribFormat(cr.vao, 0, 3, GL_FLOAT, GL_FALSE, 0); glVertexArrayAttribBinding(cr.vao, 0, 0);
    glEnableVertexArrayAttrib(cr.vao, 1); glVertexArrayAttribFormat(cr.vao, 1, 3, GL_BYTE, GL_TRUE, 12); glVertexArrayAttribBinding(cr.vao, 1, 0);
    glEnableVertexArrayAttrib(cr.vao, 2); glVertexArrayAttribFormat(cr.vao, 2, 1, GL_UNSIGNED_BYTE, GL_FALSE, 15); glVertexArrayAttribBinding(cr.vao, 2, 0);
    glEnableVertexArrayAttrib(cr.vao, 3); glVertexArrayAttribFormat(cr.vao, 3, 2, GL_FLOAT, GL_FALSE, 16); glVertexArrayAttribBinding(cr.vao, 3, 0);
    glEnableVertexArrayAttrib(cr.vao, 4); glVertexArrayAttribIFormat(cr.vao, 4, 1, GL_UNSIGNED_INT, 24); glVertexArrayAttribBinding(cr.vao, 4, 0);
    glEnableVertexArrayAttrib(cr.vao, 5); glVertexArrayAttribFormat(cr.vao, 5, 4, GL_UNSIGNED_BYTE, GL_FALSE, 28); glVertexArrayAttribBinding(cr.vao, 5, 0);
    std::printf("[CITY-STREAM] %s local 22km: %zu/%zu chunks | %zu verts | %.1f MB GPU\n",
                cr.region_id.c_str(), cr.chunks.size(), cr.disk_chunks.size(), data.vertices.size() / kCityVertexBytes,
                (data.vertices.size() + data.indices.size() * sizeof(uint32_t)) / (1024.0 * 1024.0));
}

void WorldObjects::unload_city_region_gpu(CityRegionGpu &cr) {
    if (cr.vao) { glDeleteVertexArrays(1, &cr.vao); cr.vao = 0; }
    if (cr.vbo) { glDeleteBuffers(1, &cr.vbo); cr.vbo = 0; }
    if (cr.ebo) { glDeleteBuffers(1, &cr.ebo); cr.ebo = 0; }
    cr.chunks.clear();
    cr.macro_grid.clear();
    cr.gpu_streamed = false;
}

void WorldObjects::update_city_region_transforms() {
    for (CityRegionGpu &cr : city_regions_) {
        cr.off_e = static_cast<float>((cr.anchor_lon - pack_->anchor_lon()) * pack_->anchor_m_lon());
        cr.off_n = static_cast<float>((cr.anchor_lat - pack_->anchor_lat()) * pack_->anchor_m_lat());
        cr.scl_e = static_cast<float>(pack_->anchor_m_lon() / std::max(1.0, cr.m_lon));
        cr.scl_n = static_cast<float>(pack_->anchor_m_lat() / std::max(1.0, cr.m_lat));
    }
}

void WorldObjects::cancel_road_stream() {
    if (road_stream_.job.valid()) road_stream_.job.wait();
    road_stream_ = {};
}

void WorldObjects::reset_city_stream() {
    cancel_road_stream();
    if (city_stream_.job.valid()) city_stream_.job.wait();
    if (city_stream_.vbo) glDeleteBuffers(1, &city_stream_.vbo);
    if (city_stream_.ebo) glDeleteBuffers(1, &city_stream_.ebo);
    city_stream_ = {};
}

void WorldObjects::ensure_city_regions_for_camera(double cam_east_m, double cam_north_m, bool preload) {
    constexpr double recenter_m = 4200.0;
    constexpr size_t upload_budget = 2 * 1024 * 1024;
    const double now = PlatformWindow::now_sec();
    double clat = pack_->anchor_lat() + cam_north_m / pack_->anchor_m_lat();
    double clon = pack_->anchor_lon() + cam_east_m / pack_->anchor_m_lon();
    auto &pending = city_stream_;
    for (CityRegionGpu &cr : city_regions_) {
        double nearest_lat = std::clamp(clat, cr.lat_min, cr.lat_max);
        double nearest_lon = std::clamp(clon, cr.lon_min, cr.lon_max);
        const bool in_range = nspain::haversine_m(clat, clon, nearest_lat, nearest_lon) <= 26000.0;
        const double le = (cam_east_m - cr.off_e) / cr.scl_e;
        const double ln = (cam_north_m - cr.off_n) / cr.scl_n;
        if (&cr == pending.region) {
            if (pending.job.valid() && pending.job.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
                continue;
            if (!in_range || std::hypot(le - pending.le, ln - pending.ln) > recenter_m) {
                reset_city_stream();
            } else {
                if (pending.job.valid()) {
                    auto data = pending.job.get();
                    if (!data) {
                        cr.stream_retry_time = now + 5.0;
                        reset_city_stream();
                        continue;
                    }
                    pending.data = std::move(*data);
                    if (!pending.data.vertices.empty()) {
                        glCreateBuffers(1, &pending.vbo);
                        glCreateBuffers(1, &pending.ebo);
                        glNamedBufferStorage(pending.vbo, pending.data.vertices.size(), nullptr, GL_DYNAMIC_STORAGE_BIT);
                        glNamedBufferStorage(pending.ebo, pending.data.indices.size() * sizeof(uint32_t), nullptr, GL_DYNAMIC_STORAGE_BIT);
                    }
                }
                const size_t vertex_bytes = pending.data.vertices.size();
                const size_t total_bytes = vertex_bytes + pending.data.indices.size() * sizeof(uint32_t);
                if (pending.uploaded < vertex_bytes) {
                    size_t bytes = std::min(upload_budget, vertex_bytes - pending.uploaded);
                    glNamedBufferSubData(pending.vbo, pending.uploaded, bytes, pending.data.vertices.data() + pending.uploaded);
                    pending.uploaded += bytes;
                } else if (pending.uploaded < total_bytes) {
                    size_t bytes = std::min(upload_budget, total_bytes - pending.uploaded);
                    size_t offset = pending.uploaded - vertex_bytes;
                    glNamedBufferSubData(pending.ebo, offset, bytes, pending.data.indices.data() + offset / sizeof(uint32_t));
                    pending.uploaded += bytes;
                }
                if (pending.uploaded == total_bytes) {
                    publish_city_region(cr, std::move(pending.data), pending.le, pending.ln, pending.vbo, pending.ebo);
                    pending = {};
                }
                continue;
            }
        }
        if (!in_range) {
            if (cr.gpu_streamed) unload_city_region_gpu(cr);
            continue;
        }
        if (pending.region || now < cr.stream_retry_time ||
            (cr.gpu_streamed && std::hypot(le - cr.stream_le, ln - cr.stream_ln) <= recenter_m)) continue;
        if (!load_city_region_meta(cr)) {
            cr.stream_retry_time = now + 5.0;
            continue;
        }
        if (preload) {
            auto data = read_city_region(cr, le, ln);
            if (!data) continue;
            GLuint vbo = 0, ebo = 0;
            if (!data->vertices.empty()) {
                glCreateBuffers(1, &vbo);
                glCreateBuffers(1, &ebo);
                glNamedBufferStorage(vbo, data->vertices.size(), data->vertices.data(), 0);
                glNamedBufferStorage(ebo, data->indices.size() * sizeof(uint32_t), data->indices.data(), 0);
            }
            publish_city_region(cr, std::move(*data), le, ln, vbo, ebo);
            continue;
        }
        // City metadata is immutable once indexed. GL buffer creation and
        // publication stay on the render thread while the pack is read here.
        pending.region = &cr;
        pending.le = le;
        pending.ln = ln;
        pending.job = std::async(std::launch::async, read_city_region, std::cref(cr), le, ln);
    }
}

bool WorldObjects::load_baked_cities(const std::string &pack_dir) {
    reset_city_stream();
    for (auto &cr : city_regions_) unload_city_region_gpu(cr);
    city_regions_.clear();
    baked_city_bldgs_ = 0;

    for (const auto &reg : pack_->regions()) {
        if (reg.city_path.empty() || !std::filesystem::exists(reg.city_path)) continue;
        CityRegionGpu cr;
        cr.region_id = reg.region_id;
        cr.city_path = reg.city_path;
        cr.lat_min = reg.hdr.lat_min; cr.lon_min = reg.hdr.lon_min;
        cr.lat_max = reg.hdr.lat_max; cr.lon_max = reg.hdr.lon_max;
        cr.anchor_lat = reg.anchor_lat; cr.anchor_lon = reg.anchor_lon;
        cr.m_lat = reg.m_lat; cr.m_lon = reg.m_lon;
        city_regions_.push_back(std::move(cr));
    }
    if (city_regions_.empty()) {
        std::string fallback = pack_dir + "/es_aragon_cities.nscity";
        if (std::filesystem::exists(fallback)) {
            CityRegionGpu cr;
            cr.region_id = "es_aragon_pyrenees";
            cr.city_path = fallback;
            cr.lat_min = pack_->lat_min(); cr.lon_min = pack_->lon_min();
            cr.lat_max = pack_->lat_max(); cr.lon_max = pack_->lon_max();
            cr.anchor_lat = pack_->anchor_lat(); cr.anchor_lon = pack_->anchor_lon();
            cr.m_lat = pack_->anchor_m_lat(); cr.m_lon = pack_->anchor_m_lon();
            city_regions_.push_back(std::move(cr));
        }
    }
    update_city_region_transforms();
    ensure_city_regions_for_camera(0.0, 0.0, true);
    for (const auto &cr : city_regions_) baked_city_bldgs_ += cr.bldgs;
    baked_map_wide_ = !city_regions_.empty();
    return baked_map_wide_;
}

bool WorldObjects::load_textured_glb_append(
    const std::string &path,
    std::vector<float> &verts, std::vector<uint32_t> &indices,
    DrawCmd &out_cmd
) {
    cgltf_options opts{};
    cgltf_data *data = nullptr;
    if (cgltf_parse_file(&opts, path.c_str(), &data) != cgltf_result_success) {
        std::fprintf(stderr, "[GLB] Failed parse: %s\n", path.c_str());
        return false;
    }
    if (cgltf_load_buffers(&opts, data, path.c_str()) != cgltf_result_success) {
        cgltf_free(data);
        return false;
    }

    out_cmd.baseVertex = static_cast<int32_t>(verts.size() / 8);
    out_cmd.firstIndex = static_cast<uint32_t>(indices.size());
    uint32_t vert_cursor = 0;

    for (size_t mi = 0; mi < data->meshes_count; mi++) {
        const cgltf_mesh &m = data->meshes[mi];
        for (size_t pi = 0; pi < m.primitives_count; pi++) {
            const cgltf_primitive &pr = m.primitives[pi];
            const cgltf_accessor *pos_a = nullptr, *nrm_a = nullptr, *uv_a = nullptr;
            for (size_t ai = 0; ai < pr.attributes_count; ai++) {
                if (pr.attributes[ai].type == cgltf_attribute_type_position) pos_a = pr.attributes[ai].data;
                else if (pr.attributes[ai].type == cgltf_attribute_type_normal) nrm_a = pr.attributes[ai].data;
                else if (pr.attributes[ai].type == cgltf_attribute_type_texcoord) uv_a = pr.attributes[ai].data;
            }
            if (!pos_a) continue;
            size_t vc = pos_a->count;
            for (size_t v = 0; v < vc; v++) {
                float p[3] = {0, 0, 0}, n[3] = {0, 1, 0}, t[2] = {0, 0};
                cgltf_accessor_read_float(pos_a, v, p, 3);
                if (nrm_a) cgltf_accessor_read_float(nrm_a, v, n, 3);
                if (uv_a) cgltf_accessor_read_float(uv_a, v, t, 2);
                verts.insert(verts.end(), {p[0], p[1], p[2], n[0], n[1], n[2], t[0], t[1]});
            }
            if (pr.indices) {
                for (size_t ii = 0; ii < pr.indices->count; ii++) {
                    indices.push_back(vert_cursor + static_cast<uint32_t>(cgltf_accessor_read_index(pr.indices, ii)));
                }
            } else {
                for (size_t ii = 0; ii < vc; ii++) indices.push_back(vert_cursor + static_cast<uint32_t>(ii));
            }
            vert_cursor += static_cast<uint32_t>(vc);
        }
    }
    out_cmd.count = static_cast<uint32_t>(indices.size()) - out_cmd.firstIndex;
    out_cmd.instanceCount = 0;
    cgltf_free(data);
    return out_cmd.count > 0;
}

bool WorldObjects::load_colored_glb(const std::string &path, GlbMeshData &out, bool keep_cpu_geom) {
    cgltf_options opts{};
    cgltf_data *data = nullptr;
    if (cgltf_parse_file(&opts, path.c_str(), &data) != cgltf_result_success) return false;
    if (cgltf_load_buffers(&opts, data, path.c_str()) != cgltf_result_success) {
        cgltf_free(data);
        return false;
    }

    // Detect which GLB images are linear data maps (normal / metallic-roughness) vs sRGB albedo + sampler wrap mode!
    std::vector<bool> img_is_linear(data->images_count, false);
    std::vector<bool> img_clamp(data->images_count, false);
    for (size_t mi = 0; mi < data->materials_count; mi++) {
        const cgltf_material &mat = data->materials[mi];
        auto mark_tex = [&](const cgltf_texture_view &tv, bool is_lin) {
            if (!tv.texture || !tv.texture->image) return;
            size_t idx_img = static_cast<size_t>(tv.texture->image - data->images);
            if (idx_img >= data->images_count) return;
            if (is_lin) img_is_linear[idx_img] = true;
            if (tv.texture->sampler && tv.texture->sampler->wrap_s == 33071) img_clamp[idx_img] = true;
        };
        if (mat.has_pbr_metallic_roughness) {
            mark_tex(mat.pbr_metallic_roughness.base_color_texture, false);
            mark_tex(mat.pbr_metallic_roughness.metallic_roughness_texture, true);
        }
        mark_tex(mat.normal_texture, true);
    }

    // Decode all embedded GLB images with exact color space (sRGB albedo vs linear normal/MR)!
    std::vector<GLuint> img_tex(data->images_count, 0);
    out.owned_textures.clear();
    for (size_t ii = 0; ii < data->images_count; ii++) {
        const cgltf_image &im = data->images[ii];
        if (!im.buffer_view || !im.buffer_view->buffer || !im.buffer_view->buffer->data) continue;
        const uint8_t *bytes = static_cast<const uint8_t *>(im.buffer_view->buffer->data) + im.buffer_view->offset;
        int w = 0, h = 0, ch = 0;
        unsigned char *pix = stbi_load_from_memory(bytes, static_cast<int>(im.buffer_view->size), &w, &h, &ch, 4);
        if (!pix || w <= 0 || h <= 0) { if (pix) stbi_image_free(pix); continue; }
        int levels = 1;
        for (int m = std::max(w, h); m > 1; m >>= 1) levels++;
        GLuint tex = 0;
        glCreateTextures(GL_TEXTURE_2D, 1, &tex);
        glTextureStorage2D(tex, levels, img_is_linear[ii] ? GL_RGBA8 : GL_SRGB8_ALPHA8, w, h);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTextureSubImage2D(tex, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pix);
        glGenerateTextureMipmap(tex);
        glTextureParameteri(tex, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTextureParameteri(tex, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        GLint wrap = img_clamp[ii] ? GL_CLAMP_TO_EDGE : GL_REPEAT;
        glTextureParameteri(tex, GL_TEXTURE_WRAP_S, wrap);
        glTextureParameteri(tex, GL_TEXTURE_WRAP_T, wrap);
        glTextureParameterf(tex, GL_TEXTURE_MAX_ANISOTROPY, 4.0f);
        stbi_image_free(pix);
        img_tex[ii] = tex;
        out.owned_textures.push_back(tex);
    }

    // Preserve complete glTF node hierarchy & local transforms for real-time per-part animation!
    out.nodes.clear();
    out.node_index.clear();
    out.nodes.resize(data->nodes_count);
    for (size_t ni = 0; ni < data->nodes_count; ni++) {
        const cgltf_node &nd = data->nodes[ni];
        GlbNode &gn = out.nodes[ni];
        gn.name = nd.name ? std::string(nd.name) : ("node_" + std::to_string(ni));
        gn.parent = nd.parent ? static_cast<int>(nd.parent - data->nodes) : -1;
        float ml[16];
        cgltf_node_transform_local(&nd, ml);
        std::memcpy(gn.base_local.m, ml, sizeof(ml));
        out.node_index[gn.name] = static_cast<int>(ni);
    }

    std::vector<float> verts; // pos(3), nrm(3), col(4), uv(2) = 12 floats (in node-local space!)
    std::vector<uint32_t> indices;
    out.submeshes.clear();

    for (size_t ni = 0; ni < data->nodes_count; ni++) {
        const cgltf_node &nd = data->nodes[ni];
        if (!nd.mesh) continue;
        const cgltf_mesh &m = *nd.mesh;
        for (size_t pi = 0; pi < m.primitives_count; pi++) {
            const cgltf_primitive &pr = m.primitives[pi];
            const cgltf_accessor *pos_a = nullptr, *nrm_a = nullptr, *col_a = nullptr, *uv_a = nullptr;
            for (size_t ai = 0; ai < pr.attributes_count; ai++) {
                if (pr.attributes[ai].type == cgltf_attribute_type_position) pos_a = pr.attributes[ai].data;
                else if (pr.attributes[ai].type == cgltf_attribute_type_normal) nrm_a = pr.attributes[ai].data;
                else if (pr.attributes[ai].type == cgltf_attribute_type_color) col_a = pr.attributes[ai].data;
                else if (pr.attributes[ai].type == cgltf_attribute_type_texcoord) uv_a = pr.attributes[ai].data;
            }
            if (!pos_a) continue;

            GlbSubmesh sm{};
            sm.base_vertex = static_cast<int32_t>(verts.size() / 12);
            sm.first_index = static_cast<uint32_t>(indices.size());
            sm.node_idx = static_cast<int>(ni);
            auto resolve_tex = [&](const cgltf_texture_view &tv) -> GLuint {
                if (!tv.texture || !tv.texture->image) return 0;
                size_t idx_img = static_cast<size_t>(tv.texture->image - data->images);
                return (idx_img < img_tex.size()) ? img_tex[idx_img] : 0;
            };
            if (pr.material && pr.material->has_pbr_metallic_roughness) {
                const auto &pbr = pr.material->pbr_metallic_roughness;
                sm.base_color = {pbr.base_color_factor[0], pbr.base_color_factor[1], pbr.base_color_factor[2], pbr.base_color_factor[3]};
                sm.metallic = pbr.metallic_factor;
                sm.roughness = pbr.roughness_factor;
                sm.albedo_tex = resolve_tex(pbr.base_color_texture);
                sm.mr_tex = resolve_tex(pbr.metallic_roughness_texture);
            }
            if (pr.material) {
                sm.material_name = pr.material->name ? pr.material->name : "";
                sm.normal_tex = resolve_tex(pr.material->normal_texture);
                sm.emissive_col = {pr.material->emissive_factor[0], pr.material->emissive_factor[1], pr.material->emissive_factor[2]};
                sm.double_sided = pr.material->double_sided != 0;
                sm.unlit = pr.material->unlit != 0;
            }

            size_t vc = pos_a->count;
            for (size_t v = 0; v < vc; v++) {
                float p[3] = {0, 0, 0}, n[3] = {0, 1, 0}, c[4] = {1, 1, 1, 1}, uv[2] = {0, 0};
                cgltf_accessor_read_float(pos_a, v, p, 3);
                if (nrm_a) cgltf_accessor_read_float(nrm_a, v, n, 3);
                if (col_a) cgltf_accessor_read_float(col_a, v, c, 4);
                if (uv_a)  cgltf_accessor_read_float(uv_a, v, uv, 2);
                Vec3 nn = normalize({n[0], n[1], n[2]});
                verts.insert(verts.end(), {p[0], p[1], p[2], nn.x, nn.y, nn.z, c[0], c[1], c[2], c[3], uv[0], uv[1]});
            }
            if (pr.indices) {
                for (size_t ii = 0; ii < pr.indices->count; ii++) {
                    indices.push_back(static_cast<uint32_t>(cgltf_accessor_read_index(pr.indices, ii)));
                }
            } else {
                for (size_t ii = 0; ii < vc; ii++) indices.push_back(static_cast<uint32_t>(ii));
            }
            sm.index_count = static_cast<uint32_t>(indices.size()) - sm.first_index;
            if (sm.index_count > 0) out.submeshes.push_back(sm);
        }
    }
    cgltf_free(data);
    if (verts.empty() || indices.empty()) return false;

    glCreateVertexArrays(1, &out.vao);
    glCreateBuffers(1, &out.vbo);
    glCreateBuffers(1, &out.ebo);
    glNamedBufferStorage(out.vbo, static_cast<GLsizeiptr>(verts.size() * sizeof(float)), verts.data(), 0);
    glNamedBufferStorage(out.ebo, static_cast<GLsizeiptr>(indices.size() * sizeof(uint32_t)), indices.data(), 0);
    glVertexArrayVertexBuffer(out.vao, 0, out.vbo, 0, 12 * sizeof(float));
    glVertexArrayElementBuffer(out.vao, out.ebo);
    glEnableVertexArrayAttrib(out.vao, 0); glVertexArrayAttribFormat(out.vao, 0, 3, GL_FLOAT, GL_FALSE, 0); glVertexArrayAttribBinding(out.vao, 0, 0);
    glEnableVertexArrayAttrib(out.vao, 1); glVertexArrayAttribFormat(out.vao, 1, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float)); glVertexArrayAttribBinding(out.vao, 1, 0);
    glEnableVertexArrayAttrib(out.vao, 2); glVertexArrayAttribFormat(out.vao, 2, 4, GL_FLOAT, GL_FALSE, 6 * sizeof(float)); glVertexArrayAttribBinding(out.vao, 2, 0);
    glEnableVertexArrayAttrib(out.vao, 3); glVertexArrayAttribFormat(out.vao, 3, 2, GL_FLOAT, GL_FALSE, 10 * sizeof(float)); glVertexArrayAttribBinding(out.vao, 3, 0);
    if (keep_cpu_geom) {
        out.cpu_verts = std::move(verts);
        out.cpu_indices = std::move(indices);
    }
    return true;
}

bool WorldObjects::init(const PackReader *pack, const std::string &asset_root, const std::string &shader_root, const std::string &pack_dir) {
    pack_ = pack;
    cull_comp_     = compile_compute_file(shader_root + "/cull_instances.comp");
    city_prog_     = compile_program_files(shader_root + "/city_baked.vert", shader_root + "/city_baked.frag");
    city_far_prog_ = compile_program_files(shader_root + "/city_baked.vert", shader_root + "/city_baked_far.frag");
    bldg_prog_     = compile_program_files(shader_root + "/building.vert", shader_root + "/building.frag");
    bldg_sh_prog_  = compile_program_files(shader_root + "/bldg_shadow.vert", shader_root + "/bldg_shadow.frag");
    bldg_lod1_prog_= compile_program_files(shader_root + "/bldg_lod1.vert", shader_root + "/bldg_lod1.frag");
    rwy_prog_      = compile_program_files(shader_root + "/runway.vert",   shader_root + "/runway.frag");
    road_prog_     = compile_program_files(shader_root + "/road.vert",     shader_root + "/road.frag");
    traffic_prog_  = compile_program_files(shader_root + "/traffic.vert",  shader_root + "/traffic.frag");
    landmark_prog_ = compile_program_files(shader_root + "/landmark.vert", shader_root + "/landmark.frag");
    if (!cull_comp_ || !city_prog_ || !city_far_prog_ || !bldg_prog_ || !bldg_sh_prog_ || !bldg_lod1_prog_ || !rwy_prog_ || !road_prog_ || !traffic_prog_ || !landmark_prog_) return false;

    u_city_vp_   = glGetUniformLocation(city_prog_, "u_vp");
    u_city_cam_  = glGetUniformLocation(city_prog_, "u_cam_enu");
    u_city_sun_  = glGetUniformLocation(city_prog_, "u_sun_dir");
    u_city_cool_ = glGetUniformLocation(city_prog_, "u_haze_cool");
    u_city_warm_ = glGetUniformLocation(city_prog_, "u_haze_warm");
    u_city_hd_   = glGetUniformLocation(city_prog_, "u_haze_density");
    u_city_alt_  = glGetUniformLocation(city_prog_, "u_cam_alt_m");
    u_city_ph_   = glGetUniformLocation(city_prog_, "u_sun_phase10");
    u_city_mb_   = glGetUniformLocation(city_prog_, "u_mtn_bounds");
    u_city_roff_ = glGetUniformLocation(city_prog_, "u_reg_offset");
    u_city_rscl_ = glGetUniformLocation(city_prog_, "u_reg_scale");

    u_cfar_vp_   = glGetUniformLocation(city_far_prog_, "u_vp");
    u_cfar_cam_  = glGetUniformLocation(city_far_prog_, "u_cam_enu");
    u_cfar_sun_  = glGetUniformLocation(city_far_prog_, "u_sun_dir");
    u_cfar_cool_ = glGetUniformLocation(city_far_prog_, "u_haze_cool");
    u_cfar_warm_ = glGetUniformLocation(city_far_prog_, "u_haze_warm");
    u_cfar_hd_   = glGetUniformLocation(city_far_prog_, "u_haze_density");
    u_cfar_alt_  = glGetUniformLocation(city_far_prog_, "u_cam_alt_m");
    u_cfar_fs_   = glGetUniformLocation(city_far_prog_, "u_far_start");
    u_cfar_fe_   = glGetUniformLocation(city_far_prog_, "u_far_end");
    u_cfar_ph_   = glGetUniformLocation(city_far_prog_, "u_sun_phase10");
    u_cfar_mb_   = glGetUniformLocation(city_far_prog_, "u_mtn_bounds");
    u_cfar_roff_ = glGetUniformLocation(city_far_prog_, "u_reg_offset");
    u_cfar_rscl_ = glGetUniformLocation(city_far_prog_, "u_reg_scale");

    u_rwy_vp_    = glGetUniformLocation(rwy_prog_, "u_vp");
    u_rwy_cam_   = glGetUniformLocation(rwy_prog_, "u_cam_enu");
    u_rwy_sun_   = glGetUniformLocation(rwy_prog_, "u_sun_dir");
    u_rwy_cool_  = glGetUniformLocation(rwy_prog_, "u_haze_cool");
    u_rwy_warm_  = glGetUniformLocation(rwy_prog_, "u_haze_warm");
    u_rwy_hd_    = glGetUniformLocation(rwy_prog_, "u_haze_density");

    u_road_vp_   = glGetUniformLocation(road_prog_, "u_vp");
    u_road_cam_  = glGetUniformLocation(road_prog_, "u_cam_enu");
    u_road_sun_  = glGetUniformLocation(road_prog_, "u_sun_dir");
    u_road_cool_ = glGetUniformLocation(road_prog_, "u_haze_cool");
    u_road_warm_ = glGetUniformLocation(road_prog_, "u_haze_warm");
    u_road_hd_   = glGetUniformLocation(road_prog_, "u_haze_density");
    u_road_alt_  = glGetUniformLocation(road_prog_, "u_cam_alt_m");
    u_road_ph_   = glGetUniformLocation(road_prog_, "u_sun_phase10");

    u_car_vp_    = glGetUniformLocation(traffic_prog_, "u_vp");
    u_car_cam_   = glGetUniformLocation(traffic_prog_, "u_cam_enu");
    u_car_sun_   = glGetUniformLocation(traffic_prog_, "u_sun_dir");
    u_car_cool_  = glGetUniformLocation(traffic_prog_, "u_haze_cool");
    u_car_warm_  = glGetUniformLocation(traffic_prog_, "u_haze_warm");
    u_car_hd_    = glGetUniformLocation(traffic_prog_, "u_haze_density");
    u_car_alt_   = glGetUniformLocation(traffic_prog_, "u_cam_alt_m");

    u_lm_vp_     = glGetUniformLocation(landmark_prog_, "u_vp");
    u_lm_cam_    = glGetUniformLocation(landmark_prog_, "u_cam_enu");
    u_lm_sun_    = glGetUniformLocation(landmark_prog_, "u_sun_dir");
    u_lm_cool_   = glGetUniformLocation(landmark_prog_, "u_haze_cool");
    u_lm_warm_   = glGetUniformLocation(landmark_prog_, "u_haze_warm");
    u_lm_hd_     = glGetUniformLocation(landmark_prog_, "u_haze_density");
    u_lm_alt_    = glGetUniformLocation(landmark_prog_, "u_cam_alt_m");

    u_cull_tot_  = glGetUniformLocation(cull_comp_, "u_total_bldgs");
    u_cull_max_  = glGetUniformLocation(cull_comp_, "u_max_per_arch");
    u_cull_max_l1_= glGetUniformLocation(cull_comp_, "u_max_lod1");
    u_cull_cam_  = glGetUniformLocation(cull_comp_, "u_cam_enu");
    u_cull_dist_ = glGetUniformLocation(cull_comp_, "u_max_dist_m");
    u_cull_far_  = glGetUniformLocation(cull_comp_, "u_far_dist_m");
    u_cull_frust_= glGetUniformLocation(cull_comp_, "u_frustum");

    u_bldg_vp_   = glGetUniformLocation(bldg_prog_, "u_vp");
    u_bldg_cam_  = glGetUniformLocation(bldg_prog_, "u_cam_enu");
    u_bldg_sun_  = glGetUniformLocation(bldg_prog_, "u_sun_dir");
    u_bldg_cool_ = glGetUniformLocation(bldg_prog_, "u_haze_cool");
    u_bldg_warm_ = glGetUniformLocation(bldg_prog_, "u_haze_warm");
    u_bldg_hd_   = glGetUniformLocation(bldg_prog_, "u_haze_density");
    u_bldg_alt_  = glGetUniformLocation(bldg_prog_, "u_cam_alt_m");
    u_bldg_fs_   = glGetUniformLocation(bldg_prog_, "u_fade_start");
    u_bldg_fe_   = glGetUniformLocation(bldg_prog_, "u_fade_end");

    u_bsh_vp_    = glGetUniformLocation(bldg_sh_prog_, "u_vp");
    u_bsh_cam_   = glGetUniformLocation(bldg_sh_prog_, "u_cam_enu");
    u_bsh_sun_   = glGetUniformLocation(bldg_sh_prog_, "u_sun_dir");
    u_bsh_fs_    = glGetUniformLocation(bldg_sh_prog_, "u_fade_start");
    u_bsh_fe_    = glGetUniformLocation(bldg_sh_prog_, "u_fade_end");

    u_bl1_vp_    = glGetUniformLocation(bldg_lod1_prog_, "u_vp");
    u_bl1_cam_   = glGetUniformLocation(bldg_lod1_prog_, "u_cam_enu");
    u_bl1_sun_   = glGetUniformLocation(bldg_lod1_prog_, "u_sun_dir");
    u_bl1_cool_  = glGetUniformLocation(bldg_lod1_prog_, "u_haze_cool");
    u_bl1_warm_  = glGetUniformLocation(bldg_lod1_prog_, "u_haze_warm");
    u_bl1_hd_    = glGetUniformLocation(bldg_lod1_prog_, "u_haze_density");
    u_bl1_alt_   = glGetUniformLocation(bldg_lod1_prog_, "u_cam_alt_m");
    u_bl1_is_    = glGetUniformLocation(bldg_lod1_prog_, "u_lod_in_start");
    u_bl1_ie_    = glGetUniformLocation(bldg_lod1_prog_, "u_lod_in_end");
    u_bl1_fs_    = glGetUniformLocation(bldg_lod1_prog_, "u_far_start");
    u_bl1_fe_    = glGetUniformLocation(bldg_lod1_prog_, "u_far_end");



    // 1. Load all 6 Spanish building .glb archetypes into ONE single merged VAO for 1-call glMultiDrawElementsIndirect!
    static const char *kBldgFiles[6] = {
        "/models/architecture/aragon_house_stone.glb",
        "/models/architecture/aragon_house_terra.glb",
        "/models/architecture/aragon_apartment.glb",
        "/models/architecture/aragon_church_tower.glb",
        "/models/architecture/industrial_hangar.glb",
        "/models/architecture/rural_barn.glb"
    };
    std::vector<float> bv;
    std::vector<uint32_t> bi;
    for (int a = 0; a < 6; a++) {
        load_textured_glb_append(asset_root + kBldgFiles[a], bv, bi, bldg_base_cmds_[a]);
        bldg_base_cmds_[a].baseInstance = static_cast<uint32_t>(a * MAX_PER_ARCH);
    }
    glCreateVertexArrays(1, &bldg_vao_);
    glCreateBuffers(1, &bldg_vbo_);
    glCreateBuffers(1, &bldg_ebo_);
    glNamedBufferStorage(bldg_vbo_, static_cast<GLsizeiptr>(bv.size() * sizeof(float)), bv.data(), 0);
    glNamedBufferStorage(bldg_ebo_, static_cast<GLsizeiptr>(bi.size() * sizeof(uint32_t)), bi.data(), 0);
    glVertexArrayVertexBuffer(bldg_vao_, 0, bldg_vbo_, 0, 8 * sizeof(float));
    glVertexArrayElementBuffer(bldg_vao_, bldg_ebo_);
    glEnableVertexArrayAttrib(bldg_vao_, 0); glVertexArrayAttribFormat(bldg_vao_, 0, 3, GL_FLOAT, GL_FALSE, 0); glVertexArrayAttribBinding(bldg_vao_, 0, 0);
    glEnableVertexArrayAttrib(bldg_vao_, 1); glVertexArrayAttribFormat(bldg_vao_, 1, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float)); glVertexArrayAttribBinding(bldg_vao_, 1, 0);
    glEnableVertexArrayAttrib(bldg_vao_, 2); glVertexArrayAttribFormat(bldg_vao_, 2, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(float)); glVertexArrayAttribBinding(bldg_vao_, 2, 0);

    // Load map-wide pre-baked exact-polygon populated zones (ALL cities, towns & villages!)
    load_baked_cities(pack_dir);

    const auto init_frame = pack_->world_frame();
    upload_buildings_gpu(build_buildings_cpu(*init_frame));
    glCreateBuffers(1, &bldg_cmd_ssbo_);
    glNamedBufferData(bldg_cmd_ssbo_, sizeof(bldg_base_cmds_), bldg_base_cmds_, GL_DYNAMIC_DRAW);
    glCreateBuffers(1, &bldg_vis_ssbo_);
    glNamedBufferStorage(bldg_vis_ssbo_, static_cast<GLsizeiptr>(6 * MAX_PER_ARCH * sizeof(uint32_t)), nullptr, 0);

    // 1-quad instanced GPU ground shadow & soft contact-AO mesh (reuses bldg_vis_ssbo_ directly!)
    static const float kShQuad[8] = {-1.0f, -1.0f, 1.0f, -1.0f, 1.0f, 1.0f, -1.0f, 1.0f};
    static const uint32_t kShIdx[6] = {0, 1, 2, 0, 2, 3};
    for (int a = 0; a < 6; a++) {
        bldg_sh_base_cmds_[a] = {6u, 0u, 0u, 0, static_cast<uint32_t>(a * MAX_PER_ARCH)};
    }
    glCreateVertexArrays(1, &bldg_sh_vao_);
    glCreateBuffers(1, &bldg_sh_vbo_);
    glCreateBuffers(1, &bldg_sh_ebo_);
    glNamedBufferStorage(bldg_sh_vbo_, sizeof(kShQuad), kShQuad, 0);
    glNamedBufferStorage(bldg_sh_ebo_, sizeof(kShIdx), kShIdx, 0);
    glVertexArrayVertexBuffer(bldg_sh_vao_, 0, bldg_sh_vbo_, 0, 2 * sizeof(float));
    glVertexArrayElementBuffer(bldg_sh_vao_, bldg_sh_ebo_);
    glEnableVertexArrayAttrib(bldg_sh_vao_, 0);
    glVertexArrayAttribFormat(bldg_sh_vao_, 0, 2, GL_FLOAT, GL_FALSE, 0);
    glVertexArrayAttribBinding(bldg_sh_vao_, 0, 0);
    glCreateBuffers(1, &bldg_sh_cmd_ssbo_);
    glNamedBufferData(bldg_sh_cmd_ssbo_, sizeof(bldg_sh_base_cmds_), bldg_sh_base_cmds_, GL_DYNAMIC_DRAW);

    // 10-triangle Low-LOD Far Building Block mesh (5 faces: 4 walls + roof, 0..22km continuous city skyline!)
    static const float kL1V[] = {
        -0.5f,-0.35f, 0.5f,  0,0, 1,   0.5f,-0.35f, 0.5f,  0,0, 1,   0.5f, 0.92f, 0.5f,  0,0, 1,  -0.5f, 0.92f, 0.5f,  0,0, 1,
         0.5f,-0.35f,-0.5f,  0,0,-1,  -0.5f,-0.35f,-0.5f,  0,0,-1,  -0.5f, 0.92f,-0.5f,  0,0,-1,   0.5f, 0.92f,-0.5f,  0,0,-1,
         0.5f,-0.35f, 0.5f,  1,0, 0,   0.5f,-0.35f,-0.5f,  1,0, 0,   0.5f, 0.92f,-0.5f,  1,0, 0,   0.5f, 0.92f, 0.5f,  1,0, 0,
        -0.5f,-0.35f,-0.5f, -1,0, 0,  -0.5f,-0.35f, 0.5f, -1,0, 0,  -0.5f, 0.92f, 0.5f, -1,0, 0,  -0.5f, 0.92f,-0.5f, -1,0, 0,
        -0.5f, 0.92f, 0.5f,  0,1, 0,   0.5f, 0.92f, 0.5f,  0,1, 0,   0.5f, 0.92f,-0.5f,  0,1, 0,  -0.5f, 0.92f,-0.5f,  0,1, 0,
    };
    static const uint32_t kL1I[] = {
         0, 1, 2,  0, 2, 3,   4, 5, 6,  4, 6, 7,   8, 9,10,  8,10,11,
        12,13,14, 12,14,15,  16,17,18, 16,18,19
    };
    bldg_lod1_base_cmd_ = {30u, 0u, 0u, 0, 0u};
    glCreateVertexArrays(1, &bldg_lod1_vao_);
    glCreateBuffers(1, &bldg_lod1_vbo_);
    glCreateBuffers(1, &bldg_lod1_ebo_);
    glNamedBufferStorage(bldg_lod1_vbo_, sizeof(kL1V), kL1V, 0);
    glNamedBufferStorage(bldg_lod1_ebo_, sizeof(kL1I), kL1I, 0);
    glVertexArrayVertexBuffer(bldg_lod1_vao_, 0, bldg_lod1_vbo_, 0, 6 * sizeof(float));
    glVertexArrayElementBuffer(bldg_lod1_vao_, bldg_lod1_ebo_);
    glEnableVertexArrayAttrib(bldg_lod1_vao_, 0); glVertexArrayAttribFormat(bldg_lod1_vao_, 0, 3, GL_FLOAT, GL_FALSE, 0); glVertexArrayAttribBinding(bldg_lod1_vao_, 0, 0);
    glEnableVertexArrayAttrib(bldg_lod1_vao_, 1); glVertexArrayAttribFormat(bldg_lod1_vao_, 1, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float)); glVertexArrayAttribBinding(bldg_lod1_vao_, 1, 0);
    glCreateBuffers(1, &bldg_lod1_cmd_ssbo_);
    glNamedBufferData(bldg_lod1_cmd_ssbo_, sizeof(bldg_lod1_base_cmd_), &bldg_lod1_base_cmd_, GL_DYNAMIC_DRAW);
    glCreateBuffers(1, &bldg_lod1_vis_ssbo_);
    glNamedBufferStorage(bldg_lod1_vis_ssbo_, static_cast<GLsizeiptr>(MAX_LOD1 * sizeof(uint32_t)), nullptr, 0);

    bldg_alb_     = load_tex2d(asset_root + "/pbr/building_atlas_albedo.jpg", true);
    bldg_nrm_     = load_tex2d(asset_root + "/pbr/building_atlas_normal.jpg", false);
    bldg_orm_     = load_tex2d(asset_root + "/pbr/building_atlas_orm.png", false);
    interior_tex_ = load_tex2d(asset_root + "/pbr/interior_rooms.jpg", true);

    // 3. Runways (with GGX PBR asphalt/gravel normal maps), Streamed OSM Roads, Hero Landmarks & Moving 3D Traffic!
    asph_tex_ = load_tex2d(asset_root + "/pbr/asphalt_albedo.jpg", true);
    grav_tex_ = load_tex2d(asset_root + "/pbr/gravel_albedo.jpg", true);
    asph_nrm_ = load_tex2d(asset_root + "/pbr/asphalt_normal.jpg", false);
    grav_nrm_ = load_tex2d(asset_root + "/pbr/gravel_normal.jpg", false);

    // 50km corridor mountain 10-TOD shadow + valley Sky AO GPU lookup texture (512x384 RG8, 384 KB!)
    glCreateTextures(GL_TEXTURE_2D, 1, &mtn_gi_tex_);
    glTextureStorage2D(mtn_gi_tex_, 1, GL_RG8, pack_->corridor_dem_w(), pack_->corridor_dem_h());
    glTextureParameteri(mtn_gi_tex_, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(mtn_gi_tex_, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(mtn_gi_tex_, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(mtn_gi_tex_, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    static_assert(sizeof(RoadVert) == 28, "RoadVert layout mismatch");
    glCreateVertexArrays(1, &road_vao_);
    glCreateBuffers(1, &road_vbo_);
    glNamedBufferData(road_vbo_, static_cast<GLsizeiptr>(MAX_ROAD_VERTS * sizeof(RoadVert)), nullptr, GL_DYNAMIC_DRAW);
    glVertexArrayVertexBuffer(road_vao_, 0, road_vbo_, 0, sizeof(RoadVert));
    glEnableVertexArrayAttrib(road_vao_, 0); glVertexArrayAttribFormat(road_vao_, 0, 3, GL_FLOAT, GL_FALSE, 0); glVertexArrayAttribBinding(road_vao_, 0, 0);
    glEnableVertexArrayAttrib(road_vao_, 1); glVertexArrayAttribFormat(road_vao_, 1, 2, GL_FLOAT, GL_FALSE, 12); glVertexArrayAttribBinding(road_vao_, 1, 0);
    glEnableVertexArrayAttrib(road_vao_, 2); glVertexArrayAttribFormat(road_vao_, 2, 1, GL_FLOAT, GL_FALSE, 20); glVertexArrayAttribBinding(road_vao_, 2, 0);
    glEnableVertexArrayAttrib(road_vao_, 3); glVertexArrayAttribIFormat(road_vao_, 3, 1, GL_UNSIGNED_INT, 24); glVertexArrayAttribBinding(road_vao_, 3, 0);

    glCreateBuffers(1, &car_ssbo_);
    glNamedBufferData(car_ssbo_, static_cast<GLsizeiptr>(MAX_CARS * sizeof(CarGpu)), nullptr, GL_STREAM_DRAW);

    upload_runways_gpu(build_runways_cpu(*init_frame));
    upload_landmarks_gpu(build_landmarks_cpu(*init_frame));
    build_car_mesh();
    return true;
}

void WorldObjects::shutdown() {
    cancel_staged_reanchor_upload();
    reset_city_stream();
    auto del_p = [](GLuint &p) { if (p) { glDeleteProgram(p); p = 0; } };
    auto del_v = [](GLuint &v) { if (v) { glDeleteVertexArrays(1, &v); v = 0; } };
    auto del_b = [](GLuint &b) { if (b) { glDeleteBuffers(1, &b); b = 0; } };
    auto del_t = [](GLuint &t) { if (t) { glDeleteTextures(1, &t); t = 0; } };
    del_p(cull_comp_); del_p(city_prog_); del_p(city_far_prog_); del_p(bldg_prog_); del_p(bldg_sh_prog_); del_p(bldg_lod1_prog_); del_p(rwy_prog_);
    del_p(road_prog_); del_p(traffic_prog_); del_p(landmark_prog_);
    for (auto &cr : city_regions_) unload_city_region_gpu(cr);
    city_regions_.clear();
    del_v(bldg_vao_); del_b(bldg_vbo_); del_b(bldg_ebo_);
    del_b(bldg_all_ssbo_); del_b(bldg_cmd_ssbo_); del_b(bldg_vis_ssbo_);
    del_v(bldg_sh_vao_); del_b(bldg_sh_vbo_); del_b(bldg_sh_ebo_); del_b(bldg_sh_cmd_ssbo_);
    del_v(bldg_lod1_vao_); del_b(bldg_lod1_vbo_); del_b(bldg_lod1_ebo_); del_b(bldg_lod1_cmd_ssbo_); del_b(bldg_lod1_vis_ssbo_);
    del_v(rwy_vao_); del_b(rwy_vbo_);
    del_v(road_vao_); del_b(road_vbo_);
    del_v(car_vao_); del_b(car_vbo_); del_b(car_ebo_); del_b(car_ssbo_);
    del_v(lm_vao_); del_b(lm_vbo_);
    del_t(bldg_alb_); del_t(bldg_nrm_); del_t(bldg_orm_); del_t(interior_tex_); del_t(mtn_gi_tex_);
    del_t(asph_tex_); del_t(grav_tex_); del_t(asph_nrm_); del_t(grav_nrm_);
}

std::vector<WorldObjects::BldgGpu> WorldObjects::build_buildings_cpu(const PackReader::WorldFrame &frame) const {
    struct ExclZone { double lat, lon, rad_m; };
    static const ExclZone kHeroExcl[] = {
        {41.6569, -0.8787,  68.0},
        {41.6693, -0.9073,  36.0},
        {42.1404, -0.4086,  44.0},
        {42.3257, -0.6119,  55.0},
        {42.3175,  0.2037, 130.0},
    };
    const auto &bldgs = pack_->buildings();
    std::vector<BldgGpu> gpu_b;
    if (!bldgs.empty()) {
        gpu_b.reserve(std::min<size_t>(bldgs.size(), 32768));
        for (const BuildingRec &b : bldgs) {
            bool covered_by_city = false;
            for (const auto &cr : city_regions_) {
                if (b.lat >= cr.lat_min && b.lat <= cr.lat_max && b.lon >= cr.lon_min && b.lon <= cr.lon_max) {
                    covered_by_city = true;
                    break;
                }
            }
            if (covered_by_city && !b.airport_feature) continue;
            bool overlap_hero = false;
            for (const auto &ez : kHeroExcl) {
                double de = (b.lon - ez.lon) * frame.metres_lon;
                double dn = (b.lat - ez.lat) * frame.metres_lat;
                if (de * de + dn * dn < ez.rad_m * ez.rad_m) { overlap_hero = true; break; }
            }
            if (overlap_hero) continue;
            float east_m  = static_cast<float>((b.lon - frame.anchor_lon) * frame.metres_lon);
            float north_m = static_cast<float>((b.lat - frame.anchor_lat) * frame.metres_lat);
            float bw = std::min(b.width_m, b.archetype == 4 ? 72.0f : 56.0f);
            float bd = std::min(b.depth_m, b.archetype == 4 ? 54.0f : 44.0f);
            float bh = (b.archetype == 4) ? std::min(b.height_m, 9.5f) : b.height_m;
            BldgGpu bg{};
            bg.pos_yaw[0] = east_m; bg.pos_yaw[1] = b.ground_m; bg.pos_yaw[2] = north_m; bg.pos_yaw[3] = b.yaw_rad;
            bg.scale_arch[0] = bw; bg.scale_arch[1] = bh; bg.scale_arch[2] = bd; bg.scale_arch[3] = static_cast<float>(b.archetype);
            bg.roof_ao[0] = b.roof_r / 255.0f; bg.roof_ao[1] = b.roof_g / 255.0f; bg.roof_ao[2] = b.roof_b / 255.0f; bg.roof_ao[3] = b.canyon_ao / 255.0f;
            gpu_b.push_back(bg);
        }
    }
    return gpu_b;
}

void WorldObjects::upload_buildings_gpu(const std::vector<BldgGpu> &gpu_b) {
    if (bldg_all_ssbo_) { glDeleteBuffers(1, &bldg_all_ssbo_); bldg_all_ssbo_ = 0; }
    total_bldgs_ = static_cast<int>(gpu_b.size());
    glCreateBuffers(1, &bldg_all_ssbo_);
    glNamedBufferStorage(bldg_all_ssbo_, static_cast<GLsizeiptr>(std::max<size_t>(1, gpu_b.size()) * sizeof(BldgGpu)), gpu_b.empty() ? nullptr : gpu_b.data(), 0);
}

WorldObjects::PreparedReanchor WorldObjects::prepare_reanchor_cpu(const PackReader::WorldFrame &frame) const {
    PreparedReanchor out;
    out.gpu_b = build_buildings_cpu(frame);
    out.rv = build_runways_cpu(frame);
    out.lv = build_landmarks_cpu(frame);
    return out;
}

void WorldObjects::commit_reanchor(PreparedReanchor &&prep) {
    cancel_staged_reanchor_upload();
    update_city_region_transforms();
    upload_buildings_gpu(prep.gpu_b);
    road_center_e_ = 1e12;
    road_center_n_ = 1e12;
    mtn_gi_phase_ = -10.0f;
    upload_runways_gpu(prep.rv);
    upload_landmarks_gpu(prep.lv);
}

void WorldObjects::begin_staged_reanchor_upload(PreparedReanchor &&prep) {
    cancel_staged_reanchor_upload();
    staged_reanchor_.data = std::move(prep);
}

void WorldObjects::pump_staged_reanchor_upload(size_t byte_budget) {
    if (staged_reanchor_.phase >= 3 || byte_budget == 0) return;
    auto stage_buffer = [&](GLuint &buffer, size_t &uploaded, size_t byte_count, size_t minimum_size,
                           const void *source) {
        if (!buffer) {
            glCreateBuffers(1, &buffer);
            const GLsizeiptr bytes = static_cast<GLsizeiptr>(std::max(minimum_size, byte_count));
            glNamedBufferStorage(buffer, bytes, nullptr, GL_DYNAMIC_STORAGE_BIT);
            uploaded = 0;
        }
        const size_t remaining = byte_count - uploaded;
        const size_t chunk = std::min(remaining, byte_budget);
        if (chunk > 0) {
            glNamedBufferSubData(buffer, static_cast<GLintptr>(uploaded), static_cast<GLsizeiptr>(chunk),
                                 static_cast<const uint8_t *>(source) + uploaded);
            uploaded += chunk;
            byte_budget -= chunk;
        }
        return uploaded >= byte_count;
    };

    while (staged_reanchor_.phase < 3 && byte_budget > 0) {
        bool complete = false;
        switch (staged_reanchor_.phase) {
        case 0: {
            const auto &data = staged_reanchor_.data.gpu_b;
            const size_t bytes = data.size() * sizeof(BldgGpu);
            complete = stage_buffer(staged_reanchor_.buildings, staged_reanchor_.buildings_uploaded,
                                    bytes, sizeof(BldgGpu), data.data());
            break;
        }
        case 1: {
            const auto &data = staged_reanchor_.data.rv;
            const size_t bytes = data.size() * sizeof(float);
            complete = stage_buffer(staged_reanchor_.runways, staged_reanchor_.runways_uploaded,
                                    bytes, 10 * sizeof(float), data.data());
            break;
        }
        case 2: {
            const auto &data = staged_reanchor_.data.lv;
            const size_t bytes = data.size() * sizeof(float);
            complete = stage_buffer(staged_reanchor_.landmarks, staged_reanchor_.landmarks_uploaded,
                                    bytes, 12 * sizeof(float), data.data());
            break;
        }
        default:
            return;
        }
        if (!complete) return;
        ++staged_reanchor_.phase;
    }
}

void WorldObjects::commit_staged_reanchor_upload() {
    if (!staged_reanchor_upload_ready()) return;
    update_city_region_transforms();

    GLuint old = bldg_all_ssbo_;
    bldg_all_ssbo_ = staged_reanchor_.buildings;
    staged_reanchor_.buildings = 0;
    total_bldgs_ = static_cast<int>(staged_reanchor_.data.gpu_b.size());
    if (old) glDeleteBuffers(1, &old);

    old = rwy_vao_;
    rwy_vao_ = create_runway_vao(staged_reanchor_.runways);
    if (old) glDeleteVertexArrays(1, &old);
    old = rwy_vbo_;
    rwy_vbo_ = staged_reanchor_.runways;
    staged_reanchor_.runways = 0;
    if (old) glDeleteBuffers(1, &old);
    rwy_vert_count_ = static_cast<int>(staged_reanchor_.data.rv.size() / 10);

    old = lm_vao_;
    lm_vao_ = create_landmark_vao(staged_reanchor_.landmarks);
    if (old) glDeleteVertexArrays(1, &old);
    old = lm_vbo_;
    lm_vbo_ = staged_reanchor_.landmarks;
    staged_reanchor_.landmarks = 0;
    if (old) glDeleteBuffers(1, &old);
    lm_vert_count_ = static_cast<int>(staged_reanchor_.data.lv.size() / 12);

    road_center_e_ = 1e12;
    road_center_n_ = 1e12;
    mtn_gi_phase_ = -10.0f;
    staged_reanchor_ = {};
}

void WorldObjects::cancel_staged_reanchor_upload() {
    if (staged_reanchor_.buildings) glDeleteBuffers(1, &staged_reanchor_.buildings);
    if (staged_reanchor_.runways) glDeleteBuffers(1, &staged_reanchor_.runways);
    if (staged_reanchor_.landmarks) glDeleteBuffers(1, &staged_reanchor_.landmarks);
    staged_reanchor_ = {};
}

void WorldObjects::on_reanchor() {
    const auto frame = pack_->world_frame();
    if (!frame) return;
    commit_reanchor(prepare_reanchor_cpu(*frame));
}

std::vector<float> WorldObjects::build_runways_cpu(const PackReader::WorldFrame &frame) const {
    std::vector<float> rv; // pos(3), nrm(3), uv(2), mat_id(1), feat_id(1) = 10 floats
    auto push_surface_vertex = [&](double e, double n, double u, double v, float mat_id, float feat_id, double y_off) {
        const double lat = frame.anchor_lat + n / frame.metres_lat;
        const double lon = frame.anchor_lon + e / frame.metres_lon;
        const float ground = static_cast<float>(pack_->sample_elevation(lat, lon) + y_off);
        rv.insert(rv.end(), {static_cast<float>(e), ground, static_cast<float>(n),
                             0.0f, 1.0f, 0.0f, static_cast<float>(u), static_cast<float>(v), mat_id, feat_id});
    };
    auto push_surface_triangle = [&](const Vec2 &a, const Vec2 &b, const Vec2 &c,
                                     const Vec2 &uv_a, const Vec2 &uv_b, const Vec2 &uv_c,
                                     float mat_id, float feat_id, double y_off) {
        push_surface_vertex(a.x, a.y, uv_a.x, uv_a.y, mat_id, feat_id, y_off);
        push_surface_vertex(b.x, b.y, uv_b.x, uv_b.y, mat_id, feat_id, y_off);
        push_surface_vertex(c.x, c.y, uv_c.x, uv_c.y, mat_id, feat_id, y_off);
    };
    // Material IDs: -1=grass/ortho-only, 0=dirt, 1=gravel, 2=asphalt, 3=concrete
    auto material_for_surface = [](std::string surface) {
        for (char &ch : surface) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (surface.empty() || surface == "unknown" || surface.find("grass") != std::string::npos ||
            surface.find("turf") != std::string::npos || surface.find("grs") != std::string::npos) return -1;
        if (surface.find("con") != std::string::npos || surface.find("pcc") != std::string::npos ||
            surface.find("horm") != std::string::npos) return 3;
        if (surface.find("asp") != std::string::npos || surface.find("asf") != std::string::npos ||
            surface.find("bit") != std::string::npos || surface.find("pav") != std::string::npos ||
            surface.find("hard") != std::string::npos || surface.find("tar") != std::string::npos) return 2;
        if (surface.find("grav") != std::string::npos || surface.find("gvl") != std::string::npos ||
            surface.find("compact") != std::string::npos || surface.find("crush") != std::string::npos) return 1;
        if (surface.find("dirt") != std::string::npos || surface.find("earth") != std::string::npos ||
            surface.find("sand") != std::string::npos || surface.find("clay") != std::string::npos ||
            surface.find("unpav") != std::string::npos) return 0;
        return -1;
    };
    auto append_runway = [&](const AirportRunway &rw) {
        double ea = (rw.lon_a - frame.anchor_lon) * frame.metres_lon;
        double na = (rw.lat_a - frame.anchor_lat) * frame.metres_lat;
        double eb = (rw.lon_b - frame.anchor_lon) * frame.metres_lon;
        double nb = (rw.lat_b - frame.anchor_lat) * frame.metres_lat;
        double de = eb - ea, dn = nb - na;
        double len = std::hypot(de, dn);
        if (len < 120.0) return;
        double rx = dn / len * (rw.width_m * 0.5);
        double rn = -de / len * (rw.width_m * 0.5);
        // A grass strip is part of the ortho and ground material. An elevated
        // overlay would float above the grass and cut through a ground camera.
        const int material = material_for_surface(rw.surface);
        if (material < 0) return;
        const float mat_f = static_cast<float>(material);

        const int segs = std::clamp(static_cast<int>(len / 22.0), 8, 96);
        auto sample_rwy_h = [&](double e, double n) -> float {
            auto se = [&](double ee, double nn) {
                return pack_->sample_elevation(frame.anchor_lat + nn / frame.metres_lat,
                                               frame.anchor_lon + ee / frame.metres_lon);
            };
            double hc = se(e, n);
            double hl = se(e - rx, n - rn);
            double hr = se(e + rx, n + rn);
            return static_cast<float>(std::max({hc, hl, hr}) + 0.55);
        };
        for (int s = 0; s < segs; s++) {
            double t0 = static_cast<double>(s) / segs;
            double t1 = static_cast<double>(s + 1) / segs;
            double e0 = ea + de * t0, n0 = na + dn * t0;
            double e1 = ea + de * t1, n1 = na + dn * t1;
            float h0 = sample_rwy_h(e0, n0);
            float h1 = sample_rwy_h(e1, n1);
            float v0 = static_cast<float>(t0 * len), v1 = static_cast<float>(t1 * len);

            auto push = [&](double e, float y, double n, float u, float v) {
                rv.insert(rv.end(), {static_cast<float>(e), y, static_cast<float>(n),
                                     0.0f, 1.0f, 0.0f, u, v, mat_f, 1.0f});
            };
            push(e0 - rx, h0, n0 - rn, 0.0f, v0);
            push(e0 + rx, h0, n0 + rn, 1.0f, v0);
            push(e1 + rx, h1, n1 + rn, 1.0f, v1);
            push(e0 - rx, h0, n0 - rn, 0.0f, v0);
            push(e1 + rx, h1, n1 + rn, 1.0f, v1);
            push(e1 - rx, h1, n1 - rn, 0.0f, v1);
        }
    };
    auto append_strip = [&](const AirportGroundFeature &feature, int fallback_mat, float feat_id, double y_off) {
        int material = material_for_surface(feature.surface);
        if (material < 0 && feature.surface == "unknown") material = fallback_mat;
        if (material < 0 || feature.points_lat_lon.size() < 2) return;
        const float mat_f = static_cast<float>(material);
        const double half_width = std::clamp(feature.width_m, 3.5, 80.0) * 0.5;
        double cum_len = 0.0;
        for (size_t i = 1; i < feature.points_lat_lon.size(); ++i) {
            const auto [lat0, lon0] = feature.points_lat_lon[i - 1];
            const auto [lat1, lon1] = feature.points_lat_lon[i];
            const double e0 = (lon0 - frame.anchor_lon) * frame.metres_lon;
            const double n0 = (lat0 - frame.anchor_lat) * frame.metres_lat;
            const double e1 = (lon1 - frame.anchor_lon) * frame.metres_lon;
            const double n1 = (lat1 - frame.anchor_lat) * frame.metres_lat;
            const double de = e1 - e0, dn = n1 - n0, len = std::hypot(de, dn);
            if (len < 0.5) continue;
            const double oe = dn / len * half_width, on = -de / len * half_width;
            const int subs = std::clamp(static_cast<int>(len / 18.0), 1, 24);
            for (int s = 0; s < subs; ++s) {
                double t0 = static_cast<double>(s) / subs;
                double t1 = static_cast<double>(s + 1) / subs;
                double se0 = e0 + de * t0, sn0 = n0 + dn * t0;
                double se1 = e0 + de * t1, sn1 = n0 + dn * t1;
                const Vec2 a{static_cast<float>(se0 - oe), static_cast<float>(sn0 - on)};
                const Vec2 b{static_cast<float>(se0 + oe), static_cast<float>(sn0 + on)};
                const Vec2 c{static_cast<float>(se1 + oe), static_cast<float>(sn1 + on)};
                const Vec2 d{static_cast<float>(se1 - oe), static_cast<float>(sn1 - on)};
                float v0 = static_cast<float>(cum_len + len * t0);
                float v1 = static_cast<float>(cum_len + len * t1);
                push_surface_triangle(a, b, c, {0.0f, v0}, {1.0f, v0}, {1.0f, v1}, mat_f, feat_id, y_off);
                push_surface_triangle(a, c, d, {0.0f, v0}, {1.0f, v1}, {0.0f, v1}, mat_f, feat_id, y_off);
            }
            cum_len += len;
        }
    };
    auto append_apron = [&](const AirportGroundFeature &feature, int fallback_mat) {
        int material = material_for_surface(feature.surface);
        if (material < 0 && feature.surface == "unknown") material = fallback_mat;
        if (material < 0 || feature.points_lat_lon.size() < 4) return;
        std::vector<Vec2> polygon;
        polygon.reserve(std::min<size_t>(feature.points_lat_lon.size(), 128));
        const size_t stride = std::max<size_t>(1, (feature.points_lat_lon.size() + 127) / 128);
        for (size_t i = 0; i < feature.points_lat_lon.size(); i += stride) {
            const auto [lat, lon] = feature.points_lat_lon[i];
            polygon.push_back({static_cast<float>((lon - frame.anchor_lon) * frame.metres_lon),
                               static_cast<float>((lat - frame.anchor_lat) * frame.metres_lat)});
        }
        if (polygon.size() < 3) return;
        if (std::hypot(polygon.front().x - polygon.back().x, polygon.front().y - polygon.back().y) < 0.5)
            polygon.pop_back();
        if (polygon.size() < 3) return;
        double signed_area = 0.0;
        Vec2 center{};
        for (size_t i = 0; i < polygon.size(); ++i) {
            const Vec2 &a = polygon[i], &b = polygon[(i + 1) % polygon.size()];
            signed_area += a.x * b.y - b.x * a.y;
            center.x += a.x; center.y += a.y;
        }
        center.x /= polygon.size(); center.y /= polygon.size();
        if (std::abs(signed_area) < 1.0) return;
        if (signed_area < 0.0) std::reverse(polygon.begin(), polygon.end());
        auto cross2 = [](const Vec2 &a, const Vec2 &b, const Vec2 &c) {
            return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        };
        auto inside_triangle = [&](const Vec2 &p, const Vec2 &a, const Vec2 &b, const Vec2 &c) {
            return cross2(a, b, p) >= -0.01 && cross2(b, c, p) >= -0.01 && cross2(c, a, p) >= -0.01;
        };
        std::vector<size_t> remaining(polygon.size());
        for (size_t i = 0; i < remaining.size(); ++i) remaining[i] = i;
        const float mat_f = static_cast<float>(material);
        auto emit_apron_tri = [&](auto &self, const Vec2 &a, const Vec2 &b, const Vec2 &c, int depth) -> void {
            float l0 = std::hypot(b.x - a.x, b.y - a.y);
            float l1 = std::hypot(c.x - b.x, c.y - b.y);
            float l2 = std::hypot(a.x - c.x, a.y - c.y);
            if (depth < 3 && std::max({l0, l1, l2}) > 40.0f) {
                Vec2 ab{(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f};
                Vec2 bc{(b.x + c.x) * 0.5f, (b.y + c.y) * 0.5f};
                Vec2 ca{(c.x + a.x) * 0.5f, (c.y + a.y) * 0.5f};
                self(self, a, ab, ca, depth + 1);
                self(self, ab, b, bc, depth + 1);
                self(self, ca, bc, c, depth + 1);
                self(self, ab, bc, ca, depth + 1);
                return;
            }
            push_surface_triangle(a, b, c,
                {a.x - center.x, a.y - center.y}, {b.x - center.x, b.y - center.y},
                {c.x - center.x, c.y - center.y}, mat_f, 0.0f, 0.09);
        };
        size_t guard = polygon.size() * polygon.size();
        while (remaining.size() > 3 && guard-- > 0) {
            bool clipped = false;
            for (size_t i = 0; i < remaining.size(); ++i) {
                const size_t ia = remaining[(i + remaining.size() - 1) % remaining.size()];
                const size_t ib = remaining[i];
                const size_t ic = remaining[(i + 1) % remaining.size()];
                if (cross2(polygon[ia], polygon[ib], polygon[ic]) <= 0.01) continue;
                bool contains = false;
                for (size_t j : remaining) {
                    if (j == ia || j == ib || j == ic) continue;
                    if (inside_triangle(polygon[j], polygon[ia], polygon[ib], polygon[ic])) {
                        contains = true; break;
                    }
                }
                if (contains) continue;
                const Vec2 a = polygon[ia], b = polygon[ib], c = polygon[ic];
                emit_apron_tri(emit_apron_tri, a, b, c, 0);
                remaining.erase(remaining.begin() + static_cast<std::ptrdiff_t>(i));
                clipped = true;
                break;
            }
            if (!clipped) break;
        }
        if (remaining.size() == 3) {
            const Vec2 a = polygon[remaining[0]], b = polygon[remaining[1]], c = polygon[remaining[2]];
            emit_apron_tri(emit_apron_tri, a, b, c, 0);
        }
    };
    for (const auto &a : pack_->airports()) {
        int ap_default_mat = material_for_surface(a.surface);
        if (ap_default_mat < 0) {
            ap_default_mat = (a.kind == "large_airport" || a.kind == "medium_airport") ? 2 : 1;
        }
        if (a.runways.empty()) {
            AirportRunway rw{a.rwy_lat_a, a.rwy_lon_a, a.rwy_elev_a,
                             a.rwy_lat_b, a.rwy_lon_b, a.rwy_elev_b,
                             a.rwy_width_m, a.surface};
            append_runway(rw);
        } else {
            for (const AirportRunway &rw : a.runways) append_runway(rw);
        }
        for (const AirportGroundFeature &feature : a.ground_features) {
            if (feature.kind == AirportGroundFeature::Kind::Taxiway) append_strip(feature, ap_default_mat, 2.0f, 0.12);
            else if (feature.kind == AirportGroundFeature::Kind::AccessRoad) append_strip(feature, 2, 3.0f, 0.07);
            else append_apron(feature, (a.kind == "large_airport" || a.kind == "medium_airport") ? 3 : ap_default_mat);
        }
    }
    return rv;
}

void WorldObjects::upload_runways_gpu(const std::vector<float> &rv) {
    if (rwy_vao_) { glDeleteVertexArrays(1, &rwy_vao_); rwy_vao_ = 0; }
    if (rwy_vbo_) { glDeleteBuffers(1, &rwy_vbo_); rwy_vbo_ = 0; }
    rwy_vert_count_ = static_cast<int>(rv.size() / 10);
    glCreateBuffers(1, &rwy_vbo_);
    glNamedBufferStorage(rwy_vbo_, static_cast<GLsizeiptr>(std::max<size_t>(10, rv.size()) * sizeof(float)), rv.empty() ? nullptr : rv.data(), 0);
    rwy_vao_ = create_runway_vao(rwy_vbo_);
}

GLuint WorldObjects::create_runway_vao(GLuint vbo) {
    GLuint vao = 0;
    glCreateVertexArrays(1, &vao);
    glVertexArrayVertexBuffer(vao, 0, vbo, 0, 10 * sizeof(float));
    glEnableVertexArrayAttrib(vao, 0); glVertexArrayAttribFormat(vao, 0, 3, GL_FLOAT, GL_FALSE, 0); glVertexArrayAttribBinding(vao, 0, 0);
    glEnableVertexArrayAttrib(vao, 1); glVertexArrayAttribFormat(vao, 1, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float)); glVertexArrayAttribBinding(vao, 1, 0);
    glEnableVertexArrayAttrib(vao, 2); glVertexArrayAttribFormat(vao, 2, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(float)); glVertexArrayAttribBinding(vao, 2, 0);
    glEnableVertexArrayAttrib(vao, 3); glVertexArrayAttribFormat(vao, 3, 1, GL_FLOAT, GL_FALSE, 8 * sizeof(float)); glVertexArrayAttribBinding(vao, 3, 0);
    glEnableVertexArrayAttrib(vao, 4); glVertexArrayAttribFormat(vao, 4, 1, GL_FLOAT, GL_FALSE, 9 * sizeof(float)); glVertexArrayAttribBinding(vao, 4, 0);
    return vao;
}

std::vector<float> WorldObjects::build_landmarks_cpu(const PackReader::WorldFrame &frame) const {
    std::vector<float> lv;
    lv.reserve(24000 * 12);

    // 1. BASILICA DE NUESTRA SENORA DEL PILAR (Zaragoza, 41.6569, -0.8787, exact PNOA aerial alignment yaw=-27 deg)
    //    True linear PBR reflectances (sRGB^2.2) so stone, Aragonese brick & glazed tiles look photographic, NOT pastel!
    {
        LmBuilder b(lv, pack_, frame, 41.6569, -0.8787, -27.0f, 0.0);
        const Vec3 brick{0.27f, 0.14f, 0.085f};
        const Vec3 stone{0.37f, 0.31f, 0.23f};
        const Vec3 roof{0.20f, 0.095f, 0.055f};
        const Vec3 spire_slate{0.055f, 0.075f, 0.105f};
        const Vec3 gold_dome{0.34f, 0.22f, 0.065f};
        const Vec3 blue_dome{0.055f, 0.125f, 0.22f};
        const Vec3 green_dome{0.055f, 0.155f, 0.085f};

        // Subtle dressed-stone plinth steps directly beneath the Basilica (Plaza del Pilar uses textured urban terrain!)
        const Vec3 plaza_curb{0.22f, 0.20f, 0.17f};
        b.add_box(0.0f, -2.5f, 0.0f, 136.0f, 3.1f, 70.0f, plaza_curb, 0.78f, 0.0f, 0.14f);

        // Main Baroque basilica body + recessed upper clerestory + cornice ledges
        b.add_box(0.0f, -4.0f, 0.0f, 128.0f, 24.5f, 64.0f, brick, 0.78f, 0.0f, 0.55f);
        b.add_box(0.0f, 19.5f, 0.0f, 131.0f, 2.2f, 66.8f, stone, 0.72f, 0.0f, 0.65f);
        b.add_box(0.0f, 21.7f, 0.0f, 122.0f, 4.5f, 56.0f, roof,  0.82f, 0.0f, 0.20f);
        // Pilaster columns & grand south Plaza portico
        for (int i = -5; i <= 5; i++) {
            float px = i * 11.2f;
            b.add_box(px, 0.0f,  32.6f, 2.2f, 20.5f, 1.4f, stone, 0.72f, 0.0f, 0.70f);
            b.add_box(px, 0.0f, -32.6f, 2.2f, 20.5f, 1.4f, stone, 0.72f, 0.0f, 0.60f);
        }
        b.add_box(0.0f, 0.0f, 33.8f, 34.0f, 23.5f, 3.6f, stone, 0.70f, 0.0f, 0.85f);

        // 4 Iconic 92m Corner Mudejar-Baroque Bell Towers!
        const float tx[2] = {-57.0f, 57.0f};
        const float tz[2] = {-27.0f, 27.0f};
        for (float x : tx) {
            for (float z : tz) {
                b.add_box(x, -4.0f, z, 11.5f, 58.0f, 11.5f, brick, 0.76f, 0.0f, 0.70f);
                b.add_box(x, 53.5f, z, 12.8f,  2.2f, 12.8f, stone, 0.70f, 0.0f, 0.85f);
                b.add_cyl(x, 55.7f, z,  5.0f,  4.4f, 17.5f, 8, stone, 0.68f, 0.0f, 0.90f);
                b.add_cyl(x, 73.2f, z,  4.6f,  0.4f, 15.5f, 8, spire_slate, 0.34f, 0.35f, 0.45f);
                b.add_cyl(x, 88.5f, z,  0.35f, 0.1f,  4.5f, 6, gold_dome, 0.20f, 0.85f, 0.40f);
            }
        }
        // Grand Central Cupola + 10 Polychrome Glazed Ceramic Domes (signature Zaragoza skyline!)
        b.add_cyl(0.0f, 25.0f, 0.0f, 14.5f, 14.2f, 11.0f, 16, stone, 0.66f, 0.0f, 0.90f);
        b.add_dome(0.0f, 35.8f, 0.0f, 14.2f, 13.5f, 14.2f, 7, 16, blue_dome, 0.24f, 0.18f, 0.50f);
        b.add_cyl(0.0f, 49.0f, 0.0f,  2.8f,  2.5f,  6.0f, 10, stone, 0.62f, 0.0f, 1.0f);
        b.add_cyl(0.0f, 55.0f, 0.0f,  2.5f,  0.2f,  4.5f, 10, gold_dome, 0.22f, 0.80f, 0.40f);

        const float dx[4] = {-36.0f, -15.0f, 15.0f, 36.0f};
        const float dz[2] = {-16.5f, 16.5f};
        int di = 0;
        for (float x : dx) {
            for (float z : dz) {
                const Vec3 &dc = (di % 3 == 0) ? gold_dome : ((di % 3 == 1) ? green_dome : blue_dome);
                b.add_cyl(x, 25.0f, z, 6.8f, 6.6f, 5.5f, 12, stone, 0.68f, 0.0f, 0.70f);
                b.add_dome(x, 30.3f, z, 6.6f, 7.2f, 6.6f, 6, 12, dc, 0.22f, 0.22f, 0.45f);
                b.add_cyl(x, 37.3f, z, 1.2f, 0.2f, 3.8f, 8, gold_dome, 0.25f, 0.70f, 0.30f);
                di++;
            }
        }
        for (float x : {-36.0f, 36.0f}) {
            b.add_cyl(x, 25.0f, 0.0f, 7.6f, 7.4f, 6.2f, 12, stone, 0.68f, 0.0f, 0.75f);
            b.add_dome(x, 31.0f, 0.0f, 7.4f, 8.0f, 7.4f, 6, 12, gold_dome, 0.22f, 0.24f, 0.50f);
            b.add_cyl(x, 38.8f, 0.0f, 1.4f, 0.2f, 4.2f, 8, gold_dome, 0.25f, 0.75f, 0.30f);
        }
    }

    // 2. PUENTE DE PIEDRA (Historic 15th-century 7-arch stone bridge spanning Rio Ebro, exact PNOA aerial piers)
    {
        LmBuilder b(lv, pack_, frame, 41.6578, -0.8755, 62.0f, 0.0);
        const Vec3 lime{0.34f, 0.29f, 0.22f};
        const Vec3 deck_asph{0.075f, 0.075f, 0.080f};
        const Vec3 bronze{0.12f, 0.095f, 0.055f};
        b.add_box(0.0f, 7.0f, 0.0f, 224.0f, 2.2f, 13.8f, lime, 0.80f, 0.0f, 0.45f);
        b.add_box(0.0f, 9.2f, 0.0f, 224.0f, 0.35f, 11.4f, deck_asph, 0.68f, 0.0f, 0.0f);
        b.add_box(0.0f, 9.2f,  6.5f, 224.0f, 1.35f, 0.85f, lime, 0.76f, 0.0f, 0.65f);
        b.add_box(0.0f, 9.2f, -6.5f, 224.0f, 1.35f, 0.85f, lime, 0.76f, 0.0f, 0.65f);
        for (int p = -3; p <= 3; p++) {
            float px = p * 31.0f;
            b.add_box(px, -6.5f, 0.0f, 7.2f, 14.0f, 17.5f, lime * 0.86f, 0.84f, 0.0f, 0.35f);
            b.add_cyl(px, -6.5f, -9.2f, 3.6f, 3.2f, 14.0f, 6, lime * 0.82f, 0.84f, 0.0f, 0.30f);
            b.add_cyl(px, -6.5f,  9.2f, 3.6f, 3.2f, 14.0f, 6, lime * 0.82f, 0.84f, 0.0f, 0.30f);
        }
        for (float bx : {-106.0f, 106.0f}) {
            for (float bz : {-6.8f, 6.8f}) {
                b.add_cyl(bx, 9.2f, bz, 0.85f, 0.72f, 7.5f, 8, lime, 0.70f, 0.0f, 0.80f);
                b.add_box(bx, 16.7f, bz, 1.6f, 1.8f, 2.2f, bronze, 0.32f, 0.82f, 0.45f);
            }
        }
    }

    // 3. TORRE DEL AGUA (Zaragoza Expo 2008 76m glass-and-steel water-drop tower, 41.6693, -0.9073!)
    {
        LmBuilder b(lv, pack_, frame, 41.6693, -0.9073, 12.0f, 0.0);
        const Vec3 granite{0.08f, 0.085f, 0.095f};
        const Vec3 glass{0.12f, 0.25f, 0.34f};
        const Vec3 steel{0.48f, 0.50f, 0.52f};
        b.add_box(0.0f, -3.0f, 0.0f, 28.0f, 13.0f, 20.0f, granite, 0.65f, 0.05f, 0.30f);
        b.add_cyl( 0.0f, 10.0f,  0.0f, 11.5f, 10.0f, 54.0f, 16, glass, 0.08f, 0.42f, 0.55f);
        b.add_cyl( 8.5f, 10.0f,  0.0f,  7.5f,  4.8f, 56.0f, 12, glass * 1.06f, 0.08f, 0.45f, 0.55f);
        b.add_dome(2.0f, 64.0f,  0.0f, 11.5f, 11.5f, 10.0f,  6, 16, glass, 0.08f, 0.45f, 0.65f);
        for (int i = 0; i < 6; i++) {
            float fy = 16.0f + i * 9.0f;
            b.add_cyl(2.0f, fy, 0.0f, 12.2f - i * 0.25f, 12.2f - i * 0.25f, 0.75f, 16, steel, 0.28f, 0.78f, 0.40f, false);
        }
    }

    // 4. CATEDRAL DE HUESCA (Gothic sandstone cathedral + octagonal tower, 42.1404, -0.4086!)
    {
        LmBuilder b(lv, pack_, frame, 42.1404, -0.4086, -12.0f, 0.0);
        const Vec3 sand{0.34f, 0.26f, 0.16f};
        const Vec3 tile{0.22f, 0.095f, 0.055f};
        b.add_box(0.0f, -4.0f, 0.0f, 56.0f, 22.0f, 32.0f, sand, 0.78f, 0.0f, 0.55f);
        b.add_box(0.0f, 18.0f, 0.0f, 54.0f,  4.8f, 24.0f, tile, 0.80f, 0.0f, 0.15f);
        b.add_box(0.0f, -4.0f, 0.0f, 18.0f, 24.5f, 40.0f, sand * 1.03f, 0.76f, 0.0f, 0.60f);
        b.add_cyl(-22.0f, -4.0f, -15.0f, 6.2f, 5.6f, 44.0f, 8, sand, 0.74f, 0.0f, 0.85f);
        b.add_cyl(-22.0f, 40.0f, -15.0f, 5.2f, 0.3f, 11.0f, 8, tile * 0.8f, 0.68f, 0.0f, 0.45f);
    }

    // 5. CASTILLO DE LOARRE (Iconic hilltop Romanesque fortress, 42.3257, -0.6119!)
    {
        LmBuilder b(lv, pack_, frame, 42.3257, -0.6119, 15.0f, 0.0);
        const Vec3 rock{0.32f, 0.27f, 0.20f};
        const Vec3 keep{0.28f, 0.23f, 0.17f};
        b.add_box(0.0f, -6.0f, 0.0f, 62.0f, 16.0f, 44.0f, rock, 0.84f, 0.0f, 0.35f);
        b.add_box(-8.0f, 6.0f, -6.0f, 14.0f, 22.0f, 14.0f, keep, 0.82f, 0.0f, 0.55f);
        b.add_cyl(14.0f, 6.0f, -4.0f, 8.5f, 8.2f, 15.0f, 12, rock * 1.04f, 0.80f, 0.0f, 0.55f);
        b.add_dome(14.0f, 21.0f, -4.0f, 8.2f, 5.5f, 8.2f, 5, 12, {0.20f, 0.11f, 0.065f}, 0.78f, 0.0f, 0.25f);
        for (int i = 0; i < 6; i++) {
            float a = -0.35f + i * 0.72f;
            b.add_cyl(std::cos(a) * 34.0f, -6.0f, std::sin(a) * 26.0f, 4.2f, 4.0f, 16.5f, 10, rock, 0.84f, 0.0f, 0.40f);
        }
    }

    // 6. TORRE SUMERGIDA DE MEDIANO (Iconic 16th-century church bell tower rising out of Embalse de Mediano, 42.3175, 0.2037!)
    {
        LmBuilder b(lv, pack_, frame, 42.3175, 0.2037, 8.0f, 0.0);
        const Vec3 stone{0.28f, 0.24f, 0.18f};
        const Vec3 tile{0.20f, 0.095f, 0.055f};
        b.add_box(0.0f, -12.0f, 0.0f, 7.6f, 34.0f, 7.6f, stone, 0.82f, 0.0f, 0.30f);
        b.add_box(0.0f,  22.0f, 0.0f, 8.4f,  1.2f, 8.4f, stone * 1.08f, 0.78f, 0.0f, 0.40f);
        b.add_cyl(0.0f,  23.2f, 0.0f, 5.4f,  0.3f, 5.2f, 4, tile, 0.78f, 0.0f, 0.20f);
    }

    // 7. UNIVERSAL AIRPORT GROUND PROPS (OSM fences/walls/hedges, grass runway markers, 3D windsocks across all regions!)
    const Vec3 fence_steel{0.26f, 0.28f, 0.30f};
    const Vec3 fence_wire{0.18f, 0.20f, 0.22f};
    const Vec3 wall_stone{0.34f, 0.31f, 0.26f};
    const Vec3 hedge_leaf{0.06f, 0.13f, 0.045f};
    const Vec3 marker_white{0.76f, 0.76f, 0.72f};
    const Vec3 sock_orange{0.78f, 0.16f, 0.03f};
    const Vec3 sock_white{0.80f, 0.80f, 0.78f};

    for (const auto &a : pack_->airports()) {
        // 7a. OSM perimeter fences, walls, and hedges draped on DEM
        for (const auto &fn : a.fences) {
            const bool is_wall = (fn.kind == "wall");
            const bool is_hedge = (fn.kind == "hedge");
            for (size_t pi = 1; pi < fn.points_lat_lon.size(); ++pi) {
                auto [lat0, lon0] = fn.points_lat_lon[pi - 1];
                auto [lat1, lon1] = fn.points_lat_lon[pi];
                double e0 = (lon0 - frame.anchor_lon) * frame.metres_lon;
                double n0 = (lat0 - frame.anchor_lat) * frame.metres_lat;
                double e1 = (lon1 - frame.anchor_lon) * frame.metres_lon;
                double n1 = (lat1 - frame.anchor_lat) * frame.metres_lat;
                double de = e1 - e0, dn = n1 - n0;
                double seg_len = std::hypot(de, dn);
                if (seg_len < 1.5 || seg_len > 1500.0) continue;
                float yaw_deg = static_cast<float>(std::atan2(-dn, de) * RAD2DEG_D);
                int spans = std::clamp(static_cast<int>(std::ceil(seg_len / (is_wall ? 14.0 : 12.0))), 1, 40);
                double span_len = seg_len / spans;
                for (int s = 0; s < spans; ++s) {
                    double tm = (s + 0.5) / spans;
                    double mlat = lat0 + (lat1 - lat0) * tm;
                    double mlon = lon0 + (lon1 - lon0) * tm;
                    LmBuilder fb(lv, pack_, frame, mlat, mlon, yaw_deg, 0.0);
                    float flen = static_cast<float>(span_len);
                    if (is_wall) {
                        fb.add_box(0.0f, -0.35f, 0.0f, flen, 1.95f, 0.35f, wall_stone, 0.86f, 0.0f, 0.0f);
                    } else if (is_hedge) {
                        fb.add_box(0.0f, -0.25f, 0.0f, flen, 1.65f, 0.95f, hedge_leaf, 0.92f, 0.0f, 0.0f);
                    } else {
                        // Open European airport perimeter post-and-rail fence (never a solid wall!)
                        fb.add_box(0.0f, 1.68f, 0.0f, flen, 0.06f, 0.06f, fence_steel, 0.45f, 0.65f, 0.0f);
                        fb.add_box(0.0f, 1.08f, 0.0f, flen, 0.045f, 0.045f, fence_wire, 0.55f, 0.50f, 0.0f);
                        fb.add_box(0.0f, 0.48f, 0.0f, flen, 0.045f, 0.045f, fence_wire, 0.55f, 0.50f, 0.0f);
                        int posts = std::max(1, static_cast<int>(std::round(span_len / 4.0)));
                        for (int pk = 0; pk < posts; ++pk) {
                            float px = -flen * 0.5f + (flen * pk) / posts;
                            fb.add_box(px, -0.25f, 0.0f, 0.09f, 2.05f, 0.09f, fence_steel, 0.40f, 0.70f, 0.0f);
                        }
                    }
                }
            }
        }

        // 7b. ICAO white edge & threshold markers along grass runways (makes grass strips unmistakable from air & ground!)
        auto add_grass_markers = [&](const AirportRunway &rw) {
            std::string sl = rw.surface;
            for (char &ch : sl) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (sl.find("grass") == std::string::npos && sl.find("turf") == std::string::npos && sl.find("grs") == std::string::npos) return;
            double ea = (rw.lon_a - frame.anchor_lon) * frame.metres_lon;
            double na = (rw.lat_a - frame.anchor_lat) * frame.metres_lat;
            double eb = (rw.lon_b - frame.anchor_lon) * frame.metres_lon;
            double nb = (rw.lat_b - frame.anchor_lat) * frame.metres_lat;
            double de = eb - ea, dn = nb - na;
            double len = std::hypot(de, dn);
            if (len < 120.0) return;
            double half_w = std::max(10.0, rw.width_m * 0.52);
            double rx = dn / len * half_w, rn = -de / len * half_w;
            float yaw_deg = static_cast<float>(std::atan2(-dn, de) * RAD2DEG_D);
            int n_side = std::clamp(static_cast<int>(len / 55.0), 4, 24);
            for (int k = 0; k <= n_side; ++k) {
                double t = static_cast<double>(k) / n_side;
                double ce = ea + de * t, cn = na + dn * t;
                const bool is_end = (k == 0 || k == n_side);
                for (double side : {-1.0, 1.0}) {
                    double me = ce + rx * side, mn = cn + rn * side;
                    double mlat = frame.anchor_lat + mn / frame.metres_lat;
                    double mlon = frame.anchor_lon + me / frame.metres_lon;
                    LmBuilder mb(lv, pack_, frame, mlat, mlon, yaw_deg, 0.0);
                    // Low white painted gable board (ICAO grass runway side marker)
                    mb.add_box(0.0f, -0.05f, 0.0f, is_end ? 2.4f : 1.9f, 0.24f, 0.42f, marker_white, 0.68f, 0.0f, 0.0f);
                    if (is_end) {
                        // Threshold L-bar wing pointing inward toward runway centerline
                        mb.add_box(0.0f, -0.05f, static_cast<float>(-side * 1.15f), 0.50f, 0.24f, 2.2f, marker_white, 0.68f, 0.0f, 0.0f);
                    }
                }
            }
        };
        if (a.runways.empty()) {
            AirportRunway rw{a.rwy_lat_a, a.rwy_lon_a, a.rwy_elev_a, a.rwy_lat_b, a.rwy_lon_b, a.rwy_elev_b, a.rwy_width_m, a.surface};
            add_grass_markers(rw);
        } else {
            for (const auto &rw : a.runways) add_grass_markers(rw);
        }

        // 7c. 3D Windsocks (OSM mapped positions, or a sensible abeam-midfield fallback on every airfield)
        std::vector<std::pair<double, double>> ws_pts = a.windsocks;
        if (ws_pts.empty()) {
            double ea = (a.rwy_lon_a - frame.anchor_lon) * frame.metres_lon;
            double na = (a.rwy_lat_a - frame.anchor_lat) * frame.metres_lat;
            double eb = (a.rwy_lon_b - frame.anchor_lon) * frame.metres_lon;
            double nb = (a.rwy_lat_b - frame.anchor_lat) * frame.metres_lat;
            double de = eb - ea, dn = nb - na;
            double len = std::hypot(de, dn);
            if (len > 100.0) {
                double offset_m = std::max(34.0, a.rwy_width_m * 1.2 + 16.0);
                double we = (ea + eb) * 0.5 + (dn / len) * offset_m;
                double wn = (na + nb) * 0.5 - (de / len) * offset_m;
                ws_pts.emplace_back(frame.anchor_lat + wn / frame.metres_lat,
                                    frame.anchor_lon + we / frame.metres_lon);
            }
        }
        for (const auto &[wlat, wlon] : ws_pts) {
            LmBuilder wb(lv, pack_, frame, wlat, wlon, 25.0f, 0.0);
            // Concrete base pad + white/orange steel mast + 5-ring orange/white fabric cone
            wb.add_cyl(0.0f, -0.20f, 0.0f, 0.55f, 0.50f, 0.35f, 8, wall_stone, 0.85f, 0.0f, 0.0f);
            wb.add_cyl(0.0f,  0.10f, 0.0f, 0.09f, 0.06f, 6.20f, 6, sock_white, 0.42f, 0.55f, 0.0f);
            for (int r = 0; r < 5; ++r) {
                float x = 0.45f + r * 0.52f;
                float rad = 0.34f - r * 0.045f;
                const Vec3 &col = (r % 2 == 0) ? sock_orange : sock_white;
                wb.add_box(x, 5.75f - r * 0.07f, 0.0f, 0.50f, rad * 1.85f, rad * 1.85f, col, 0.78f, 0.0f, 0.0f);
            }
        }
    }

    return lv;
}

void WorldObjects::upload_landmarks_gpu(const std::vector<float> &lv) {
    if (lm_vao_) { glDeleteVertexArrays(1, &lm_vao_); lm_vao_ = 0; }
    if (lm_vbo_) { glDeleteBuffers(1, &lm_vbo_); lm_vbo_ = 0; }
    lm_vert_count_ = static_cast<int>(lv.size() / 12);
    glCreateBuffers(1, &lm_vbo_);
    glNamedBufferStorage(lm_vbo_, static_cast<GLsizeiptr>(std::max<size_t>(12, lv.size()) * sizeof(float)), lv.empty() ? nullptr : lv.data(), 0);
    lm_vao_ = create_landmark_vao(lm_vbo_);
}

GLuint WorldObjects::create_landmark_vao(GLuint vbo) {
    GLuint vao = 0;
    glCreateVertexArrays(1, &vao);
    glVertexArrayVertexBuffer(vao, 0, vbo, 0, 12 * sizeof(float));
    glEnableVertexArrayAttrib(vao, 0); glVertexArrayAttribFormat(vao, 0, 3, GL_FLOAT, GL_FALSE, 0); glVertexArrayAttribBinding(vao, 0, 0);
    glEnableVertexArrayAttrib(vao, 1); glVertexArrayAttribFormat(vao, 1, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float)); glVertexArrayAttribBinding(vao, 1, 0);
    glEnableVertexArrayAttrib(vao, 2); glVertexArrayAttribFormat(vao, 2, 4, GL_FLOAT, GL_FALSE, 6 * sizeof(float)); glVertexArrayAttribBinding(vao, 2, 0);
    glEnableVertexArrayAttrib(vao, 3); glVertexArrayAttribFormat(vao, 3, 2, GL_FLOAT, GL_FALSE, 10 * sizeof(float)); glVertexArrayAttribBinding(vao, 3, 0);
    return vao;
}

void WorldObjects::build_car_mesh() {
    // Tapered European sedan/hatchback mesh in [-0.5..0.5] x [0..1] x [-0.5..0.5], nose at +Z (+0.5)!
    std::vector<float> cv; // pos(3), nrm(3), part(1) = 7 floats
    std::vector<uint32_t> ci;
    auto push_quad = [&](Vec3 a, Vec3 b, Vec3 d, Vec3 e, float part) {
        Vec3 n = normalize(cross(b - a, d - a));
        uint32_t base = static_cast<uint32_t>(cv.size() / 7);
        for (const Vec3 &p : {a, b, d, e}) cv.insert(cv.end(), {p.x, p.y, p.z, n.x, n.y, n.z, part});
        ci.insert(ci.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    };
    auto add_prism = [&](float z0, float z1, float yb0, float yt0, float wb0, float wt0,
                         float yb1, float yt1, float wb1, float wt1, float side_p, float top_p, float front_p, float rear_p) {
        // Front (+z1) & rear (-z0)
        if (front_p >= 0.0f) push_quad({-wb1, yb1, z1}, { wb1, yb1, z1}, { wt1, yt1, z1}, {-wt1, yt1, z1}, front_p);
        if (rear_p  >= 0.0f) push_quad({ wb0, yb0, z0}, {-wb0, yb0, z0}, {-wt0, yt0, z0}, { wt0, yt0, z0}, rear_p);
        // Right (+X) & Left (-X)
        push_quad({ wb1, yb1, z1}, { wb0, yb0, z0}, { wt0, yt0, z0}, { wt1, yt1, z1}, side_p);
        push_quad({-wb0, yb0, z0}, {-wb1, yb1, z1}, {-wt1, yt1, z1}, {-wt0, yt0, z0}, side_p);
        // Top
        push_quad({-wt1, yt1, z1}, { wt1, yt1, z1}, { wt0, yt0, z0}, {-wt0, yt0, z0}, top_p);
    };
    // Minimal, clean 2-part low-poly car silhouette (sleek lower body + subtle dark cabin + flush light quads, zero clunky wheel boxes!)
    add_prism(-0.46f,  0.46f,  0.10f, 0.54f, 0.44f, 0.42f,  0.10f, 0.50f, 0.44f, 0.41f, 0.0f, 0.0f, 0.0f, 0.0f);
    add_prism(-0.26f,  0.18f,  0.52f, 0.88f, 0.40f, 0.34f,  0.50f, 0.86f, 0.40f, 0.34f, 1.0f, 0.0f, 1.0f, 1.0f);
    // Flush front headlights (+Z) & rear taillights (-Z)
    push_quad({-0.38f, 0.28f,  0.465f}, {-0.22f, 0.28f,  0.465f}, {-0.22f, 0.38f,  0.465f}, {-0.38f, 0.38f,  0.465f}, 2.0f);
    push_quad({ 0.22f, 0.28f,  0.465f}, { 0.38f, 0.28f,  0.465f}, { 0.38f, 0.38f,  0.465f}, { 0.22f, 0.38f,  0.465f}, 2.0f);
    push_quad({-0.22f, 0.30f, -0.465f}, {-0.38f, 0.30f, -0.465f}, {-0.38f, 0.40f, -0.465f}, {-0.22f, 0.40f, -0.465f}, 3.0f);
    push_quad({ 0.38f, 0.30f, -0.465f}, { 0.22f, 0.30f, -0.465f}, { 0.22f, 0.40f, -0.465f}, { 0.38f, 0.40f, -0.465f}, 3.0f);

    car_idx_count_ = static_cast<int>(ci.size());
    glCreateVertexArrays(1, &car_vao_);
    glCreateBuffers(1, &car_vbo_);
    glCreateBuffers(1, &car_ebo_);
    glNamedBufferStorage(car_vbo_, static_cast<GLsizeiptr>(cv.size() * sizeof(float)), cv.data(), 0);
    glNamedBufferStorage(car_ebo_, static_cast<GLsizeiptr>(ci.size() * sizeof(uint32_t)), ci.data(), 0);
    glVertexArrayVertexBuffer(car_vao_, 0, car_vbo_, 0, 7 * sizeof(float));
    glVertexArrayElementBuffer(car_vao_, car_ebo_);
    glEnableVertexArrayAttrib(car_vao_, 0); glVertexArrayAttribFormat(car_vao_, 0, 3, GL_FLOAT, GL_FALSE, 0); glVertexArrayAttribBinding(car_vao_, 0, 0);
    glEnableVertexArrayAttrib(car_vao_, 1); glVertexArrayAttribFormat(car_vao_, 1, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float)); glVertexArrayAttribBinding(car_vao_, 1, 0);
    glEnableVertexArrayAttrib(car_vao_, 2); glVertexArrayAttribFormat(car_vao_, 2, 1, GL_FLOAT, GL_FALSE, 6 * sizeof(float)); glVertexArrayAttribBinding(car_vao_, 2, 0);
}

void WorldObjects::rebuild_local_roads_and_traffic(double cam_e, double cam_n, int quality) {
    if (pack_->roads().empty()) return;

    auto commit_roads = [&](PreparedRoads &&res) {
        road_center_e_ = res.ce;
        road_center_n_ = res.cn;
        road_last_q_ = res.q;
        road_vert_count_ = static_cast<int>(res.rv.size());
        if (road_vert_count_ > 0) {
            glNamedBufferSubData(road_vbo_, 0, static_cast<GLsizeiptr>(res.rv.size() * sizeof(RoadVert)), res.rv.data());
        }
        active_segs_ = std::move(res.segs);
        road_adj_ = std::move(res.adj);
        agents_ = std::move(res.agents);
        car_gpu_buf_.resize(agents_.size());
        active_cars_ = static_cast<int>(agents_.size());
        prev_traffic_t_ = -1.0f;
    };

    if (road_stream_.pending) {
        if (road_stream_.job.valid() &&
            road_stream_.job.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            PreparedRoads ready = road_stream_.job.get();
            road_stream_ = {};
            commit_roads(std::move(ready));
        } else {
            return;
        }
    }

    double move = std::hypot(cam_e - road_center_e_, cam_n - road_center_n_);
    if (move < 750.0 && quality == road_last_q_) return;

    const auto frame = pack_->world_frame();
    if (!frame) return;

    if (road_last_q_ < 0 || std::abs(road_center_e_) > 1e11) {
        PreparedRoads init_res = build_roads_cpu(pack_, frame, cam_e, cam_n, quality);
        commit_roads(std::move(init_res));
        return;
    }

    road_stream_.pending = true;
    road_stream_.ce = cam_e;
    road_stream_.cn = cam_n;
    road_stream_.q = quality;
    const PackReader *pack_ptr = pack_;
    road_stream_.job = std::async(std::launch::async, [pack_ptr, frame, cam_e, cam_n, quality]() {
        return build_roads_cpu(pack_ptr, frame, cam_e, cam_n, quality);
    });
}

WorldObjects::PreparedRoads WorldObjects::build_roads_cpu(
    const PackReader *pack_ptr, std::shared_ptr<const PackReader::WorldFrame> frame_ptr,
    double cam_e, double cam_n, int quality) {
    PreparedRoads out;
    out.ce = cam_e;
    out.cn = cam_n;
    out.q = quality;
    const PackReader::WorldFrame &frame = *frame_ptr;

    double rad_m = (quality <= 0) ? 1800.0 : (quality == 1 ? 2400.0 : (quality == 2 ? 3000.0 : (quality == 3 ? 3600.0 : 4800.0)));
    double cam_lat = frame.anchor_lat + cam_n / frame.metres_lat;
    double cam_lon = frame.anchor_lon + cam_e / frame.metres_lon;
    double dlat = rad_m / frame.metres_lat;
    double dlon = rad_m / frame.metres_lon;

    std::vector<uint32_t> r_idxs;
    pack_ptr->query_roads_in_box(cam_lat - dlat, cam_lat + dlat, cam_lon - dlon, cam_lon + dlon, r_idxs);
    const auto &all_roads = pack_ptr->roads();
    const auto &all_pts = pack_ptr->road_pts();
    std::sort(r_idxs.begin(), r_idxs.end(), [&](uint32_t a, uint32_t b) { return all_roads[a].kind < all_roads[b].kind; });

    auto &rv = out.rv;
    rv.reserve(std::min<size_t>(MAX_ROAD_VERTS, r_idxs.size() * 18));
    auto &segs = out.segs;
    segs.reserve(6144);

    static constexpr float kHalfW[4]    = {8.8f, 6.4f, 4.7f, 3.4f};
    static constexpr float kLaneOff[4]  = {3.9f, 2.5f, 1.7f, 1.1f};
    static constexpr float kLiftM[4]    = {0.42f, 0.36f, 0.30f, 0.24f};

    auto sample_road_y = [&](double e, double n, float baked_gnd, uint8_t kind, bool need_dem) -> float {
        double h = static_cast<double>(baked_gnd);
        if (need_dem || h < 1.0) {
            double dh = pack_ptr->sample_elevation(frame.anchor_lat + n / frame.metres_lat,
                                                   frame.anchor_lon + e / frame.metres_lon);
            h = (h < 1.0) ? dh : std::max(h, dh);
        }
        return static_cast<float>(h + kLiftM[kind]);
    };

    for (uint32_t ridx : r_idxs) {
        const RoadRec &rr = all_roads[ridx];
        if (rr.count < 2 || rr.offset + rr.count > all_pts.size()) continue;
        uint8_t k = std::min<uint8_t>(rr.kind, 3);
        float hw = kHalfW[k];
        float v_accum = 0.0f;

        for (uint16_t pi = 0; pi + 1 < rr.count; pi++) {
            const RoadPt &p0 = all_pts[rr.offset + pi];
            const RoadPt &p1 = all_pts[rr.offset + pi + 1];
            double e0 = (p0.lon_e6 * 1e-6 - frame.anchor_lon) * frame.metres_lon;
            double n0 = (p0.lat_e6 * 1e-6 - frame.anchor_lat) * frame.metres_lat;
            double e1 = (p1.lon_e6 * 1e-6 - frame.anchor_lon) * frame.metres_lon;
            double n1 = (p1.lat_e6 * 1e-6 - frame.anchor_lat) * frame.metres_lat;
            double mid_e = 0.5 * (e0 + e1), mid_n = 0.5 * (n0 + n1);
            double mid_d = std::hypot(mid_e - cam_e, mid_n - cam_n);
            if (mid_d > rad_m || (k == 3 && mid_d > 1600.0)) continue;

            double de = e1 - e0, dn = n1 - n0;
            double seg_len = std::hypot(de, dn);
            if (seg_len < 2.0) continue;
            double ue = de / seg_len, un = dn / seg_len;

            if (p0.ground_m < 620.0f && pack_ptr->is_near_building(frame,
                    frame.anchor_lat + mid_n / frame.metres_lat,
                    frame.anchor_lon + mid_e / frame.metres_lon, 18.0)) {
                if (seg_len > 6.0 && mid_d < 2000.0 && (k <= 1 || (k == 2 && segs.size() < 2600)) && segs.size() < 6000) {
                    float y0 = p0.ground_m + 0.12f;
                    float y1 = p1.ground_m + 0.12f;
                    segs.push_back({
                        static_cast<float>(e0), y0, static_cast<float>(n0),
                        static_cast<float>(e1), y1, static_cast<float>(n1),
                        static_cast<float>(seg_len), static_cast<float>(std::atan2(ue, -un)), kLaneOff[k], k
                    });
                }
                continue;
            }
            if (static_cast<int>(rv.size()) + 48 >= MAX_ROAD_VERTS) continue;

            double rx = un * hw, rn = -ue * hw;
            int steps = std::clamp(static_cast<int>(std::ceil(seg_len / 34.0)), 1, 12);
            if (static_cast<int>(rv.size()) + steps * 6 >= MAX_ROAD_VERTS) break;

            double prev_e = e0, prev_n = n0;
            float prev_y = sample_road_y(prev_e, prev_n, p0.ground_m, k, false);
            uint32_t prev_gi = pack_ptr->sample_baked_gi_u32_en(frame, prev_e, prev_n);
            for (int st = 1; st <= steps; st++) {
                double t = static_cast<double>(st) / steps;
                double cur_e = e0 + de * t;
                double cur_n = n0 + dn * t;
                float g_interp = p0.ground_m + (p1.ground_m - p0.ground_m) * static_cast<float>(t);
                float cur_y = sample_road_y(cur_e, cur_n, g_interp, k, steps > 1 && st < steps);
                uint32_t cur_gi = pack_ptr->sample_baked_gi_u32_en(frame, cur_e, cur_n);
                float sub_len = static_cast<float>(seg_len / steps);
                float v0 = v_accum;
                float v1 = v_accum + sub_len;
                v_accum = v1;

                auto push = [&](double e, float y, double n, float u, float v, uint32_t gi) {
                    rv.push_back({{static_cast<float>(e), y, static_cast<float>(n)}, {u, v}, static_cast<float>(k), gi});
                };
                push(prev_e - rx, prev_y, prev_n - rn, 0.0f, v0, prev_gi);
                push(prev_e + rx, prev_y, prev_n + rn, 1.0f, v0, prev_gi);
                push(cur_e  + rx, cur_y,  cur_n  + rn, 1.0f, v1, cur_gi);
                push(prev_e - rx, prev_y, prev_n - rn, 0.0f, v0, prev_gi);
                push(cur_e  + rx, cur_y,  cur_n  + rn, 1.0f, v1, cur_gi);
                push(cur_e  - rx, cur_y,  cur_n  - rn, 0.0f, v1, cur_gi);

                if (sub_len > 18.0f && mid_d < 2200.0 && k <= 2 && segs.size() < 4000) {
                    segs.push_back({
                        static_cast<float>(prev_e), prev_y, static_cast<float>(prev_n),
                        static_cast<float>(cur_e),  cur_y,  static_cast<float>(cur_n),
                        sub_len, static_cast<float>(std::atan2(ue, -un)), kLaneOff[k], k
                    });
                }
                prev_e = cur_e; prev_n = cur_n; prev_y = cur_y; prev_gi = cur_gi;
            }
        }
    }

    auto &adj = out.adj;
    if (!segs.empty()) {
        std::unordered_map<int64_t, uint32_t> node_map;
        node_map.reserve(segs.size() * 2);
        auto get_node = [&](float e, float n) -> uint32_t {
            int32_t qe = static_cast<int32_t>(std::lround(e * 0.4f));
            int32_t qn = static_cast<int32_t>(std::lround(n * 0.4f));
            int64_t key = pack_tc(qe, qn);
            auto it = node_map.find(key);
            if (it != node_map.end()) return it->second;
            uint32_t nid = static_cast<uint32_t>(adj.size());
            node_map.emplace(key, nid);
            adj.emplace_back();
            return nid;
        };
        for (uint32_t si = 0; si < static_cast<uint32_t>(segs.size()); ++si) {
            RoadSeg &sg = segs[si];
            sg.node_a = get_node(sg.e0, sg.n0);
            sg.node_b = get_node(sg.e1, sg.n1);
            if (sg.node_a == sg.node_b) continue;
            adj[sg.node_a].push_back((si << 1u) | 0u);
            adj[sg.node_b].push_back((si << 1u) | 1u);
        }
    }

    if (!segs.empty()) {
        int target = (quality <= 0) ? 110 : (quality == 1 ? 210 : (quality == 2 ? 340 : (quality == 3 ? 520 : 820)));
        target = std::min(target, std::max(8, static_cast<int>(segs.size()) / 3));
        target = std::min(target, MAX_CARS);
        out.agents.reserve(target);
        for (int i = 0; i < target; i++) {
            uint32_t h = static_cast<uint32_t>(i) * 2654435761u ^ 0x9E3779B9u;
            TrafficAgent ag{};
            ag.rng = h | 1u;
            ag.speed_scale = 0.84f + 0.32f * static_cast<float>((h >> 16) & 0xFFu) / 255.0f;
            uint32_t vt = (h >> 24) % 100u;
            if (vt < 84u) {
                ag.w = 1.42f; ag.h = 1.02f; ag.l = 3.25f;
            } else {
                ag.w = 1.54f; ag.h = 1.28f; ag.l = 3.75f;
            }
            ag.col_id = static_cast<float>((h >> 4) & 7u);
            plan_vehicle_route_on(segs, adj, ag, true);
            if (!ag.route.empty()) out.agents.push_back(std::move(ag));
        }
    }
    return out;
}

void WorldObjects::plan_vehicle_route(TrafficAgent &ag, bool random_progress) {
    plan_vehicle_route_on(active_segs_, road_adj_, ag, random_progress);
}

void WorldObjects::plan_vehicle_route_on(
    const std::vector<RoadSeg> &active_segs_,
    const std::vector<std::vector<uint32_t>> &road_adj_,
    TrafficAgent &ag, bool random_progress) {
    ag.route.clear();
    ag.step = 0;
    ag.dist_m = 0.0f;
    if (active_segs_.empty() || road_adj_.empty()) return;

    auto next_u32 = [&]() -> uint32_t {
        ag.rng = ag.rng * 1664525u + 1013904223u;
        return ag.rng;
    };
    auto edge_dir = [&](uint32_t code, float &ue, float &un, uint32_t &dst_node) {
        const RoadSeg &sg = active_segs_[code >> 1u];
        float sgn = (code & 1u) ? -1.0f : 1.0f;
        float invl = 1.0f / std::max(0.01f, sg.len);
        ue = sgn * (sg.e1 - sg.e0) * invl;
        un = sgn * (sg.n1 - sg.n0) * invl;
        dst_node = (code & 1u) ? sg.node_a : sg.node_b;
    };

    // Pick a real connected start edge (bias toward highways & main avenues).
    uint32_t start_code = 0;
    bool found = false;
    for (int tries = 0; tries < 10; ++tries) {
        uint32_t si = next_u32() % static_cast<uint32_t>(active_segs_.size());
        const RoadSeg &sg = active_segs_[si];
        if (sg.node_a == sg.node_b) continue;
        uint32_t code = (si << 1u) | (next_u32() & 1u);
        uint32_t dst = (code & 1u) ? sg.node_a : sg.node_b;
        if (!found || (sg.kind <= 1 && !road_adj_[dst].empty())) {
            start_code = code;
            found = true;
            if (sg.kind <= 1 && road_adj_[dst].size() >= 2) break;
        }
    }
    if (!found) return;

    const int max_edges = 14 + static_cast<int>(next_u32() % 30u);
    std::unordered_set<uint32_t> visited_nodes;
    visited_nodes.reserve(static_cast<size_t>(max_edges) + 2);
    ag.route.reserve(max_edges);

    uint32_t cur_code = start_code;
    {
        const RoadSeg &sg = active_segs_[cur_code >> 1u];
        visited_nodes.insert((cur_code & 1u) ? sg.node_b : sg.node_a);
    }
    for (int s = 0; s < max_edges; ++s) {
        ag.route.push_back(cur_code);
        float ue = 0.0f, un = 0.0f;
        uint32_t cur_node = 0;
        edge_dir(cur_code, ue, un, cur_node);
        visited_nodes.insert(cur_node);
        if (cur_node >= road_adj_.size()) break;

        const uint8_t cur_kind = active_segs_[cur_code >> 1u].kind;
        uint32_t best_next = UINT32_MAX;
        float best_score = -1e9f;
        for (uint32_t cand : road_adj_[cur_node]) {
            if ((cand >> 1u) == (cur_code >> 1u)) continue; // Never U-turn back onto same segment.
            float cue = 0.0f, cun = 0.0f;
            uint32_t ndst = 0;
            edge_dir(cand, cue, cun, ndst);
            if (visited_nodes.count(ndst)) continue; // Strictly acyclic A->B journey (no loops!).
            float dot_fwd = ue * cue + un * cun;
            if (dot_fwd < -0.35f) continue;
            uint8_t ckind = active_segs_[cand >> 1u].kind;
            float kind_bonus = (ckind == cur_kind) ? 0.65f : ((ckind < cur_kind) ? 0.35f : -0.25f);
            float jitter = static_cast<float>(next_u32() & 255u) * (0.38f / 255.0f);
            float sc = dot_fwd * 1.4f + kind_bonus + jitter;
            if (sc > best_score) { best_score = sc; best_next = cand; }
        }
        if (best_next == UINT32_MAX) break;
        cur_code = best_next;
    }

    if (random_progress && !ag.route.empty()) {
        ag.step = static_cast<uint16_t>(next_u32() % ag.route.size());
        const RoadSeg &sg = active_segs_[ag.route[ag.step] >> 1u];
        ag.dist_m = sg.len * (static_cast<float>(next_u32() & 1023u) / 1024.0f);
    }
    const RoadSeg &sg0 = active_segs_[ag.route[ag.step] >> 1u];
    float base_spd = (sg0.kind == 0) ? 28.0f : ((sg0.kind == 1) ? 16.5f : 11.0f);
    ag.cur_speed_mps = base_spd * ag.speed_scale;
    float ue0 = 0.0f, un0 = 0.0f; uint32_t d0 = 0;
    edge_dir(ag.route[ag.step], ue0, un0, d0);
    ag.cur_yaw = std::atan2(ue0, -un0);
}

void WorldObjects::set_anisotropy(int max_aniso) {
    float max_hw = 1.0f;
    glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &max_hw);
    const float a = std::clamp(static_cast<float>(max_aniso), 1.0f, max_hw);
    for (GLuint t : {bldg_alb_, bldg_nrm_, bldg_orm_, interior_tex_, asph_tex_, grav_tex_, asph_nrm_, grav_nrm_}) {
        if (t) glTextureParameterf(t, GL_TEXTURE_MAX_ANISOTROPY, a);
    }
}

void WorldObjects::update_and_render(
    const Mat4 &vp, const Frustum &frustum,
    double cam_east_m, double cam_north_m, double cam_alt_m,
    const AtmosphereState &atmo, float time_sec, const Vec2 &wind_en_mps, GLuint refl_env_tex, int quality,
    int max_new_tree_cells, int world_detail,
    float dist_scale, int road_quality
) {
    const int wq = (world_detail < 0) ? std::clamp(quality, 0, 3) : std::clamp(world_detail, 0, 4);
    const int base_rq = (road_quality < 0) ? wq : std::clamp(road_quality, 0, 4);
    const float ds = std::clamp(dist_scale, 0.65f, 1.15f);
    const float phase10 = sun_dir_to_phase10(atmo.sun_dir);
    if (mtn_gi_tex_ && std::abs(phase10 - mtn_gi_phase_) > 0.04f) {
        mtn_gi_phase_ = phase10;
        pack_->fill_corridor_gi_rg8(phase10, mtn_gi_cpu_);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTextureSubImage2D(mtn_gi_tex_, 0, 0, 0, pack_->corridor_dem_w(), pack_->corridor_dem_h(), GL_RG, GL_UNSIGNED_BYTE, mtn_gi_cpu_.data());
    }
    const float mtn_min_e = static_cast<float>(pack_->corridor_min_e());
    const float mtn_min_n = static_cast<float>(pack_->corridor_min_n());
    const float mtn_inv_e = 1.0f / static_cast<float>(std::max(1.0, pack_->corridor_span_e()));
    const float mtn_inv_n = 1.0f / static_cast<float>(std::max(1.0, pack_->corridor_span_n()));
    GLuint env_t = refl_env_tex ? refl_env_tex : asph_tex_;
    // 1. OurAirports Runways (GGX PBR Asphalt/Gravel + Normal Maps + Soft Organic Shoulder Blend!)
    if (rwy_vert_count_ > 0) {
        glUseProgram(rwy_prog_);
        glUniformMatrix4fv(u_rwy_vp_, 1, GL_FALSE, vp.m);
        glUniform3f(u_rwy_cam_, static_cast<float>(cam_east_m), static_cast<float>(cam_alt_m), static_cast<float>(cam_north_m));
        glUniform3f(u_rwy_sun_, atmo.sun_dir.x, atmo.sun_dir.y, atmo.sun_dir.z);
        glUniform3f(u_rwy_cool_, atmo.haze_cool.x, atmo.haze_cool.y, atmo.haze_cool.z);
        glUniform3f(u_rwy_warm_, atmo.haze_warm.x, atmo.haze_warm.y, atmo.haze_warm.z);
        glUniform1f(u_rwy_hd_, atmo.haze_density);
        glBindTextureUnit(0, asph_tex_);
        glBindTextureUnit(1, grav_tex_);
        glBindTextureUnit(2, asph_nrm_);
        glBindTextureUnit(3, grav_nrm_);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(1.5f, 4.0f); // Reverse-Z positive bias pulls runway slightly forward!
        glDisable(GL_CULL_FACE);
        glBindVertexArray(rwy_vao_);
        glDrawArrays(GL_TRIANGLES, 0, rwy_vert_count_);
        glEnable(GL_CULL_FACE);
        glDisable(GL_POLYGON_OFFSET_FILL);
        glDisable(GL_BLEND);
    }

    // 2. Real OpenStreetMap Streets, Avenues, Motorways & Sidewalks (streamed asynchronously around camera!)
    rebuild_local_roads_and_traffic(cam_east_m, cam_north_m, base_rq);
    if (road_vert_count_ > 0) {
        glUseProgram(road_prog_);
        glUniformMatrix4fv(u_road_vp_, 1, GL_FALSE, vp.m);
        glUniform3f(u_road_cam_, static_cast<float>(cam_east_m), static_cast<float>(cam_alt_m), static_cast<float>(cam_north_m));
        glUniform3f(u_road_sun_, atmo.sun_dir.x, atmo.sun_dir.y, atmo.sun_dir.z);
        glUniform3f(u_road_cool_, atmo.haze_cool.x, atmo.haze_cool.y, atmo.haze_cool.z);
        glUniform3f(u_road_warm_, atmo.haze_warm.x, atmo.haze_warm.y, atmo.haze_warm.z);
        glUniform1f(u_road_hd_, atmo.haze_density);
        glUniform1f(u_road_alt_, static_cast<float>(cam_alt_m));
        glUniform1f(u_road_ph_, phase10);
        glBindTextureUnit(0, asph_tex_);
        glBindTextureUnit(1, asph_nrm_);
        glBindTextureUnit(2, grav_tex_);
        glBindTextureUnit(3, grav_nrm_);
        glBindTextureUnit(4, env_t);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(2.0f, 6.0f);
        glDisable(GL_CULL_FACE);
        glBindVertexArray(road_vao_);
        glDrawArrays(GL_TRIANGLES, 0, road_vert_count_);
        glEnable(GL_CULL_FACE);
        glDisable(GL_POLYGON_OFFSET_FILL);
        glDisable(GL_BLEND);
    }

    // 3. Iconic Northern Spain Hero Landmarks (El Pilar, Puente de Piedra, Torre del Agua, Catedral Huesca, Loarre, Mediano!)
    if (lm_vert_count_ > 0) {
        glUseProgram(landmark_prog_);
        glUniformMatrix4fv(u_lm_vp_, 1, GL_FALSE, vp.m);
        glUniform3f(u_lm_cam_, static_cast<float>(cam_east_m), static_cast<float>(cam_alt_m), static_cast<float>(cam_north_m));
        glUniform3f(u_lm_sun_, atmo.sun_dir.x, atmo.sun_dir.y, atmo.sun_dir.z);
        glUniform3f(u_lm_cool_, atmo.haze_cool.x, atmo.haze_cool.y, atmo.haze_cool.z);
        glUniform3f(u_lm_warm_, atmo.haze_warm.x, atmo.haze_warm.y, atmo.haze_warm.z);
        glUniform1f(u_lm_hd_, atmo.haze_density);
        glUniform1f(u_lm_alt_, static_cast<float>(cam_alt_m));
        glBindVertexArray(lm_vao_);
        glDrawArrays(GL_TRIANGLES, 0, lm_vert_count_);
    }

    // 4. Live Moving 3D PBR Spanish Traffic (individual road-graph routes, car-following, right-hand lanes!)
    if (!agents_.empty() && car_idx_count_ > 0) {
        const float dt = (prev_traffic_t_ >= 0.0f) ? std::clamp(time_sec - prev_traffic_t_, 0.0f, 0.10f) : 0.016f;
        prev_traffic_t_ = time_sec;

        // Lightweight same-edge headway table so trailing vehicles brake behind slower traffic.
        thread_local std::unordered_map<uint32_t, std::pair<float, float>> lane_occ; // edge_code -> (dist_m, speed)
        lane_occ.clear();
        for (const TrafficAgent &ag : agents_) {
            if (ag.step < ag.route.size()) {
                uint32_t code = ag.route[ag.step];
                auto it = lane_occ.find(code);
                if (it == lane_occ.end() || ag.dist_m > it->second.first) lane_occ[code] = {ag.dist_m, ag.cur_speed_mps};
            }
        }

        for (size_t i = 0; i < agents_.size(); i++) {
            TrafficAgent &ag = agents_[i];
            if (ag.route.empty() || ag.step >= ag.route.size()) {
                plan_vehicle_route(ag, false);
                if (ag.route.empty()) continue;
            }
            uint32_t code = ag.route[ag.step];
            const RoadSeg &sg = active_segs_[code >> 1u];
            float sgn = (code & 1u) ? -1.0f : 1.0f;
            float base_spd = (sg.kind == 0) ? 28.0f : ((sg.kind == 1) ? 16.5f : 11.0f);
            float target_spd = base_spd * ag.speed_scale;

            // Brake smoothly before sharp intersection turns on the vehicle's planned route.
            if (static_cast<size_t>(ag.step) + 1 < ag.route.size() && (sg.len - ag.dist_m) < 22.0f) {
                uint32_t ncode = ag.route[ag.step + 1];
                const RoadSeg &nsg = active_segs_[ncode >> 1u];
                float nsgn = (ncode & 1u) ? -1.0f : 1.0f;
                float u1e = sgn * (sg.e1 - sg.e0) / std::max(0.01f, sg.len);
                float u1n = sgn * (sg.n1 - sg.n0) / std::max(0.01f, sg.len);
                float u2e = nsgn * (nsg.e1 - nsg.e0) / std::max(0.01f, nsg.len);
                float u2n = nsgn * (nsg.n1 - nsg.n0) / std::max(0.01f, nsg.len);
                float dot_turn = u1e * u2e + u1n * u2n;
                if (dot_turn < 0.72f) target_spd = std::min(target_spd, 6.5f);
            }
            // Same-edge headway braking so cars don't stack on top of each other.
            auto occ = lane_occ.find(code);
            if (occ != lane_occ.end() && occ->second.first > ag.dist_m + 1.0f && (occ->second.first - ag.dist_m) < 14.0f) {
                target_spd = std::min(target_spd, occ->second.second * 0.85f);
            }
            float accel = (target_spd > ag.cur_speed_mps) ? 3.5f : 7.0f;
            ag.cur_speed_mps += std::clamp(target_spd - ag.cur_speed_mps, -accel * dt, accel * dt);
            ag.dist_m += ag.cur_speed_mps * dt;

            while (ag.step < ag.route.size()) {
                const RoadSeg &csg = active_segs_[ag.route[ag.step] >> 1u];
                if (ag.dist_m < csg.len) break;
                ag.dist_m -= csg.len;
                ag.step++;
            }
            if (ag.step >= ag.route.size()) {
                // Reached individual destination -> dispatch a brand-new A->B trip!
                plan_vehicle_route(ag, false);
                if (ag.route.empty()) continue;
            }

            code = ag.route[ag.step];
            const RoadSeg &fsg = active_segs_[code >> 1u];
            sgn = (code & 1u) ? -1.0f : 1.0f;
            float p = std::clamp(ag.dist_m / std::max(0.5f, fsg.len), 0.0f, 1.0f);
            float t = (sgn > 0.0f) ? p : (1.0f - p);
            float de = fsg.e1 - fsg.e0, dn = fsg.n1 - fsg.n0;
            float invl = 1.0f / std::max(0.01f, fsg.len);
            float ue = de * invl, un = dn * invl;
            float loff = sgn * fsg.lane_off;
            float ce = fsg.e0 + de * t + loff * un;
            float cy = fsg.y0 + (fsg.y1 - fsg.y0) * t + 0.06f;
            float cn = fsg.n0 + dn * t - loff * ue;
            float target_yaw = std::atan2(sgn * ue, -sgn * un);
            float dyaw = target_yaw - ag.cur_yaw;
            while (dyaw >  3.14159265f) dyaw -= 6.2831853f;
            while (dyaw < -3.14159265f) dyaw += 6.2831853f;
            ag.cur_yaw += dyaw * std::min(1.0f, dt * 8.0f);

            CarGpu &cg = car_gpu_buf_[i];
            cg.pos_yaw[0] = ce; cg.pos_yaw[1] = cy; cg.pos_yaw[2] = cn; cg.pos_yaw[3] = ag.cur_yaw;
            cg.scale_col[0] = ag.w; cg.scale_col[1] = ag.h; cg.scale_col[2] = ag.l; cg.scale_col[3] = ag.col_id;
        }
        glNamedBufferSubData(car_ssbo_, 0, static_cast<GLsizeiptr>(car_gpu_buf_.size() * sizeof(CarGpu)), car_gpu_buf_.data());

        glUseProgram(traffic_prog_);
        glUniformMatrix4fv(u_car_vp_, 1, GL_FALSE, vp.m);
        glUniform3f(u_car_cam_, static_cast<float>(cam_east_m), static_cast<float>(cam_alt_m), static_cast<float>(cam_north_m));
        glUniform3f(u_car_sun_, atmo.sun_dir.x, atmo.sun_dir.y, atmo.sun_dir.z);
        glUniform3f(u_car_cool_, atmo.haze_cool.x, atmo.haze_cool.y, atmo.haze_cool.z);
        glUniform3f(u_car_warm_, atmo.haze_warm.x, atmo.haze_warm.y, atmo.haze_warm.z);
        glUniform1f(u_car_hd_, atmo.haze_density);
        glUniform1f(u_car_alt_, static_cast<float>(cam_alt_m));
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, car_ssbo_);
        glBindVertexArray(car_vao_);
        glDrawElementsInstanced(GL_TRIANGLES, car_idx_count_, GL_UNSIGNED_INT, nullptr, static_cast<GLsizei>(car_gpu_buf_.size()));
    }

    // 4b. Map-Wide Pre-Baked Exact-Polygon Populated Zones (streamed per region + 4km macro grid cull + Early-Z!)
    if (!city_regions_.empty()) {
        ensure_city_regions_for_camera(cam_east_m, cam_north_m);
        static constexpr float kCityMaxD[5] = {10500.0f, 15000.0f, 17500.0f, 22500.0f, 28000.0f};
        static constexpr float kCityNear[5] = {  450.0f,   680.0f,   720.0f,  2200.0f,  3500.0f};
        float max_city_d = kCityMaxD[wq] * ds;
        float near_cut   = kCityNear[wq] * std::clamp(ds, 0.80f, 1.05f);
        float max_d2 = max_city_d * max_city_d;
        float near_cut2 = near_cut * near_cut;

        struct VisChunk { uint32_t idx; float d2; };
        thread_local std::vector<VisChunk> near_ch, far_ch;
        thread_local std::vector<GLsizei> city_draw_counts;
        thread_local std::vector<const void *> city_draw_indices;
        thread_local std::vector<GLint> city_draw_bases;

        for (const CityRegionGpu &cr : city_regions_) {
            if (!cr.vao || cr.chunks.empty()) continue;
            near_ch.clear();
            far_ch.clear();
            double cam_le = (cam_east_m  - cr.off_e) / cr.scl_e;
            double cam_ln = (cam_north_m - cr.off_n) / cr.scl_n;
            int32_t mx0 = static_cast<int32_t>(std::floor((cam_le - max_city_d) / 4000.0));
            int32_t mx1 = static_cast<int32_t>(std::floor((cam_le + max_city_d) / 4000.0));
            int32_t my0 = static_cast<int32_t>(std::floor((cam_ln - max_city_d) / 4000.0));
            int32_t my1 = static_cast<int32_t>(std::floor((cam_ln + max_city_d) / 4000.0));
            for (int32_t my = my0; my <= my1; my++) {
                for (int32_t mx = mx0; mx <= mx1; mx++) {
                    auto it = cr.macro_grid.find(pack_tc(mx, my));
                    if (it == cr.macro_grid.end()) continue;
                    for (uint32_t i : it->second) {
                        const CityChunkInfo &ch = cr.chunks[i];
                        float we = ch.ce * cr.scl_e + cr.off_e;
                        float wn = ch.cn * cr.scl_n + cr.off_n;
                        float dx = we - static_cast<float>(cam_east_m);
                        float dn = wn - static_cast<float>(cam_north_m);
                        float d2 = dx * dx + dn * dn;
                        if (d2 > max_d2) continue;
                        float curv = d2 / (2.0f * 6378137.0f);
                        Vec3 ctr{dx, ch.cy - static_cast<float>(cam_alt_m) - curv, -dn};
                        bool vis = true;
                        for (int p = 0; p < 5; p++) {
                            if (dot({frustum.planes[p].x, frustum.planes[p].y, frustum.planes[p].z}, ctr) + frustum.planes[p].w < -ch.rad) {
                                vis = false;
                                break;
                            }
                        }
                        if (!vis) continue;
                        if (d2 <= near_cut2) near_ch.push_back({i, d2});
                        else far_ch.push_back({i, d2});
                    }
                }
            }
            if (near_ch.empty() && far_ch.empty()) continue;
            std::sort(near_ch.begin(), near_ch.end(), [](const VisChunk &a, const VisChunk &b) { return a.d2 < b.d2; });
            std::sort(far_ch.begin(), far_ch.end(), [](const VisChunk &a, const VisChunk &b) { return a.d2 < b.d2; });
            // Per-frame index budget keeps dense megacities (Barcelona) within iGPU frame time
            // while pulling far-fade smoothly to the farthest included chunk distance.
            static constexpr size_t kCityMaxIdx[5] = {950000u, 1700000u, 2200000u, 6000000u, 9000000u};
            const size_t max_city_idx = static_cast<size_t>(kCityMaxIdx[wq] * std::clamp(ds * ds, 0.50f, 1.15f));
            size_t used_idx = 0;
            for (const VisChunk &vc : near_ch) used_idx += cr.chunks[vc.idx].index_count;
            float eff_far_d = max_city_d;
            if (!far_ch.empty()) {
                size_t keep_far = 0;
                for (; keep_far < far_ch.size(); ++keep_far) {
                    const uint32_t cnt = cr.chunks[far_ch[keep_far].idx].index_count;
                    if (keep_far > 0 && used_idx + cnt > max_city_idx) break;
                    used_idx += cnt;
                }
                if (keep_far < far_ch.size()) {
                    far_ch.resize(keep_far);
                    if (!far_ch.empty()) {
                        eff_far_d = std::clamp(std::sqrt(far_ch.back().d2), near_cut + 1200.0f, max_city_d);
                    }
                }
            }
            glBindVertexArray(cr.vao);

            auto draw_chunk_batch = [&](const std::vector<VisChunk> &list) {
                if (list.empty()) return;
                city_draw_counts.clear();
                city_draw_indices.clear();
                city_draw_bases.clear();
                city_draw_counts.reserve(list.size());
                city_draw_indices.reserve(list.size());
                city_draw_bases.reserve(list.size());
                for (const VisChunk &vc : list) {
                    const CityChunkInfo &ch = cr.chunks[vc.idx];
                    city_draw_counts.push_back(static_cast<GLsizei>(ch.index_count));
                    city_draw_indices.push_back(reinterpret_cast<const void *>(static_cast<uintptr_t>(ch.first_index) * sizeof(uint32_t)));
                    city_draw_bases.push_back(ch.base_vertex);
                }
                // One multi-draw call replaces hundreds of per-chunk GL calls while
                // preserving the exact baked city meshes, sort order, and materials.
                glMultiDrawElementsBaseVertex(GL_TRIANGLES, city_draw_counts.data(), GL_UNSIGNED_INT,
                                              city_draw_indices.data(), static_cast<GLsizei>(city_draw_counts.size()),
                                              city_draw_bases.data());
            };

            if (!near_ch.empty()) {
                glUseProgram(city_prog_);
                glUniformMatrix4fv(u_city_vp_, 1, GL_FALSE, vp.m);
                glUniform3f(u_city_cam_, static_cast<float>(cam_east_m), static_cast<float>(cam_alt_m), static_cast<float>(cam_north_m));
                glUniform3f(u_city_sun_, atmo.sun_dir.x, atmo.sun_dir.y, atmo.sun_dir.z);
                glUniform3f(u_city_cool_, atmo.haze_cool.x, atmo.haze_cool.y, atmo.haze_cool.z);
                glUniform3f(u_city_warm_, atmo.haze_warm.x, atmo.haze_warm.y, atmo.haze_warm.z);
                glUniform1f(u_city_hd_, atmo.haze_density);
                glUniform1f(u_city_alt_, static_cast<float>(cam_alt_m));
                glUniform1f(u_city_ph_, phase10);
                glUniform4f(u_city_mb_, mtn_min_e, mtn_min_n, mtn_inv_e, mtn_inv_n);
                glUniform2f(u_city_roff_, cr.off_e, cr.off_n);
                glUniform2f(u_city_rscl_, cr.scl_e, cr.scl_n);
                glBindTextureUnit(0, bldg_alb_);
                glBindTextureUnit(1, bldg_nrm_);
                glBindTextureUnit(2, bldg_orm_);
                glBindTextureUnit(3, interior_tex_);
                glBindTextureUnit(4, mtn_gi_tex_);
                glBindTextureUnit(5, env_t);
                draw_chunk_batch(near_ch);
            }

            if (!far_ch.empty()) {
                glUseProgram(city_far_prog_);
                glUniformMatrix4fv(u_cfar_vp_, 1, GL_FALSE, vp.m);
                glUniform3f(u_cfar_cam_, static_cast<float>(cam_east_m), static_cast<float>(cam_alt_m), static_cast<float>(cam_north_m));
                glUniform3f(u_cfar_sun_, atmo.sun_dir.x, atmo.sun_dir.y, atmo.sun_dir.z);
                glUniform3f(u_cfar_cool_, atmo.haze_cool.x, atmo.haze_cool.y, atmo.haze_cool.z);
                glUniform3f(u_cfar_warm_, atmo.haze_warm.x, atmo.haze_warm.y, atmo.haze_warm.z);
                glUniform1f(u_cfar_hd_, atmo.haze_density);
                glUniform1f(u_cfar_alt_, static_cast<float>(cam_alt_m));
                glUniform1f(u_cfar_fs_, eff_far_d * 0.80f);
                glUniform1f(u_cfar_fe_, eff_far_d);
                glUniform1f(u_cfar_ph_, phase10);
                glUniform4f(u_cfar_mb_, mtn_min_e, mtn_min_n, mtn_inv_e, mtn_inv_n);
                glUniform2f(u_cfar_roff_, cr.off_e, cr.off_n);
                glUniform2f(u_cfar_rscl_, cr.scl_e, cr.scl_n);
                glBindTextureUnit(4, mtn_gi_tex_);
                draw_chunk_batch(far_ch);
            }
        }
    }

    // 5. GPU-Driven 2-Tier Building LOD (Rural Towns & Villages 0..22km!) + Directional Ground Shadows & Contact AO!
    if (total_bldgs_ > 0) {
        static constexpr float kBldgRad[5] = {2400.0f, 3300.0f, 2800.0f, 5800.0f, 7500.0f};
        float b_rad = kBldgRad[wq] * ds;
        float far_rad = wq >= 4 ? 26000.0f : 22000.0f;
        glNamedBufferSubData(bldg_cmd_ssbo_, 0, sizeof(bldg_base_cmds_), bldg_base_cmds_);
        glNamedBufferSubData(bldg_sh_cmd_ssbo_, 0, sizeof(bldg_sh_base_cmds_), bldg_sh_base_cmds_);
        glNamedBufferSubData(bldg_lod1_cmd_ssbo_, 0, sizeof(bldg_lod1_base_cmd_), &bldg_lod1_base_cmd_);

        glUseProgram(cull_comp_);
        glUniform1ui(u_cull_tot_, static_cast<GLuint>(total_bldgs_));
        glUniform1ui(u_cull_max_, static_cast<GLuint>(MAX_PER_ARCH));
        glUniform1ui(u_cull_max_l1_, static_cast<GLuint>(MAX_LOD1));
        glUniform3f(u_cull_cam_, static_cast<float>(cam_east_m), static_cast<float>(cam_alt_m), static_cast<float>(cam_north_m));
        glUniform1f(u_cull_dist_, b_rad);
        glUniform1f(u_cull_far_, far_rad);
        float fp[20];
        for (int i = 0; i < 5; i++) {
            fp[i * 4 + 0] = frustum.planes[i].x; fp[i * 4 + 1] = frustum.planes[i].y;
            fp[i * 4 + 2] = frustum.planes[i].z; fp[i * 4 + 3] = frustum.planes[i].w;
        }
        glUniform4fv(u_cull_frust_, 5, fp);

        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, bldg_all_ssbo_);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, bldg_cmd_ssbo_);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, bldg_vis_ssbo_);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, bldg_sh_cmd_ssbo_);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, bldg_lod1_cmd_ssbo_);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, bldg_lod1_vis_ssbo_);
        glDispatchCompute(static_cast<GLuint>((total_bldgs_ + 63) / 64), 1, 1);
        glMemoryBarrier(GL_COMMAND_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);

        // 5a. 1-Call GPU-Instanced Directional Cast Sun Shadows + Soft Contact AO Pads (grounds every building onto streets!)
        glUseProgram(bldg_sh_prog_);
        glUniformMatrix4fv(u_bsh_vp_, 1, GL_FALSE, vp.m);
        glUniform3f(u_bsh_cam_, static_cast<float>(cam_east_m), static_cast<float>(cam_alt_m), static_cast<float>(cam_north_m));
        glUniform3f(u_bsh_sun_, atmo.sun_dir.x, atmo.sun_dir.y, atmo.sun_dir.z);
        glUniform1f(u_bsh_fs_, b_rad * 0.55f);
        glUniform1f(u_bsh_fe_, b_rad * 0.85f);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);
        glDisable(GL_CULL_FACE);
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(3.5f, 10.0f);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, bldg_all_ssbo_);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, bldg_vis_ssbo_);
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, bldg_sh_cmd_ssbo_);
        glBindVertexArray(bldg_sh_vao_);
        glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT, nullptr, 6, 0);
        glDisable(GL_POLYGON_OFFSET_FILL);
        glEnable(GL_CULL_FACE);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);

        // 5b. Tier-0 Full 3D Spanish Architectural Buildings (0 .. b_rad, cross-fades cleanly into LOD1!)
        glUseProgram(bldg_prog_);
        glUniformMatrix4fv(u_bldg_vp_, 1, GL_FALSE, vp.m);
        glUniform3f(u_bldg_cam_, static_cast<float>(cam_east_m), static_cast<float>(cam_alt_m), static_cast<float>(cam_north_m));
        glUniform3f(u_bldg_sun_, atmo.sun_dir.x, atmo.sun_dir.y, atmo.sun_dir.z);
        glUniform3f(u_bldg_cool_, atmo.haze_cool.x, atmo.haze_cool.y, atmo.haze_cool.z);
        glUniform3f(u_bldg_warm_, atmo.haze_warm.x, atmo.haze_warm.y, atmo.haze_warm.z);
        glUniform1f(u_bldg_hd_, atmo.haze_density);
        glUniform1f(u_bldg_alt_, static_cast<float>(cam_alt_m));
        glUniform1f(u_bldg_fs_, b_rad * 0.80f);
        glUniform1f(u_bldg_fe_, b_rad);

        glBindTextureUnit(0, bldg_alb_);
        glBindTextureUnit(1, bldg_nrm_);
        glBindTextureUnit(2, bldg_orm_);
        glBindTextureUnit(3, interior_tex_);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, bldg_all_ssbo_);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, bldg_vis_ssbo_);
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, bldg_cmd_ssbo_);
        glBindVertexArray(bldg_vao_);
        glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT, nullptr, 6, 0);

        // 5c. Tier-1 Ultra-Fast 10-Tri Low-LOD Far City Blocks (b_rad*0.80 .. 22km -> ZERO disappearing cities!)
        glUseProgram(bldg_lod1_prog_);
        glUniformMatrix4fv(u_bl1_vp_, 1, GL_FALSE, vp.m);
        glUniform3f(u_bl1_cam_, static_cast<float>(cam_east_m), static_cast<float>(cam_alt_m), static_cast<float>(cam_north_m));
        glUniform3f(u_bl1_sun_, atmo.sun_dir.x, atmo.sun_dir.y, atmo.sun_dir.z);
        glUniform3f(u_bl1_cool_, atmo.haze_cool.x, atmo.haze_cool.y, atmo.haze_cool.z);
        glUniform3f(u_bl1_warm_, atmo.haze_warm.x, atmo.haze_warm.y, atmo.haze_warm.z);
        glUniform1f(u_bl1_hd_, atmo.haze_density);
        glUniform1f(u_bl1_alt_, static_cast<float>(cam_alt_m));
        glUniform1f(u_bl1_is_, b_rad * 0.80f);
        glUniform1f(u_bl1_ie_, b_rad);
        glUniform1f(u_bl1_fs_, far_rad * 0.78f);
        glUniform1f(u_bl1_fe_, far_rad);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, bldg_all_ssbo_);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, bldg_lod1_vis_ssbo_);
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, bldg_lod1_cmd_ssbo_);
        glBindVertexArray(bldg_lod1_vao_);
        glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT, nullptr, 1, 0);
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
    }

    glBindVertexArray(0);
}

} // namespace nsim
