#pragma once

#include "gl_loader.hpp"
#include "math3d.hpp"
#include "pack_reader.hpp"
#include "volumetric_clouds.hpp"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace nsim {

// Volumetric ray-cast ground-level vegetation (ZERO 3D tree meshes: 1 proxy quad / 2 tris per tree!).
// Renders interlocking multi-lobe 3D crowns + HQ botanical leaf clusters + 3D bark trunks with
// exact Reverse-Z depth writing and Hi-Z conservative depth rejection for <80 C iGPU efficiency.
class AirfieldVegetation {
public:
    ~AirfieldVegetation();
    bool init(const std::string &assets, const std::string &shaders);
    void prepare(const PackReader &pack, const AirportInfo &airport);
    void update_around(const PackReader &pack, double lat_deg, double lon_deg, float agl);
    void invalidate();
    void draw(const Mat4 &vp, Vec3 camera_enu, const AtmosphereState &atmo,
              float agl, float time_sec, Vec2 wind_en, int season, float snowline_m,
              int veg_detail = 2, float density_scale = 1.0f);
    int instance_count() const { return count_; }

private:
    struct TreeVolGpu {
        float east, ground_y, north, height;
        float pnoa_r, pnoa_g, pnoa_b, species_seed;
    };

    void prepare_at(const PackReader &pack, const PackReader::WorldFrame &frame,
                    double center_lat, double center_lon, const char *label,
                    std::vector<TreeVolGpu> *out_upload = nullptr, uint64_t expected_gen = 0);
    void worker_loop();
    GLuint load_tex2d(const std::string &path, bool srgb);

    static constexpr int max_instances = 2800;
    GLuint prog_ = 0;
    GLuint vao_ = 0;
    GLuint ssbo_ = 0;
    GLuint leaf_albedo_ = 0;
    GLuint leaf_normal_ = 0;
    GLuint bark_albedo_ = 0;

    GLint u_vp_ = -1, u_cam_ = -1, u_agl_ = -1, u_time_ = -1, u_wind_ = -1;
    GLint u_sun_ = -1, u_cool_ = -1, u_warm_ = -1, u_haze_ = -1;
    GLint u_season_ = -1, u_snowline_ = -1;

    Vec2 centre_{};
    Vec2 req_centre_{};
    int count_ = 0;
    bool prepared_ = false;
    bool req_pending_coords_ = false;

    const PackReader *worker_pack_ = nullptr;
    std::thread worker_;
    std::mutex mtx_;
    std::condition_variable cv_;
    bool stop_ = false;
    bool job_queued_ = false;
    bool job_running_ = false;
    double job_lat_ = 0.0, job_lon_ = 0.0;
    uint64_t job_gen_ = 0;
    std::atomic<uint64_t> generation_{1};
    bool ready_upload_ = false;
    Vec2 ready_centre_{};
    std::vector<TreeVolGpu> ready_data_;
};

} // namespace nsim
