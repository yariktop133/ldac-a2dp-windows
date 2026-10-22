using System.IO;
using System.Text.Json;
using System.Text.Json.Serialization;
using Windows.Devices.Bluetooth;
using Windows.Devices.Enumeration;

namespace LdacConfig;

/// <summary>Paired Bluetooth devices of the Audio/Video major class (headphones, speakers).</summary>
public static class BluetoothAudioDevices
{
    public static async Task<List<(string Name, ulong Address, bool Connected)>> FindPairedAsync()
    {
        var result = new List<(string, ulong, bool)>();
        var selector = BluetoothDevice.GetDeviceSelectorFromPairingState(true);
        foreach (var info in await DeviceInformation.FindAllAsync(selector))
        {
            using var dev = await BluetoothDevice.FromIdAsync(info.Id);
            if (dev is null || dev.ClassOfDevice.MajorClass != BluetoothMajorClass.AudioVideo)
                continue;
            result.Add((dev.Name, dev.BluetoothAddress, dev.ConnectionStatus == BluetoothConnectionStatus.Connected));
        }
        return result.OrderBy(d => d.Item1, StringComparer.CurrentCultureIgnoreCase).ToList();
    }
}

/// <summary>State of the LdacSvc encoder service, straight from the service control manager.</summary>
public static class EncoderService
{
    [System.Runtime.InteropServices.DllImport("advapi32.dll", SetLastError = true, CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
    private static extern IntPtr OpenSCManagerW(string? machine, string? database, uint access);
    [System.Runtime.InteropServices.DllImport("advapi32.dll", SetLastError = true, CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
    private static extern IntPtr OpenServiceW(IntPtr scm, string name, uint access);
    [System.Runtime.InteropServices.DllImport("advapi32.dll", SetLastError = true)]
    private static extern bool QueryServiceStatus(IntPtr service, out ServiceStatus status);
    [System.Runtime.InteropServices.DllImport("advapi32.dll")]
    private static extern bool CloseServiceHandle(IntPtr handle);

    [System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Sequential)]
    private struct ServiceStatus
    {
        public uint Type, State, ControlsAccepted, Win32ExitCode, SpecificExitCode, CheckPoint, WaitHint;
    }

    /// <summary>"работает", "остановлена", "не установлена", ...</summary>
    public static string State()
    {
        IntPtr scm = OpenSCManagerW(null, null, 0x0001 /* CONNECT */);
        if (scm == IntPtr.Zero)
            return "неизвестно";
        try
        {
            IntPtr svc = OpenServiceW(scm, "LdacSvc", 0x0004 /* QUERY_STATUS */);
            if (svc == IntPtr.Zero)
                return "не установлена";
            try
            {
                if (!QueryServiceStatus(svc, out var st))
                    return "неизвестно";
                return st.State switch
                {
                    1 => "остановлена",
                    2 => "запускается",
                    3 => "останавливается",
                    4 => "работает",
                    _ => "приостановлена",
                };
            }
            finally { CloseServiceHandle(svc); }
        }
        finally { CloseServiceHandle(scm); }
    }
}

/// <summary>
/// settings.json: { "devices": { "AA:BB:CC:DD:EE:FF": { ...DeviceSettings... } } }
/// Lives in %ProgramData%\LdacA2dp so the encoder service (running as a system service) can read it.
/// </summary>
public static class SettingsStore
{
    public static readonly string Root =
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData), "LdacA2dp");

    private static string SettingsPath => Path.Combine(Root, "settings.json");
    private static string StatusPath(string address) => Path.Combine(Root, "status", address.Replace(':', '-') + ".json");

    private static readonly JsonSerializerOptions Json = new()
    {
        WriteIndented = true,
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        Converters = { new JsonStringEnumConverter() },
    };

    private sealed class FileModel
    {
        public Dictionary<string, DeviceSettings> Devices { get; set; } = new();
    }

    private static FileModel _cache = new();

    public static void Load()
    {
        try
        {
            if (File.Exists(SettingsPath))
                _cache = JsonSerializer.Deserialize<FileModel>(File.ReadAllText(SettingsPath), Json) ?? new();
        }
        catch (Exception)
        {
            _cache = new(); // corrupt file: start from defaults, it is rewritten on next save
        }
    }

    public static DeviceSettings For(string address) =>
        _cache.Devices.TryGetValue(address, out var s) ? s : _cache.Devices[address] = new DeviceSettings();

    public static void Save()
    {
        Directory.CreateDirectory(Root);
        var tmp = SettingsPath + ".tmp";
        File.WriteAllText(tmp, JsonSerializer.Serialize(_cache, Json));
        File.Move(tmp, SettingsPath, overwrite: true);
    }

    public static LinkStatus? ReadStatus(string address)
    {
        try
        {
            var path = StatusPath(address);
            if (!File.Exists(path))
                return null;
            var s = JsonSerializer.Deserialize<LinkStatus>(File.ReadAllText(path), Json);
            if (s is not null)
                s.Written = File.GetLastWriteTimeUtc(path);
            return s;
        }
        catch (Exception)
        {
            return null;
        }
    }
}
