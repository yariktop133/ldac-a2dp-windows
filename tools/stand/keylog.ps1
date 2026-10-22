# Test stand: log media keys (as Windows sees them) to C:\stand\keys.log for -Seconds.
# Must run in the interactive session (schtasks /it), not from guestcontrol.
param([int]$Seconds = 60)
Add-Type -ReferencedAssemblies System.Windows.Forms -TypeDefinition @'
using System;
using System.IO;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Windows.Forms;
public static class KeyLog {
    delegate IntPtr Proc(int code, IntPtr wp, IntPtr lp);
    [DllImport("user32.dll")] static extern IntPtr SetWindowsHookEx(int id, Proc fn, IntPtr mod, uint tid);
    [DllImport("user32.dll")] static extern IntPtr CallNextHookEx(IntPtr h, int code, IntPtr wp, IntPtr lp);
    [DllImport("kernel32.dll")] static extern IntPtr GetModuleHandle(string name);
    static Proc keep; static StreamWriter log;
    public static void Run(int seconds, string path) {
        log = new StreamWriter(path, false); log.AutoFlush = true;
        log.WriteLine(DateTime.Now.ToString("HH:mm:ss") + " listening");
        keep = (code, wp, lp) => {
            if (code >= 0 && (wp == (IntPtr)0x100 || wp == (IntPtr)0x101)) {
                int vk = Marshal.ReadInt32(lp);
                string name = vk == 0xB3 ? "PLAY_PAUSE" : vk == 0xB2 ? "STOP" : vk == 0xB0 ? "NEXT" : vk == 0xB1 ? "PREV"
                            : vk == 0xAF ? "VOLUME_UP" : vk == 0xAE ? "VOLUME_DOWN" : vk == 0xAD ? "MUTE" : null;
                if (name != null) log.WriteLine(DateTime.Now.ToString("HH:mm:ss.fff") + " " + name + (wp == (IntPtr)0x100 ? " down" : " up"));
            }
            return CallNextHookEx(IntPtr.Zero, code, wp, lp);
        };
        SetWindowsHookEx(13, keep, GetModuleHandle(null), 0);
        var t = new Timer(); t.Interval = seconds * 1000; t.Tick += (s, e) => Application.Exit(); t.Start();
        Application.Run();
        log.WriteLine(DateTime.Now.ToString("HH:mm:ss") + " done");
    }
}
'@
[KeyLog]::Run($Seconds, 'C:\stand\keys.log')
