# Latency the render endpoint reports to Windows: device period and stream latency of a
# shared-mode stream (what an application sees), for the endpoint whose name contains -Match.
# Read-only: opens and releases a stream, plays nothing.
param([string] $Match = 'JBL')
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
[Guid("1CB9AD4C-DBFA-4c32-B178-C2F568A703B2"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IAudioClient {
    int Initialize(int shareMode, uint flags, long bufferDuration, long periodicity, IntPtr format, IntPtr session);
    int GetBufferSize(out uint frames);
    int GetStreamLatency(out long latency);
    int GetCurrentPadding(out uint frames);
    int IsFormatSupported(int shareMode, IntPtr format, out IntPtr closest);
    int GetMixFormat(out IntPtr format);
    int GetDevicePeriod(out long defaultPeriod, out long minimumPeriod);
}
[Guid("D666063F-1587-4E43-81F1-B948E807363F"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IMMDevice {
    int Activate(ref Guid iid, int ctx, IntPtr p, [MarshalAs(UnmanagedType.IUnknown)] out object o);
    int OpenPropertyStore(int access, out IPropertyStore store);
    int GetId([MarshalAs(UnmanagedType.LPWStr)] out string id);
}
[Guid("886d8eeb-8cf2-4446-8d02-cdba1dbdcf99"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IPropertyStore { int GetCount(out int c); int GetAt(int i, out PropertyKey k); int GetValue(ref PropertyKey k, out PropVariant v); }
[StructLayout(LayoutKind.Sequential)] struct PropertyKey { public Guid fmtid; public int pid; }
[StructLayout(LayoutKind.Explicit)] struct PropVariant { [FieldOffset(0)] public short vt; [FieldOffset(8)] public IntPtr p; }
[Guid("0BD7A1BE-7A1A-44DB-8397-CC5392387B5E"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IMMDeviceCollection { int GetCount(out uint c); int Item(uint i, out IMMDevice d); }
[Guid("A95664D2-9614-4F35-A746-DE8DB63617E6"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IMMDeviceEnumerator { int EnumAudioEndpoints(int flow, int state, out IMMDeviceCollection c); }
[ComImport, Guid("BCDE0395-E52F-467C-8E3D-C4579291692E")] class MMDeviceEnumerator {}
public static class Lat {
    public static string Run(string match) {
        var en = (IMMDeviceEnumerator)(new MMDeviceEnumerator());
        IMMDeviceCollection all; en.EnumAudioEndpoints(0, 1, out all);
        uint n; all.GetCount(out n);
        var sb = new System.Text.StringBuilder();
        for (uint i = 0; i < n; i++) {
            IMMDevice d; all.Item(i, out d);
            IPropertyStore ps; d.OpenPropertyStore(0, out ps);
            var key = new PropertyKey { fmtid = new Guid("a45c254e-df1c-4efd-8020-67d146a850e0"), pid = 14 }; // PKEY_Device_FriendlyName
            PropVariant v; ps.GetValue(ref key, out v);
            string name = Marshal.PtrToStringUni(v.p);
            if (name == null || name.IndexOf(match, StringComparison.OrdinalIgnoreCase) < 0) continue;
            Guid iid = typeof(IAudioClient).GUID; object o;
            d.Activate(ref iid, 23, IntPtr.Zero, out o);
            var c = (IAudioClient)o;
            long def, min; c.GetDevicePeriod(out def, out min);
            IntPtr fmt; c.GetMixFormat(out fmt);
            int rate = Marshal.ReadInt32(fmt, 4); short bits = Marshal.ReadInt16(fmt, 14);
            int hr = c.Initialize(0, 0, 0, 0, fmt, IntPtr.Zero);
            long lat = 0; uint buf = 0;
            if (hr == 0) { c.GetStreamLatency(out lat); c.GetBufferSize(out buf); }
            sb.AppendFormat("{0}: mix {1} Hz/{2}-bit, period {3:0.0} ms (min {4:0.0}), stream latency {5:0.0} ms, buffer {6:0.0} ms{7}\n",
                name, rate, bits, def / 1e4, min / 1e4, lat / 1e4, rate > 0 ? buf * 1000.0 / rate : 0, hr == 0 ? "" : " (init 0x" + hr.ToString("X8") + ")");
            Marshal.ReleaseComObject(c);
        }
        return sb.Length > 0 ? sb.ToString() : "no active render endpoint matching '" + match + "'";
    }
}
'@
[Lat]::Run($Match)
