param(
  [Parameter(Mandatory = $true)][string]$Path,
  [Parameter(Mandatory = $true)][string]$StopFile
)

$ErrorActionPreference = 'Continue'
try { Set-Content -Path $Path -Value '' -Encoding ASCII } catch { }

Add-Type @'
using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;

public static class EpoDbwinListener {
  const uint PAGE_READWRITE = 0x04;
  const uint FILE_MAP_READ = 0x0004;
  const uint WAIT_OBJECT_0 = 0;

  [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
  static extern IntPtr CreateFileMapping(IntPtr hFile, IntPtr sa, uint protect,
                                         uint maxHigh, uint maxLow, string name);
  [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
  static extern IntPtr CreateEvent(IntPtr sa, bool manual, bool initial, string name);
  [DllImport("kernel32.dll", SetLastError = true)]
  static extern IntPtr MapViewOfFile(IntPtr map, uint access, uint high, uint low, UIntPtr bytes);
  [DllImport("kernel32.dll", SetLastError = true)]
  static extern bool UnmapViewOfFile(IntPtr p);
  [DllImport("kernel32.dll", SetLastError = true)]
  static extern bool SetEvent(IntPtr h);
  [DllImport("kernel32.dll", SetLastError = true)]
  static extern uint WaitForSingleObject(IntPtr h, uint ms);
  [DllImport("kernel32.dll", SetLastError = true)]
  static extern bool CloseHandle(IntPtr h);

  public static void Run(string path, string stopFile) {
    IntPtr map = CreateFileMapping(new IntPtr(-1), IntPtr.Zero, PAGE_READWRITE, 0, 4096, "DBWIN_BUFFER");
    IntPtr view = MapViewOfFile(map, FILE_MAP_READ, 0, 0, (UIntPtr)4096);
    IntPtr bufferReady = CreateEvent(IntPtr.Zero, false, false, "DBWIN_BUFFER_READY");
    IntPtr dataReady = CreateEvent(IntPtr.Zero, false, false, "DBWIN_DATA_READY");
    if (map == IntPtr.Zero || view == IntPtr.Zero || bufferReady == IntPtr.Zero || dataReady == IntPtr.Zero) {
      return;
    }
    SetEvent(bufferReady);
    using (var writer = new StreamWriter(path, true)) {
      writer.AutoFlush = true;
      while (!File.Exists(stopFile)) {
        if (WaitForSingleObject(dataReady, 200) != WAIT_OBJECT_0) {
          continue;
        }
        int pid = Marshal.ReadInt32(view);
        string text = Marshal.PtrToStringAnsi(new IntPtr(view.ToInt64() + 4));
        SetEvent(bufferReady);
        if (text != null) {
          string trimmed = text.TrimEnd('\r', '\n');
          if (trimmed.StartsWith("EPOCANVA_TRACE|")) {
            writer.WriteLine(pid.ToString() + "|" + trimmed);
          }
        }
      }
    }
    UnmapViewOfFile(view);
    CloseHandle(map);
    CloseHandle(bufferReady);
    CloseHandle(dataReady);
  }
}
'@

[EpoDbwinListener]::Run($Path, $StopFile)
