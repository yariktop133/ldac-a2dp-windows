#include "svc.h"

#include <windows.h>
#include <mmdeviceapi.h>  // before functiondiscoverykeys: it brings DEFINE_PROPERTYKEY
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>

#include <chrono>
#include <thread>

extern "C" {
#include "../tools/ldacenc/wav.h"
}

using Microsoft::WRL::ComPtr;

namespace {

// Converts one interleaved buffer in `fmt` to float, keeping at most two channels.
void AppendFloat(const BYTE *data, UINT32 frames, const WAVEFORMATEX *fmt, bool isFloat, int outCh,
                 std::vector<float> &out)
{
    int inCh = fmt->nChannels;
    int bytes = fmt->wBitsPerSample / 8;
    size_t base = out.size();
    out.resize(base + (size_t)frames * outCh);
    for (UINT32 f = 0; f < frames; f++) {
        for (int c = 0; c < outCh; c++) {
            const BYTE *p = data + ((size_t)f * inCh + (c < inCh ? c : 0)) * bytes;
            float v;
            if (isFloat)
                v = *(const float *)p;
            else if (bytes == 2)
                v = *(const int16_t *)p / 32768.0f;
            else if (bytes == 3)
                v = (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24) / 2147483648.0f;
            else
                v = *(const int32_t *)p / 2147483648.0f;
            out[base + (size_t)f * outCh + c] = v;
        }
    }
}

class LoopbackSource : public AudioSource {
public:
    explicit LoopbackSource(std::string match) : match_(std::move(match)) {}

    ~LoopbackSource() override
    {
        if (client_)
            client_->Stop();
        if (fmt_)
            CoTaskMemFree(fmt_);
    }

    bool Start(std::string &err) override
    {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(hr) && hr != RPC_E_CHANGED_MODE)
            return err = "CoInitializeEx failed", false;

        ComPtr<IMMDeviceEnumerator> en;
        hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en));
        if (FAILED(hr))
            return err = "MMDeviceEnumerator unavailable", false;

        ComPtr<IMMDevice> dev;
        if (match_.empty()) {
            hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
        } else {
            ComPtr<IMMDeviceCollection> all;
            UINT n = 0;
            en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &all);
            if (all)
                all->GetCount(&n);
            std::wstring want(MultiByteToWideChar(CP_UTF8, 0, match_.c_str(), -1, nullptr, 0), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, match_.c_str(), -1, want.data(), (int)want.size());
            want.resize(wcslen(want.c_str()));
            for (UINT i = 0; i < n && !dev; i++) {
                ComPtr<IMMDevice> d;
                ComPtr<IPropertyStore> props;
                PROPVARIANT name;
                PropVariantInit(&name);
                if (SUCCEEDED(all->Item(i, &d)) && SUCCEEDED(d->OpenPropertyStore(STGM_READ, &props)) &&
                    SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &name)) && name.vt == VT_LPWSTR &&
                    wcsstr(name.pwszVal, want.c_str()))
                    dev = d;
                PropVariantClear(&name);
            }
            hr = dev ? S_OK : E_FAIL;
        }
        if (FAILED(hr) || !dev)
            return err = "render endpoint not found" + (match_.empty() ? std::string() : " for '" + match_ + "'"), false;

        {
            ComPtr<IPropertyStore> props;
            PROPVARIANT name;
            PropVariantInit(&name);
            if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props)) &&
                SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &name)) && name.vt == VT_LPWSTR) {
                int len = WideCharToMultiByte(CP_UTF8, 0, name.pwszVal, -1, nullptr, 0, nullptr, nullptr);
                name_.resize(len > 0 ? len - 1 : 0);
                WideCharToMultiByte(CP_UTF8, 0, name.pwszVal, -1, name_.data(), len, nullptr, nullptr);
            }
            PropVariantClear(&name);
        }

        if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client_)) ||
            FAILED(client_->GetMixFormat(&fmt_)))
            return err = "cannot open the endpoint", false;
        isFloat_ = fmt_->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                   (fmt_->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                    ((WAVEFORMATEXTENSIBLE *)fmt_)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
        channels_ = fmt_->nChannels >= 2 ? 2 : 1;

        hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 2000000, 0, fmt_, nullptr);
        if (FAILED(hr))
            return err = "loopback Initialize failed", false;
        if (FAILED(client_->GetService(IID_PPV_ARGS(&capture_))) || FAILED(client_->Start()))
            return err = "loopback start failed", false;
        return true;
    }

    uint32_t Rate() const override { return fmt_ ? fmt_->nSamplesPerSec : 0; }
    int Channels() const override { return channels_; }
    std::string Describe() const override
    {
        return "loopback of '" + name_ + "' (" + std::to_string(Rate()) + " Hz, " +
               std::to_string(fmt_->wBitsPerSample) + "-bit" + (isFloat_ ? " float" : "") + ")";
    }

    // Loopback delivers nothing while the endpoint plays nothing: the caller
    // treats an empty read as silence.
    bool Read(std::vector<float> &out, int timeoutMs) override
    {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        for (;;) {
            UINT32 packet = 0;
            if (FAILED(capture_->GetNextPacketSize(&packet)))
                return false;
            if (packet)
                break;
            if (std::chrono::steady_clock::now() >= deadline)
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        for (;;) {
            BYTE *data;
            UINT32 frames = 0;
            DWORD flags = 0;
            if (capture_->GetBuffer(&data, &frames, &flags, nullptr, nullptr) != S_OK)
                break;
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT)
                out.resize(out.size() + (size_t)frames * channels_, 0.0f);
            else
                AppendFloat(data, frames, fmt_, isFloat_, channels_, out);
            capture_->ReleaseBuffer(frames);
            UINT32 next = 0;
            if (FAILED(capture_->GetNextPacketSize(&next)) || !next)
                break;
        }
        return true;
    }

private:
    std::string match_, name_;
    ComPtr<IAudioClient> client_;
    ComPtr<IAudioCaptureClient> capture_;
    WAVEFORMATEX *fmt_ = nullptr;
    bool isFloat_ = false;
    int channels_ = 2;
};

// WAV file played in real time (so the sink sees a live-like packet rate).
class WavSource : public AudioSource {
public:
    explicit WavSource(std::string path) : path_(std::move(path)) {}
    ~WavSource() override { wav_close(&wav_); }

    bool Start(std::string &err) override
    {
        if (wav_open(&wav_, path_.c_str()) != 0)
            return err = "cannot read " + path_, false;
        fmt_.nChannels = wav_.channels;
        fmt_.wBitsPerSample = (WORD)(wav_.bytes_per_sample * 8);
        channels_ = wav_.channels >= 2 ? 2 : 1;
        start_ = std::chrono::steady_clock::now();
        return true;
    }

    uint32_t Rate() const override { return wav_.sample_rate; }
    int Channels() const override { return channels_; }
    std::string Describe() const override
    {
        return "file " + path_ + " (" + std::to_string(Rate()) + " Hz, " + std::to_string(fmt_.wBitsPerSample) + "-bit)";
    }

    bool Read(std::vector<float> &out, int timeoutMs) override
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(timeoutMs));
        double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
        uint64_t due = (uint64_t)(elapsed * wav_.sample_rate);
        if (due <= delivered_)
            return true;
        size_t want = (size_t)(due - delivered_);
        std::vector<BYTE> raw(want * wav_.channels * wav_.bytes_per_sample);
        size_t got = wav_read(&wav_, raw.data(), want);
        if (got == 0)
            return false;
        AppendFloat(raw.data(), (UINT32)got, &fmt_, wav_.format == WAV_F32, channels_, out);
        delivered_ += got;
        return true;
    }

private:
    std::string path_;
    wav_file wav_{};
    WAVEFORMATEX fmt_{};
    int channels_ = 2;
    uint64_t delivered_ = 0;
    std::chrono::steady_clock::time_point start_;
};

}  // namespace

std::unique_ptr<AudioSource> MakeLoopbackSource(const std::string &endpointMatch)
{
    return std::make_unique<LoopbackSource>(endpointMatch);
}

std::unique_ptr<AudioSource> MakeWavSource(const std::string &path)
{
    return std::make_unique<WavSource>(path);
}
