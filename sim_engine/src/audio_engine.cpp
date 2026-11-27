#include "audio_engine.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>
#else
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
    const float w = 6.2831853f * std::clamp(cutoff_hz, 5.0f, 20000.0f) * kInvSr;
    return w / (1.0f + w);
}

inline double wrap_phase(double p) {
    return p - std::floor(p);
}

template <size_t N>
inline float read_delay_linear(const std::array<float, N> &buf, unsigned int write_idx, float delay_samples) {
    constexpr unsigned int kMask = static_cast<unsigned int>(N - 1);
    const float clamped = std::clamp(delay_samples, 0.0f, static_cast<float>(N - 2));
    const unsigned int i0 = static_cast<unsigned int>(clamped);
    const float frac = clamped - static_cast<float>(i0);
    const float s0 = buf[(write_idx - i0) & kMask];
    const float s1 = buf[(write_idx - i0 - 1u) & kMask];
    return s0 + (s1 - s0) * frac;
}

// Binaural 3D spatializer: pinna front/back + elevation timbre -> fractional Woodworth ITD -> far-ear head-shadow ILD.
template <size_t N>
inline void spatialize_binaural(
    float mono_in,
    std::array<float, N> &delay_buf, unsigned int &delay_idx,
    float pan, float front, float elev, float depth01,
    float &lp_front, float &lp_shadow_l, float &lp_shadow_r,
    float &out_l, float &out_r
) {
    const float p = std::clamp(pan, -1.0f, 1.0f);
    const float f = std::clamp(front, -1.0f, 1.0f);
    const float e = std::clamp(elev, -1.0f, 1.0f);
    const float d = std::clamp(depth01, 0.0f, 1.0f);

    // Pinna front/back & overhead shelf: sounds behind head or sharply above/below lose upper crispness naturally.
    const float pinna_cut = std::clamp(8800.0f + f * 5800.0f - std::abs(e) * 2400.0f, 2600.0f, 16500.0f);
    lp_front += (mono_in - lp_front) * one_pole_alpha(pinna_cut);
    const float back_blend = d * std::clamp(0.28f + 0.62f * (0.5f - 0.5f * f) + 0.25f * std::abs(e), 0.0f, 0.92f);
    const float shaped = mono_in + (lp_front - mono_in) * back_blend;

    constexpr unsigned int kMask = static_cast<unsigned int>(N - 1);
    delay_idx = (delay_idx + 1u) & kMask;
    delay_buf[delay_idx] = shaped;

    // Fractional ITD up to ~0.60 ms (~28.8 samples @ 48 kHz), cosine-tapered at high elevation.
    const float elev_cos = 1.0f - 0.16f * std::abs(e);
    const float itd = 28.5f * p * d * elev_cos;
    const float dl = (itd > 0.0f) ? (1.0f + itd) : 1.0f;
    const float dr = (itd < 0.0f) ? (1.0f - itd) : 1.0f;
    const float ear_l = read_delay_linear(delay_buf, delay_idx, dl);
    const float ear_r = read_delay_linear(delay_buf, delay_idx, dr);

    // Head-shadow diffraction ILD: contralateral ear rolls off highs down to ~2.3 kHz.
    const float shadow_amt_l = std::clamp( std::max(0.0f,  p) * d, 0.0f, 1.0f);
    const float shadow_amt_r = std::clamp( std::max(0.0f, -p) * d, 0.0f, 1.0f);
    lp_shadow_l += (ear_l - lp_shadow_l) * one_pole_alpha(15500.0f - shadow_amt_l * 13200.0f);
    lp_shadow_r += (ear_r - lp_shadow_r) * one_pole_alpha(15500.0f - shadow_amt_r * 13200.0f);
    const float filt_l = ear_l + (lp_shadow_l - ear_l) * (shadow_amt_l * 0.85f);
    const float filt_r = ear_r + (lp_shadow_r - ear_r) * (shadow_amt_r * 0.85f);

    // Constant-power + ILD level asymmetry.
    const float pan_eff = p * d * 0.86f;
    const float gain_l = std::sqrt(0.5f * (1.0f - pan_eff)) * 1.41421356f;
    const float gain_r = std::sqrt(0.5f * (1.0f + pan_eff)) * 1.41421356f;
    out_l = filt_l * gain_l;
    out_r = filt_r * gain_r;
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
        thunder_pan = std::clamp(next_noise() * 0.78f, -0.78f, 0.78f);
    }
    prev_lightning = st.lightning_flash;

    // Spatial attenuation, Doppler shift, air absorption & binaural coordinates.
    const float dist = std::max(0.0f, st.aircraft_dist_m);
    const float ac_atten = (st.in_cockpit && !st.walking) ? 1.0f : (1.0f / (1.0f + dist * 0.055f));
    const float target_pan = std::clamp(st.aircraft_pan, -1.0f, 1.0f);
    const float target_front = std::clamp(st.aircraft_front, -1.0f, 1.0f);
    const float target_elev = std::clamp(st.aircraft_elev, -1.0f, 1.0f);
    const float target_wind_pan = std::clamp(st.wind_pan, -1.0f, 1.0f);
    const float target_doppler = (st.in_cockpit && !st.walking)
        ? 1.0f
        : (340.0f / std::clamp(340.0f - st.aircraft_rel_speed_mps, 235.0f, 445.0f));

    const bool traffic_audible = (st.traffic_rpm > 50.0f && st.traffic_dist_m < 2400.0f);
    const float target_tr_atten = traffic_audible
        ? ((st.in_cockpit && !st.walking ? 0.34f : 0.72f) / (1.0f + std::max(0.0f, st.traffic_dist_m) * 0.022f))
        : 0.0f;
    const float target_tr_pan = std::clamp(st.traffic_pan, -1.0f, 1.0f);
    const float target_tr_front = std::clamp(st.traffic_front, -1.0f, 1.0f);
    const float target_tr_doppler = 340.0f / std::clamp(340.0f - st.traffic_rel_speed_mps, 220.0f, 460.0f);

    const float stall_pan = std::clamp(st.cockpit_stall_pan, -1.0f, 1.0f);
    const float stall_l = std::sqrt(0.5f * (1.0f - stall_pan * 0.75f)) * 1.4142f;
    const float stall_r = std::sqrt(0.5f * (1.0f + stall_pan * 0.75f)) * 1.4142f;
    const float avn_pan = std::clamp(st.cockpit_avionics_pan, -1.0f, 1.0f);
    const float avn_l = std::sqrt(0.5f * (1.0f - avn_pan * 0.70f)) * 1.4142f;
    const float avn_r = std::sqrt(0.5f * (1.0f + avn_pan * 0.70f)) * 1.4142f;
    const float flap_pan = std::clamp(st.cockpit_flaps_pan, -1.0f, 1.0f);
    const float flap_l = std::sqrt(0.5f * (1.0f - flap_pan * 0.70f)) * 1.4142f;
    const float flap_r = std::sqrt(0.5f * (1.0f + flap_pan * 0.70f)) * 1.4142f;

    const float wb = std::clamp(st.wheel_bias, -0.65f, 0.65f);
    const float gnd_l = std::sqrt(0.5f * (1.0f - wb * 0.55f)) * 1.4142f;
    const float gnd_r = std::sqrt(0.5f * (1.0f + wb * 0.55f)) * 1.4142f;

    const float turb = std::clamp(st.turb_rms, 0.0f, 1.5f);
    const float rough = std::clamp(st.ground_roughness, 0.02f, 0.6f);
    const float brake = (st.on_ground && !st.walking && st.ground_speed_mps > 1.2f)
        ? std::clamp(st.brake_amount, 0.0f, 1.0f) : 0.0f;
    const float walk_spd = st.walking ? std::clamp(st.walk_speed_mps, 0.0f, 6.0f) : 0.0f;

    const float slew = 1.0f - std::exp(-kInvSr * 18.0f);
    const float spatial_slew = 1.0f - std::exp(-kInvSr * 26.0f);
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
        sm_pan += (target_pan - sm_pan) * spatial_slew;
        sm_front += (target_front - sm_front) * spatial_slew;
        sm_elev += (target_elev - sm_elev) * spatial_slew;
        sm_wind_pan += (target_wind_pan - sm_wind_pan) * spatial_slew;
        sm_doppler += (target_doppler - sm_doppler) * slew;
        sm_tr_pan += (target_tr_pan - sm_tr_pan) * spatial_slew;
        sm_tr_front += (target_tr_front - sm_tr_front) * spatial_slew;
        sm_tr_gain += (target_tr_atten - sm_tr_gain) * slew;
        sm_tr_doppler += (target_tr_doppler - sm_tr_doppler) * slew;

        const float w_noise = next_noise();
        const float w_side = next_noise();
        // Equal-variance decorrelated L/R noise for enveloping cabin airflow, rain & thunder width.
        const float n_l = 0.65f * w_noise + 0.76f * w_side;
        const float n_r = 0.65f * w_noise - 0.76f * w_side;

        // 1. Flat-4 360 4-cylinder 4-stroke piston engine + 2-blade fixed-pitch propeller (with Doppler + air absorption).
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

            // 4-stroke 4-cyl fires twice per crankshaft rev; external Doppler shifts frequency naturally.
            const double f_fire = static_cast<double>(sm_rpm) * (2.0 / 60.0) * static_cast<double>(sm_doppler);
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

            const float idle_chug = 1.0f + (1.0f - std::clamp(rpm01, 0.0f, 1.0f)) * 0.25f * static_cast<float>(std::sin(ps));
            const float prop_chop = (0.04f + 0.16f * rpm01 * load) * (0.55f + 0.45f * static_cast<float>(std::sin(p1))) * w_noise;
            raw = std::tanh(raw * (1.25f + 0.85f * load)) * idle_chug + prop_chop;

            // Warm cabin muffling inside vs atmospheric distance air absorption outside.
            const float air_cut = std::clamp(11500.0f / (1.0f + dist * 0.0085f), 460.0f, 11500.0f);
            const float cabin_cut = 980.0f + 480.0f * rpm01;
            const float cutoff = cabin_cut + (air_cut - cabin_cut) * (1.0f - sm_cabin);
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

        // Binaural spatialization for player engine/prop (cockpit head-tracked OR 3D exterior sphere) + cabin early reflection.
        float ac_l = 0.0f, ac_r = 0.0f;
        const float ac_mix = eng * ac_atten;
        const float spatial_depth = 0.65f + 0.35f * (1.0f - sm_cabin);
        spatialize_binaural(ac_mix, eng_delay_, eng_delay_idx_,
                            sm_pan, sm_front, sm_elev, spatial_depth,
                            lp_eng_front, lp_eng_shadow_l, lp_eng_shadow_r, ac_l, ac_r);
        if (sm_cabin > 0.02f) {
            // 4.4 ms damped aluminum cabin bounce adds acoustic cockpit space without blurring head-turn panning.
            constexpr unsigned int kCabinMask = SynthState::kCabinBufSize - 1u;
            cabin_refl_idx_ = (cabin_refl_idx_ + 1u) & kCabinMask;
            cabin_refl_[cabin_refl_idx_] = ac_mix;
            const float refl_tap = cabin_refl_[(cabin_refl_idx_ - 211u) & kCabinMask];
            lp_cabin_refl += (refl_tap - lp_cabin_refl) * one_pole_alpha(1350.0f);
            const float refl_mix = lp_cabin_refl * (0.11f * sm_cabin);
            ac_l += refl_mix;
            ac_r += refl_mix;
        }

        // 2b. Nearby AI traffic aircraft spatialized flyby (Doppler + air absorption + binaural pan).
        float tr_l = 0.0f, tr_r = 0.0f;
        if (sm_tr_gain > 0.002f && st.traffic_rpm > 50.0f) {
            const double tr_harm = st.traffic_is_airliner ? 3.8 : (2.0 / 60.0);
            const double tr_f = static_cast<double>(st.traffic_rpm) * tr_harm * static_cast<double>(sm_tr_doppler);
            phase_tr_fire = wrap_phase(phase_tr_fire + tr_f * kInvSr);
            phase_tr_sub = wrap_phase(phase_tr_sub + 0.5 * tr_f * kInvSr);
            const float tr_raw = st.traffic_is_airliner
                ? (0.28f * static_cast<float>(std::sin(phase_tr_fire * kTwoPi)) + 0.42f * lp_wind_l1)
                : (0.45f * static_cast<float>(std::sin(phase_tr_fire * kTwoPi))
                   + 0.25f * static_cast<float>(std::sin(phase_tr_sub * kTwoPi))
                   + 0.18f * static_cast<float>(std::sin(phase_tr_fire * 2.0 * kTwoPi)));
            const float tr_cut = std::clamp(8000.0f / (1.0f + std::max(0.0f, st.traffic_dist_m) * 0.0095f), 260.0f, 8000.0f);
            lp_tr_eng += (tr_raw - lp_tr_eng) * one_pole_alpha(tr_cut);
            spatialize_binaural(lp_tr_eng * sm_tr_gain * 0.22f, tr_delay_, tr_delay_idx_,
                                sm_tr_pan, sm_tr_front, 0.0f, 1.0f,
                                lp_tr_front, lp_tr_shadow_l, lp_tr_shadow_r, tr_l, tr_r);
        }

        // 3. Stereo decorrelated airframe slipstream, crosswind bearing & outdoor breeze.
        const float wind_cut1 = 480.0f + sm_ias * 9.0f;
        const float wind_cut2 = 190.0f + sm_ias * 4.0f;
        lp_wind_l1 += (n_l - lp_wind_l1) * one_pole_alpha(wind_cut1);
        lp_wind_r1 += (n_r - lp_wind_r1) * one_pole_alpha(wind_cut1);
        lp_wind_l2 += (lp_wind_l1 - lp_wind_l2) * one_pole_alpha(wind_cut2);
        lp_wind_r2 += (lp_wind_r1 - lp_wind_r2) * one_pole_alpha(wind_cut2);
        const float ias_norm = std::clamp((sm_ias - 24.0f) / 110.0f, 0.0f, 1.5f);
        const float gust_mod_l = 1.0f + turb * 0.38f * std::clamp(lp_wind_l2 * 2.2f, -0.5f, 0.8f);
        const float gust_mod_r = 1.0f + turb * 0.38f * std::clamp(lp_wind_r2 * 2.2f, -0.5f, 0.8f);
        const float wbias_l = 1.0f - 0.26f * sm_wind_pan;
        const float wbias_r = 1.0f + 0.26f * sm_wind_pan;
        float wind_l = (lp_wind_l1 * 0.65f + lp_wind_l2 * 0.85f) * (ias_norm * ias_norm * 0.24f) * gust_mod_l * wbias_l;
        float wind_r = (lp_wind_r1 * 0.65f + lp_wind_r2 * 0.85f) * (ias_norm * ias_norm * 0.24f) * gust_mod_r * wbias_r;
        if (st.walking || !st.in_cockpit) {
            const float outdoor_w = std::clamp(sm_wind / 16.0f, 0.05f, 1.2f);
            wind_l += (lp_wind_l1 * 0.55f + lp_wind_l2 * 0.70f) * outdoor_w * 0.14f * wbias_l;
            wind_r += (lp_wind_r1 * 0.55f + lp_wind_r2 * 0.70f) * outdoor_w * 0.14f * wbias_r;
        }

        // 4. Stereo rain drumming on windscreen/cabin roof + spatial rolling thunder.
        float wx_l = 0.0f, wx_r = 0.0f;
        if (sm_rain > 0.01f) {
            hp_rain_out_l = 0.86f * (hp_rain_out_l + n_l - hp_rain_prev_l);
            hp_rain_prev_l = n_l;
            hp_rain_out_r = 0.86f * (hp_rain_out_r + n_r - hp_rain_prev_r);
            hp_rain_prev_r = n_r;
            const float rain_cut = sm_cabin > 0.5f ? 2950.0f : 5200.0f;
            lp_rain_l += (hp_rain_out_l - lp_rain_l) * one_pole_alpha(rain_cut);
            lp_rain_r += (hp_rain_out_r - lp_rain_r) * one_pole_alpha(rain_cut);
            const float drop_thresh = 0.986f - 0.014f * std::min(sm_rain, 1.0f);
            const float drop_l = (std::abs(n_l) > drop_thresh) ? (n_l * 0.42f) : 0.0f;
            const float drop_r = (std::abs(n_r) > drop_thresh) ? (n_r * 0.42f) : 0.0f;
            const float r_bias_l = 1.0f - 0.18f * sm_wind_pan;
            const float r_bias_r = 1.0f + 0.18f * sm_wind_pan;
            wx_l += (lp_rain_l * 0.14f + drop_l * (0.10f + 0.10f * sm_cabin)) * sm_rain * r_bias_l;
            wx_r += (lp_rain_r * 0.14f + drop_r * (0.10f + 0.10f * sm_cabin)) * sm_rain * r_bias_r;
        }
        if (thunder_env > 0.002f) {
            lp_thunder += (w_noise - lp_thunder) * one_pole_alpha(96.0f);
            lp_thunder_r += ((0.7f * w_noise + 0.7f * w_side) - lp_thunder_r) * one_pole_alpha(82.0f);
            const float th_l = std::tanh(lp_thunder * 4.5f) * thunder_env * 0.38f * std::sqrt(0.5f * (1.0f - thunder_pan)) * 1.4142f;
            const float th_r = std::tanh(lp_thunder_r * 4.5f) * thunder_env * 0.38f * std::sqrt(0.5f * (1.0f + thunder_pan)) * 1.4142f;
            wx_l += th_l;
            wx_r += th_r;
            thunder_env *= std::exp(-kInvSr * 0.55f);
        }

        // 5. Head-tracked cockpit alerts, flap servo, avionics fan/gyro, ground roll, touchdown & footsteps.
        float ckpt_l = 0.0f, ckpt_r = 0.0f;
        if (st.stall_warn && !st.on_ground && sm_ias > 18.0f && st.in_cockpit && !st.walking) {
            phase_stall = wrap_phase(phase_stall + 2760.0 * kInvSr);
            const float reed = static_cast<float>(std::sin(phase_stall * kTwoPi))
                             + 0.42f * static_cast<float>(std::sin(phase_stall * 2.0 * kTwoPi))
                             + 0.18f * static_cast<float>(std::sin(phase_stall * 3.0 * kTwoPi));
            const float s_sig = std::tanh(reed * 0.9f) * 0.13f;
            ckpt_l += s_sig * stall_l;
            ckpt_r += s_sig * stall_r;
        }
        if (st.flaps_moving && st.in_cockpit && !st.walking) {
            phase_flap = wrap_phase(phase_flap + 315.0 * kInvSr);
            const float f_sig = (0.65f * static_cast<float>(std::sin(phase_flap * kTwoPi)) +
                                 0.35f * static_cast<float>(std::sin(phase_flap * 3.0 * kTwoPi))) * 0.045f;
            ckpt_l += f_sig * flap_l;
            ckpt_r += f_sig * flap_r;
        }
        if (st.avionics_powered && st.in_cockpit && !st.walking) {
            phase_gyro = wrap_phase(phase_gyro + 1480.0 * kInvSr);
            const float a_sig = (0.012f * static_cast<float>(std::sin(phase_gyro * kTwoPi)) + 0.006f * lp_wind_l1);
            ckpt_l += a_sig * avn_l;
            ckpt_r += a_sig * avn_r;
        }

        // Ground roll & braking rumble with asymmetric main-gear weight bias.
        if (sm_gnd > 0.015f) {
            lp_gnd += (w_noise - lp_gnd) * one_pole_alpha(85.0f + rough * 220.0f);
            float g_sig = lp_gnd * sm_gnd * (0.20f + rough * 0.45f);
            if (brake > 0.12f) g_sig += (0.5f * (lp_wind_l1 + lp_wind_r1)) * brake * sm_gnd * 0.09f;
            ckpt_l += g_sig * gnd_l;
            ckpt_r += g_sig * gnd_r;
        }
        // Touchdown thump & brief tire chirp.
        if (td_env > 0.002f) {
            phase_td = wrap_phase(phase_td + (78.0 + 38.0 * td_env) * kInvSr);
            const float thump = static_cast<float>(std::sin(phase_td * kTwoPi)) * td_env * 0.42f * td_strength;
            const float chirp_l = (td_env > 0.65f) ? (lp_wind_l1 * (td_env - 0.65f) * 0.55f * td_strength) : 0.0f;
            const float chirp_r = (td_env > 0.65f) ? (lp_wind_r1 * (td_env - 0.65f) * 0.55f * td_strength) : 0.0f;
            ckpt_l += (thump + chirp_l) * gnd_l;
            ckpt_r += (thump + chirp_r) * gnd_r;
            td_env *= std::exp(-kInvSr * 7.5f);
        }
        // Walkaround footsteps on grass/gravel.
        if (walk_spd > 0.35f) {
            const double prev_step = phase_step;
            phase_step = wrap_phase(phase_step + (1.55 + walk_spd * 0.22) * kInvSr);
            if (phase_step < prev_step) step_env = 1.0f;
        }
        if (step_env > 0.003f) {
            ckpt_l += (lp_wind_l1 * 0.45f + lp_gnd * 0.55f) * step_env * 0.18f;
            ckpt_r += (lp_wind_r1 * 0.45f + lp_gnd * 0.55f) * step_env * 0.18f;
            step_env *= std::exp(-kInvSr * 18.0f);
        }

        // Binaural stereo mix + soft limiting (prevents any harsh digital clipping).
        const float left = std::tanh((ac_l + tr_l + wind_l + wx_l + ckpt_l) * sm_gain);
        const float right = std::tanh((ac_r + tr_r + wind_r + wx_r + ckpt_r) * sm_gain);

        out[i * 2 + 0] = static_cast<int16_t>(std::clamp(left * 32000.0f, -32767.0f, 32767.0f));
        out[i * 2 + 1] = static_cast<int16_t>(std::clamp(right * 32000.0f, -32767.0f, 32767.0f));
    }
}

AudioEngine::~AudioEngine() {
    shutdown();
}

bool AudioEngine::open_alsa() {
#ifdef _WIN32
    WAVEFORMATEX wf{};
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 2;
    wf.nSamplesPerSec = kSampleRate;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = static_cast<WORD>(wf.nChannels * (wf.wBitsPerSample / 8));
    wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;

    HANDLE evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!evt) return false;

    HWAVEOUT hwo = nullptr;
    const MMRESULT rc = waveOutOpen(&hwo, WAVE_MAPPER, &wf, reinterpret_cast<DWORD_PTR>(evt), 0, CALLBACK_EVENT);
    if (rc != MMSYSERR_NOERROR || !hwo) {
        CloseHandle(evt);
        return false;
    }
    pcm_ = hwo;
    win_event_ = evt;
    std::printf("[AUDIO] Procedural C172S audio engine active (%u Hz stereo via Windows waveOut)\n", kSampleRate);
    return true;
#else
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
    return false;
#endif
}

void AudioEngine::close_alsa() {
#ifdef _WIN32
    if (pcm_) {
        HWAVEOUT hwo = reinterpret_cast<HWAVEOUT>(pcm_);
        waveOutReset(hwo);
        waveOutClose(hwo);
        pcm_ = nullptr;
    }
    if (win_event_) {
        CloseHandle(reinterpret_cast<HANDLE>(win_event_));
        win_event_ = nullptr;
    }
#else
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
        std::printf("[AUDIO] No audio playback device available; running silently.\n");
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
#ifdef _WIN32
    if (win_event_) SetEvent(reinterpret_cast<HANDLE>(win_event_));
#endif
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
#ifdef _WIN32
    constexpr int kRingBlocks = 4;
    HWAVEOUT hwo = reinterpret_cast<HWAVEOUT>(pcm_);
    HANDLE evt = reinterpret_cast<HANDLE>(win_event_);
    std::array<std::array<int16_t, kPeriodFrames * 2>, kRingBlocks> ring{};
    std::array<WAVEHDR, kRingBlocks> hdrs{};
    for (int i = 0; i < kRingBlocks; ++i) {
        hdrs[i].lpData = reinterpret_cast<LPSTR>(ring[i].data());
        hdrs[i].dwBufferLength = static_cast<DWORD>(ring[i].size() * sizeof(int16_t));
        waveOutPrepareHeader(hwo, &hdrs[i], sizeof(WAVEHDR));
        hdrs[i].dwFlags |= WHDR_DONE;
    }
    while (running_.load(std::memory_order_acquire)) {
        bool submitted = false;
        for (int i = 0; i < kRingBlocks; ++i) {
            if ((hdrs[i].dwFlags & WHDR_DONE) != 0) {
                const AudioState st = slots_[read_slot_.load(std::memory_order_acquire)];
                synth.render_stereo(st, ring[i].data(), kPeriodFrames);
                hdrs[i].dwFlags &= ~WHDR_DONE;
                if (waveOutWrite(hwo, &hdrs[i], sizeof(WAVEHDR)) == MMSYSERR_NOERROR) {
                    submitted = true;
                } else {
                    hdrs[i].dwFlags |= WHDR_DONE;
                }
            }
        }
        if (!submitted) {
            WaitForSingleObject(evt, 20);
        }
    }
    waveOutReset(hwo);
    for (int i = 0; i < kRingBlocks; ++i) {
        waveOutUnprepareHeader(hwo, &hdrs[i], sizeof(WAVEHDR));
    }
#else
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
#endif
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

    // Spatial stereo divergence check: pan right -> R > L; pan left -> L > R.
    auto measure_lr = [&](float pan_val, double &out_l, double &out_r) {
        SynthState sp_synth;
        AudioState ext = idle;
        ext.in_cockpit = false;
        ext.aircraft_dist_m = 12.0f;
        ext.aircraft_pan = pan_val;
        ext.aircraft_front = 0.25f;
        for (int w = 0; w < 4; ++w) sp_synth.render_stereo(ext, buf.data(), kPeriodFrames);
        double sq_l = 0.0, sq_r = 0.0;
        int n = 0;
        for (int b = 0; b < 6; ++b) {
            sp_synth.render_stereo(ext, buf.data(), kPeriodFrames);
            for (unsigned int f = 0; f < kPeriodFrames; ++f) {
                const double vl = static_cast<double>(buf[f * 2 + 0]) / 32768.0;
                const double vr = static_cast<double>(buf[f * 2 + 1]) / 32768.0;
                sq_l += vl * vl;
                sq_r += vr * vr;
                ++n;
            }
        }
        out_l = std::sqrt(sq_l / std::max(1, n));
        out_r = std::sqrt(sq_r / std::max(1, n));
    };
    double l_right = 0.0, r_right = 0.0, l_left = 0.0, r_left = 0.0;
    measure_lr(0.90f, l_right, r_right);
    measure_lr(-0.90f, l_left, r_left);
    if (!(r_right > l_right * 1.35 && l_left > r_left * 1.35)) {
        std::fprintf(stderr, "[FAIL] AudioEngine spatial check R(%.4f,%.4f) L(%.4f,%.4f)\n",
                     l_right, r_right, l_left, r_left);
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
