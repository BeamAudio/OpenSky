#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace nsim {

struct AudioState {
    float rpm = 0.0f;
    float throttle = 0.0f;
    float ias_kt = 0.0f;
    float ground_speed_mps = 0.0f;
    float ground_roughness = 0.05f;
    float wind_speed_mps = 0.0f;
    float turb_rms = 0.0f;
    float precip_mm_hr = 0.0f;
    float snow_factor = 0.0f;
    float lightning_flash = 0.0f;
    float brake_amount = 0.0f;
    float walk_speed_mps = 0.0f;
    float aircraft_dist_m = 0.0f;
    float aircraft_pan = 0.0f; // -1 left .. +1 right
    float aircraft_front = 1.0f; // -1 behind listener .. +1 directly ahead
    float aircraft_elev = 0.0f; // -1 below .. +1 above listener
    float aircraft_rel_speed_mps = 0.0f; // + approaching listener, - receding (Doppler)
    float wind_pan = 0.0f; // -1 crosswind from left .. +1 from right
    float wheel_bias = 0.0f; // -1 left main gear load .. +1 right main gear load
    float cockpit_stall_pan = -0.08f; // Head-tracked cockpit reed L/R pan
    float cockpit_avionics_pan = 0.36f; // Head-tracked center/right stack L/R pan
    float cockpit_flaps_pan = 0.14f; // Head-tracked overhead flap motor L/R pan
    float traffic_dist_m = 5000.0f; // Nearest AI traffic distance (m)
    float traffic_pan = 0.0f; // -1 left .. +1 right
    float traffic_front = 1.0f; // -1 behind .. +1 ahead
    float traffic_rel_speed_mps = 0.0f; // + approaching, - receding
    float traffic_rpm = 0.0f; // >0 when nearby AI aircraft is running
    bool traffic_is_airliner = false;
    float master_volume = 0.72f;
    bool engine_running = false;
    bool starter_cranking = false;
    bool sputtering = false;
    bool on_ground = true;
    bool stall_warn = false;
    bool flaps_moving = false;
    bool avionics_powered = false;
    bool in_cockpit = true;
    bool walking = false;
    bool paused = true;
    bool muted = false;
    uint32_t touchdown_seq = 0; // Increment on touchdown to trigger thump/chirp.
    float touchdown_sink_mps = 0.0f;
};

// Zero-dependency procedural aviation audio engine (Linux ALSA via dlopen libasound.so.2, or Windows WinMM waveOut).
// Runs synthesis in a dedicated low-CPU thread (<0.4% of 1 core) with binaural ITD/ILD/pinna spatialization.
class AudioEngine {
public:
    static constexpr unsigned int kSampleRate = 48000;
    static constexpr unsigned int kPeriodFrames = 512;

    AudioEngine() = default;
    ~AudioEngine();

    bool init(bool enable_output = true);
    void shutdown();

    void update_state(const AudioState &state);
    void set_muted(bool muted);
    bool is_muted() const { return muted_.load(std::memory_order_relaxed); }
    bool is_active() const { return active_.load(std::memory_order_relaxed); }

    // Offline deterministic DSP self-check (used by --selftest; requires no audio hardware).
    static bool run_selftest();
    // Render a timestamp-aligned procedural audio track for scripted promo captures.
    static bool write_wav(const std::string &path, const std::vector<AudioState> &timeline,
                          double frame_seconds);

private:
    struct SynthState {
        static constexpr unsigned int kItdBufSize = 64;   // 1.33 ms @ 48 kHz (> 0.66 ms max head ITD)
        static constexpr unsigned int kCabinBufSize = 256; // 5.3 ms early cockpit reflection ring

        double phase_fire = 0.0;
        double phase_sub = 0.0;
        double phase_starter = 0.0;
        double phase_comp = 0.0;
        double phase_stall = 0.0;
        double phase_flap = 0.0;
        double phase_gyro = 0.0;
        double phase_step = 0.0;
        double phase_td = 0.0;
        double phase_tr_fire = 0.0;
        double phase_tr_sub = 0.0;

        float sm_rpm = 0.0f;
        float sm_thr = 0.0f;
        float sm_ias = 0.0f;
        float sm_wind = 0.0f;
        float sm_rain = 0.0f;
        float sm_gnd = 0.0f;
        float sm_gain = 0.0f;
        float sm_cabin = 1.0f;
        float sm_pan = 0.0f;
        float sm_front = 1.0f;
        float sm_elev = 0.0f;
        float sm_wind_pan = 0.0f;
        float sm_doppler = 1.0f;
        float sm_tr_pan = 0.0f;
        float sm_tr_front = 1.0f;
        float sm_tr_gain = 0.0f;
        float sm_tr_doppler = 1.0f;

        float lp_engine = 0.0f;
        float lp_eng_air = 0.0f;
        float lp_eng_front = 0.0f;
        float lp_eng_shadow_l = 0.0f;
        float lp_eng_shadow_r = 0.0f;
        float lp_cabin_refl = 0.0f;
        float lp_tr_eng = 0.0f;
        float lp_tr_front = 0.0f;
        float lp_tr_shadow_l = 0.0f;
        float lp_tr_shadow_r = 0.0f;
        float lp_wind_l1 = 0.0f;
        float lp_wind_r1 = 0.0f;
        float lp_wind_l2 = 0.0f;
        float lp_wind_r2 = 0.0f;
        float lp_rain_l = 0.0f;
        float lp_rain_r = 0.0f;
        float lp_thunder = 0.0f;
        float lp_thunder_r = 0.0f;
        float lp_gnd = 0.0f;
        float hp_rain_prev_l = 0.0f;
        float hp_rain_out_l = 0.0f;
        float hp_rain_prev_r = 0.0f;
        float hp_rain_out_r = 0.0f;

        std::array<float, kItdBufSize> eng_delay_{};
        std::array<float, kItdBufSize> tr_delay_{};
        std::array<float, kCabinBufSize> cabin_refl_{};
        unsigned int eng_delay_idx_ = 0;
        unsigned int tr_delay_idx_ = 0;
        unsigned int cabin_refl_idx_ = 0;

        float thunder_env = 0.0f;
        float thunder_pan = 0.0f;
        float prev_lightning = 0.0f;
        float td_env = 0.0f;
        float td_strength = 0.0f;
        float td_bias = 0.0f;
        uint32_t last_td_seq = 0;
        float step_env = 0.0f;
        float sputter_gain = 1.0f;
        float sputter_timer = 0.0f;
        uint32_t rng = 0x51A7C172u;

        float next_noise();
        void render_stereo(const AudioState &st, int16_t *out_interleaved, unsigned int frames);
    };

    void audio_thread_main();
    bool open_alsa();
    void close_alsa();

    // Lock-free double-buffered state handoff (main thread writes inactive slot & flips index).
    std::array<AudioState, 2> slots_{};
    std::atomic<int> read_slot_{0};
    std::atomic<bool> running_{false};
    std::atomic<bool> active_{false};
    std::atomic<bool> muted_{false};

    void *alsa_lib_ = nullptr;
    void *pcm_ = nullptr;
    void *win_event_ = nullptr;
    std::thread thread_;
};

} // namespace nsim
