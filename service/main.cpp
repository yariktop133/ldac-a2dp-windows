// ldacsvc - see svc.h.
//
//   ldacsvc [--address AA:BB:CC:DD:EE:FF] [--source loopback[:endpoint]|wav:file]
//           [--sink driver|null] [--link T:KBPS,...] [--seconds N] [-v]
//
// --sink null runs the whole pipeline against a simulated radio link, so the
// encoder, ABR and status reporting can be checked without the driver.
#include "svc.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

static std::atomic<bool> g_stop{ false };

static BOOL WINAPI OnCtrl(DWORD)
{
    g_stop = true;
    return TRUE;
}

static void Usage()
{
    fprintf(stderr,
            "usage: ldacsvc [--address AA:BB:CC:DD:EE:FF] [--source loopback[:endpoint]|wav:file]\n"
            "               [--sink driver|null] [--link T:KBPS,...] [--seconds N] [-v]\n");
}

int main(int argc, char **argv)
{
    std::string address, source = "loopback", sinkKind = "driver", link = "0:2000";
    double seconds = 0;
    bool verbose = false;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--address") address = next();
        else if (a == "--source") source = next();
        else if (a == "--sink") sinkKind = next();
        else if (a == "--link") link = next();
        else if (a == "--seconds") seconds = atof(next().c_str());
        else if (a == "-v") verbose = true;
        else return Usage(), 2;
    }
    uint64_t addr = ParseAddress(address);
    if (!address.empty() && !addr)
        return fprintf(stderr, "bad --address\n"), 2;
    if (address.empty() && sinkKind == "null")
        address = "00:00:00:00:00:00";
    SetConsoleCtrlHandler(OnCtrl, TRUE);
    SetConsoleOutputCP(CP_UTF8);

    // settings -> preferences
    DeviceSettings settings;
    std::string note;
    LoadSettings(address.empty() ? std::string() : FormatAddress(addr), settings, note);
    printf("settings: %s\n", note.c_str());
    a2dp_prefs prefs = ToPrefs(settings);

    // sink: negotiate the stream
    std::unique_ptr<MediaSink> sink = sinkKind == "null" ? MakeNullSink(link) : MakeDriverSink(addr);
    LinkInfo info;
    std::string err;
    if (!sink->Open(prefs, info, err))
        return fprintf(stderr, "sink: %s\n", err.c_str()), 1;
    if (address.empty())
        address = sink->Describe().substr(sink->Describe().rfind(' ') + 1);
    else
        address = FormatAddress(addr);
    printf("link: %s, %s %u Hz %s, MTU %d, delay %.1f ms\n", sink->Describe().c_str(), info.codec.c_str(),
           info.sampleRate, ChannelModeName(info.ldacCm).c_str(), info.mtu, info.delay100us / 10.0);

    LdacEncoder enc;
    bool s16 = !settings.bits24;
    if (!enc.Init(info, settings.eqmid, settings.adaptive, s16, err))
        return fprintf(stderr, "encoder: %s\n", err.c_str()), 1;
    printf("encoder: LDAC %s%s, %s input\n", EqmidName(settings.eqmid), settings.adaptive ? " + ABR" : "",
           s16 ? "16-bit" : "float");

    // source
    std::unique_ptr<AudioSource> src;
    if (source.rfind("wav:", 0) == 0)
        src = MakeWavSource(source.substr(4));
    else
        src = MakeLoopbackSource(source.rfind("loopback:", 0) == 0 ? source.substr(9) : "");
    if (!src->Start(err))
        return fprintf(stderr, "source: %s\n", err.c_str()), 1;
    printf("source: %s\n", src->Describe().c_str());

    Resampler rs(src->Rate(), info.sampleRate, src->Channels());
    std::vector<float> in, resampled, mapped;
    std::vector<std::vector<uint8_t>> packets;

    using Clock = std::chrono::steady_clock;
    auto start = Clock::now(), lastSound = start, nextTick = start, nextReport = start + std::chrono::seconds(1);
    bool streaming = false, finished = false;
    unsigned long long sent = 0, dropped = 0, sentBytes = 0, sentAtReport = 0, bytesAtReport = 0;
    int bitDepth = s16 ? 16 : 24;

    while (!g_stop && !finished) {
        in.clear();
        if (!src->Read(in, 5))
            finished = true;
        auto now = Clock::now();

        float peak = 0;
        for (float v : in)
            peak = std::max(peak, std::fabs(v));
        if (peak > 1e-4f)
            lastSound = now;
        bool silent = now - lastSound > std::chrono::seconds(3);

        // START only while there is audio, SUSPEND after 3 s of silence (as the reference does).
        if (!streaming && !silent && !in.empty()) {
            if (!sink->Start(err)) {
                fprintf(stderr, "sink: %s\n", err.c_str());
                break;
            }
            streaming = true;
            if (verbose)
                printf("START\n");
        } else if (streaming && silent) {
            sink->Suspend();
            streaming = false;
            if (verbose)
                printf("SUSPEND (silence)\n");
        }

        if (streaming && !in.empty()) {
            resampled.clear();
            rs.Process(in.data(), in.size() / src->Channels(), resampled);
            const float *frames = resampled.data();
            size_t count = resampled.size() / src->Channels();
            if (info.channels != src->Channels()) {  // stereo source, mono link
                mapped.resize(count);
                for (size_t i = 0; i < count; i++)
                    mapped[i] = 0.5f * (resampled[2 * i] + resampled[2 * i + 1]);
                frames = mapped.data();
            }
            packets.clear();
            enc.Encode(frames, count, packets);
            for (auto &p : packets) {
                if (sink->Send(p.data(), p.size())) {
                    sent++;
                    sentBytes += p.size();
                } else {
                    dropped++;
                }
            }
        }
        sink->Pump();

        if (now >= nextTick) {
            enc.Tick(sink->QueueDepth());
            nextTick += std::chrono::milliseconds(LdacEncoder::kAbrIntervalMs);
            if (nextTick < now)
                nextTick = now;
        }
        if (now >= nextReport) {
            double t = std::chrono::duration<double>(now - start).count();
            WriteStatus(address, info, bitDepth, enc.NominalKbps());
            printf("%7.1fs %-9s %s %4d kbps | %4llu pkt/s %5.0f kbit/s on air | queue %2u | dropped %llu\n", t,
                   streaming ? "streaming" : "suspended", EqmidName(enc.Eqmid()), enc.NominalKbps(),
                   sent - sentAtReport, (sentBytes - bytesAtReport) * 8 / 1000.0, sink->QueueDepth(), dropped);
            fflush(stdout);
            sentAtReport = sent;
            bytesAtReport = sentBytes;
            nextReport += std::chrono::seconds(1);
        }
        if (seconds > 0 && now - start > std::chrono::duration<double>(seconds))
            break;
    }

    if (streaming)
        sink->Suspend();
    ClearStatus(address);
    printf("done: %llu packets sent, %llu dropped\n", sent, dropped);
    return 0;
}
