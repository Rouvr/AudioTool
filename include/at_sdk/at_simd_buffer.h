#pragma once
// ============================================================================
// AudioTool (AT) — SIMD Planar Buffer & Real-Time DSP Utilities
// ============================================================================

#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <vector>

#if defined(_MSC_VER)
    #include <malloc.h>
    #include <immintrin.h>
#endif

namespace at {

// Enables SSE/AVX Denormals-Are-Zero (DAZ) and Flush-To-Zero (FTZ) on the calling thread
inline void enable_realtime_fpu_flags() noexcept {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    _mm_setcsr(_mm_getcsr() | 0x8040);
#endif
}

// Converts decibels (dB) to linear voltage amplitude multiplier
inline float db_to_linear(float db, float min_silence_db = -96.0f) noexcept {
    if (db <= min_silence_db) {
        return 0.0f;
    }
    return std::pow(10.0f, db * 0.05f);
}

// Converts linear amplitude to decibels (dB)
inline float linear_to_db(float linear, float min_db = -96.0f) noexcept {
    if (linear <= 1e-5f) {
        return min_db;
    }
    float db = 20.0f * std::log10(linear);
    return std::max(db, min_db);
}

// Per-sample linear parameter smoother to eliminate zipper noise / clicks
class SmoothedFloat {
public:
    SmoothedFloat(float initial_val = 1.0f) noexcept
        : current_(initial_val), target_(initial_val), step_(0.0f), steps_remaining_(0), ramp_samples_(240) {}

    void prepare(uint32_t sample_rate, float ramp_time_ms = 5.0f) noexcept {
        ramp_samples_ = std::max<uint32_t>(1u, static_cast<uint32_t>((sample_rate * ramp_time_ms) / 1000.0f));
        current_ = target_;
        steps_remaining_ = 0;
    }

    void set_immediate(float val) noexcept {
        current_ = val;
        target_ = val;
        step_ = 0.0f;
        steps_remaining_ = 0;
    }

    void set_target(float new_target) noexcept {
        if (std::abs(new_target - target_) < 1e-6f) {
            return;
        }
        target_ = new_target;
        if (ramp_samples_ <= 1) {
            current_ = target_;
            steps_remaining_ = 0;
        } else {
            step_ = (target_ - current_) / static_cast<float>(ramp_samples_);
            steps_remaining_ = ramp_samples_;
        }
    }

    float next_value() noexcept {
        if (steps_remaining_ > 0) {
            current_ += step_;
            --steps_remaining_;
            if (steps_remaining_ == 0) {
                current_ = target_;
            }
        }
        return current_;
    }

    float current_value() const noexcept { return current_; }
    float target_value() const noexcept { return target_; }
    bool is_smoothing() const noexcept { return steps_remaining_ > 0; }

private:
    float    current_;
    float    target_;
    float    step_;
    uint32_t steps_remaining_;
    uint32_t ramp_samples_;
};

// 64-byte SIMD-aligned planar multi-channel audio buffer
class AlignedPlanarBuffer {
public:
    AlignedPlanarBuffer() = default;

    AlignedPlanarBuffer(uint32_t num_channels, uint32_t num_frames) {
        resize(num_channels, num_frames);
    }

    ~AlignedPlanarBuffer() {
        free_memory();
    }

    AlignedPlanarBuffer(const AlignedPlanarBuffer&) = delete;
    AlignedPlanarBuffer& operator=(const AlignedPlanarBuffer&) = delete;

    AlignedPlanarBuffer(AlignedPlanarBuffer&& other) noexcept
        : data_block_(other.data_block_),
          channel_ptrs_(std::move(other.channel_ptrs_)),
          num_channels_(other.num_channels_),
          num_frames_(other.num_frames_),
          stride_floats_(other.stride_floats_) {
        other.data_block_ = nullptr;
        other.num_channels_ = 0;
        other.num_frames_ = 0;
    }

    AlignedPlanarBuffer& operator=(AlignedPlanarBuffer&& other) noexcept {
        if (this != &other) {
            free_memory();
            data_block_ = other.data_block_;
            channel_ptrs_ = std::move(other.channel_ptrs_);
            num_channels_ = other.num_channels_;
            num_frames_ = other.num_frames_;
            stride_floats_ = other.stride_floats_;
            other.data_block_ = nullptr;
            other.num_channels_ = 0;
            other.num_frames_ = 0;
        }
        return *this;
    }

    void resize(uint32_t num_channels, uint32_t num_frames) {
        free_memory();
        num_channels_ = num_channels;
        num_frames_ = num_frames;
        if (num_channels == 0 || num_frames == 0) {
            return;
        }
        // Round stride up to multiple of 16 floats (64 bytes)
        stride_floats_ = (num_frames + 15u) & ~15u;
        size_t total_bytes = static_cast<size_t>(num_channels_) * stride_floats_ * sizeof(float);
#if defined(_MSC_VER)
        data_block_ = static_cast<float*>(_aligned_malloc(total_bytes, 64));
#else
        data_block_ = static_cast<float*>(std::aligned_alloc(64, total_bytes));
#endif
        std::memset(data_block_, 0, total_bytes);
        channel_ptrs_.resize(num_channels_);
        for (uint32_t ch = 0; ch < num_channels_; ++ch) {
            channel_ptrs_[ch] = data_block_ + static_cast<size_t>(ch) * stride_floats_;
        }
    }

    void clear() noexcept {
        if (data_block_ && num_channels_ > 0 && stride_floats_ > 0) {
            std::memset(data_block_, 0, static_cast<size_t>(num_channels_) * stride_floats_ * sizeof(float));
        }
    }

    uint32_t num_channels() const noexcept { return num_channels_; }
    uint32_t num_frames() const noexcept { return num_frames_; }
    float* channel(uint32_t ch) noexcept { return channel_ptrs_[ch]; }
    const float* channel(uint32_t ch) const noexcept { return channel_ptrs_[ch]; }
    float* const* channel_ptrs() noexcept { return channel_ptrs_.data(); }
    const float* const* channel_ptrs() const noexcept { return channel_ptrs_.data(); }

private:
    void free_memory() noexcept {
        if (data_block_) {
#if defined(_MSC_VER)
            _aligned_free(data_block_);
#else
            std::free(data_block_);
#endif
            data_block_ = nullptr;
        }
        channel_ptrs_.clear();
    }

    float*              data_block_    = nullptr;
    std::vector<float*> channel_ptrs_;
    uint32_t            num_channels_  = 0;
    uint32_t            num_frames_    = 0;
    uint32_t            stride_floats_ = 0;
};

} // namespace at

