using System.ComponentModel;
using System.Runtime.CompilerServices;
using System.Text.Json.Serialization;

namespace LdacConfig;

public abstract class Observable : INotifyPropertyChanged
{
    public event PropertyChangedEventHandler? PropertyChanged;

    protected void Raise([CallerMemberName] string? name = null) =>
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));

    protected bool Set<T>(ref T field, T value, [CallerMemberName] string? name = null)
    {
        if (EqualityComparer<T>.Default.Equals(field, value))
            return false;
        field = value;
        Raise(name);
        return true;
    }
}

public enum CodecType { LDAC, SBC }
public enum DriverChoice { Microsoft, Ldac }
public enum LdacQuality { MQ, SQ, HQ }

/// <summary>
/// Per-device settings. Serialized to %ProgramData%\LdacA2dp\settings.json and read by the
/// encoder service; field meaning matches a2dp_prefs in avdtp/include/a2dp_codec.h
/// (see docs/settings-format.md).
/// </summary>
public sealed class DeviceSettings : Observable
{
    private DriverChoice _driver = DriverChoice.Ldac;
    private CodecType _codec = CodecType.LDAC;
    private bool _allowMono;
    private bool _allowStereo = true;
    private bool _rate44 = true, _rate48 = true, _rate88 = true, _rate96 = true;
    private bool _bits16 = true, _bits24 = true;
    private string _channelMode = "Stereo";
    private LdacQuality _quality = LdacQuality.SQ;
    private bool _adaptive = true;
    private int _holdSeconds = 3;

    public DriverChoice Driver { get => _driver; set => Set(ref _driver, value); }

    public CodecType Codec
    {
        get => _codec;
        set
        {
            if (!Set(ref _codec, value))
                return;
            Raise(nameof(IsLdac));
            Raise(nameof(ChannelModes));
            if (!ChannelModes.Contains(ChannelMode))
                ChannelMode = ChannelModes[0];
            if (value == CodecType.SBC && !Rate44 && !Rate48)
                Rate48 = true;
        }
    }

    [JsonIgnore] public bool IsLdac => Codec == CodecType.LDAC;

    [JsonIgnore]
    public string[] ChannelModes => Codec == CodecType.LDAC
        ? ["Stereo", "Dual Channel"]
        : ["Joint Stereo", "Stereo", "Dual Channel"];

    // "Allow at least one" rules: refuse to clear the last option in a group.
    public bool AllowMono { get => _allowMono; set => SetGuarded(ref _allowMono, value, !value && !AllowStereo); }
    public bool AllowStereo { get => _allowStereo; set => SetGuarded(ref _allowStereo, value, !value && !AllowMono); }

    public bool Rate44 { get => _rate44; set => SetGuarded(ref _rate44, value, !value && RateCount() == 1); }
    public bool Rate48 { get => _rate48; set => SetGuarded(ref _rate48, value, !value && RateCount() == 1); }
    public bool Rate88 { get => _rate88; set => SetGuarded(ref _rate88, value, !value && RateCount() == 1); }
    public bool Rate96 { get => _rate96; set => SetGuarded(ref _rate96, value, !value && RateCount() == 1); }

    public bool Bits16 { get => _bits16; set => SetGuarded(ref _bits16, value, !value && !Bits24); }
    public bool Bits24 { get => _bits24; set => SetGuarded(ref _bits24, value, !value && !Bits16); }

    public string ChannelMode { get => _channelMode; set => Set(ref _channelMode, value); }
    public LdacQuality Quality { get => _quality; set => Set(ref _quality, value); }
    public bool Adaptive { get => _adaptive; set => Set(ref _adaptive, value); }

    /// <summary>Seconds the stream stays started after the last sound (3 = as Windows; more keeps
    /// a series of short sounds from losing their start to the link setup).</summary>
    public int HoldSeconds { get => _holdSeconds; set => Set(ref _holdSeconds, value); }
    [JsonIgnore] public int[] HoldChoices { get; } = [3, 10, 15, 30];

    private int RateCount() => Codec == CodecType.SBC
        ? (_rate44 ? 1 : 0) + (_rate48 ? 1 : 0)
        : (_rate44 ? 1 : 0) + (_rate48 ? 1 : 0) + (_rate88 ? 1 : 0) + (_rate96 ? 1 : 0);

    private void SetGuarded(ref bool field, bool value, bool wouldLeaveNone, [CallerMemberName] string? name = null)
    {
        if (wouldLeaveNone)
        {
            Raise(name); // bounce the checkbox back
            return;
        }
        Set(ref field, value, name);
    }

    /// <summary>"Most commonly used settings" for the selected codec.</summary>
    public void ApplyPreset()
    {
        _allowMono = false; _allowStereo = true;
        _rate44 = _rate48 = true;
        _rate88 = _rate96 = Codec == CodecType.LDAC;
        _bits16 = _bits24 = true;
        _channelMode = ChannelModes[0];
        _quality = LdacQuality.SQ;
        _adaptive = true;
        _holdSeconds = 3;
        Raise(string.Empty); // refresh all bindings
    }
}

public sealed class BtAudioDevice : Observable
{
    private bool _connected;

    public required string Name { get; init; }
    public required ulong Address { get; init; }
    public string AddressText => FormatAddress(Address);

    public static string FormatAddress(ulong address) =>
        string.Join(":", BitConverter.GetBytes(address).Take(6).Reverse().Select(b => b.ToString("X2")));
    public bool Connected { get => _connected; set => Set(ref _connected, value); }
    public required DeviceSettings Settings { get; init; }
}

/// <summary>Live link state, written by the encoder service to status\&lt;address&gt;.json
/// once a second while it serves the headphones (see service/settings.cpp WriteStatus).</summary>
public sealed class LinkStatus
{
    public string Codec { get; set; } = "";
    public string ChannelMode { get; set; } = "";
    public int SampleRate { get; set; }
    public int BitDepth { get; set; }
    public int BitrateKbps { get; set; }
    public bool Streaming { get; set; }
    public string Quality { get; set; } = "";
    public long DroppedPackets { get; set; }
    public string Avrcp { get; set; } = "unknown";
    public bool AbsoluteVolume { get; set; }
    public double VolumeDb { get; set; }
    public bool Muted { get; set; }
    [System.Text.Json.Serialization.JsonIgnore] public DateTime Written { get; set; }
}
