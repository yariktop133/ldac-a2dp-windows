# Plays a 16-bit PCM WAV to the render endpoint whose name contains -Match (shared mode,
# Windows converts the rate/format), without touching the default device.
param([Parameter(Mandatory)][string] $Wav, [string] $Match = 'JBL', [double] $PauseAt = -1, [double] $PauseFor = 0)
Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Threading;
using System.Runtime.InteropServices;
[Guid("1CB9AD4C-DBFA-4c32-B178-C2F568A703B2"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IAudioClient {
    int Initialize(int shareMode, uint flags, long bufferDuration, long periodicity, IntPtr format, IntPtr session);
    int GetBufferSize(out uint frames); int GetStreamLatency(out long l); int GetCurrentPadding(out uint frames);
    int IsFormatSupported(int m, IntPtr f, out IntPtr c); int GetMixFormat(out IntPtr f); int GetDevicePeriod(out long d, out long m);
    int Start(); int Stop(); int Reset(); int SetEventHandle(IntPtr h);
    int GetService(ref Guid iid, [MarshalAs(UnmanagedType.IUnknown)] out object o);
}
[Guid("F294ACFC-3146-4483-A7BF-ADDCA7C260E2"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IAudioRenderClient { int GetBuffer(uint frames, out IntPtr data); int ReleaseBuffer(uint frames, uint flags); }
[Guid("D666063F-1587-4E43-81F1-B948E807363F"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IMMDevice { int Activate(ref Guid iid, int ctx, IntPtr p, [MarshalAs(UnmanagedType.IUnknown)] out object o); int OpenPropertyStore(int a, out IPropertyStore s); }
[Guid("886d8eeb-8cf2-4446-8d02-cdba1dbdcf99"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IPropertyStore { int GetCount(out int c); int GetAt(int i, out PK k); int GetValue(ref PK k, out PV v); }
[StructLayout(LayoutKind.Sequential)] struct PK { public Guid f; public int p; }
[StructLayout(LayoutKind.Explicit)] struct PV { [FieldOffset(0)] public short vt; [FieldOffset(8)] public IntPtr p; }
[Guid("0BD7A1BE-7A1A-44DB-8397-CC5392387B5E"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IMMDeviceCollection { int GetCount(out uint c); int Item(uint i, out IMMDevice d); }
[Guid("A95664D2-9614-4F35-A746-DE8DB63617E6"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IMMDeviceEnumerator { int EnumAudioEndpoints(int f, int s, out IMMDeviceCollection c); }
[ComImport, Guid("BCDE0395-E52F-467C-8E3D-C4579291692E")] class E {}
public static class Player {
    public static string Play(string wav, string match, double pauseAt, double pauseFor) {
        var en = (IMMDeviceEnumerator)(new E()); IMMDeviceCollection all; en.EnumAudioEndpoints(0, 1, out all);
        uint n; all.GetCount(out n); IMMDevice dev = null; string name = null;
        for (uint i = 0; i < n && dev == null; i++) {
            IMMDevice d; all.Item(i, out d); IPropertyStore s; d.OpenPropertyStore(0, out s);
            var k = new PK { f = new Guid("a45c254e-df1c-4efd-8020-67d146a850e0"), p = 14 }; PV v; s.GetValue(ref k, out v);
            string nm = Marshal.PtrToStringUni(v.p);
            if (nm != null && nm.IndexOf(match, StringComparison.OrdinalIgnoreCase) >= 0 && nm.IndexOf("Hands-Free", StringComparison.OrdinalIgnoreCase) < 0) { dev = d; name = nm; }
        }
        if (dev == null) return "no endpoint matching " + match;
        byte[] file = File.ReadAllBytes(wav);
        int fmtPos = 12; while (System.Text.Encoding.ASCII.GetString(file, fmtPos, 4) != "fmt ") fmtPos += 8 + BitConverter.ToInt32(file, fmtPos + 4);
        short ch = BitConverter.ToInt16(file, fmtPos + 10); int rate = BitConverter.ToInt32(file, fmtPos + 12); short bits = BitConverter.ToInt16(file, fmtPos + 22);
        int dataPos = 12; while (System.Text.Encoding.ASCII.GetString(file, dataPos, 4) != "data") dataPos += 8 + BitConverter.ToInt32(file, dataPos + 4);
        int dataLen = BitConverter.ToInt32(file, dataPos + 4); dataPos += 8;
        int block = ch * bits / 8;
        IntPtr wfx = Marshal.AllocHGlobal(18);
        Marshal.WriteInt16(wfx, 0, 1); Marshal.WriteInt16(wfx, 2, ch); Marshal.WriteInt32(wfx, 4, rate);
        Marshal.WriteInt32(wfx, 8, rate * block); Marshal.WriteInt16(wfx, 12, (short)block); Marshal.WriteInt16(wfx, 14, bits); Marshal.WriteInt16(wfx, 16, 0);
        Guid iid = typeof(IAudioClient).GUID; object o; dev.Activate(ref iid, 23, IntPtr.Zero, out o); var c = (IAudioClient)o;
        int hr = c.Initialize(0, 0x80000000 | 0x08000000, 2000000, 0, wfx, IntPtr.Zero); // AUTOCONVERTPCM | SRC_DEFAULT_QUALITY, 200 ms
        if (hr != 0) return name + ": Initialize 0x" + hr.ToString("X8");
        uint bufFrames; c.GetBufferSize(out bufFrames);
        Guid rid = typeof(IAudioRenderClient).GUID; object ro; c.GetService(ref rid, out ro); var rc = (IAudioRenderClient)ro;
        int pos = 0; bool started = false, paused = false; var sw = System.Diagnostics.Stopwatch.StartNew();
        while (pos < dataLen) {
            double t = sw.Elapsed.TotalSeconds;
            if (pauseAt >= 0 && !paused && t >= pauseAt) { c.Stop(); paused = true; Thread.Sleep((int)(pauseFor * 1000)); c.Start(); }
            uint pad; c.GetCurrentPadding(out pad);
            uint frames = Math.Min(bufFrames - pad, (uint)((dataLen - pos) / block));
            if (frames > 0) {
                IntPtr p; rc.GetBuffer(frames, out p); Marshal.Copy(file, dataPos + pos, p, (int)frames * block); rc.ReleaseBuffer(frames, 0);
                pos += (int)frames * block;
            }
            if (!started) { c.Start(); started = true; }
            Thread.Sleep(10);
        }
        Thread.Sleep(300); c.Stop(); Marshal.FreeHGlobal(wfx);
        return name + ": played " + (dataLen / (double)(rate * block)).ToString("0.0") + " s";
    }
}
'@
[Player]::Play((Resolve-Path $Wav).Path, $Match, $PauseAt, $PauseFor)
