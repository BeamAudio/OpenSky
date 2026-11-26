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

// Zero-dependency procedural aviation audio engine (Linux ALSA via dlopen libasound.so.2).
// Runs synthesis in a dedicated low-CPU thread (<0.3% of 1 core) so rendering/thermals stay cool.
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
        double phase_fire = 0.0;
        double phase_sub = 0.0;
        double phase_starter = 0.0;
        double phase_comp = 0.0;
        double phase_stall = 0.0;
        double phase_flap = 0.0;
        double phase_gyro = 0.0;
        double phase_step = 0.0;
        double phase_td = 0.0;

        float sm_rpm = 0.0f;
        float sm_thr = 0.0f;
        float sm_ias = 0.0f;
        float sm_wind = 0.0f;
        float sm_rain = 0.0f;
        float sm_gnd = 0.0f;
        float sm_gain = 0.0f;
        float sm_cabin = 1.0f;

        float lp_engine = 0.0f;
        float lp_wind1 = 0.0f;
        float lp_wind2 = 0.0f;
        float lp_rain = 0.0f;
        float lp_thunder = 0.0f;
        float lp_gnd = 0.0f;
        float hp_rain_prev = 0.0f;
        float hp_rain_out = 0.0f;

        float thunder_env = 0.0f;
        float prev_lightning = 0.0f;
        float td_env = 0.0f;
        float td_strength = 0.0f;
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
    std::thread thread_;
};

} // namespace nsim
