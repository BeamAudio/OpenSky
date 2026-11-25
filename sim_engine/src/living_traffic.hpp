#pragma once

#include "gl_loader.hpp"
#include "math3d.hpp"
#include "pack_reader.hpp"
#include "volumetric_clouds.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace nsim {

// Living Aerial + Maritime Traffic Simulator (iGPU-first, 100% individual point-to-point routes):
//  - Aerial: individual GA VFR & Commercial IFR flights between real PackReader airports
//    (verified ramp turnaround -> runway takeoff -> climb/bank -> terrain-cleared cruise ->
//     3 deg runway final approach -> rollout -> new destination), plus parked ramp GA planes!
//  - Maritime: individual port-to-port vessels (cargo/container ships, ferries, coastal yachts)
//    routed via Dijkstra over a real Mediterranean coastal + offshore shipping lane graph (zero loops!).
class LivingTraffic {
public:
    LivingTraffic() = default;
    ~LivingTraffic();

    bool init(const PackReader *pack, const std::string &shader_root);
    void on_reanchor();
    void shutdown();

    void update_and_render(
        const Mat4 &vp,
        double cam_east_m, double cam_north_m, double cam_alt_m,
        const AtmosphereState &atmo, float time_sec, int quality
    );

    int active_aircraft() const { return active_aircraft_; }
    int active_ships() const { return active_ships_; }

    // Headless deterministic self-check for `--selftest` (no GL context required).
    static bool run_selftest(const PackReader &pack);

private:
    enum class MeshKind : int { GA = 0, Airliner = 1, Cargo = 2, Ferry = 3, Boat = 4, Count = 5 };
    enum class AirPhase : uint8_t { Parked = 0, Takeoff, Climb, Cruise, Approach, Rollout };

    struct MeshRange {
        uint32_t first_index = 0;
        uint32_t index_count = 0;
        int32_t base_vertex = 0;
    };
    struct EntityGpu {
        float pos_yaw[4];   // e, alt, n, yaw
        float pr_scale[4];  // pitch, roll, scale, livery
        float lights[4];    // beacon_phase, strobe_on, landing_on, wake_speed01
    };

    struct AirAgent {
        bool is_airliner = false;
        AirPhase phase = AirPhase::Cruise;
        int orig_ap = -1;
        int dest_ap = -1;
        // Individual flight plan waypoints in lat/lon/alt_m
        double rwy_a_lat = 0.0, rwy_a_lon = 0.0, rwy_a_elev = 0.0;
        double rwy_b_lat = 0.0, rwy_b_lon = 0.0, rwy_b_elev = 0.0;
        double dest_a_lat = 0.0, dest_a_lon = 0.0, dest_a_elev = 0.0;
        double dest_b_lat = 0.0, dest_b_lon = 0.0, dest_b_elev = 0.0;
        double park_lat = 0.0, park_lon = 0.0, park_elev = 0.0, park_hdg_deg = 0.0;
        double app_lat = 0.0, app_lon = 0.0; // 4.5 km final approach fix aligned to destination runway
        double cruise_alt_m = 2200.0;
        double cruise_spd_mps = 60.0;
        // Live kinematic state (lat/lon/alt_m)
        double lat = 0.0, lon = 0.0, alt_m = 0.0;
        float speed_mps = 0.0f;
        float heading_rad = 0.0f;
        float pitch_rad = 0.0f;
        float roll_rad = 0.0f;
        float phase_timer = 0.0f;
        float livery = 0.0f;
        float beacon_phase = 0.0f;
        uint32_t rng = 1u;
    };

    struct SeaNode {
        const char *name = "";
        double lat = 0.0;
        double lon = 0.0;
        bool is_port = false;
        bool deep_water = true;
    };
    struct ShipAgent {
        MeshKind kind = MeshKind::Cargo;
        std::vector<int> route; // Individual Dijkstra port-to-port node sequence (zero loops!)
        int step = 0;
        double lat = 0.0, lon = 0.0, water_elev_m = 0.0;
        float speed_mps = 8.0f;
        float cruise_spd_mps = 8.0f;
        float heading_rad = 0.0f;
        float lane_offset_m = 140.0f; // Right-hand maritime traffic separation
        float dwell_sec = 0.0f;
        float scale = 1.0f;
        float livery = 0.0f;
        float beacon_phase = 0.0f;
        uint32_t rng = 1u;
    };

    struct ParkedStatic {
        double lat = 0.0, lon = 0.0, elev_m = 0.0;
        float heading_rad = 0.0f;
        float livery = 0.0f;
    };

    void build_meshes();
    void init_air_fleet();
    void plan_air_flight(AirAgent &a, bool stagger_phase);
    void step_air_agent(AirAgent &a, float dt);

    void init_sea_graph_and_fleet();
    bool plan_ship_route(ShipAgent &s, int forced_orig, bool stagger_progress);
    void step_ship_agent(ShipAgent &s, float dt);
    void refresh_nearby_parked_ga(double cam_lat, double cam_lon);

    const PackReader *pack_ = nullptr;
    GLuint prog_ = 0;
    GLuint vao_ = 0, vbo_ = 0, ebo_ = 0, ssbo_ = 0;
    MeshRange ranges_[static_cast<int>(MeshKind::Count)]{};

    GLint u_vp_ = -1, u_cam_ = -1, u_base_ = -1, u_sun_ = -1, u_cool_ = -1, u_warm_ = -1, u_hd_ = -1, u_time_ = -1;

    std::vector<AirAgent> air_agents_;
    std::vector<SeaNode> sea_nodes_;
    std::vector<std::vector<std::pair<int, float>>> sea_adj_;
    std::vector<int> active_ports_;
    std::vector<int> deep_ports_;
    std::vector<ShipAgent> ship_agents_;

    std::vector<ParkedStatic> ramp_statics_;
    double ramp_center_lat_ = 1e9, ramp_center_lon_ = 1e9;

    std::vector<EntityGpu> gpu_batches_[static_cast<int>(MeshKind::Count)];
    std::vector<EntityGpu> gpu_flat_;
    float prev_t_ = -1.0f;
    int active_aircraft_ = 0;
    int active_ships_ = 0;
    static constexpr int MAX_ENTITIES = 256;
};

} // namespace nsim
