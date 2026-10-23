// pairhelper: pairs a Bluetooth device whose name contains argv[0] using custom
// pairing (accepts ConfirmOnly / PIN ceremonies the way the Settings "Add device"
// flow does). .NET Framework 4.x console app calling WinRT; built by
// build-pairhelper.cmd, run inside the test VM.
using System;
using System.Linq;
using Windows.Devices.Bluetooth;
using Windows.Devices.Enumeration;

static class Program
{
    static int Main(string[] args)
    {
        if (args.Length < 1)
        {
            Console.WriteLine("usage: pairhelper <name part> [seconds]");
            return 2;
        }
        string name = args[0];
        int seconds = args.Length > 1 ? int.Parse(args[1]) : 60;
        string selector = BluetoothDevice.GetDeviceSelectorFromPairingState(false);
        DeviceInformation target = null;
        DateTime deadline = DateTime.Now.AddSeconds(seconds);
        while (target == null && DateTime.Now < deadline)
        {
            var all = DeviceInformation.FindAllAsync(selector).AsTask().Result;
            foreach (var d in all)
                Console.WriteLine("seen: " + d.Name);
            target = all.FirstOrDefault(d => d.Name.IndexOf(name, StringComparison.OrdinalIgnoreCase) >= 0);
        }
        if (target == null)
        {
            Console.WriteLine("not found: " + name);
            return 3;
        }

        Console.WriteLine("pairing " + target.Name + " (" + target.Id + ")");
        var custom = target.Pairing.Custom;
        custom.PairingRequested += (sender, e) =>
        {
            Console.WriteLine("ceremony: " + e.PairingKind + (string.IsNullOrEmpty(e.Pin) ? "" : " pin " + e.Pin));
            if (e.PairingKind == DevicePairingKinds.ProvidePin)
                e.Accept("0000");
            else
                e.Accept();
        };
        var kinds = DevicePairingKinds.ConfirmOnly | DevicePairingKinds.ConfirmPinMatch |
                    DevicePairingKinds.DisplayPin | DevicePairingKinds.ProvidePin;
        var result = custom.PairAsync(kinds, DevicePairingProtectionLevel.Default).AsTask().Result;
        Console.WriteLine("status: " + result.Status + ", protection: " + result.ProtectionLevelUsed);
        if (result.Status == DevicePairingResultStatus.RequiredHandlerNotRegistered)
        {
            // a ceremony custom pairing does not offer: let Windows run its own (basic pairing)
            result = target.Pairing.PairAsync().AsTask().Result;
            Console.WriteLine("basic pairing status: " + result.Status + ", protection: " + result.ProtectionLevelUsed);
        }
        return result.Status == DevicePairingResultStatus.Paired || result.Status == DevicePairingResultStatus.AlreadyPaired ? 0 : 1;
    }
}
