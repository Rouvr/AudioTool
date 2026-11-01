#pragma once
// ============================================================================
// AudioTool (AT) — Standard Audio DSP Algorithms & Building Blocks
//
// Implements industry-standard open-source audio DSP architectures:
//   - Biquad Filters: Robert Bristow-Johnson (RBJ) Audio EQ Cookbook
//   - Reverberator: Jezar at Dreampoint Freeverb Architecture
//   - Dynamics: JAES Digital Dynamic Range Compressor & Hard-Ceiling Limiter
//   - Noise Gate: Hysteresis-Gated Attenuator with Attack/Hold/Release
//   - Stereo Delay: Circular Buffer with Lowpass Feedback & Ping-Pong
//   - Waveshaping & Saturation: Analog Tape Saturation, Tube Overdrive,
//     Hard-Clipping Distortion, and Bitcrusher Quantizer
// ============================================================================

#include "at_sdk/at_abi.h"
#include "at_sdk/at_simd_buffer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace at::dsp {

constexpr double kPi = 3.14159265358979323846;
constexpr float  kAntiDenormal = 1e-25f;

// ============================================================================
// 1. Biquad IIR Filter (Robert Bristow-Johnson Audio EQ Cookbook)
// ============================================================================
enum class BiquadType : uint32_t {
    LowPass = 0,
    HighPass,
    BandPass,
    Notch,
    PeakingEQ,
    LowShelf,
    HighShelf
};

class Biquad {
public:
    Biquad() noexcept { reset(); }

    void reset() noexcept {
        z1_l_ = z2_l_ = 0.0f;
        z1_r_ = z2_r_ = 0.0f;
    }

    void configure(BiquadType type, float sample_rate, float frequency, float q, float gain_db = 0.0f) noexcept {
        if (sample_rate <= 0.0f) sample_rate = 48000.0f;

        // Clamp frequency below Nyquist
        float f0 = std::clamp(frequency, 10.0f, sample_rate * 0.49f);
        float Q = std::max(0.05f, q);
        double w0 = 2.0 * kPi * (static_cast<double>(f0) / static_cast<double>(sample_rate));
        double cos_w0 = std::cos(w0);
        double sin_w0 = std::sin(w0);
        double alpha = sin_w0 / (2.0 * static_cast<double>(Q));
        double A = std::pow(10.0, static_cast<double>(gain_db) / 40.0);

        double b0 = 0.0, b1 = 0.0, b2 = 0.0;
        double a0 = 1.0, a1 = 0.0, a2 = 0.0;

        switch (type) {
        case BiquadType::LowPass:
            b0 = (1.0 - cos_w0) * 0.5;
            b1 = 1.0 - cos_w0;
            b2 = (1.0 - cos_w0) * 0.5;
            a0 = 1.0 + alpha;
            a1 = -2.0 * cos_w0;
            a2 = 1.0 - alpha;
            break;

        case BiquadType::HighPass:
            b0 = (1.0 + cos_w0) * 0.5;
            b1 = -(1.0 + cos_w0);
            b2 = (1.0 + cos_w0) * 0.5;
            a0 = 1.0 + alpha;
            a1 = -2.0 * cos_w0;
            a2 = 1.0 - alpha;
            break;

        case BiquadType::BandPass:
            b0 = alpha;
            b1 = 0.0;
            b2 = -alpha;
            a0 = 1.0 + alpha;
            a1 = -2.0 * cos_w0;
            a2 = 1.0 - alpha;
            break;

        case BiquadType::Notch:
            b0 = 1.0;
            b1 = -2.0 * cos_w0;
            b2 = 1.0;
            a0 = 1.0 + alpha;
            a1 = -2.0 * cos_w0;
            a2 = 1.0 - alpha;
            break;

        case BiquadType::PeakingEQ: {
            b0 = 1.0 + alpha * A;
            b1 = -2.0 * cos_w0;
            b2 = 1.0 - alpha * A;
            a0 = 1.0 + alpha / A;
            a1 = -2.0 * cos_w0;
            a2 = 1.0 - alpha / A;
            break;
        }

        case BiquadType::LowShelf: {
            double sqrtA2 = 2.0 * std::sqrt(A) * alpha;
            b0 = A * ((A + 1.0) - (A - 1.0) * cos_w0 + sqrtA2);
            b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cos_w0);
            b2 = A * ((A + 1.0) - (A - 1.0) * cos_w0 - sqrtA2);
            a0 = (A + 1.0) + (A - 1.0) * cos_w0 + sqrtA2;
            a1 = -2.0 * ((A - 1.0) + (A + 1.0) * cos_w0);
            a2 = (A + 1.0) + (A - 1.0) * cos_w0 - sqrtA2;
            break;
        }

        case BiquadType::HighShelf: {
            double sqrtA2 = 2.0 * std::sqrt(A) * alpha;
            b0 = A * ((A + 1.0) + (A - 1.0) * cos_w0 + sqrtA2);
            b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cos_w0);
            b2 = A * ((A + 1.0) + (A - 1.0) * cos_w0 - sqrtA2);
            a0 = (A + 1.0) - (A - 1.0) * cos_w0 + sqrtA2;
            a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cos_w0);
            a2 = (A + 1.0) - (A - 1.0) * cos_w0 - sqrtA2;
            break;
        }
        }

        double inv_a0 = 1.0 / a0;
        b0_ = static_cast<float>(b0 * inv_a0);
        b1_ = static_cast<float>(b1 * inv_a0);
        b2_ = static_cast<float>(b2 * inv_a0);
        a1_ = static_cast<float>(a1 * inv_a0);
        a2_ = static_cast<float>(a2 * inv_a0);
    }

    // Direct Form II Transposed (Left Channel)
    inline float process_left(float in) noexcept {
        float out = b0_ * in + z1_l_;
        z1_l_ = b1_ * in - a1_ * out + z2_l_ + kAntiDenormal;
        z2_l_ = b2_ * in - a2_ * out;
        return out;
    }

    // Direct Form II Transposed (Right Channel)
    inline float process_right(float in) noexcept {
        float out = b0_ * in + z1_r_;
        z1_r_ = b1_ * in - a1_ * out + z2_r_ + kAntiDenormal;
        z2_r_ = b2_ * in - a2_ * out;
        return out;
    }

private:
    float b0_ = 1.0f, b1_ = 0.0f, b2_ = 0.0f;
    float a1_ = 0.0f, a2_ = 0.0f;
    float z1_l_ = 0.0f, z2_l_ = 0.0f;
    float z1_r_ = 0.0f, z2_r_ = 0.0f;
};

// ============================================================================
// 2. Freeverb Reverb Engine (Jezar at Dreampoint Architecture)
// ============================================================================
class CombFilter {
public:
    void init(size_t size) {
        buffer_.assign(size, 0.0f);
        buf_size_ = size;
        buf_idx_ = 0;
        filter_store_ = 0.0f;
    }

    void mute() {
        std::fill(buffer_.begin(), buffer_.end(), 0.0f);
        filter_store_ = 0.0f;
    }

    void set_damp(float val) noexcept {
        damp1_ = val;
        damp2_ = 1.0f - val;
    }

    void set_feedback(float val) noexcept {
        feedback_ = val;
    }

    inline float process(float in) noexcept {
        if (buf_size_ == 0) return in;
        float output = buffer_[buf_idx_];
        filter_store_ = (output * damp2_) + (filter_store_ * damp1_) + kAntiDenormal;
        buffer_[buf_idx_] = in + (filter_store_ * feedback_);
        if (++buf_idx_ >= buf_size_) buf_idx_ = 0;
        return output;
    }

private:
    std::vector<float> buffer_;
    size_t             buf_size_ = 0;
    size_t             buf_idx_ = 0;
    float              filter_store_ = 0.0f;
    float              feedback_ = 0.5f;
    float              damp1_ = 0.5f;
    float              damp2_ = 0.5f;
};

class AllpassFilter {
public:
    void init(size_t size) {
        buffer_.assign(size, 0.0f);
        buf_size_ = size;
        buf_idx_ = 0;
        feedback_ = 0.5f;
    }

    void mute() {
        std::fill(buffer_.begin(), buffer_.end(), 0.0f);
    }

    void set_feedback(float val) noexcept { feedback_ = val; }

    inline float process(float in) noexcept {
        if (buf_size_ == 0) return in;
        float buf_out = buffer_[buf_idx_];
        float out = -in + buf_out;
        buffer_[buf_idx_] = in + (buf_out * feedback_) + kAntiDenormal;
        if (++buf_idx_ >= buf_size_) buf_idx_ = 0;
        return out;
    }

private:
    std::vector<float> buffer_;
    size_t             buf_size_ = 0;
    size_t             buf_idx_ = 0;
    float              feedback_ = 0.5f;
};

class FreeverbEngine {
public:
    static constexpr size_t kNumCombs = 8;
    static constexpr size_t kNumAllpasses = 4;

    void prepare(float sample_rate) {
        sample_rate_ = sample_rate > 0.0f ? sample_rate : 48000.0f;
        float scale = sample_rate_ / 44100.0f;

        const size_t comb_tunings_l[kNumCombs] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
        const size_t allpass_tunings_l[kNumAllpasses] = {556, 441, 341, 225};
        const size_t kStereoSpread = 23;

        for (size_t i = 0; i < kNumCombs; ++i) {
            combs_l_[i].init(static_cast<size_t>(comb_tunings_l[i] * scale));
            combs_r_[i].init(static_cast<size_t>((comb_tunings_l[i] + kStereoSpread) * scale));
        }

        for (size_t i = 0; i < kNumAllpasses; ++i) {
            allpass_l_[i].init(static_cast<size_t>(allpass_tunings_l[i] * scale));
            allpass_r_[i].init(static_cast<size_t>((allpass_tunings_l[i] + kStereoSpread) * scale));
            allpass_l_[i].set_feedback(0.5f);
            allpass_r_[i].set_feedback(0.5f);
        }

        update_parameters();
    }

    void reset() {
        for (auto& c : combs_l_) c.mute();
        for (auto& c : combs_r_) c.mute();
        for (auto& a : allpass_l_) a.mute();
        for (auto& a : allpass_r_) a.mute();
    }

    void set_room_size(float size) noexcept {
        room_size_ = std::clamp(size, 0.0f, 1.0f);
        update_parameters();
    }

    void set_damping(float damp) noexcept {
        damping_ = std::clamp(damp, 0.0f, 1.0f);
        update_parameters();
    }

    void set_width(float width) noexcept {
        width_ = std::clamp(width, 0.0f, 1.0f);
        wet1_ = wet_ * (width_ * 0.5f + 0.5f);
        wet2_ = wet_ * ((1.0f - width_) * 0.5f);
    }

    void set_wet(float wet) noexcept {
        wet_ = wet;
        set_width(width_);
    }

    void set_dry(float dry) noexcept {
        dry_ = dry;
    }

    void set_freeze(bool freeze) noexcept {
        freeze_ = freeze;
        update_parameters();
    }

    inline void process_stereo(float in_l, float in_r, float& out_l, float& out_r) noexcept {
        float mono_in = (in_l + in_r) * 0.015f; // Jezar fixed gain scale
        float comb_out_l = 0.0f;
        float comb_out_r = 0.0f;

        for (size_t i = 0; i < kNumCombs; ++i) {
            comb_out_l += combs_l_[i].process(mono_in);
            comb_out_r += combs_r_[i].process(mono_in);
        }

        for (size_t i = 0; i < kNumAllpasses; ++i) {
            comb_out_l = allpass_l_[i].process(comb_out_l);
            comb_out_r = allpass_r_[i].process(comb_out_r);
        }

        out_l = (comb_out_l * wet1_ + comb_out_r * wet2_) + (in_l * dry_);
        out_r = (comb_out_r * wet1_ + comb_out_l * wet2_) + (in_r * dry_);
    }

private:
    void update_parameters() noexcept {
        float fb = freeze_ ? 1.0f : (room_size_ * 0.28f + 0.7f);
        float d  = freeze_ ? 0.0f : (damping_ * 0.4f);

        for (size_t i = 0; i < kNumCombs; ++i) {
            combs_l_[i].set_feedback(fb);
            combs_r_[i].set_feedback(fb);
            combs_l_[i].set_damp(d);
            combs_r_[i].set_damp(d);
        }
    }

    float sample_rate_ = 48000.0f;
    float room_size_   = 0.5f;
    float damping_     = 0.5f;
    float width_       = 1.0f;
    float wet_         = 0.3f;
    float dry_         = 1.0f;
    float wet1_        = 0.3f;
    float wet2_        = 0.0f;
    bool  freeze_      = false;

    CombFilter    combs_l_[kNumCombs];
    CombFilter    combs_r_[kNumCombs];
    AllpassFilter allpass_l_[kNumAllpasses];
    AllpassFilter allpass_r_[kNumAllpasses];
};

// ============================================================================
// 3. Dynamic Range Compressor (JAES Logarithmic Characteristic)
// ============================================================================
class Compressor {
public:
    void prepare(float sample_rate) noexcept {
        sample_rate_ = sample_rate > 0.0f ? sample_rate : 48000.0f;
        update_coefficients();
        envelope_db_ = -96.0f;
    }

    void set_parameters(float threshold_db, float ratio, float attack_ms, float release_ms,
                        float knee_db, float makeup_db) noexcept {
        threshold_db_ = threshold_db;
        ratio_ = std::max(1.0f, ratio);
        attack_ms_ = std::max(0.05f, attack_ms);
        release_ms_ = std::max(1.0f, release_ms);
        knee_db_ = std::max(0.0f, knee_db);
        makeup_gain_ = at::db_to_linear(makeup_db);
        update_coefficients();
    }

    inline void process_stereo(float in_l, float in_r, float& out_l, float& out_r) noexcept {
        // Peak level detection across stereo channels
        float max_abs = std::max(std::abs(in_l), std::abs(in_r));
        float in_db = at::linear_to_db(max_abs);

        // Smooth Knee Static Compression Curve
        float gr_target_db = 0.0f;
        float half_knee = knee_db_ * 0.5f;
        float delta = in_db - threshold_db_;

        if (knee_db_ > 0.0f && delta > -half_knee && delta < half_knee) {
            // In the soft knee
            float x = delta + half_knee;
            gr_target_db = ((1.0f / ratio_) - 1.0f) * (x * x) / (2.0f * knee_db_);
        } else if (delta >= half_knee) {
            // Above the knee
            gr_target_db = ((1.0f / ratio_) - 1.0f) * delta;
        }

        // Smooth Ballistics
        if (gr_target_db < envelope_db_) {
            envelope_db_ = alpha_att_ * envelope_db_ + (1.0f - alpha_att_) * gr_target_db;
        } else {
            envelope_db_ = alpha_rel_ * envelope_db_ + (1.0f - alpha_rel_) * gr_target_db;
        }

        float lin_gain = at::db_to_linear(envelope_db_) * makeup_gain_;
        out_l = in_l * lin_gain;
        out_r = in_r * lin_gain;
    }

    float current_gain_reduction_db() const noexcept {
        return envelope_db_;
    }

private:
    void update_coefficients() noexcept {
        alpha_att_ = std::exp(-1.0f / (0.001f * attack_ms_ * sample_rate_));
        alpha_rel_ = std::exp(-1.0f / (0.001f * release_ms_ * sample_rate_));
    }

    float sample_rate_  = 48000.0f;
    float threshold_db_ = -18.0f;
    float ratio_        = 4.0f;
    float attack_ms_    = 10.0f;
    float release_ms_   = 100.0f;
    float knee_db_      = 6.0f;
    float makeup_gain_  = 1.0f;
    float alpha_att_    = 0.9f;
    float alpha_rel_    = 0.99f;
    float envelope_db_  = -96.0f;
};

// ============================================================================
// 4. Hard-Ceiling Audio Limiter
// ============================================================================
class Limiter {
public:
    void prepare(float sample_rate) noexcept {
        sample_rate_ = sample_rate > 0.0f ? sample_rate : 48000.0f;
        update_coefficients();
        env_peak_ = 0.0f;
    }

    void set_parameters(float ceiling_db, float pre_gain_db, float release_ms) noexcept {
        ceiling_linear_ = at::db_to_linear(ceiling_db);
        pre_gain_linear_ = at::db_to_linear(pre_gain_db);
        release_ms_ = std::max(1.0f, release_ms);
        update_coefficients();
    }

    inline void process_stereo(float in_l, float in_r, float& out_l, float& out_r) noexcept {
        float x_l = in_l * pre_gain_linear_;
        float x_r = in_r * pre_gain_linear_;
        float peak = std::max(std::abs(x_l), std::abs(x_r));

        // Instantaneous attack, smooth exponential decay release
        if (peak > env_peak_) {
            env_peak_ = peak;
        } else {
            env_peak_ = env_peak_ * alpha_rel_ + kAntiDenormal;
        }

        float gain = 1.0f;
        if (env_peak_ > ceiling_linear_ && env_peak_ > 1e-6f) {
            gain = ceiling_linear_ / env_peak_;
        }

        out_l = std::clamp(x_l * gain, -ceiling_linear_, ceiling_linear_);
        out_r = std::clamp(x_r * gain, -ceiling_linear_, ceiling_linear_);
    }

private:
    void update_coefficients() noexcept {
        alpha_rel_ = std::exp(-1.0f / (0.001f * release_ms_ * sample_rate_));
    }

    float sample_rate_     = 48000.0f;
    float ceiling_linear_  = 0.988f; // -0.1 dB
    float pre_gain_linear_ = 1.0f;
    float release_ms_      = 50.0f;
    float alpha_rel_       = 0.999f;
    float env_peak_        = 0.0f;
};

// ============================================================================
// 5. Noise Gate (Hysteresis-Gated Attenuator)
// ============================================================================
class NoiseGate {
public:
    enum class GateState { Closed, Opening, Open, Holding, Releasing };

    void prepare(float sample_rate) noexcept {
        sample_rate_ = sample_rate > 0.0f ? sample_rate : 48000.0f;
        update_coefficients();
        state_ = GateState::Closed;
        current_gain_ = 0.0f;
        hold_counter_ = 0;
    }

    void set_parameters(float threshold_db, float attack_ms, float hold_ms, float release_ms, float floor_db) noexcept {
        threshold_db_ = threshold_db;
        thresh_linear_ = at::db_to_linear(threshold_db);
        attack_ms_ = std::max(0.1f, attack_ms);
        hold_ms_ = std::max(0.0f, hold_ms);
        release_ms_ = std::max(1.0f, release_ms);
        floor_linear_ = at::db_to_linear(floor_db);
        hold_samples_ = static_cast<uint32_t>((hold_ms_ * 0.001f) * sample_rate_);
        update_coefficients();
    }

    inline void process_stereo(float in_l, float in_r, float& out_l, float& out_r) noexcept {
        float peak = std::max(std::abs(in_l), std::abs(in_r));

        switch (state_) {
        case GateState::Closed:
            current_gain_ = floor_linear_;
            if (peak >= thresh_linear_) {
                state_ = GateState::Opening;
            }
            break;

        case GateState::Opening:
            current_gain_ += attack_step_;
            if (current_gain_ >= 1.0f) {
                current_gain_ = 1.0f;
                state_ = GateState::Open;
            }
            break;

        case GateState::Open:
            current_gain_ = 1.0f;
            if (peak < thresh_linear_) {
                state_ = GateState::Holding;
                hold_counter_ = hold_samples_;
            }
            break;

        case GateState::Holding:
            current_gain_ = 1.0f;
            if (peak >= thresh_linear_) {
                state_ = GateState::Open;
            } else if (hold_counter_ > 0) {
                --hold_counter_;
            } else {
                state_ = GateState::Releasing;
            }
            break;

        case GateState::Releasing:
            if (peak >= thresh_linear_) {
                state_ = GateState::Opening;
            } else {
                current_gain_ *= release_factor_;
                if (current_gain_ <= floor_linear_) {
                    current_gain_ = floor_linear_;
                    state_ = GateState::Closed;
                }
            }
            break;
        }

        out_l = in_l * current_gain_;
        out_r = in_r * current_gain_;
    }

    bool is_open() const noexcept {
        return state_ == GateState::Open || state_ == GateState::Opening || state_ == GateState::Holding;
    }

private:
    void update_coefficients() noexcept {
        uint32_t att_samples = std::max(1u, static_cast<uint32_t>((attack_ms_ * 0.001f) * sample_rate_));
        attack_step_ = 1.0f / static_cast<float>(att_samples);
        release_factor_ = std::exp(-1.0f / (0.001f * release_ms_ * sample_rate_));
    }

    float       sample_rate_    = 48000.0f;
    float       threshold_db_   = -45.0f;
    float       thresh_linear_  = 0.0056f;
    float       attack_ms_      = 2.0f;
    float       hold_ms_        = 50.0f;
    float       release_ms_     = 150.0f;
    float       floor_linear_   = 0.0001f;
    float       attack_step_    = 0.01f;
    float       release_factor_ = 0.99f;
    float       current_gain_   = 0.0f;
    uint32_t    hold_samples_   = 2400;
    uint32_t    hold_counter_   = 0;
    GateState   state_          = GateState::Closed;
};

// ============================================================================
// 6. Stereo Delay with Tone Damping & Ping-Pong
// ============================================================================
class StereoDelay {
public:
    void prepare(float sample_rate, float max_delay_ms = 2000.0f) {
        sample_rate_ = sample_rate > 0.0f ? sample_rate : 48000.0f;
        size_t max_samples = static_cast<size_t>((max_delay_ms * 0.001f) * sample_rate_) + 16;
        buffer_l_.assign(max_samples, 0.0f);
        buffer_r_.assign(max_samples, 0.0f);
        write_pos_ = 0;
        damp_filter_l_.reset();
        damp_filter_r_.reset();
        set_tone(8000.0f);
    }

    void reset() {
        std::fill(buffer_l_.begin(), buffer_l_.end(), 0.0f);
        std::fill(buffer_r_.begin(), buffer_r_.end(), 0.0f);
        write_pos_ = 0;
        damp_filter_l_.reset();
        damp_filter_r_.reset();
    }

    void set_parameters(float delay_ms, float feedback, float tone_hz, float mix, bool ping_pong) noexcept {
        delay_samples_ = std::clamp<size_t>(
            static_cast<size_t>((delay_ms * 0.001f) * sample_rate_), 1, buffer_l_.size() - 1);
        feedback_ = std::clamp(feedback, 0.0f, 0.95f);
        mix_ = std::clamp(mix, 0.0f, 1.0f);
        ping_pong_ = ping_pong;
        set_tone(tone_hz);
    }

    void set_tone(float tone_hz) noexcept {
        damp_filter_l_.configure(BiquadType::LowPass, sample_rate_, tone_hz, 0.707f);
        damp_filter_r_.configure(BiquadType::LowPass, sample_rate_, tone_hz, 0.707f);
    }

    inline void process_stereo(float in_l, float in_r, float& out_l, float& out_r) noexcept {
        if (buffer_l_.empty()) {
            out_l = in_l;
            out_r = in_r;
            return;
        }

        size_t buf_size = buffer_l_.size();
        size_t read_pos = (write_pos_ + buf_size - delay_samples_) % buf_size;

        float del_l = buffer_l_[read_pos];
        float del_r = buffer_r_[read_pos];

        // Apply lowpass feedback damping filter
        float damped_l = damp_filter_l_.process_left(del_l);
        float damped_r = damp_filter_r_.process_right(del_r);

        if (ping_pong_) {
            // Cross-feed left into right and right into left
            buffer_l_[write_pos_] = in_l + (damped_r * feedback_) + kAntiDenormal;
            buffer_r_[write_pos_] = in_r + (damped_l * feedback_) + kAntiDenormal;
        } else {
            buffer_l_[write_pos_] = in_l + (damped_l * feedback_) + kAntiDenormal;
            buffer_r_[write_pos_] = in_r + (damped_r * feedback_) + kAntiDenormal;
        }

        write_pos_ = (write_pos_ + 1) % buf_size;

        // Dry/Wet mix
        out_l = in_l * (1.0f - mix_) + del_l * mix_;
        out_r = in_r * (1.0f - mix_) + del_r * mix_;
    }

private:
    float              sample_rate_   = 48000.0f;
    std::vector<float> buffer_l_;
    std::vector<float> buffer_r_;
    size_t             write_pos_     = 0;
    size_t             delay_samples_ = 16800;
    float              feedback_      = 0.4f;
    float              mix_           = 0.3f;
    bool               ping_pong_     = false;
    Biquad             damp_filter_l_;
    Biquad             damp_filter_r_;
};

// ============================================================================
// 7. Nonlinear Waveshaping: Distortion, Overdrive, Saturation, Bitcrusher
// ============================================================================

// Hard-Clipping Distortion with Foldback and Tone Tilt
class DistortionProcessor {
public:
    void prepare(float sample_rate) {
        sample_rate_ = sample_rate > 0.0f ? sample_rate : 48000.0f;
        tone_filter_.reset();
        tone_filter_.configure(BiquadType::LowPass, sample_rate_, 5000.0f, 0.707f);
    }

    void set_parameters(float drive_db, float hardness, float tone_hz, float output_db, float mix) noexcept {
        drive_linear_ = at::db_to_linear(drive_db);
        hardness_ = std::clamp(hardness, 0.0f, 1.0f);
        output_linear_ = at::db_to_linear(output_db);
        mix_ = std::clamp(mix, 0.0f, 1.0f);
        tone_filter_.configure(BiquadType::LowPass, sample_rate_, tone_hz, 0.707f);
    }

    inline void process_stereo(float in_l, float in_r, float& out_l, float& out_r) noexcept {
        float x_l = in_l * drive_linear_;
        float x_r = in_r * drive_linear_;

        // Blend soft tanh with hard clip
        auto shape = [this](float x) -> float {
            float soft = std::tanh(x);
            float hard = std::clamp(x, -1.0f, 1.0f);
            return (soft * (1.0f - hardness_)) + (hard * hardness_);
        };

        float dist_l = tone_filter_.process_left(shape(x_l)) * output_linear_;
        float dist_r = tone_filter_.process_right(shape(x_r)) * output_linear_;

        out_l = in_l * (1.0f - mix_) + dist_l * mix_;
        out_r = in_r * (1.0f - mix_) + dist_r * mix_;
    }

private:
    float  sample_rate_   = 48000.0f;
    float  drive_linear_  = 3.98f; // +12 dB
    float  hardness_      = 0.8f;
    float  output_linear_ = 0.5f;
    float  mix_           = 1.0f;
    Biquad tone_filter_;
};

// Tube-Style Overdrive (Asymmetric Cubic Soft Clipping)
class OverdriveProcessor {
public:
    void prepare(float sample_rate) {
        sample_rate_ = sample_rate > 0.0f ? sample_rate : 48000.0f;
        warmth_filter_.reset();
        tone_filter_.reset();
        warmth_filter_.configure(BiquadType::LowShelf, sample_rate_, 250.0f, 0.707f, 3.0f);
        tone_filter_.configure(BiquadType::LowPass, sample_rate_, 6000.0f, 0.707f);
    }

    void set_parameters(float drive_db, float warmth_db, float tone_hz, float output_db, float mix) noexcept {
        drive_linear_ = at::db_to_linear(drive_db);
        output_linear_ = at::db_to_linear(output_db);
        mix_ = std::clamp(mix, 0.0f, 1.0f);
        warmth_filter_.configure(BiquadType::LowShelf, sample_rate_, 250.0f, 0.707f, warmth_db);
        tone_filter_.configure(BiquadType::LowPass, sample_rate_, tone_hz, 0.707f);
    }

    inline void process_stereo(float in_l, float in_r, float& out_l, float& out_r) noexcept {
        float wl = warmth_filter_.process_left(in_l) * drive_linear_;
        float wr = warmth_filter_.process_right(in_r) * drive_linear_;

        // Cubic diode soft saturation f(x) = x - (x^3)/3
        auto tube_saturate = [](float x) -> float {
            float biased = x + 0.1f; // Slight asymmetric bias generates warm 2nd harmonics
            if (biased > 1.2f) return 0.8f;
            if (biased < -1.2f) return -0.8f;
            return biased - (biased * biased * biased) * 0.3333333f;
        };

        float sat_l = tone_filter_.process_left(tube_saturate(wl)) * output_linear_;
        float sat_r = tone_filter_.process_right(tube_saturate(wr)) * output_linear_;

        out_l = in_l * (1.0f - mix_) + sat_l * mix_;
        out_r = in_r * (1.0f - mix_) + sat_r * mix_;
    }

private:
    float  sample_rate_   = 48000.0f;
    float  drive_linear_  = 3.98f;
    float  output_linear_ = 0.707f;
    float  mix_           = 1.0f;
    Biquad warmth_filter_;
    Biquad tone_filter_;
};

// Analog Tape Saturation (Hyperbolic Tangent with Tape Bias)
class SaturationProcessor {
public:
    void prepare(float sample_rate) {
        sample_rate_ = sample_rate > 0.0f ? sample_rate : 48000.0f;
        hiss_filter_.reset();
        hiss_filter_.configure(BiquadType::LowPass, sample_rate_, 16000.0f, 0.707f);
    }

    void set_parameters(float drive_db, float bias, float cutoff_hz, float output_db, float mix) noexcept {
        drive_linear_ = at::db_to_linear(drive_db);
        bias_ = std::clamp(bias, -0.8f, 0.8f);
        output_linear_ = at::db_to_linear(output_db);
        mix_ = std::clamp(mix, 0.0f, 1.0f);
        hiss_filter_.configure(BiquadType::LowPass, sample_rate_, cutoff_hz, 0.707f);
    }

    inline void process_stereo(float in_l, float in_r, float& out_l, float& out_r) noexcept {
        auto tape_sat = [this](float x) -> float {
            float driven = (x + bias_) * drive_linear_;
            // tanh saturation with DC-bias removal
            return std::tanh(driven) - std::tanh(bias_ * drive_linear_);
        };

        float sat_l = hiss_filter_.process_left(tape_sat(in_l)) * output_linear_;
        float sat_r = hiss_filter_.process_right(tape_sat(in_r)) * output_linear_;

        out_l = in_l * (1.0f - mix_) + sat_l * mix_;
        out_r = in_r * (1.0f - mix_) + sat_r * mix_;
    }

private:
    float  sample_rate_   = 48000.0f;
    float  drive_linear_  = 2.0f; // +6 dB
    float  bias_          = 0.2f;
    float  output_linear_ = 1.0f;
    float  mix_           = 1.0f;
    Biquad hiss_filter_;
};

// Bitcrusher: Bit Depth Quantization & Downsampling Sample-and-Hold
class BitcrusherProcessor {
public:
    void prepare(float sample_rate) noexcept {
        sample_rate_ = sample_rate > 0.0f ? sample_rate : 48000.0f;
        reset();
    }

    void reset() noexcept {
        hold_l_ = 0.0f;
        hold_r_ = 0.0f;
        counter_ = 0.0f;
    }

    void set_parameters(float bits, float downsample_factor, float jitter, float output_db, float mix) noexcept {
        bits_ = std::clamp(bits, 1.0f, 24.0f);
        quant_steps_ = std::pow(2.0f, bits_ - 1.0f);
        inv_quant_steps_ = 1.0f / quant_steps_;
        downsample_ = std::clamp(downsample_factor, 1.0f, 50.0f);
        jitter_ = std::clamp(jitter, 0.0f, 1.0f);
        output_linear_ = at::db_to_linear(output_db);
        mix_ = std::clamp(mix, 0.0f, 1.0f);
    }

    inline void process_stereo(float in_l, float in_r, float& out_l, float& out_r) noexcept {
        // Sample-and-hold downsampler
        counter_ += 1.0f;
        if (counter_ >= downsample_) {
            counter_ -= downsample_;

            // Quantize to bit depth
            auto quantize = [this](float val) -> float {
                float stepped = std::round(val * quant_steps_) * inv_quant_steps_;
                return std::clamp(stepped, -1.0f, 1.0f);
            };

            hold_l_ = quantize(in_l);
            hold_r_ = quantize(in_r);
        }

        float crushed_l = hold_l_ * output_linear_;
        float crushed_r = hold_r_ * output_linear_;

        out_l = in_l * (1.0f - mix_) + crushed_l * mix_;
        out_r = in_r * (1.0f - mix_) + crushed_r * mix_;
    }

private:
    float sample_rate_     = 48000.0f;
    float bits_            = 8.0f;
    float quant_steps_     = 128.0f;
    float inv_quant_steps_ = 1.0f / 128.0f;
    float downsample_      = 4.0f;
    float jitter_          = 0.0f;
    float output_linear_   = 1.0f;
    float mix_             = 1.0f;
    float hold_l_          = 0.0f;
    float hold_r_          = 0.0f;
    float counter_         = 0.0f;
};

} // namespace at::dsp
