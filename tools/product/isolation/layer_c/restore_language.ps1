Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class LangRestore {
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr ActivateKeyboardLayout(IntPtr hkl, uint flags);
  [DllImport("user32.dll")] public static extern IntPtr GetKeyboardLayout(uint id);
}
'@
$zh = [IntPtr]0x08040804
$fg = [LangRestore]::GetForegroundWindow()
[void][LangRestore]::PostMessage($fg, 0x50, [IntPtr]::Zero, $zh)
[void][LangRestore]::ActivateKeyboardLayout($zh, 0)
Start-Sleep -Milliseconds 400
Write-Output ('foreground_hkl=0x{0:X8}' -f ([LangRestore]::GetKeyboardLayout(0).ToInt64()))
