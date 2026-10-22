// ldacsvc - see svc.h.
//
//   ldacsvc [--address AA:BB:CC:DD:EE:FF] [--source endpoint|loopback[:name]|wav:file]
//           [--sink driver|null] [--link T:KBPS,...] [--seconds N] [--dump file] [-v]
//   ldacsvc install [--address ...] [--source ...]   (administrator)
//   ldacsvc uninstall
//
// Without a command it runs one session in the console. `install` registers the
// LdacSvc service (auto start), which runs sessions forever and reconnects;
// the options given to `install` go to the service command line.
//
// --dump writes the bare codec frames (media packets without RTP and payload
// headers): raw SBC can be decoded with `ffmpeg -f sbc -i file out.wav`.
//
// Debugging: -v adds per-second gap/queue figures and a 50 ms queue trace for
// 3 s after START; --pcm-dump file writes the raw float PCM from the source
// (and file.txt: ms, frames, streaming, warming, rate per block).
//
// --sink null runs the whole pipeline against a simulated radio link, so the
// encoder, ABR and status reporting can be checked without the driver.
#include "svc.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>

static std::atomic<bool> g_stop{ false };

static BOOL WINAPI OnCtrl(DWORD)
{
    g_stop = true;
    return TRUE;
}

static void Usage()
{
    fprintf(stderr,
            "usage: ldacsvc [--address AA:BB:CC:DD:EE:FF] [--source endpoint|loopback[:name]|wav:file]\n"
            "               [--sink driver|null] [--link T:KBPS,...] [--seconds N] [--dump file] [-v]\n"
            "       ldacsvc install [--address ...] [--source ...]\n"
            "       ldacsvc uninstall\n"
            "       ldacsvc status [AA:BB:CC:DD:EE:FF]\n");
}

int main(int argc, char **argv)
{
    SessionOptions o;
    bool service = false;
    int first = 1;
    std::string command = argc > 1 ? argv[1] : "";
    if (command == "install" || command == "uninstall")
        first = 2;
    if (command == "uninstall")
        return argc == 2 ? ServiceUninstall() : (Usage(), 2);
    if (command == "status")
        return DriverPrintStatus(argc > 2 ? ParseAddress(argv[2]) : 0);

    std::string passThrough;  // for install: the session options, re-quoted
    for (int i = first; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            std::string v = i + 1 < argc ? argv[++i] : "";
            passThrough += " " + a + " \"" + v + "\"";
            return v;
        };
        if (a == "--address") o.address = next();
        else if (a == "--source") o.source = next();
        else if (a == "--sink") o.sinkKind = next();
        else if (a == "--link") o.link = next();
        else if (a == "--seconds") o.seconds = atof(next().c_str());
        else if (a == "--dump") o.dumpPath = next();
        else if (a == "--pcm-dump") o.pcmDumpPath = next();
        else if (a == "-v") o.verbose = true, passThrough += " -v";
        else if (a == "--service") service = true;
        else return Usage(), 2;
    }
    if (!o.address.empty() && !ParseAddress(o.address))
        return fprintf(stderr, "bad --address\n"), 2;

    if (command == "install") {
        if (o.sinkKind != "driver" || o.seconds > 0 || !o.dumpPath.empty())
            return fprintf(stderr, "install: only --address, --source and -v make sense for the service\n"), 2;
        return ServiceInstall(passThrough.empty() ? "" : passThrough.substr(1));
    }
    if (service)
        return ServiceDispatch(o);

    SetConsoleCtrlHandler(OnCtrl, TRUE);
    SetConsoleOutputCP(CP_UTF8);
    std::string err;
    SessionEnd end = RunSession(o, g_stop, err);
    if (end == SessionEnd::Error) {
        fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    return 0;
}
