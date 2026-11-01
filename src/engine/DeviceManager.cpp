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
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <avrt.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace at {

namespace {

// Latency management (in multiples of the engine buffer size).
// Consumers wait until kPrimeBlocks are buffered before starting and discard
// frames back down to kPrimeBlocks when more than kMaxBlocks accumulate
// (crude clock-drift compensation until ASRC is implemented, see docs/07).
constexpr uint32_t kPrimeBlocks = 2;
constexpr uint32_t kMaxBlocks   = 6;

std::string wide_to_utf8(const wchar_t* wstr) {
    if (!wstr || !*wstr) return {};
    int len = static_cast<int>(std::wcslen(wstr));
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

std::string hr_to_string(HRESULT hr) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
    return buf;
}

class ComInitGuard {
public:
    ComInitGuard() { hr_ = CoInitializeEx(nullptr, COINIT_MULTITHREADED); }
    ~ComInitGuard() {
        if (SUCCEEDED(hr_)) CoUninitialize();
    }
private:
    HRESULT hr_ = E_FAIL;
};

template <typename T>
void safe_release(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

// Describes the sample layout of a WASAPI buffer
struct SampleLayout {
    bool     is_float        = true;
    uint32_t channels        = 2;
    uint32_t bits_per_sample = 32; // container size
    uint32_t block_align     = 8;
};

SampleLayout layout_from_format(const WAVEFORMATEX* wfx) {
    SampleLayout l;
    l.channels        = wfx->nChannels;
    l.bits_per_sample = wfx->wBitsPerSample;
    l.block_align     = wfx->nBlockAlign;
    l.is_float        = (wfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
    if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wfx->cbSize >= 22) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wfx);
        l.is_float = IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) != 0;
    }
    return l;
}

float read_sample(const BYTE* p, const SampleLayout& l) {
    if (l.is_float && l.bits_per_sample == 32) {
        float v;
        std::memcpy(&v, p, sizeof(float));
        return v;
    }
    switch (l.bits_per_sample) {
        case 16: {
            int16_t v;
            std::memcpy(&v, p, 2);
            return static_cast<float>(v) / 32768.0f;
        }
        case 24: {
            int32_t v = (static_cast<int32_t>(p[0]) << 8) | (static_cast<int32_t>(p[1]) << 16) | (static_cast<int32_t>(p[2]) << 24);
            return static_cast<float>(v) / 2147483648.0f;
        }
        case 32: {
            int32_t v;
            std::memcpy(&v, p, 4);
            return static_cast<float>(v) / 2147483648.0f;
        }
        default:
            return 0.0f;
    }
}

void write_sample(BYTE* p, const SampleLayout& l, float v) {
    v = std::clamp(v, -1.0f, 1.0f);
    if (l.is_float && l.bits_per_sample == 32) {
        std::memcpy(p, &v, sizeof(float));
        return;
    }
    switch (l.bits_per_sample) {
        case 16: {
            int16_t s = static_cast<int16_t>(std::lrint(v * 32767.0f));
            std::memcpy(p, &s, 2);
            break;
        }
        case 24: {
            int32_t s = static_cast<int32_t>(std::lrint(v * 8388607.0f));
            p[0] = static_cast<BYTE>(s & 0xFF);
            p[1] = static_cast<BYTE>((s >> 8) & 0xFF);
            p[2] = static_cast<BYTE>((s >> 16) & 0xFF);
            break;
        }
        case 32: {
            int32_t s = static_cast<int32_t>(std::llrint(static_cast<double>(v) * 2147483647.0));
            std::memcpy(p, &s, 4);
            break;
        }
        default:
            break;
    }
}

// Converts an arbitrary WASAPI buffer to interleaved stereo float
void to_interleaved_stereo(const BYTE* data, uint32_t frames, const SampleLayout& l, std::vector<float>& out) {
    out.resize(static_cast<size_t>(frames) * 2);
    const uint32_t bytes_per_sample = l.bits_per_sample / 8;
    for (uint32_t f = 0; f < frames; ++f) {
        const BYTE* frame = data + static_cast<size_t>(f) * l.block_align;
        const float left  = read_sample(frame, l);
        const float right = (l.channels > 1) ? read_sample(frame + bytes_per_sample, l) : left;
        out[f * 2 + 0] = left;
        out[f * 2 + 1] = right;
    }
}

// Converts interleaved stereo float to an arbitrary WASAPI buffer layout
void from_interleaved_stereo(const float* in, uint32_t frames, const SampleLayout& l, BYTE* data) {
    const uint32_t bytes_per_sample = l.bits_per_sample / 8;
    for (uint32_t f = 0; f < frames; ++f) {
        BYTE* frame = data + static_cast<size_t>(f) * l.block_align;
        const float left  = in[f * 2 + 0];
        const float right = in[f * 2 + 1];
        for (uint32_t ch = 0; ch < l.channels; ++ch) {
            float v;
            if (l.channels == 1) {
                v = 0.5f * (left + right);
            } else {
                v = (ch == 0) ? left : (ch == 1 ? right : 0.0f);
            }
            write_sample(frame + ch * bytes_per_sample, l, v);
        }
    }
}

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
    const uint32_t sr = sample_rate_.load();

    // Deterministic virtual endpoints for automated self-testing
    list.push_back(AudioDeviceInfo{"virtual://test_input", "Virtual Test Input (Synthetic Source)", true, false, false, sr, 2});
    list.push_back(AudioDeviceInfo{"virtual://test_output", "Virtual Test Output (Synthetic Sink)", false, true, false, sr, 2});

    ComInitGuard com_guard;
    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
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
                if (FAILED(collection->Item(i, &dev)) || !dev) continue;

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
                    if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &var_name)) && var_name.vt == VT_LPWSTR &&
                        var_name.pwszVal) {
                        friendly = wide_to_utf8(var_name.pwszVal);
                    }
                    PropVariantClear(&var_name);
                    props->Release();
                }

                uint32_t native_sr = 48000;
                uint32_t native_ch = 2;
                IAudioClient* audio_client = nullptr;
                if (SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&audio_client))) &&
                    audio_client) {
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
    const bool sr_changed = (sample_rate_.load() != sample_rate);
    sample_rate_.store(sample_rate);
    buffer_size_frames_.store(buffer_size_frames);

    for (auto& dev : devices_) {
        dev->tick_bus.resize(2, buffer_size_frames);
        dev->primed = false;
        if (sr_changed && !dev->is_virtual) {
            stop_wasapi_stream(dev.get());
            dev->ring_buffer.clear();
            start_wasapi_stream(dev.get());
        }
    }
}

AT_DeviceStreamHandle DeviceManager::acquire_device(const char* endpoint_id, bool is_input_capture, bool is_loopback) {
    if (!endpoint_id || !*endpoint_id) {
        return nullptr;
    }

    std::string key(endpoint_id);
    std::string os_id = key;
    const std::string loop_prefix = AT_LOOPBACK_PREFIX;
    if (key.rfind(loop_prefix, 0) == 0) {
        is_loopback = true;
        os_id = key.substr(loop_prefix.size());
    }
    if (is_loopback) {
        is_input_capture = true; // Loopback is always a capture stream on a render endpoint
    }

    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& dev : devices_) {
        if (dev->endpoint_key == key && dev->is_input_capture == is_input_capture && dev->is_loopback == is_loopback) {
            dev->ref_count++;
            return reinterpret_cast<AT_DeviceStreamHandle>(dev.get());
        }
    }

    const uint32_t bs = buffer_size_frames_.load();
    auto new_dev = std::make_unique<HiddenMasterDevice>();
    new_dev->endpoint_key     = key;
    new_dev->os_endpoint_id   = os_id;
    new_dev->is_input_capture = is_input_capture;
    new_dev->is_loopback      = is_loopback;
    new_dev->is_virtual       = (key.rfind("virtual://", 0) == 0);
    new_dev->ref_count        = 1;
    new_dev->ring_buffer.resize(std::max<uint32_t>(16384u, bs * 16u), 2);
    new_dev->tick_bus.resize(2, bs);

    HiddenMasterDevice* raw_ptr = new_dev.get();
    devices_.push_back(std::move(new_dev));

    if (raw_ptr->is_virtual) {
        raw_ptr->set_status(AT_DEVICE_STATUS_RUNNING, "Virtual endpoint");
    } else {
        start_wasapi_stream(raw_ptr);
    }
    return reinterpret_cast<AT_DeviceStreamHandle>(raw_ptr);
}

void DeviceManager::release_device(AT_DeviceStreamHandle handle) {
    if (!handle) return;
    std::unique_ptr<HiddenMasterDevice> to_destroy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto* target = reinterpret_cast<HiddenMasterDevice*>(handle);
        for (auto it = devices_.begin(); it != devices_.end(); ++it) {
            if (it->get() == target) {
                if (target->ref_count > 0) target->ref_count--;
                if (target->ref_count == 0) {
                    to_destroy = std::move(*it);
                    devices_.erase(it);
                }
                break;
            }
        }
    }
    if (to_destroy) {
        stop_wasapi_stream(to_destroy.get());
    }
}

int32_t DeviceManager::get_device_status(AT_DeviceStreamHandle handle, std::string* out_message) const {
    if (!handle) {
        if (out_message) *out_message = "No device";
        return AT_DEVICE_STATUS_NONE;
    }
    auto* dev = reinterpret_cast<HiddenMasterDevice*>(handle);
    if (out_message) {
        std::lock_guard<std::mutex> lock(dev->status_mutex);
        *out_message = dev->status_message;
    }
    return dev->status.load(std::memory_order_acquire);
}

size_t DeviceManager::open_device_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return devices_.size();
}

void DeviceManager::begin_audio_tick(uint32_t num_frames) {
    for (auto& dev : devices_) {
        if (dev->tick_bus.num_frames() < num_frames) {
            dev->tick_bus.resize(2, num_frames);
        }
        if (!dev->is_input_capture) {
            dev->tick_bus.clear(); // Output accumulation bus starts at silence every tick
            continue;
        }

        if (dev->is_virtual) {
            // Deterministic: no priming / latency clamping for test endpoints
            dev->ring_buffer.pop_planar(dev->tick_bus.channel_ptrs(), 2, num_frames);
            continue;
        }

        const uint32_t avail = dev->ring_buffer.available_to_read();
        if (!dev->primed) {
            if (avail >= kPrimeBlocks * num_frames) {
                dev->primed = true;
            } else {
                dev->tick_bus.clear();
                continue;
            }
        }
        if (avail > kMaxBlocks * num_frames) {
            dev->ring_buffer.skip(avail - kPrimeBlocks * num_frames);
            dev->drops.fetch_add(1, std::memory_order_relaxed);
        }
        const uint32_t got = dev->ring_buffer.pop_planar(dev->tick_bus.channel_ptrs(), 2, num_frames);
        if (got < num_frames) {
            dev->underruns.fetch_add(1, std::memory_order_relaxed);
            dev->primed = false;
        }
    }
}

AT_Result DeviceManager::read_hardware_input(
    AT_DeviceStreamHandle handle, float* const* out_planar_channels, uint32_t num_channels, uint32_t num_frames) {
    if (!handle || !out_planar_channels) return AT_ERR_INVALID_ARG;
    auto* dev = reinterpret_cast<HiddenMasterDevice*>(handle);
    if (!dev->is_input_capture) return AT_ERR_INVALID_ARG;

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
    AT_DeviceStreamHandle handle, const float* const* in_planar_channels, uint32_t num_channels, uint32_t num_frames) {
    if (!handle || !in_planar_channels || num_channels == 0) return AT_ERR_INVALID_ARG;
    auto* dev = reinterpret_cast<HiddenMasterDevice*>(handle);
    if (dev->is_input_capture) return AT_ERR_INVALID_ARG;

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
        if (dev->is_input_capture) continue;
        const uint32_t frames = std::min(num_frames, dev->tick_bus.num_frames());
        if (!dev->is_virtual) {
            // Safety hard-clip on physical outputs
            for (uint32_t ch = 0; ch < dev->tick_bus.num_channels(); ++ch) {
                float* ptr = dev->tick_bus.channel(ch);
                for (uint32_t f = 0; f < frames; ++f) {
                    ptr[f] = std::clamp(ptr[f], -1.0f, 1.0f);
                }
            }
            // Don't fill the ring while the device is not running (avoids stale latency build-up)
            if (dev->status.load(std::memory_order_relaxed) != AT_DEVICE_STATUS_RUNNING) continue;
        }
        dev->ring_buffer.push_planar(dev->tick_bus.channel_ptrs(), 2, frames);
    }
}

uint32_t DeviceManager::inject_virtual_input_frames(
    const std::string& endpoint_id, const float* const* in_planar_channels, uint32_t num_channels, uint32_t num_frames) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& dev : devices_) {
        if (dev->endpoint_key == endpoint_id && dev->is_input_capture) {
            return dev->ring_buffer.push_planar(in_planar_channels, num_channels, num_frames);
        }
    }
    return 0;
}

uint32_t DeviceManager::extract_virtual_output_frames(
    const std::string& endpoint_id, float* const* out_planar_channels, uint32_t num_channels, uint32_t num_frames) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& dev : devices_) {
        if (dev->endpoint_key == endpoint_id && !dev->is_input_capture) {
            return dev->ring_buffer.pop_planar(out_planar_channels, num_channels, num_frames);
        }
    }
    return 0;
}

void DeviceManager::start_wasapi_stream(HiddenMasterDevice* dev) {
    if (!dev || dev->is_virtual || dev->running.load()) return;
    dev->primed = false;
    dev->set_status(AT_DEVICE_STATUS_STARTING, "Opening device...");
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

    IMMDeviceEnumerator* enumerator     = nullptr;
    IMMDevice*           mm_dev         = nullptr;
    IAudioClient*        audio_client   = nullptr;
    IAudioCaptureClient* capture_client = nullptr;
    IAudioRenderClient*  render_client  = nullptr;
    WAVEFORMATEX*        mix_fmt        = nullptr;
    HANDLE               buffer_event   = nullptr;

    auto fail = [&](const std::string& msg) { dev->set_status(AT_DEVICE_STATUS_FAILED, msg); };

    do {
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                      reinterpret_cast<void**>(&enumerator));
        if (FAILED(hr) || !enumerator) {
            fail("MMDeviceEnumerator unavailable " + hr_to_string(hr));
            break;
        }

        const std::string& id = dev->os_endpoint_id;
        if (id == "default_input") {
            hr = enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &mm_dev);
        } else if (id == "default_output") {
            hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &mm_dev);
        } else {
            std::wstring wide_id = utf8_to_wide(id);
            hr = enumerator->GetDevice(wide_id.c_str(), &mm_dev);
        }
        if (FAILED(hr) || !mm_dev) {
            fail("Device not found / offline");
            break;
        }

        hr = mm_dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&audio_client));
        if (FAILED(hr) || !audio_client) {
            fail("IAudioClient activation failed " + hr_to_string(hr));
            break;
        }

        const uint32_t engine_sr = sample_rate_.load();
        DWORD base_flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
        if (dev->is_loopback) base_flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;

        // Preferred: 32-bit float stereo at the engine rate, WASAPI converts channels / rate
        WAVEFORMATEX desired = {};
        desired.wFormatTag      = WAVE_FORMAT_IEEE_FLOAT;
        desired.nChannels       = 2;
        desired.nSamplesPerSec  = engine_sr;
        desired.wBitsPerSample  = 32;
        desired.nBlockAlign     = 8;
        desired.nAvgBytesPerSec = engine_sr * 8;
        desired.cbSize          = 0;

        const REFERENCE_TIME hns_buffer = 200000; // 20 ms
        SampleLayout layout;
        hr = audio_client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                      base_flags | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                      hns_buffer, 0, &desired, nullptr);
        if (SUCCEEDED(hr)) {
            layout = layout_from_format(&desired);
        } else {
            // Fallback: device mix format (must match the engine sample rate; channels/bit depth are converted here)
            safe_release(audio_client);
            hr = mm_dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&audio_client));
            if (FAILED(hr) || !audio_client || FAILED(audio_client->GetMixFormat(&mix_fmt)) || !mix_fmt) {
                fail("Could not query device mix format");
                break;
            }
            if (mix_fmt->nSamplesPerSec != engine_sr) {
                fail("Device runs at " + std::to_string(mix_fmt->nSamplesPerSec) + " Hz, engine at " +
                     std::to_string(engine_sr) + " Hz (no resampler)");
                break;
            }
            hr = audio_client->Initialize(AUDCLNT_SHAREMODE_SHARED, base_flags, hns_buffer, 0, mix_fmt, nullptr);
            if (FAILED(hr)) {
                fail("IAudioClient::Initialize failed " + hr_to_string(hr));
                break;
            }
            layout = layout_from_format(mix_fmt);
        }

        buffer_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!buffer_event || FAILED(audio_client->SetEventHandle(buffer_event))) {
            fail("SetEventHandle failed");
            break;
        }

        UINT32 buffer_frame_count = 0;
        audio_client->GetBufferSize(&buffer_frame_count);

        if (dev->is_input_capture) {
            hr = audio_client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&capture_client));
        } else {
            hr = audio_client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&render_client));
        }
        if (FAILED(hr)) {
            fail("GetService failed " + hr_to_string(hr));
            break;
        }

        if (FAILED(audio_client->Start())) {
            fail("IAudioClient::Start failed");
            break;
        }
        dev->set_status(AT_DEVICE_STATUS_RUNNING,
                        std::string(dev->is_loopback ? "Loopback running" : "Running") + " (" + std::to_string(engine_sr) + " Hz)");

        std::vector<float> scratch;
        bool device_lost = false;

        while (dev->running.load(std::memory_order_relaxed) && !device_lost) {
            // Event-driven, with timeout fallback (loopback streams do not signal while the endpoint is silent)
            WaitForSingleObject(buffer_event, 10);

            if (capture_client) {
                UINT32 packet_length = 0;
                while (true) {
                    hr = capture_client->GetNextPacketSize(&packet_length);
                    if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
                        device_lost = true;
                        break;
                    }
                    if (FAILED(hr) || packet_length == 0) break;

                    BYTE* data = nullptr;
                    UINT32 frames_available = 0;
                    DWORD flags = 0;
                    hr = capture_client->GetBuffer(&data, &frames_available, &flags, nullptr, nullptr);
                    if (FAILED(hr)) {
                        if (hr == AUDCLNT_E_DEVICE_INVALIDATED) device_lost = true;
                        break;
                    }
                    if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) || !data) {
                        scratch.assign(static_cast<size_t>(frames_available) * 2, 0.0f);
                    } else {
                        to_interleaved_stereo(data, frames_available, layout, scratch);
                    }
                    if (dev->ring_buffer.push_interleaved(scratch.data(), 2, frames_available) < frames_available) {
                        dev->drops.fetch_add(1, std::memory_order_relaxed);
                    }
                    capture_client->ReleaseBuffer(frames_available);
                }
            } else if (render_client) {
                UINT32 padding = 0;
                hr = audio_client->GetCurrentPadding(&padding);
                if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
                    device_lost = true;
                    break;
                }
                if (FAILED(hr)) continue;

                const UINT32 writable = (buffer_frame_count > padding) ? (buffer_frame_count - padding) : 0;
                if (writable == 0) continue;

                const uint32_t bs    = buffer_size_frames_.load(std::memory_order_relaxed);
                uint32_t       avail = dev->ring_buffer.available_to_read();

                if (!dev->primed) {
                    if (avail >= kPrimeBlocks * bs) {
                        dev->primed = true;
                    } else {
                        continue; // WASAPI plays silence meanwhile
                    }
                }
                if (avail > kMaxBlocks * bs + writable) {
                    dev->ring_buffer.skip(avail - kPrimeBlocks * bs);
                    dev->drops.fetch_add(1, std::memory_order_relaxed);
                    avail = dev->ring_buffer.available_to_read();
                }

                const UINT32 to_write = std::min<UINT32>(writable, avail);
                if (to_write == 0) {
                    dev->underruns.fetch_add(1, std::memory_order_relaxed);
                    dev->primed = false;
                    continue;
                }

                BYTE* data = nullptr;
                hr = render_client->GetBuffer(to_write, &data);
                if (FAILED(hr) || !data) {
                    if (hr == AUDCLNT_E_DEVICE_INVALIDATED) device_lost = true;
                    continue;
                }
                scratch.resize(static_cast<size_t>(to_write) * 2);
                dev->ring_buffer.pop_interleaved(scratch.data(), 2, to_write);
                from_interleaved_stereo(scratch.data(), to_write, layout, data);
                render_client->ReleaseBuffer(to_write, 0);
            }
        }

        audio_client->Stop();
        if (device_lost) {
            fail("Device disconnected / invalidated");
        } else {
            dev->set_status(AT_DEVICE_STATUS_NONE, "Stopped");
        }
    } while (false);

    safe_release(capture_client);
    safe_release(render_client);
    safe_release(audio_client);
    safe_release(mm_dev);
    safe_release(enumerator);
    if (mix_fmt) CoTaskMemFree(mix_fmt);
    if (buffer_event) CloseHandle(buffer_event);
    if (hMmcss) AvRevertMmThreadCharacteristics(hMmcss);
}

} // namespace at
