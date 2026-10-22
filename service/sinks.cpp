#include "svc.h"

#include <windows.h>
#include <winioctl.h>
#include <initguid.h>
#include <cfgmgr32.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <thread>

extern "C" {
#include "avdtp_source.h"
}
#include "../driver/LdacA2dp/public.h"

namespace {

using Clock = std::chrono::steady_clock;

// ---- LdacA2dp driver ----------------------------------------------------------

class DriverSink : public MediaSink {
public:
    explicit DriverSink(uint64_t address) : address_(address) {}

    ~DriverSink() override
    {
        CloseDevice();
        if (notify_)
            CM_Unregister_Notification(notify_);  // waits for a callback in progress
        CloseHandle(closed_);
    }

    void CloseDevice()
    {
        if (handle_ != INVALID_HANDLE_VALUE) {
            CancelIoEx(handle_, nullptr);
            while (!pending_.empty()) {
                DWORD n;
                GetOverlappedResult(handle_, &pending_.front()->ov, &n, TRUE);
                CloseHandle(pending_.front()->ov.hEvent);
                pending_.pop_front();
            }
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
        SetEvent(closed_);  // lets a pending query-remove go ahead
    }

    bool Open(const a2dp_prefs &prefs, LinkInfo &link, std::string &err) override
    {
        if (!FindDevice(err))
            return false;
        WatchRemoval();

        LDACA2DP_STATUS st;
        if (!Status(st))
            return err = "IOCTL_LDACA2DP_GET_STATUS failed", false;
        if (st.AvdtpState == AVDTP_SRC_IDLE || st.AvdtpState == AVDTP_SRC_FAILED) {
            LDACA2DP_PREFS p = {};
            p.AllowLdac = prefs.allow_ldac;
            p.LdacFreqMask = prefs.ldac_freq_mask;
            p.LdacChannelModeMask = prefs.ldac_cm_mask;
            p.LdacPreferredRate = prefs.ldac_pref_rate;
            p.SbcPreferredRate = prefs.sbc_pref_rate;
            if (!Ioctl(IOCTL_LDACA2DP_CONNECT, &p, sizeof(p), nullptr, 0))
                return err = "IOCTL_LDACA2DP_CONNECT failed, error " + std::to_string(GetLastError()), false;
        }

        // The driver runs DISCOVER ... OPEN; wait until the stream is open.
        auto deadline = Clock::now() + std::chrono::seconds(20);
        for (;;) {
            if (!Status(st))
                return err = "IOCTL_LDACA2DP_GET_STATUS failed", false;
            if ((st.AvdtpState == AVDTP_SRC_OPEN || st.AvdtpState == AVDTP_SRC_STREAMING) && st.MediaMtu)
                break;
            if (st.AvdtpState == AVDTP_SRC_FAILED)
                return err = "stream negotiation failed (NTSTATUS 0x" + Hex(st.LastError) + ")", false;
            if (Clock::now() > deadline)
                return err = "timed out waiting for the stream, AVDTP state " + std::to_string(st.AvdtpState) +
                             ", last error 0x" + Hex(st.LastError), false;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        link.codec = st.Codec == LdacA2dpCodecLdac ? "LDAC" : st.Codec == LdacA2dpCodecSbc ? "SBC" : "none";
        link.sampleRate = st.SampleRate;
        link.channels = st.Channels;
        link.ldacCm = st.LdacChannelMode;
        link.mtu = st.MediaMtu;
        link.delay100us = st.DelayReport100us;
        if (st.Codec == LdacA2dpCodecSbc &&
            a2dp_sbc_parse(A2DP_CODEC_SBC, st.SbcConfig, sizeof(st.SbcConfig), &link.sbc) != 0)
            return err = "driver reported an unreadable SBC configuration", false;
        describe_ ="LdacA2dp driver, " + FormatAddress(st.RemoteAddress);
        return true;
    }

    bool Start(std::string &err) override
    {
        LDACA2DP_STATUS st;
        if (Status(st) && st.AvdtpState == AVDTP_SRC_STREAMING)
            return true;
        if (!Ioctl(IOCTL_LDACA2DP_START, nullptr, 0, nullptr, 0))
            return err = "IOCTL_LDACA2DP_START failed, error " + std::to_string(GetLastError()), false;
        auto deadline = Clock::now() + std::chrono::seconds(3);
        while (Status(st) && st.AvdtpState != AVDTP_SRC_STREAMING) {
            if (Clock::now() > deadline)
                return err = "START was not accepted", false;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return true;
    }

    void Suspend() override { Ioctl(IOCTL_LDACA2DP_SUSPEND, nullptr, 0, nullptr, 0); }

    bool Send(const uint8_t *packet, size_t len) override
    {
        if (pending_.size() >= LDACA2DP_MAX_INFLIGHT_MEDIA)
            return false;
        auto p = std::make_unique<Pending>();
        p->data.assign(packet, packet + len);
        p->ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!DeviceIoControl(handle_, IOCTL_LDACA2DP_SEND_MEDIA, p->data.data(), (DWORD)len, nullptr, 0, nullptr,
                             &p->ov) &&
            GetLastError() != ERROR_IO_PENDING) {
            CloseHandle(p->ov.hEvent);
            return false;
        }
        pending_.push_back(std::move(p));
        return true;
    }

    void Pump() override
    {
        while (!pending_.empty() && HasOverlappedIoCompleted(&pending_.front()->ov)) {
            DWORD n;
            GetOverlappedResult(handle_, &pending_.front()->ov, &n, FALSE);
            CloseHandle(pending_.front()->ov.hEvent);
            pending_.pop_front();
        }
    }

    // TX queue depth for LDAC ABR: packets waiting behind the one being handed to
    // the controller. libldac only raises quality after 0.5 s with a depth of
    // exactly 0, and a live stream always has one packet in flight, so counting
    // it would pin the encoder at its lowest step (seen on hardware: stuck at MQ).
    unsigned QueueDepth() const override { return pending_.empty() ? 0u : (unsigned)pending_.size() - 1; }

    uint16_t Delay100us() override
    {
        LDACA2DP_STATUS st;
        return Status(st) ? st.DelayReport100us : 0;
    }

    std::string Describe() const override { return describe_; }

    bool Probe(LDACA2DP_STATUS &st)
    {
        std::string err;
        return FindDevice(err) && Status(st);
    }

    bool Alive(std::string &why) override
    {
        if (removing_)
            return why = "the device is being removed", false;
        auto now = Clock::now();
        if (now < nextStatus_)
            return true;
        nextStatus_ = now + std::chrono::seconds(1);
        LDACA2DP_STATUS st;
        if (!Status(st))
            return why = "the device stopped answering (error " + std::to_string(GetLastError()) + ")", false;
        if (st.AvdtpState < AVDTP_SRC_OPEN || st.AvdtpState > AVDTP_SRC_SUSPENDING || !st.MediaMtu)
            return why = "the stream closed (AVDTP state " + std::to_string(st.AvdtpState) + ", last error 0x" +
                         Hex(st.LastError) + ")", false;
        return true;
    }

    void Disconnect() override
    {
        if (handle_ != INVALID_HANDLE_VALUE)
            Ioctl(IOCTL_LDACA2DP_DISCONNECT, nullptr, 0, nullptr, 0);
    }

private:
    // An open handle must not hold up a driver update or the device going away:
    // on query-remove the session loop sees removing_ and drops the sink, which
    // closes the handle; the callback waits for that (bounded).
    void WatchRemoval()
    {
        CM_NOTIFY_FILTER f = {};
        f.cbSize = sizeof(f);
        f.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEHANDLE;
        f.u.DeviceHandle.hTarget = handle_;
        CM_Register_Notification(&f, this, OnDeviceEvent, &notify_);
    }

    static DWORD CALLBACK OnDeviceEvent(HCMNOTIFICATION, PVOID context, CM_NOTIFY_ACTION action,
                                        PCM_NOTIFY_EVENT_DATA, DWORD)
    {
        auto *self = static_cast<DriverSink *>(context);
        if (action == CM_NOTIFY_ACTION_DEVICEQUERYREMOVE || action == CM_NOTIFY_ACTION_DEVICEREMOVEPENDING ||
            action == CM_NOTIFY_ACTION_DEVICEREMOVECOMPLETE) {
            self->removing_ = true;
            if (action == CM_NOTIFY_ACTION_DEVICEQUERYREMOVE)
                WaitForSingleObject(self->closed_, 3000);
        }
        return ERROR_SUCCESS;
    }

    struct Pending {
        OVERLAPPED ov{};
        std::vector<uint8_t> data;
    };

    static std::string Hex(INT32 v)
    {
        char b[16];
        snprintf(b, sizeof(b), "%08X", (unsigned)v);
        return b;
    }

    bool Ioctl(DWORD code, void *in, DWORD inLen, void *out, DWORD outLen, DWORD *got = nullptr) const
    {
        OVERLAPPED ov{};
        DWORD n = 0;
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        BOOL ok = DeviceIoControl(handle_, code, in, inLen, out, outLen, nullptr, &ov);
        if (!ok && GetLastError() == ERROR_IO_PENDING)
            ok = GetOverlappedResult(handle_, &ov, &n, TRUE);
        else if (ok)
            GetOverlappedResult(handle_, &ov, &n, FALSE);
        DWORD e = GetLastError();
        CloseHandle(ov.hEvent);
        SetLastError(e);
        if (got)
            *got = n;
        return ok != FALSE;
    }

    bool Status(LDACA2DP_STATUS &st) const
    {
        DWORD n = 0;
        return Ioctl(IOCTL_LDACA2DP_GET_STATUS, nullptr, 0, &st, sizeof(st), &n) && n == sizeof(st);
    }

    bool FindDevice(std::string &err)
    {
        ULONG len = 0;
        GUID guid = GUID_DEVINTERFACE_LDACA2DP;
        if (CM_Get_Device_Interface_List_SizeW(&len, &guid, nullptr, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) !=
                CR_SUCCESS || len <= 1)
            return err = "no LdacA2dp device: is the driver installed for these headphones?", false;
        std::vector<wchar_t> list(len);
        if (CM_Get_Device_Interface_ListW(&guid, nullptr, list.data(), len, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) !=
            CR_SUCCESS)
            return err = "CM_Get_Device_Interface_List failed", false;

        for (const wchar_t *p = list.data(); *p; p += wcslen(p) + 1) {
            handle_ = CreateFileW(p, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                  OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
            if (handle_ == INVALID_HANDLE_VALUE)
                continue;
            LDACA2DP_STATUS st;
            if (Status(st) && (address_ == 0 || st.RemoteAddress == address_))
                return true;
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
        return err = "no LdacA2dp device for " + FormatAddress(address_), false;
    }

    uint64_t address_;
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    HANDLE closed_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HCMNOTIFICATION notify_ = nullptr;
    std::atomic<bool> removing_{ false };
    Clock::time_point nextStatus_ = Clock::now() + std::chrono::seconds(1);
    std::deque<std::unique_ptr<Pending>> pending_;
    std::string describe_;
};

// ---- simulated link (no driver, no radio) ------------------------------------

class NullSink : public MediaSink {
public:
    explicit NullSink(const std::string &spec)
    {
        for (size_t i = 0; i < spec.size();) {
            size_t colon = spec.find(':', i), comma = spec.find(',', i);
            if (colon == std::string::npos)
                break;
            points_.push_back({ atof(spec.substr(i, colon - i).c_str()), atof(spec.substr(colon + 1).c_str()) });
            i = comma == std::string::npos ? spec.size() : comma + 1;
        }
        if (points_.empty())
            points_.push_back({ 0, 2000 });
    }

    bool Open(const a2dp_prefs &prefs, LinkInfo &link, std::string &err) override
    {
        // Negotiate against the capabilities captured from the JBL Live 780NC.
        a2dp_ldac local = { prefs.ldac_freq_mask, prefs.ldac_cm_mask };
        a2dp_ldac peer = { LDAC_FREQ_44100 | LDAC_FREQ_48000 | LDAC_FREQ_88200 | LDAC_FREQ_96000,
                           LDAC_CM_STEREO | LDAC_CM_DUAL | LDAC_CM_MONO };
        a2dp_config cfg;
        link.mtu = 679;
        if (prefs.allow_ldac && a2dp_select_ldac(&local, &peer, prefs.ldac_pref_rate, &cfg) == 0) {
            link.codec = "LDAC";
            link.ldacCm = cfg.ldac.cm;
        } else {
            // SBC fallback: our source capabilities against the JBL SBC SEP (3f ff 02 35).
            a2dp_sbc src = { SBC_FREQ_44100 | SBC_FREQ_48000, 0x0F, 0xF0, 0x0C, 0x03, 2, SBC_BITPOOL_HQ };
            a2dp_sbc jbl = { SBC_FREQ_44100 | SBC_FREQ_48000, 0x0F, 0xF0, 0x0C, 0x03, 2, 53 };
            if (a2dp_select_sbc(&src, &jbl, prefs.sbc_pref_rate, &cfg) != 0)
                return err = "simulated sink: no common configuration", false;
            link.codec = "SBC";
            link.sbc = cfg.sbc;
            link.ldacCm = cfg.channels == 1 ? LDAC_CM_MONO : LDAC_CM_STEREO;
        }
        link.sampleRate = cfg.sample_rate;
        link.channels = cfg.channels;
        link.delay100us = 1500;
        start_ = last_ = Clock::now();
        return true;
    }

    bool Start(std::string &) override { return true; }
    void Suspend() override { queue_.clear(); }

    bool Send(const uint8_t *, size_t len) override
    {
        if (queue_.size() >= kMaxQueue)
            return false;
        queue_.push_back(len);
        return true;
    }

    void Pump() override
    {
        auto now = Clock::now();
        double t = std::chrono::duration<double>(now - start_).count();
        double dt = std::chrono::duration<double>(now - last_).count();
        last_ = now;
        credit_ += Kbps(t) * 1000.0 / 8.0 * dt;
        while (!queue_.empty() && credit_ >= (double)queue_.front()) {
            credit_ -= (double)queue_.front();
            queue_.pop_front();
        }
        if (queue_.empty() && credit_ > 4096)
            credit_ = 4096;
    }

    unsigned QueueDepth() const override { return (unsigned)queue_.size(); }
    uint16_t Delay100us() override { return 1500; }
    std::string Describe() const override { return "simulated link"; }

    double Kbps(double t) const
    {
        double k = points_.front().second;
        for (auto &p : points_)
            if (t >= p.first)
                k = p.second;
        return k;
    }

private:
    static constexpr size_t kMaxQueue = LDACA2DP_MAX_INFLIGHT_MEDIA;
    std::vector<std::pair<double, double>> points_;
    std::deque<size_t> queue_;
    double credit_ = 0;
    Clock::time_point start_, last_;
};

}  // namespace

std::unique_ptr<MediaSink> MakeDriverSink(uint64_t address)
{
    return std::make_unique<DriverSink>(address);
}

int DriverLinkState(uint64_t address)
{
    DriverSink probe(address);
    LDACA2DP_STATUS st;
    return probe.Probe(st) ? (int)st.AvdtpState : -1;
}

std::unique_ptr<MediaSink> MakeNullSink(const std::string &linkSpec)
{
    return std::make_unique<NullSink>(linkSpec);
}
