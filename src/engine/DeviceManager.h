#pragma once
// ============================================================================
// AudioTool (AT) — HardwareDeviceManager (Hidden Master WASAPI & Virtual Nodes)
//
// One HiddenMasterDevice exists per (endpoint key, direction). Any number of
// nodes in any number of profiles can reference it (ref-counted). Capture
// devices fan out a single per-tick bus (zero-copy reads), render devices
// accumulate (+=) all writers into a single per-tick bus.
//
// Threading:
//   * acquire/release/prepare_all and begin/end_audio_tick are called with the
//     AudioEngine graph mutex held (never concurrently with each other).
//   * Each physical device has its own WASAPI worker thread which talks to the
//     engine exclusively through a lock-free SPSC ring buffer.
// ============================================================================

#include "at_sdk/at_abi.h"
#include "at_sdk/at_simd_buffer.h"
#include "common/SpscRingBuffer.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace at {

struct AudioDeviceInfo {
    std::string endpoint_id;   // Windows WASAPI Endpoint ID or "virtual://..."
    std::string friendly_name; // Human-readable device name
    bool        is_input_capture   = false;
    bool        is_output_render   = false;
    bool        is_default         = false;
    uint32_t    native_sample_rate = 48000;
    uint32_t    native_channels    = 2;
};

struct HiddenMasterDevice {
    std::string           endpoint_key;     // Key as requested by nodes (may include "loopback:")
    std::string           os_endpoint_id;   // Key with prefix stripped
    bool                  is_input_capture = false;
    bool                  is_loopback      = false;
    bool                  is_virtual       = false;
    uint32_t              ref_count        = 0;

    // Ring buffer bridging WASAPI thread / virtual test harness <-> engine audio thread
    SpscAudioRingBuffer   ring_buffer{16384, 2};

    // Per-tick planar stereo bus shared by all nodes referencing this device
    AlignedPlanarBuffer   tick_bus{2, 512};

    // Consumer-side priming state (engine for capture, WASAPI thread for render)
    bool                  primed = false;

    // Diagnostics
    std::atomic<int32_t>  status{AT_DEVICE_STATUS_NONE};
    std::atomic<uint32_t> underruns{0};
    std::atomic<uint32_t> drops{0};
    std::mutex            status_mutex;
    std::string           status_message;

    // WASAPI worker thread state (physical endpoints only)
    std::atomic<bool>     running{false};
    std::thread           wasapi_thread;

    void set_status(int32_t s, const std::string& msg) {
        {
            std::lock_guard<std::mutex> lock(status_mutex);
            status_message = msg;
        }
        status.store(s, std::memory_order_release);
    }
};

class DeviceManager {
public:
    DeviceManager();
    ~DeviceManager();

    // Enumerates available Windows WASAPI Capture & Render endpoints (+ virtual test endpoints)
    std::vector<AudioDeviceInfo> enumerate_devices() const;

    // Prepares internal tick buses for a new engine sample rate / buffer size
    void prepare_all(uint32_t sample_rate, uint32_t buffer_size_frames);

    // Acquires (or increments ref_count on) a Hidden Master Device
    AT_DeviceStreamHandle acquire_device(const char* endpoint_id, bool is_input_capture, bool is_loopback);

    // Decrements ref_count and stops device stream when ref_count reaches 0
    void release_device(AT_DeviceStreamHandle handle);

    // Returns AT_DeviceStatus and a short message
    int32_t get_device_status(AT_DeviceStreamHandle handle, std::string* out_message) const;

    // Called at the start of every engine tick
    void begin_audio_tick(uint32_t num_frames);

    // Called by input nodes during process_audio()
    AT_Result read_hardware_input(
        AT_DeviceStreamHandle handle, float* const* out_planar_channels, uint32_t num_channels, uint32_t num_frames);

    // Called by output nodes during process_audio() (accumulates += into the device bus)
    AT_Result write_hardware_output(
        AT_DeviceStreamHandle handle, const float* const* in_planar_channels, uint32_t num_channels, uint32_t num_frames);

    // Called at the end of every engine tick
    void end_audio_tick(uint32_t num_frames);

    // Number of currently open hidden master devices
    size_t open_device_count() const;

    // --- Automated Testing / Virtual Endpoint Helpers ---
    uint32_t inject_virtual_input_frames(
        const std::string& endpoint_id, const float* const* in_planar_channels, uint32_t num_channels, uint32_t num_frames);
    uint32_t extract_virtual_output_frames(
        const std::string& endpoint_id, float* const* out_planar_channels, uint32_t num_channels, uint32_t num_frames);

private:
    void start_wasapi_stream(HiddenMasterDevice* dev);
    void stop_wasapi_stream(HiddenMasterDevice* dev);
    void wasapi_worker_loop(HiddenMasterDevice* dev);

    mutable std::mutex                               mutex_;
    std::vector<std::unique_ptr<HiddenMasterDevice>> devices_;
    std::atomic<uint32_t>                            sample_rate_{48000};
    std::atomic<uint32_t>                            buffer_size_frames_{256};
};

} // namespace at
