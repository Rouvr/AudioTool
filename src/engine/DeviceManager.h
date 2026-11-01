#pragma once
// ============================================================================
// AudioTool (AT) — HardwareDeviceManager (Hidden Master WASAPI & Virtual Nodes)
// ============================================================================

#include "at_sdk/at_abi.h"
#include "at_sdk/at_simd_buffer.h"
#include "common/SpscRingBuffer.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace at {

struct AudioDeviceInfo {
    std::string endpoint_id;   // Windows WASAPI Endpoint ID or "virtual://..."
    std::string friendly_name; // Human-readable device name
    bool        is_input_capture = false;
    bool        is_output_render = false;
    bool        is_default       = false;
    uint32_t    native_sample_rate = 48000;
    uint32_t    native_channels    = 2;
};

struct HiddenMasterDevice {
    std::string           endpoint_id;
    std::string           friendly_name;
    bool                  is_input_capture = false;
    bool                  is_loopback      = false;
    bool                  is_virtual       = false;
    uint32_t              ref_count        = 0;

    // Ring buffer bridging WASAPI thread / Virtual test harness <-> Real-Time Graph AudioThread
    SpscAudioRingBuffer   ring_buffer{16384, 2};

    // Per-tick planar stereo bus shared by all nodes referencing this Hidden Master Device
    AlignedPlanarBuffer   tick_bus{2, 512};

    // WASAPI worker thread state (for physical/OS endpoints)
    std::atomic<bool>     running{false};
    std::thread           wasapi_thread;
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
    AT_DeviceStreamHandle acquire_device(
        const char* endpoint_id,
        bool is_input_capture,
        bool is_loopback);

    // Decrements ref_count and stops device stream when ref_count reaches 0
    void release_device(AT_DeviceStreamHandle handle);

    // Called at the start of every AudioThread tick
    void begin_audio_tick(uint32_t num_frames);

    // Called by VirtualInputNode during process_audio()
    AT_Result read_hardware_input(
        AT_DeviceStreamHandle handle,
        float* const* out_planar_channels,
        uint32_t num_channels,
        uint32_t num_frames);

    // Called by OutputNode during process_audio() (accumulates += into Hidden Master Output bus)
    AT_Result write_hardware_output(
        AT_DeviceStreamHandle handle,
        const float* const* in_planar_channels,
        uint32_t num_channels,
        uint32_t num_frames);

    // Called at the end of every AudioThread tick
    void end_audio_tick(uint32_t num_frames);

    // --- Automated Testing / Virtual Endpoint Helpers ---
    // Pushes synthetic planar frames into a virtual input device's ring buffer
    uint32_t inject_virtual_input_frames(
        const std::string& endpoint_id,
        const float* const* in_planar_channels,
        uint32_t num_channels,
        uint32_t num_frames);

    // Pops rendered planar frames from a virtual output device's ring buffer
    uint32_t extract_virtual_output_frames(
        const std::string& endpoint_id,
        float* const* out_planar_channels,
        uint32_t num_channels,
        uint32_t num_frames);

private:
    void start_wasapi_stream(HiddenMasterDevice* dev);
    void stop_wasapi_stream(HiddenMasterDevice* dev);
    void wasapi_worker_loop(HiddenMasterDevice* dev);

    mutable std::mutex                                              mutex_;
    std::vector<std::unique_ptr<HiddenMasterDevice>>                devices_;
    uint32_t                                                        sample_rate_        = 48000;
    uint32_t                                                        buffer_size_frames_ = 256;
};

} // namespace at
