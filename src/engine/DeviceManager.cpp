#include "engine/DeviceManager.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <avrt.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace at {

namespace {

std::string wide_to_utf8(const wchar_t* wstr) {
    if (!wstr || !*wstr) return {};
    int len = static_cast<int>( std::wcslen(wstr) );
    int needed = WideCharToMultiByte(CP_UTF8, 0, wstr, len, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr, len, out.data(), needed, nullptr, nullptr);
    return out;
}

std::wstring utf8_to_wide(const std::string& str) {
    if (str.empty()) return {};
    int needed = MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring out(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), out.data(), needed);
    return out;
}

class ComInitGuard {
public:
    ComInitGuard() {
        hr_ = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    }
    ~ComInitGuard() {
        if (SUCCEEDED(hr_)) {
            CoUninitialize();
        }
    }
private:
    HRESULT hr_ = E_FAIL;
};

} // namespace

DeviceManager::DeviceManager() = default;

DeviceManager::~DeviceManager() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& dev : devices_) {
        stop_wasapi_stream(dev.get());
    }
    devices_.clear();
}

std::vector<AudioDeviceInfo> DeviceManager::enumerate_devices() const {
    std::vector<AudioDeviceInfo> list;

    // Always include deterministic virtual endpoints for automated self-testing
    list.push_back(AudioDeviceInfo{
        "virtual://test_input",
        "Virtual Test Input (Synthetic Loopback Source)",
        true, false, false, sample_rate_, 2
    });
    list.push_back(AudioDeviceInfo{
        "virtual://test_output",
        "Virtual Test Output (Synthetic Loopback Sink)",
        false, true, false, sample_rate_, 2
    });

    ComInitGuard com_guard;
    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        nullptr,
        CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator),
        reinterpret_cast<void**>(&enumerator));

    if (FAILED(hr) || !enumerator) {
        return list;
    }

    auto enum_flow = [&](EDataFlow flow, bool is_input) {
        std::wstring default_id;
        IMMDevice* def_dev = nullptr;
        if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(flow, eConsole, &def_dev)) && def_dev) {
            LPWSTR id_str = nullptr;
            if (SUCCEEDED(def_dev->GetId(&id_str)) && id_str) {
                default_id = id_str;
                CoTaskMemFree(id_str);
            }
            def_dev->Release();
        }

        IMMDeviceCollection* collection = nullptr;
        if (SUCCEEDED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &collection)) && collection) {
            UINT count = 0;
            collection->GetCount(&count);
            for (UINT i = 0; i < count; ++i) {
                IMMDevice* dev = nullptr;
                if (SUCCEEDED(collection->Item(i, &dev)) && dev) {
                    LPWSTR id_w = nullptr;
                    std::wstring dev_id_w;
                    if (SUCCEEDED(dev->GetId(&id_w)) && id_w) {
                        dev_id_w = id_w;
                        CoTaskMemFree(id_w);
                    }

                    std::string friendly = wide_to_utf8(dev_id_w.c_str());
                    IPropertyStore* props = nullptr;
                    if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props)) && props) {
                        PROPVARIANT var_name;
                        PropVariantInit(&var_name);
                        if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &var_name)) &&
                            var_name.vt == VT_LPWSTR && var_name.pwszVal) {
                            friendly = wide_to_utf8(var_name.pwszVal);
                        }
                        PropVariantClear(&var_name);
                        props->Release();
                    }

                    uint32_t native_sr = 48000;
                    uint32_t native_ch = 2;
                    IAudioClient* audio_client = nullptr;
                    if (SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&audio_client))) && audio_client) {
                        WAVEFORMATEX* wfx = nullptr;
                        if (SUCCEEDED(audio_client->GetMixFormat(&wfx)) && wfx) {
                            native_sr = wfx->nSamplesPerSec;
                            native_ch = wfx->nChannels;
                            CoTaskMemFree(wfx);
                        }
                        audio_client->Release();
                    }

                    AudioDeviceInfo info;
                    info.endpoint_id        = wide_to_utf8(dev_id_w.c_str());
                    info.friendly_name      = friendly;
                    info.is_input_capture   = is_input;
                    info.is_output_render   = !is_input;
                    info.is_default         = (!default_id.empty() && dev_id_w == default_id);
                    info.native_sample_rate = native_sr;
                    info.native_channels    = native_ch;
                    list.push_back(std::move(info));

                    dev->Release();
                }
            }
            collection->Release();
        }
    };

    enum_flow(eCapture, true);
    enum_flow(eRender, false);

    enumerator->Release();
    return list;
}

void DeviceManager::prepare_all(uint32_t sample_rate, uint32_t buffer_size_frames) {
    std::lock_guard<std::mutex> lock(mutex_);
    const bool sr_changed = (sample_rate_ != sample_rate);
    sample_rate_ = sample_rate;
    buffer_size_frames_ = buffer_size_frames;

    for (auto& dev : devices_) {
        dev->tick_bus.resize(2, buffer_size_frames_);
        if (sr_changed && !dev->is_virtual && dev->running.load()) {
            stop_wasapi_stream(dev.get());
            start_wasapi_stream(dev.get());
        }
    }
}

AT_DeviceStreamHandle DeviceManager::acquire_device(
    const char* endpoint_id,
    bool is_input_capture,
    bool is_loopback) {
    if (!endpoint_id || !*endpoint_id) {
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    std::string id_str(endpoint_id);

    // Check if Hidden Master Device already exists for this endpoint ID & direction
    for (auto& dev : devices_) {
        if (dev->endpoint_id == id_str &&
            dev->is_input_capture == is_input_capture &&
            dev->is_loopback == is_loopback) {
            dev->ref_count++;
            return reinterpret_cast<AT_DeviceStreamHandle>(dev.get());
        }
    }

    // Create a new Hidden Master Device
    auto new_dev = std::make_unique<HiddenMasterDevice>();
    new_dev->endpoint_id      = id_str;
    new_dev->friendly_name    = id_str;
    new_dev->is_input_capture = is_input_capture;
    new_dev->is_loopback      = is_loopback;
    new_dev->is_virtual       = (id_str.rfind("virtual://", 0) == 0);
    new_dev->ref_count        = 1;
    new_dev->ring_buffer.resize(std::max<uint32_t>(16384u, buffer_size_frames_ * 16u), 2);
    new_dev->tick_bus.resize(2, buffer_size_frames_);

    HiddenMasterDevice* raw_ptr = new_dev.get();
    devices_.push_back(std::move(new_dev));

    if (!raw_ptr->is_virtual) {
        start_wasapi_stream(raw_ptr);
    }

    return reinterpret_cast<AT_DeviceStreamHandle>(raw_ptr);
}

void DeviceManager::release_device(AT_DeviceStreamHandle handle) {
    if (!handle) return;
    std::lock_guard<std::mutex> lock(mutex_);
    auto* target = reinterpret_cast<HiddenMasterDevice*>(handle);

    for (auto it = devices_.begin(); it != devices_.end(); ++it) {
        if (it->get() == target) {
            if (target->ref_count > 0) {
                target->ref_count--;
            }
            if (target->ref_count == 0) {
                stop_wasapi_stream(target);
                devices_.erase(it);
            }
            return;
        }
    }
}

void DeviceManager::begin_audio_tick(uint32_t num_frames) {
    for (auto& dev : devices_) {
        if (dev->tick_bus.num_frames() < num_frames) {
            dev->tick_bus.resize(2, num_frames);
        }
        if (dev->is_input_capture) {
            // Pull captured frames from the Hidden Master Input's ring buffer into tick_bus
            dev->ring_buffer.pop_planar(dev->tick_bus.channel_ptrs(), 2, num_frames);
        } else {
            // Zero-initialize the Hidden Master Output's accumulation bus before nodes write to it
            dev->tick_bus.clear();
        }
    }
}

AT_Result DeviceManager::read_hardware_input(
    AT_DeviceStreamHandle handle,
    float* const* out_planar_channels,
    uint32_t num_channels,
    uint32_t num_frames) {
    if (!handle || !out_planar_channels) {
        return AT_ERR_INVALID_ARG;
    }
    auto* dev = reinterpret_cast<HiddenMasterDevice*>(handle);
    if (!dev->is_input_capture) {
        return AT_ERR_INVALID_ARG;
    }

    const uint32_t frames = std::min(num_frames, dev->tick_bus.num_frames());
    for (uint32_t ch = 0; ch < num_channels; ++ch) {
        const uint32_t src_ch = std::min(ch, dev->tick_bus.num_channels() - 1);
        std::memcpy(out_planar_channels[ch], dev->tick_bus.channel(src_ch), frames * sizeof(float));
        if (frames < num_frames) {
            std::memset(out_planar_channels[ch] + frames, 0, (num_frames - frames) * sizeof(float));
        }
    }
    return AT_OK;
}

AT_Result DeviceManager::write_hardware_output(
    AT_DeviceStreamHandle handle,
    const float* const* in_planar_channels,
    uint32_t num_channels,
    uint32_t num_frames) {
    if (!handle || !in_planar_channels || num_channels == 0) {
        return AT_ERR_INVALID_ARG;
    }
    auto* dev = reinterpret_cast<HiddenMasterDevice*>(handle);
    if (dev->is_input_capture) {
        return AT_ERR_INVALID_ARG;
    }

    const uint32_t frames = std::min(num_frames, dev->tick_bus.num_frames());
    for (uint32_t ch = 0; ch < dev->tick_bus.num_channels(); ++ch) {
        const uint32_t src_ch = std::min(ch, num_channels - 1);
        float* dst = dev->tick_bus.channel(ch);
        const float* src = in_planar_channels[src_ch];
        if (!src) continue;
        for (uint32_t f = 0; f < frames; ++f) {
            dst[f] += src[f];
        }
    }
    return AT_OK;
}

void DeviceManager::end_audio_tick(uint32_t num_frames) {
    for (auto& dev : devices_) {
        if (!dev->is_input_capture) {
            const uint32_t frames = std::min(num_frames, dev->tick_bus.num_frames());
            // For physical hardware outputs, apply safety clipping [-1.0f, +1.0f]
            if (!dev->is_virtual) {
                for (uint32_t ch = 0; ch < dev->tick_bus.num_channels(); ++ch) {
                    float* ptr = dev->tick_bus.channel(ch);
                    for (uint32_t f = 0; f < frames; ++f) {
                        ptr[f] = std::clamp(ptr[f], -1.0f, 1.0f);
                    }
                }
            }
            dev->ring_buffer.push_planar(dev->tick_bus.channel_ptrs(), 2, frames);
        }
    }
}

uint32_t DeviceManager::inject_virtual_input_frames(
    const std::string& endpoint_id,
    const float* const* in_planar_channels,
    uint32_t num_channels,
    uint32_t num_frames) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& dev : devices_) {
        if (dev->endpoint_id == endpoint_id && dev->is_input_capture) {
            return dev->ring_buffer.push_planar(in_planar_channels, num_channels, num_frames);
        }
    }
    return 0;
}

uint32_t DeviceManager::extract_virtual_output_frames(
    const std::string& endpoint_id,
    float* const* out_planar_channels,
    uint32_t num_channels,
    uint32_t num_frames) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& dev : devices_) {
        if (dev->endpoint_id == endpoint_id && !dev->is_input_capture) {
            return dev->ring_buffer.pop_planar(out_planar_channels, num_channels, num_frames);
        }
    }
    return 0;
}

void DeviceManager::start_wasapi_stream(HiddenMasterDevice* dev) {
    if (!dev || dev->is_virtual || dev->running.load()) return;
    dev->running.store(true);
    dev->wasapi_thread = std::thread(&DeviceManager::wasapi_worker_loop, this, dev);
}

void DeviceManager::stop_wasapi_stream(HiddenMasterDevice* dev) {
    if (!dev) return;
    dev->running.store(false);
    if (dev->wasapi_thread.joinable()) {
        dev->wasapi_thread.join();
    }
}

void DeviceManager::wasapi_worker_loop(HiddenMasterDevice* dev) {
    ComInitGuard com_guard;

    DWORD task_idx = 0;
    HANDLE hMmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_idx);

    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        nullptr,
        CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator),
        reinterpret_cast<void**>(&enumerator));

    if (FAILED(hr) || !enumerator) {
        if (hMmcss) AvRevertMmThreadCharacteristics(hMmcss);
        return;
    }

    IMMDevice* mm_dev = nullptr;
    if (dev->endpoint_id == "default_input") {
        hr = enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &mm_dev);
    } else if (dev->endpoint_id == "default_output" || dev->endpoint_id == "default_loopback") {
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &mm_dev);
    } else {
        std::wstring wide_id = utf8_to_wide(dev->endpoint_id);
        hr = enumerator->GetDevice(wide_id.c_str(), &mm_dev);
    }

    if (FAILED(hr) || !mm_dev) {
        enumerator->Release();
        if (hMmcss) AvRevertMmThreadCharacteristics(hMmcss);
        return;
    }

    IAudioClient* audio_client = nullptr;
    hr = mm_dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&audio_client));
    if (FAILED(hr) || !audio_client) {
        mm_dev->Release();
        enumerator->Release();
        if (hMmcss) AvRevertMmThreadCharacteristics(hMmcss);
        return;
    }

    // Request 32-bit IEEE float Stereo at the engine's internal sample rate with WASAPI AutoConvertPCM
    WAVEFORMATEX desired_fmt = {};
    desired_fmt.wFormatTag      = WAVE_FORMAT_IEEE_FLOAT;
    desired_fmt.nChannels       = 2;
    desired_fmt.nSamplesPerSec  = sample_rate_;
    desired_fmt.wBitsPerSample  = 32;
    desired_fmt.nBlockAlign     = (desired_fmt.nChannels * desired_fmt.wBitsPerSample) / 8;
    desired_fmt.nAvgBytesPerSec = desired_fmt.nSamplesPerSec * desired_fmt.nBlockAlign;
    desired_fmt.cbSize          = 0;

    DWORD stream_flags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    if (dev->is_input_capture && dev->is_loopback) {
        stream_flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;
    }

    // 20 ms hardware buffer duration (in 100-ns units)
    REFERENCE_TIME hns_buffer_duration = 200000;
    hr = audio_client->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        stream_flags,
        hns_buffer_duration,
        0,
        &desired_fmt,
        nullptr);

    if (FAILED(hr)) {
        audio_client->Release();
        mm_dev->Release();
        enumerator->Release();
        if (hMmcss) AvRevertMmThreadCharacteristics(hMmcss);
        return;
    }

    UINT32 buffer_frame_count = 0;
    audio_client->GetBufferSize(&buffer_frame_count);

    IAudioCaptureClient* capture_client = nullptr;
    IAudioRenderClient*  render_client  = nullptr;

    if (dev->is_input_capture) {
        hr = audio_client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&capture_client));
    } else {
        hr = audio_client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&render_client));
    }

    if (SUCCEEDED(hr)) {
        audio_client->Start();

        while (dev->running.load(std::memory_order_relaxed)) {
            if (dev->is_input_capture && capture_client) {
                UINT32 packet_length = 0;
                while (SUCCEEDED(capture_client->GetNextPacketSize(&packet_length)) && packet_length > 0) {
                    BYTE* data = nullptr;
                    UINT32 num_frames_available = 0;
                    DWORD flags = 0;
                    if (SUCCEEDED(capture_client->GetBuffer(&data, &num_frames_available, &flags, nullptr, nullptr))) {
                        if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) || !data) {
                            std::vector<float> silence(static_cast<size_t>(num_frames_available) * 2, 0.0f);
                            dev->ring_buffer.push_interleaved(silence.data(), 2, num_frames_available);
                        } else {
                            dev->ring_buffer.push_interleaved(
                                reinterpret_cast<const float*>(data),
                                2,
                                num_frames_available);
                        }
                        capture_client->ReleaseBuffer(num_frames_available);
                    } else {
                        break;
                    }
                }
            } else if (!dev->is_input_capture && render_client) {
                UINT32 padding = 0;
                if (SUCCEEDED(audio_client->GetCurrentPadding(&padding))) {
                    UINT32 frames_avail = (buffer_frame_count > padding) ? (buffer_frame_count - padding) : 0;
                    if (frames_avail > 0) {
                        BYTE* data = nullptr;
                        if (SUCCEEDED(render_client->GetBuffer(frames_avail, &data)) && data) {
                            dev->ring_buffer.pop_interleaved(
                                reinterpret_cast<float*>(data),
                                2,
                                frames_avail);
                            render_client->ReleaseBuffer(frames_avail, 0);
                        }
                    }
                }
            }
            Sleep(2);
        }

        audio_client->Stop();
    }

    if (capture_client) capture_client->Release();
    if (render_client) render_client->Release();
    audio_client->Release();
    mm_dev->Release();
    enumerator->Release();
    if (hMmcss) AvRevertMmThreadCharacteristics(hMmcss);
}

} // namespace at
