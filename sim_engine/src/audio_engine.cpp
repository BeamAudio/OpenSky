#include "audio_engine.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>

#ifndef _WIN32
#include <dlfcn.h>
#endif

namespace nsim {
namespace {

constexpr double kTwoPi = 6.28318530717958647692;
constexpr float kInvSr = 1.0f / static_cast<float>(AudioEngine::kSampleRate);

// ALSA ABI constants from <alsa/pcm.h> (loaded dynamically via libasound.so.2).
constexpr int kSndPcmStreamPlayback = 0;
constexpr int kSndPcmFormatS16Le = 2;
constexpr int kSndPcmAccessRwInterleaved = 3;

struct AlsaApi {
    int (*open)(void **, const char *, int, int) = nullptr;
    int (*set_params)(void *, int, int, unsigned int, unsigned int, int, unsigned int) = nullptr;
    long (*writei)(void *, const void *, unsigned long) = nullptr;
    int (*recover)(void *, int, int) = nullptr;
    int (*drop)(void *) = nullptr;
    int (*close)(void *) = nullptr;

    bool load(void *lib) {
        if (!lib) return false;
#ifndef _WIN32
        open = reinterpret_cast<decltype(open)>(dlsym(lib, "snd_pcm_open"));
        set_params = reinterpret_cast<decltype(set_params)>(dlsym(lib, "snd_pcm_set_params"));
        writei = reinterpret_cast<decltype(writei)>(dlsym(lib, "snd_pcm_writei"));
        recover = reinterpret_cast<decltype(recover)>(dlsym(lib, "snd_pcm_recover"));
        drop = reinterpret_cast<decltype(drop)>(dlsym(lib, "snd_pcm_drop"));
        close = reinterpret_cast<decltype(close)>(dlsym(lib, "snd_pcm_close"));
#endif
        return open && set_params && writei && recover && drop && close;
    }
};

AlsaApi g_alsa;

inline float one_pole_alpha(float cutoff_hz) {
    const float w = 6.2831853f * std::clamp(cutoff_hz, 5.0f, 18000.0f) * kInvSr;
    return w / (1.0f + w);
}

inline double wrap_phase(double p) {
    return p - std::floor(p);
}

} // namespace

float AudioEngine::SynthState::next_noise() {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return static_cast<float>(static_cast<int32_t>(rng)) * (1.0f / 2147483648.0f);
}

void AudioEngine::SynthState::render_stereo(const AudioState &st, int16_t *out, unsigned int frames) {
    const bool audible = !st.muted && !st.paused;
    const float target_gain = audible ? std::clamp(st.master_volume, 0.0f, 1.0f) : 0.0f;
    const float target_rpm = st.engine_running ? std::clamp(st.rpm, 350.0f, 3200.0f)
                                               : (st.starter_cranking ? 190.0f : 0.0f);
    const float target_thr = std::clamp(st.throttle, 0.0f, 1.0f);
    const float target_ias = std::clamp(st.ias_kt, 0.0f, 220.0f);
    const float target_wind = std::clamp(st.wind_speed_mps, 0.0f, 40.0f);
    const float rain_wet = std::clamp(st.precip_mm_hr / 18.0f, 0.0f, 1.25f) *
                           std::clamp(1.0f - st.snow_factor * 0.78f, 0.12f, 1.0f);
    const float target_gnd = (st.on_ground && !st.walking)
        ? std::clamp(st.ground_speed_mps / 30.0f, 0.0f, 1.4f) : 0.0f;
    const float target_cabin = (st.in_cockpit && !st.walking) ? 1.0f : 0.0f;

    // Trigger one-shot transients on rising edges.
    if (st.touchdown_seq != last_td_seq) {
        last_td_seq = st.touchdown_seq;
        td_env = 1.0f;
        td_strength = std::clamp(st.touchdown_sink_mps / 3.2f, 0.25f, 1.2f);
        phase_td = 0.0;
    }
    if (st.lightning_flash > 0.25f && prev_lightning <= 0.25f) {
        thunder_env = std::max(thunder_env, std::clamp(st.lightning_flash, 0.45f, 1.0f));
    }
    prev_lightning = st.lightning_flash;

    // Spatial attenuation & stereo pan for engine/prop relative to camera.
    const float dist = std::max(0.0f, st.aircraft_dist_m);
    const float ac_atten = (st.in_cockpit && !st.walking) ? 1.0f : (1.0f / (1.0f + dist * 0.065f));
    const float pan = (st.in_cockpit && !st.walking) ? 0.0f : std::clamp(st.aircraft_pan, -1.0f, 1.0f);
    const float pan_l = std::sqrt(0.5f * (1.0f - pan));
    const float pan_r = std::sqrt(0.5f * (1.0f + pan));

    const float turb = std::clamp(st.turb_rms, 0.0f, 1.5f);
    const float rough = std::clamp(st.ground_roughness, 0.02f, 0.6f);
    const float brake = (st.on_ground && !st.walking && st.ground_speed_mps > 1.2f)
        ? std::clamp(st.brake_amount, 0.0f, 1.0f) : 0.0f;
    const float walk_spd = st.walking ? std::clamp(st.walk_speed_mps, 0.0f, 6.0f) : 0.0f;

    const float slew = 1.0f - std::exp(-kInvSr * 18.0f);
    const float gain_slew = 1.0f - std::exp(-kInvSr * 35.0f);

    for (unsigned int i = 0; i < frames; ++i) {
        sm_rpm += (target_rpm - sm_rpm) * slew;
        sm_thr += (target_thr - sm_thr) * slew;
        sm_ias += (target_ias - sm_ias) * slew;
        sm_wind += (target_wind - sm_wind) * slew;
        sm_rain += (rain_wet - sm_rain) * slew;
        sm_gnd += (target_gnd - sm_gnd) * slew;
        sm_gain += (target_gain - sm_gain) * gain_slew;
        sm_cabin += (target_cabin - sm_cabin) * slew;

        const float w_noise = next_noise();

        // 1. Lycoming IO-360-L2A 4-cylinder 4-stroke engine + 2-blade McCauley propeller.
        float eng = 0.0f;
        if (sm_rpm > 20.0f) {
            if (st.sputtering) {
                sputter_timer -= kInvSr;
                if (sputter_timer <= 0.0f) {
                    sputter_timer = 0.045f + 0.09f * std::abs(w_noise);
                    sputter_gain = (w_noise > -0.15f) ? 1.0f : 0.12f;
                }
            } else {
                sputter_gain += (1.0f - sputter_gain) * (kInvSr * 25.0f);
            }

            // 4-stroke 4-cyl fires twice per crankshaft rev (same fundamental as 2-blade prop!).
            const double f_fire = static_cast<double>(sm_rpm) * (2.0 / 60.0);
            phase_fire = wrap_phase(phase_fire + f_fire * kInvSr);
            phase_sub = wrap_phase(phase_sub + 0.5 * f_fire * kInvSr);

            const double p1 = phase_fire * kTwoPi;
            const double ps = phase_sub * kTwoPi;
            const float load = 0.35f + 0.65f * sm_thr;
            const float rpm01 = std::clamp((sm_rpm - 500.0f) / 2100.0f, 0.0f, 1.2f);

            // Harmonic stack: crankshaft thump + exhaust firing harmonics + prop blade buzz.
            float raw = 0.26f * std::sin(ps)
                      + 0.44f * std::sin(p1)
                      + (0.28f + 0.14f * load) * std::sin(2.0 * p1 + 0.4)
                      + (0.16f + 0.10f * load) * std::sin(3.0 * p1 + 1.1)
                      + 0.10f * load * std::sin(4.0 * p1 + 0.2)
                      + 0.06f * rpm01 * std::sin(6.0 * p1);

            // Idle chug modulation & prop tip air-wash pulses.
            const float idle_chug = 1.0f + (1.0f - std::clamp(rpm01, 0.0f, 1.0f)) * 0.25f * static_cast<float>(std::sin(ps));
            const float prop_chop = (0.04f + 0.16f * rpm01 * load) * (0.55f + 0.45f * static_cast<float>(std::sin(p1))) * w_noise;
            raw = std::tanh(raw * (1.25f + 0.85f * load)) * idle_chug + prop_chop;

            // Warm cabin low-pass (~980 Hz inside) vs crisp open-air exhaust (~5200 Hz outside).
            const float cutoff = 5200.0f - sm_cabin * (4220.0f - 480.0f * rpm01);
            lp_engine += (raw - lp_engine) * one_pole_alpha(cutoff);
            const float eng_amp = (0.14f + 0.28f * rpm01 + 0.10f * load) * sputter_gain;
            eng = lp_engine * eng_amp;
        }

        // 2. 24V Electric Starter Motor cranking + cylinder compression pulses.
        if (st.starter_cranking && !st.engine_running) {
            phase_starter = wrap_phase(phase_starter + 410.0 * kInvSr);
            phase_comp = wrap_phase(phase_comp + 3.4 * kInvSr);
            const float comp = 0.65f + 0.35f * static_cast<float>(std::sin(phase_comp * kTwoPi));
            const float whine = 0.60f * static_cast<float>(std::sin(phase_starter * kTwoPi))
                              + 0.35f * static_cast<float>(std::sin(phase_starter * 2.0 * kTwoPi))
                              + 0.12f * w_noise;
            eng += whine * comp * 0.16f;
        }

        // 3. Airframe slipstream, cabin wind & outdoor valley breeze.
        lp_wind1 += (w_noise - lp_wind1) * one_pole_alpha(480.0f + sm_ias * 9.0f);
        lp_wind2 += (lp_wind1 - lp_wind2) * one_pole_alpha(190.0f + sm_ias * 4.0f);
        const float ias_norm = std::clamp((sm_ias - 24.0f) / 110.0f, 0.0f, 1.5f);
        const float gust_mod = 1.0f + turb * 0.38f * std::clamp(lp_wind2 * 2.4f, -0.5f, 0.8f);
        float wind_sig = (lp_wind1 * 0.65f + lp_wind2 * 0.85f) * (ias_norm * ias_norm * 0.24f) * gust_mod;
        if (st.walking || !st.in_cockpit) {
            const float outdoor_w = std::clamp(sm_wind / 16.0f, 0.05f, 1.2f);
            wind_sig += (lp_wind1 * 0.55f + lp_wind2 * 0.70f) * outdoor_w * 0.14f;
        }

        // 4. Rain drumming on windscreen/cabin roof + thunder rumble.
        float wx_sig = 0.0f;
        if (sm_rain > 0.01f) {
            // High-passed hiss + randomized droplet taps.
            hp_rain_out = 0.86f * (hp_rain_out + w_noise - hp_rain_prev);
            hp_rain_prev = w_noise;
            lp_rain += (hp_rain_out - lp_rain) * one_pole_alpha(sm_cabin > 0.5f ? 2800.0f : 5200.0f);
            const float drop_hit = (std::abs(w_noise) > (0.988f - 0.014f * std::min(sm_rain, 1.0f)))
                ? (w_noise * 0.45f) : 0.0f;
            wx_sig += (lp_rain * 0.14f + drop_hit * (0.10f + 0.10f * sm_cabin)) * sm_rain;
        }
        if (thunder_env > 0.002f) {
            lp_thunder += (w_noise - lp_thunder) * one_pole_alpha(95.0f);
            wx_sig += std::tanh(lp_thunder * 4.5f) * thunder_env * 0.38f;
            thunder_env *= std::exp(-kInvSr * 0.55f);
        }

        // 5. Cockpit alerts, flap servo, avionics fan/gyro, ground roll, touchdown & footsteps.
        float cockpit_sig = 0.0f;
        if (st.stall_warn && !st.on_ground && sm_ias > 18.0f && st.in_cockpit && !st.walking) {
            phase_stall = wrap_phase(phase_stall + 2760.0 * kInvSr);
            const float reed = static_cast<float>(std::sin(phase_stall * kTwoPi))
                             + 0.42f * static_cast<float>(std::sin(phase_stall * 2.0 * kTwoPi))
                             + 0.18f * static_cast<float>(std::sin(phase_stall * 3.0 * kTwoPi));
            cockpit_sig += std::tanh(reed * 0.9f) * 0.13f;
        }
        if (st.flaps_moving && st.in_cockpit && !st.walking) {
            phase_flap = wrap_phase(phase_flap + 315.0 * kInvSr);
            cockpit_sig += (0.65f * static_cast<float>(std::sin(phase_flap * kTwoPi)) +
                            0.35f * static_cast<float>(std::sin(phase_flap * 3.0 * kTwoPi))) * 0.045f;
        }
        if (st.avionics_powered && st.in_cockpit && !st.walking) {
            phase_gyro = wrap_phase(phase_gyro + 1480.0 * kInvSr);
            cockpit_sig += (0.012f * static_cast<float>(std::sin(phase_gyro * kTwoPi)) + 0.006f * lp_wind1);
        }

        // Ground roll & braking rumble.
        if (sm_gnd > 0.015f) {
            lp_gnd += (w_noise - lp_gnd) * one_pole_alpha(85.0f + rough * 220.0f);
            cockpit_sig += lp_gnd * sm_gnd * (0.20f + rough * 0.45f);
            if (brake > 0.12f) {
                cockpit_sig += lp_wind1 * brake * sm_gnd * 0.09f;
            }
        }
        // Touchdown thump & brief tire chirp.
        if (td_env > 0.002f) {
            phase_td = wrap_phase(phase_td + (78.0 + 38.0 * td_env) * kInvSr);
            const float thump = static_cast<float>(std::sin(phase_td * kTwoPi)) * td_env * 0.42f * td_strength;
            const float chirp = (td_env > 0.65f) ? (lp_wind1 * (td_env - 0.65f) * 0.55f * td_strength) : 0.0f;
            cockpit_sig += thump + chirp;
            td_env *= std::exp(-kInvSr * 7.5f);
        }
        // Walkaround footsteps on grass/gravel.
        if (walk_spd > 0.35f) {
            const double prev_step = phase_step;
            phase_step = wrap_phase(phase_step + (1.55 + walk_spd * 0.22) * kInvSr);
            if (phase_step < prev_step) step_env = 1.0f;
        }
        if (step_env > 0.003f) {
            cockpit_sig += (lp_wind1 * 0.45f + lp_gnd * 0.55f) * step_env * 0.18f;
            step_env *= std::exp(-kInvSr * 18.0f);
        }

        // Stereo mix + soft limiting (prevents any harsh digital clipping).
        const float ac_mix = eng * ac_atten;
        const float mono_env = wind_sig + wx_sig + cockpit_sig;
        const float left = std::tanh((mono_env + ac_mix * pan_l * 1.4142f) * sm_gain);
        const float right = std::tanh((mono_env + ac_mix * pan_r * 1.4142f) * sm_gain);

        out[i * 2 + 0] = static_cast<int16_t>(std::clamp(left * 32000.0f, -32767.0f, 32767.0f));
        out[i * 2 + 1] = static_cast<int16_t>(std::clamp(right * 32000.0f, -32767.0f, 32767.0f));
    }
}

AudioEngine::~AudioEngine() {
    shutdown();
}

bool AudioEngine::open_alsa() {
#ifndef _WIN32
    alsa_lib_ = dlopen("libasound.so.2", RTLD_LAZY);
    if (!alsa_lib_ || !g_alsa.load(alsa_lib_)) {
        if (alsa_lib_) { dlclose(alsa_lib_); alsa_lib_ = nullptr; }
        return false;
    }
    const char *devices[] = {"default", "pipewire", "pulse", "plughw:0,0"};
    for (const char *dev : devices) {
        void *handle = nullptr;
        if (g_alsa.open(&handle, dev, kSndPcmStreamPlayback, 0) == 0 && handle) {
            // 48 kHz stereo S16_LE, ~42 ms target ring latency (2048 frames) for jitter-free iGPU playback.
            if (g_alsa.set_params(handle, kSndPcmFormatS16Le, kSndPcmAccessRwInterleaved,
                                  2, kSampleRate, 1, 42000) == 0) {
                pcm_ = handle;
                std::printf("[AUDIO] Procedural C172S audio engine active (%u Hz stereo via ALSA '%s')\n",
                            kSampleRate, dev);
                return true;
            }
            g_alsa.close(handle);
        }
    }
    dlclose(alsa_lib_);
    alsa_lib_ = nullptr;
#endif
    return false;
}

void AudioEngine::close_alsa() {
#ifndef _WIN32
    if (pcm_) {
        g_alsa.drop(pcm_);
        g_alsa.close(pcm_);
        pcm_ = nullptr;
    }
    if (alsa_lib_) {
        dlclose(alsa_lib_);
        alsa_lib_ = nullptr;
    }
#endif
}

bool AudioEngine::init(bool enable_output) {
    shutdown();
    if (!enable_output) return true;
    if (!open_alsa()) {
        std::printf("[AUDIO] No ALSA playback device available; running silently.\n");
        return true; // Non-fatal so headless / muted machines still fly.
    }
    running_.store(true, std::memory_order_release);
    active_.store(true, std::memory_order_release);
    thread_ = std::thread(&AudioEngine::audio_thread_main, this);
    return true;
}

void AudioEngine::shutdown() {
    running_.store(false, std::memory_order_release);
    active_.store(false, std::memory_order_release);
    if (thread_.joinable()) thread_.join();
    close_alsa();
}

void AudioEngine::update_state(const AudioState &state) {
    const int cur = read_slot_.load(std::memory_order_relaxed);
    const int next = 1 - cur;
    slots_[next] = state;
    slots_[next].muted = state.muted || muted_.load(std::memory_order_relaxed);
    read_slot_.store(next, std::memory_order_release);
}

void AudioEngine::set_muted(bool muted) {
    muted_.store(muted, std::memory_order_relaxed);
}

void AudioEngine::audio_thread_main() {
    SynthState synth;
    std::array<int16_t, kPeriodFrames * 2> pcm_buf{};
    while (running_.load(std::memory_order_acquire)) {
        const AudioState st = slots_[read_slot_.load(std::memory_order_acquire)];
        synth.render_stereo(st, pcm_buf.data(), kPeriodFrames);
        long written = g_alsa.writei(pcm_, pcm_buf.data(), kPeriodFrames);
        if (written < 0) {
            written = g_alsa.recover(pcm_, static_cast<int>(written), 1);
            if (written < 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(15));
            }
        }
    }
}

bool AudioEngine::run_selftest() {
    SynthState synth;
    std::array<int16_t, kPeriodFrames * 2> buf{};

    auto measure_rms = [&](const AudioState &st, int blocks) -> double {
        double sum_sq = 0.0;
        int count = 0;
        for (int b = 0; b < blocks; ++b) {
            synth.render_stereo(st, buf.data(), kPeriodFrames);
            for (int16_t s : buf) {
                const double v = static_cast<double>(s) / 32768.0;
                sum_sq += v * v;
                ++count;
            }
        }
        return std::sqrt(sum_sq / std::max(1, count));
    };

    AudioState idle{};
    idle.paused = false;
    idle.engine_running = true;
    idle.rpm = 750.0f;
    idle.throttle = 0.1f;
    const double rms_idle = measure_rms(idle, 8);

    AudioState takeoff = idle;
    takeoff.rpm = 2650.0f;
    takeoff.throttle = 1.0f;
    takeoff.ias_kt = 70.0f;
    takeoff.on_ground = false;
    takeoff.stall_warn = true;
    takeoff.precip_mm_hr = 12.0f;
    takeoff.lightning_flash = 0.8f;
    takeoff.touchdown_seq = 1;
    takeoff.touchdown_sink_mps = 2.0f;
    const double rms_takeoff = measure_rms(takeoff, 8);

    AudioState muted = takeoff;
    muted.muted = true;
    measure_rms(muted, 16); // Allow gain ramp to settle to zero.
    const double rms_muted = measure_rms(muted, 4);

    if (!(rms_idle > 0.01 && rms_takeoff > rms_idle && rms_takeoff < 0.99 && rms_muted < 0.002)) {
        std::fprintf(stderr, "[FAIL] AudioEngine selftest rms idle=%.4f takeoff=%.4f muted=%.4f\n",
                     rms_idle, rms_takeoff, rms_muted);
        return false;
    }
    return true;
}

bool AudioEngine::write_wav(const std::string &path, const std::vector<AudioState> &timeline,
                            double frame_seconds) {
    if (timeline.empty() || !std::isfinite(frame_seconds) || frame_seconds <= 0.0) return false;
    const uint64_t samples_per_frame = static_cast<uint64_t>(std::llround(frame_seconds * kSampleRate));
    const uint64_t sample_count = samples_per_frame * timeline.size();
    if (samples_per_frame == 0 || sample_count > (UINT32_MAX - 36u) / 4u) return false;
    const uint32_t data_bytes = static_cast<uint32_t>(sample_count * 4u);

    std::ofstream wav(path, std::ios::binary | std::ios::trunc);
    if (!wav) return false;
    auto u16 = [&](uint16_t value) {
        const char bytes[2] = {static_cast<char>(value & 0xffu), static_cast<char>((value >> 8u) & 0xffu)};
        wav.write(bytes, sizeof(bytes));
    };
    auto u32 = [&](uint32_t value) {
        const char bytes[4] = {static_cast<char>(value & 0xffu), static_cast<char>((value >> 8u) & 0xffu),
                               static_cast<char>((value >> 16u) & 0xffu), static_cast<char>((value >> 24u) & 0xffu)};
        wav.write(bytes, sizeof(bytes));
    };
    wav.write("RIFF", 4); u32(36u + data_bytes); wav.write("WAVEfmt ", 8);
    u32(16u); u16(1u); u16(2u); u32(kSampleRate); u32(kSampleRate * 4u); u16(4u); u16(16u);
    wav.write("data", 4); u32(data_bytes);

    SynthState synth;
    std::array<int16_t, kPeriodFrames * 2> block{};
    for (const AudioState &state : timeline) {
        uint64_t remaining = samples_per_frame;
        while (remaining > 0) {
            const unsigned int frames = static_cast<unsigned int>(std::min<uint64_t>(remaining, kPeriodFrames));
            synth.render_stereo(state, block.data(), frames);
            wav.write(reinterpret_cast<const char *>(block.data()),
                      static_cast<std::streamsize>(frames * 2u * sizeof(int16_t)));
            remaining -= frames;
        }
    }
    return static_cast<bool>(wav);
}

} // namespace nsim
