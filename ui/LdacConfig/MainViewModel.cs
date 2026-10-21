using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Globalization;
using System.IO;
using System.Windows.Data;
using System.Windows.Threading;

namespace LdacConfig;

public enum Page { Device, About, Faq }

public sealed class MainViewModel : Observable
{
    private BtAudioDevice? _selected;
    private Page _page = Page.Device;
    private string _message = "";
    private bool _loading = true;
    private readonly DispatcherTimer _statusTimer;

    public ObservableCollection<BtAudioDevice> Devices { get; } = new();

    public MainViewModel()
    {
        SettingsStore.Load();
        // The encoder service rewrites the status file while streaming; poll it.
        _statusTimer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(1) };
        _statusTimer.Tick += (_, _) => RaiseStatus();
        _statusTimer.Start();
    }

    public BtAudioDevice? Selected
    {
        get => _selected;
        set
        {
            if (!Set(ref _selected, value))
                return;
            Page = Page.Device;
            Message = "";
            Raise(nameof(HasDevice));
            RaiseStatus();
        }
    }

    public bool HasDevice => _selected is not null;
    public bool Loading { get => _loading; private set { Set(ref _loading, value); Raise(nameof(NoDevices)); } }
    public bool NoDevices => !Loading && Devices.Count == 0;

    public Page Page
    {
        get => _page;
        set
        {
            if (Set(ref _page, value))
            {
                Raise(nameof(ShowDevice));
                Raise(nameof(ShowAbout));
                Raise(nameof(ShowFaq));
            }
        }
    }

    public bool ShowDevice => Page == Page.Device;
    public bool ShowAbout => Page == Page.About;
    public bool ShowFaq => Page == Page.Faq;

    public string Message { get => _message; set => Set(ref _message, value); }

    public string SettingsPath => Path.Combine(SettingsStore.Root, "settings.json");

    // ---- current state ------------------------------------------------------

    private LinkStatus? Status => _selected is null ? null : SettingsStore.ReadStatus(_selected.AddressText);
    public bool HasStatus => Status is not null;
    public string StatusCodec => Status?.Codec ?? "—";
    public string StatusChannelMode => Status?.ChannelMode ?? "—";
    public string StatusSampleRate => Status is { } s ? FormatRate(s.SampleRate) : "—";
    public string StatusBitDepth => Status is { } s ? $"{s.BitDepth} бит" : "—";
    public string StatusBitrate => Status is { } s ? $"{s.BitrateKbps} kbps" : "—";

    private static string FormatRate(int hz) =>
        (hz / 1000.0).ToString(hz % 1000 == 0 ? "0" : "0.0", CultureInfo.InvariantCulture) + " kHz";

    private void RaiseStatus()
    {
        foreach (var p in new[] { nameof(HasStatus), nameof(StatusCodec), nameof(StatusChannelMode),
                                  nameof(StatusSampleRate), nameof(StatusBitDepth), nameof(StatusBitrate) })
            Raise(p);
    }

    // ---- actions ------------------------------------------------------------

    public async Task RefreshDevicesAsync()
    {
        Loading = true;
        var keep = _selected?.Address;
        try
        {
            var found = await BluetoothAudioDevices.FindPairedAsync();
            foreach (var d in Devices)
                d.Settings.PropertyChanged -= OnSettingsChanged;
            Devices.Clear();
            foreach (var (name, address, connected) in found)
            {
                var dev = new BtAudioDevice
                {
                    Name = name,
                    Address = address,
                    Connected = connected,
                    Settings = SettingsStore.For(BtAudioDevice.FormatAddress(address)),
                };
                dev.Settings.PropertyChanged += OnSettingsChanged;
                Devices.Add(dev);
            }
            Selected = Devices.FirstOrDefault(d => d.Address == keep) ?? Devices.FirstOrDefault();
        }
        catch (Exception ex)
        {
            Message = "Не удалось получить список устройств Bluetooth: " + ex.Message;
        }
        finally
        {
            Loading = false;
        }
    }

    private void OnSettingsChanged(object? sender, PropertyChangedEventArgs e)
    {
        try
        {
            SettingsStore.Save();
            Message = $"Настройки сохранены: {SettingsPath}";
        }
        catch (Exception ex)
        {
            Message = "Не удалось сохранить настройки: " + ex.Message;
        }
    }

    public void ApplyPreset()
    {
        if (_selected is null)
            return;
        _selected.Settings.ApplyPreset();
        OnSettingsChanged(null, new PropertyChangedEventArgs(null));
    }

    public void Reconnect()
    {
        if (_selected is null)
            return;
        OnSettingsChanged(null, new PropertyChangedEventArgs(null));
        Message = "Настройки сохранены. Переподключение с новым кодеком выполнит служба драйвера " +
                  "(этап 6), пока она не установлена — переподключите наушники вручную.";
    }
}

/// <summary>Binds a RadioButton to one value of an enum property.</summary>
public sealed class EnumEqualsConverter : IValueConverter
{
    public object Convert(object value, Type targetType, object parameter, CultureInfo culture) =>
        value?.ToString() == parameter?.ToString();

    public object ConvertBack(object value, Type targetType, object parameter, CultureInfo culture) =>
        value is true ? Enum.Parse(targetType, (string)parameter) : Binding.DoNothing;
}

public sealed class InverseBoolToVisibilityConverter : IValueConverter
{
    public object Convert(object value, Type targetType, object parameter, CultureInfo culture) =>
        value is true ? System.Windows.Visibility.Collapsed : System.Windows.Visibility.Visible;

    public object ConvertBack(object value, Type targetType, object parameter, CultureInfo culture) => Binding.DoNothing;
}
