$ErrorActionPreference = 'Stop'
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class MenuProbe {
    [StructLayout(LayoutKind.Sequential)] public struct Point { public int X, Y; }
    [StructLayout(LayoutKind.Sequential)] public struct Rect { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindow(string name, string title);
    [DllImport("user32.dll")] public static extern bool GetCursorPos(out Point point);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr window, out Rect rect);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr window, uint message, IntPtr wp, IntPtr lp);
}
'@
$app = Get-Process -Name miniTask -ErrorAction Stop | Where-Object {
    $_.Path -eq (Join-Path (Split-Path -Parent $PSScriptRoot) 'dist\miniTask.exe')
}
if (@($app).Count -ne 1) { throw 'Expected exactly one miniTask process.' }
$tray = [MenuProbe]::FindWindow('miniTask.Tray', 'miniTask')
if ($tray -eq [IntPtr]::Zero) { throw 'Tray window not found.' }
[MenuProbe+Point]$cursor = [MenuProbe+Point]::new()
if (-not [MenuProbe]::GetCursorPos([ref]$cursor)) { throw 'GetCursorPos failed.' }
# Deliberately send wParam=0, as it is undefined for NOTIFYICON_VERSION_4 WM_CONTEXTMENU.
if (-not [MenuProbe]::PostMessage($tray, 0x8001, [IntPtr]::Zero, [IntPtr]0x7B)) {
    throw 'Cannot request tray menu.'
}
$menu = [IntPtr]::Zero
for ($i = 0; $i -lt 20; $i++) {
    Start-Sleep -Milliseconds 100
    $candidate = [MenuProbe]::FindWindow('#32768', $null)
    if ($candidate -eq [IntPtr]::Zero) { continue }
    [uint32]$owner = 0
    $null = [MenuProbe]::GetWindowThreadProcessId($candidate, [ref]$owner)
    if ($owner -eq $app.Id) { $menu = $candidate; break }
}
if ($menu -eq [IntPtr]::Zero) { throw 'Tray menu did not open.' }
try {
    [MenuProbe+Rect]$rect = [MenuProbe+Rect]::new()
    if (-not [MenuProbe]::GetWindowRect($menu, [ref]$rect)) { throw 'GetWindowRect failed.' }
    $nearX = $cursor.X -ge ($rect.Left - 20) -and $cursor.X -le ($rect.Right + 20)
    $nearY = $cursor.Y -ge ($rect.Top - 20) -and $cursor.Y -le ($rect.Bottom + 20)
    [pscustomobject]@{
        Cursor = "$( $cursor.X ),$( $cursor.Y )"
        Menu = "$( $rect.Left ),$( $rect.Top ) - $( $rect.Right ),$( $rect.Bottom )"
        NearCursor = $nearX -and $nearY
    }
    if (-not ($nearX -and $nearY)) { throw 'Menu opened away from the pointer.' }
} finally {
    $null = [MenuProbe]::PostMessage($tray, 0x001F, [IntPtr]::Zero, [IntPtr]::Zero)
}
