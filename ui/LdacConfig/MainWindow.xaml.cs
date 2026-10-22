using System.Windows;

namespace LdacConfig;

public partial class MainWindow : Window
{
    private readonly MainViewModel _vm = new();

    public MainWindow()
    {
        InitializeComponent();
        DataContext = _vm;
        // 1160x1000 does not fit a 1366x768 laptop: shrink to the work area, the pages scroll.
        var area = SystemParameters.WorkArea;
        Width = Math.Min(Width, area.Width - 40);
        Height = Math.Min(Height, area.Height - 40);
        MinWidth = Math.Min(MinWidth, Width);
        MinHeight = Math.Min(MinHeight, Height);
        Loaded += async (_, _) => await _vm.RefreshDevicesAsync();
    }

    private async void OnRefresh(object sender, RoutedEventArgs e) => await _vm.RefreshDevicesAsync();
    private void OnAbout(object sender, RoutedEventArgs e) => _vm.Page = Page.About;
    private void OnFaq(object sender, RoutedEventArgs e) => _vm.Page = Page.Faq;
    private void OnPreset(object sender, RoutedEventArgs e) => _vm.ApplyPreset();
    private void OnReconnect(object sender, RoutedEventArgs e) => _vm.Reconnect();
}
