using System.Windows;

namespace LdacConfig;

public partial class MainWindow : Window
{
    private readonly MainViewModel _vm = new();

    public MainWindow()
    {
        InitializeComponent();
        DataContext = _vm;
        Loaded += async (_, _) => await _vm.RefreshDevicesAsync();
    }

    private async void OnRefresh(object sender, RoutedEventArgs e) => await _vm.RefreshDevicesAsync();
    private void OnAbout(object sender, RoutedEventArgs e) => _vm.Page = Page.About;
    private void OnFaq(object sender, RoutedEventArgs e) => _vm.Page = Page.Faq;
    private void OnPreset(object sender, RoutedEventArgs e) => _vm.ApplyPreset();
    private void OnReconnect(object sender, RoutedEventArgs e) => _vm.Reconnect();
}
