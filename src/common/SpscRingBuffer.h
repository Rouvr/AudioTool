#pragma once
// ============================================================================
// AudioTool (AT) — Lock-Free SPSC Audio Ring Buffer & Event Queue
// ============================================================================

#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>
#include <algorithm>

namespace at {

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)
#endif

// Lock-free Single-Producer Single-Consumer (SPSC) Ring Buffer for interleaved or planar stereo frames
class SpscAudioRingBuffer {
public:
    explicit SpscAudioRingBuffer(uint32_t capacity_frames = 8192, uint32_t channels = 2) {
        resize(capacity_frames, channels);
    }

    void resize(uint32_t capacity_frames, uint32_t channels) {
        // Round capacity up to power of 2 for fast mask wrapping
        uint32_t pow2 = 1;
        while (pow2 < capacity_frames) {
            pow2 <<= 1;
        }
        capacity_frames_ = pow2;
        mask_ = pow2 - 1;
        channels_ = std::max<uint32_t>(1u, channels);
        buffer_.assign(static_cast<size_t>(capacity_frames_) * channels_, 0.0f);
        write_pos_.store(0, std::memory_order_relaxed);
        read_pos_.store(0, std::memory_order_relaxed);
    }

    void clear() noexcept {
        write_pos_.store(0, std::memory_order_relaxed);
        read_pos_.store(0, std::memory_order_relaxed);
    }

    uint32_t available_to_read() const noexcept {
        const uint32_t w = write_pos_.load(std::memory_order_acquire);
        const uint32_t r = read_pos_.load(std::memory_order_relaxed);
        return w - r;
    }

    uint32_t available_to_write() const noexcept {
        const uint32_t w = write_pos_.load(std::memory_order_relaxed);
        const uint32_t r = read_pos_.load(std::memory_order_acquire);
        return capacity_frames_ - (w - r);
    }

    uint32_t capacity_frames() const noexcept { return capacity_frames_; }
    uint32_t channels() const noexcept { return channels_; }

    // Consumer-side only: discards up to num_frames of the oldest frames.
    // Used for latency clamping when a producer clock runs faster than the consumer clock.
    uint32_t skip(uint32_t num_frames) noexcept {
        const uint32_t to_skip = std::min(num_frames, available_to_read());
        const uint32_t r = read_pos_.load(std::memory_order_relaxed);
        read_pos_.store(r + to_skip, std::memory_order_release);
        return to_skip;
    }

    // Push planar stereo/multi-channel frames into the ring buffer
    uint32_t push_planar(const float* const* in_channels, uint32_t num_channels, uint32_t num_frames) noexcept {
        const uint32_t avail = available_to_write();
        const uint32_t to_write = std::min(num_frames, avail);
        const uint32_t w = write_pos_.load(std::memory_order_relaxed);

        for (uint32_t f = 0; f < to_write; ++f) {
            const size_t idx = static_cast<size_t>((w + f) & mask_) * channels_;
            for (uint32_t ch = 0; ch < channels_; ++ch) {
                const uint32_t src_ch = (ch < num_channels) ? ch : (num_channels - 1);
                buffer_[idx + ch] = in_channels[src_ch] ? in_channels[src_ch][f] : 0.0f;
            }
        }

        write_pos_.store(w + to_write, std::memory_order_release);
        return to_write;
    }

    // Push interleaved frames into the ring buffer
    uint32_t push_interleaved(const float* interleaved_data, uint32_t src_channels, uint32_t num_frames) noexcept {
        const uint32_t avail = available_to_write();
        const uint32_t to_write = std::min(num_frames, avail);
        const uint32_t w = write_pos_.load(std::memory_order_relaxed);

        for (uint32_t f = 0; f < to_write; ++f) {
            const size_t dst_idx = static_cast<size_t>((w + f) & mask_) * channels_;
            const size_t src_idx = static_cast<size_t>(f) * src_channels;
            for (uint32_t ch = 0; ch < channels_; ++ch) {
                const uint32_t src_ch = (ch < src_channels) ? ch : (src_channels > 0 ? src_channels - 1 : 0);
                buffer_[dst_idx + ch] = (src_channels > 0) ? interleaved_data[src_idx + src_ch] : 0.0f;
            }
        }

        write_pos_.store(w + to_write, std::memory_order_release);
        return to_write;
    }

    // Pop frames into planar destination channels (zero-fills any underrun tail)
    uint32_t pop_planar(float* const* out_channels, uint32_t dst_channels, uint32_t num_frames) noexcept {
        const uint32_t avail = available_to_read();
        const uint32_t to_read = std::min(num_frames, avail);
        const uint32_t r = read_pos_.load(std::memory_order_relaxed);

        for (uint32_t f = 0; f < to_read; ++f) {
            const size_t src_idx = static_cast<size_t>((r + f) & mask_) * channels_;
            for (uint32_t ch = 0; ch < dst_channels; ++ch) {
                const uint32_t src_ch = (ch < channels_) ? ch : (channels_ - 1);
                out_channels[ch][f] = buffer_[src_idx + src_ch];
            }
        }

        // Zero-fill remaining frames if ring buffer had fewer than num_frames
        if (to_read < num_frames) {
            for (uint32_t ch = 0; ch < dst_channels; ++ch) {
                std::memset(out_channels[ch] + to_read, 0, (num_frames - to_read) * sizeof(float));
            }
        }

        read_pos_.store(r + to_read, std::memory_order_release);
        return to_read;
    }

    // Pop frames into interleaved destination buffer
    uint32_t pop_interleaved(float* out_interleaved, uint32_t dst_channels, uint32_t num_frames) noexcept {
        const uint32_t avail = available_to_read();
        const uint32_t to_read = std::min(num_frames, avail);
        const uint32_t r = read_pos_.load(std::memory_order_relaxed);

        for (uint32_t f = 0; f < to_read; ++f) {
            const size_t src_idx = static_cast<size_t>((r + f) & mask_) * channels_;
            const size_t dst_idx = static_cast<size_t>(f) * dst_channels;
            for (uint32_t ch = 0; ch < dst_channels; ++ch) {
                const uint32_t src_ch = (ch < channels_) ? ch : (channels_ - 1);
                out_interleaved[dst_idx + ch] = buffer_[src_idx + src_ch];
            }
        }

        if (to_read < num_frames) {
            std::memset(
                out_interleaved + static_cast<size_t>(to_read) * dst_channels,
                0,
                static_cast<size_t>(num_frames - to_read) * dst_channels * sizeof(float));
        }

        read_pos_.store(r + to_read, std::memory_order_release);
        return to_read;
    }

private:
    std::vector<float>    buffer_;
    uint32_t              capacity_frames_ = 0;
    uint32_t              mask_            = 0;
    uint32_t              channels_        = 2;
    alignas(64) std::atomic<uint32_t> write_pos_{0};
    alignas(64) std::atomic<uint32_t> read_pos_{0};
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

} // namespace at
