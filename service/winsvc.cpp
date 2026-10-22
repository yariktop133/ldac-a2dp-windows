// ldacsvc as a Windows service (LdacSvc, auto start, LocalSystem).
//
// The service keeps one session running per headphones: it waits for the
// LdacA2dp device, streams, and after the link goes away (headphones off, out
// of range, sleep) or the settings change, starts over. Reconnecting is paced:
// a sink that comes back on its own (it opens AVDTP itself, the driver accepts
// it) is picked up within a second. Our own CONNECT follows the ACL link: when
// the headphones connect and have not opened A2DP themselves within 2 s (some
// sinks bring up only HFP), we open the stream; while they are not connected,
// nothing is paged (Windows does the same). Failed attempts on a
// connected sink back off up to a minute.
//
// Output goes to %ProgramData%\LdacA2dp\ldacsvc.log (previous run: .log.1).
#include "svc.h"

#include <windows.h>
#include <bluetoothapis.h>

#include <chrono>
#include <cstdio>
#include <ctime>

extern "C" {
#include "avdtp_source.h"
}

namespace {

SessionOptions g_opts;
std::atomic<bool> g_stop{ false };
HANDLE g_stopEvent;
SERVICE_STATUS_HANDLE g_statusHandle;
SERVICE_STATUS g_status;

void Report(DWORD state, DWORD exitCode = NO_ERROR)
{
    g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_status.dwCurrentState = state;
    g_status.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
    g_status.dwWin32ExitCode = exitCode;
    g_status.dwWaitHint = state == SERVICE_RUNNING || state == SERVICE_STOPPED ? 0 : 5000;
    SetServiceStatus(g_statusHandle, &g_status);
}

DWORD WINAPI Handler(DWORD control, DWORD, LPVOID, LPVOID)
{
    if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN) {
        Report(SERVICE_STOP_PENDING);
        g_stop = true;
        SetEvent(g_stopEvent);
        return NO_ERROR;
    }
    return control == SERVICE_CONTROL_INTERROGATE ? NO_ERROR : ERROR_CALL_NOT_IMPLEMENTED;
}

void Stamp(const char *what)
{
    time_t t = time(nullptr);
    char b[32];
    strftime(b, sizeof(b), "%Y-%m-%d %H:%M:%S", localtime(&t));
    printf("---- %s %s\n", b, what);
    fflush(stdout);
}

void OpenLog()
{
    std::string dir = DataRoot(), path = dir + "\\ldacsvc.log";
    CreateDirectoryA(dir.c_str(), nullptr);
    MoveFileExA(path.c_str(), (path + ".1").c_str(), MOVEFILE_REPLACE_EXISTING);
    // freopen, not freopen_s: the _s variant denies sharing, and the log must be
    // readable while the service runs.
    if (freopen(path.c_str(), "w", stdout))
        setvbuf(stdout, nullptr, _IONBF, 0);  // MSVC treats _IOLBF as full buffering
    freopen(path.c_str(), "a", stderr);
}

// ACL link to the headphones: 1 connected, 0 not, -1 unknown (no address, API failed).
int AclConnected(uint64_t addr)
{
    if (!addr)
        return -1;
    BLUETOOTH_DEVICE_INFO info = {};
    info.dwSize = sizeof(info);
    info.Address.ullLong = addr;
    if (BluetoothGetDeviceInfo(nullptr, &info) != ERROR_SUCCESS)
        return -1;
    return info.fConnected ? 1 : 0;
}

void Worker()
{
    using Clock = std::chrono::steady_clock;
    uint64_t addr = ParseAddress(g_opts.address);
    auto nextAttempt = Clock::now();
    int backoff = 0;  // seconds before our next own CONNECT
    bool present = false;
    bool sawIdle = true;  // the stream went down since the last session
    int lastAcl = -2;

    while (!g_stop) {
        int st = DriverLinkState(addr);
        if ((st >= 0) != present) {
            present = st >= 0;
            Stamp(present ? "LdacA2dp device present" : "waiting for the LdacA2dp device");
        }
        bool sinkBusy = st > AVDTP_SRC_IDLE && st < AVDTP_SRC_FAILED;  // negotiating or open
        if (!sinkBusy)
            sawIdle = true;  // includes the device going away (driver restart, unpaired)
        int acl = AclConnected(addr);
        if (acl != lastAcl) {
            if (acl == 1 && lastAcl == 0) {
                Stamp("headphones connected");
                backoff = 0;
                // a moment for them to open AVDTP themselves (some sinks do)
                nextAttempt = Clock::now() + std::chrono::seconds(2);
            } else {
                Stamp(acl == 1    ? "headphones connected"
                      : acl == 0 ? "headphones not connected, waiting for them"
                                 : "headphones connection state unknown, connecting on a timer");
            }
            lastAcl = acl;
        }
        // A stream that came up on its own (the headphones reconnected) is taken at
        // once; anything else waits for the backoff, so a session that keeps failing
        // on an open stream (no audio device to capture, say) does not spin.
        bool mayConnect = acl != 0 && Clock::now() >= nextAttempt;
        if (st < 0 || (!(sinkBusy && sawIdle) && !mayConnect)) {
            WaitForSingleObject(g_stopEvent, 1000);
            continue;
        }
        sawIdle = false;

        Stamp(sinkBusy ? "session (stream set up by the driver or the headphones)" : "session (connecting)");
        std::string err;
        auto started = Clock::now();
        SessionEnd end = RunSession(g_opts, g_stop, err);
        if (g_stop)
            break;
        if (end == SessionEnd::Error)
            printf("session failed: %s\n", err.c_str());

        bool lasted = Clock::now() - started > std::chrono::seconds(30);
        if (end == SessionEnd::Reconfigure || lasted)
            backoff = end == SessionEnd::Reconfigure ? 0 : 2;
        else
            backoff = backoff < 5 ? 5 : backoff * 2 > 60 ? 60 : backoff * 2;
        nextAttempt = Clock::now() + std::chrono::seconds(backoff);
        if (backoff)
            printf("next connect attempt in %d s if the headphones stay connected\n", backoff);
    }
}

void WINAPI ServiceMain(DWORD, LPWSTR *)
{
    g_statusHandle = RegisterServiceCtrlHandlerExW(kServiceName, Handler, nullptr);
    if (!g_statusHandle)
        return;
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Report(SERVICE_START_PENDING);
    OpenLog();
    Stamp("LdacSvc started");
    Report(SERVICE_RUNNING);
    Worker();
    Stamp("LdacSvc stopped");
    Report(SERVICE_STOPPED);
}

std::wstring Widen(const std::string &s)
{
    std::wstring w(s.size() + 1, L'\0');
    w.resize(MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), (int)w.size()) - 1);
    return w;
}

}  // namespace

int ServiceDispatch(const SessionOptions &o)
{
    g_opts = o;
    g_opts.reportSeconds = 60;  // a line a minute is enough in a log that runs for days
    wchar_t name[] = L"LdacSvc";
    SERVICE_TABLE_ENTRYW table[] = { { name, ServiceMain }, { nullptr, nullptr } };
    if (!StartServiceCtrlDispatcherW(table)) {
        fprintf(stderr, "--service is for the service control manager; use `ldacsvc install`\n");
        return 1;
    }
    return 0;
}

int ServiceInstall(const std::string &args)
{
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --service" + (args.empty() ? L"" : L" " + Widen(args));

    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (!scm)
        return fprintf(stderr, "OpenSCManager failed (%lu): run as administrator\n", GetLastError()), 1;
    SC_HANDLE svc = CreateServiceW(scm, kServiceName, L"LDAC A2DP encoder", SERVICE_ALL_ACCESS,
                                   SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, cmd.c_str(),
                                   nullptr, nullptr, L"Audiosrv\0bthserv\0", nullptr, nullptr);
    if (!svc && GetLastError() == ERROR_SERVICE_EXISTS) {
        svc = OpenServiceW(scm, kServiceName, SERVICE_ALL_ACCESS);
        if (svc)
            ChangeServiceConfigW(svc, SERVICE_NO_CHANGE, SERVICE_AUTO_START, SERVICE_NO_CHANGE, cmd.c_str(), nullptr,
                                 nullptr, L"Audiosrv\0bthserv\0", nullptr, nullptr, nullptr);
    }
    if (!svc) {
        fprintf(stderr, "CreateService failed (%lu)\n", GetLastError());
        CloseServiceHandle(scm);
        return 1;
    }

    wchar_t descText[] = L"Encodes system audio to LDAC/SBC for headphones driven by the LdacA2dp driver.";
    SERVICE_DESCRIPTIONW desc = { descText };
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &desc);
    SC_ACTION actions[] = { { SC_ACTION_RESTART, 5000 }, { SC_ACTION_RESTART, 5000 }, { SC_ACTION_RESTART, 30000 } };
    SERVICE_FAILURE_ACTIONSW fa = { 86400, nullptr, nullptr, 3, actions };
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_FAILURE_ACTIONS, &fa);

    bool started = StartServiceW(svc, 0, nullptr) || GetLastError() == ERROR_SERVICE_ALREADY_RUNNING;
    printf("LdacSvc installed%s: %ls\nlog: %s\\ldacsvc.log\n", started ? " and started" : " (start failed)",
           cmd.c_str(), DataRoot().c_str());
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return started ? 0 : 1;
}

int ServiceUninstall()
{
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    SC_HANDLE svc = scm ? OpenServiceW(scm, kServiceName, SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE) : nullptr;
    if (!svc) {
        fprintf(stderr, "cannot open LdacSvc (%lu)\n", GetLastError());
        if (scm)
            CloseServiceHandle(scm);
        return 1;
    }
    SERVICE_STATUS st;
    if (ControlService(svc, SERVICE_CONTROL_STOP, &st))
        for (int i = 0; i < 50 && QueryServiceStatus(svc, &st) && st.dwCurrentState != SERVICE_STOPPED; i++)
            Sleep(200);
    bool ok = DeleteService(svc) != FALSE;
    printf(ok ? "LdacSvc removed\n" : "DeleteService failed\n");
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return ok ? 0 : 1;
}
