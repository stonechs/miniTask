$ErrorActionPreference = 'Stop'
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class RotationWindow {
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr FindWindow(string name, string title);
    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);
    [DllImport("user32.dll")]
    public static extern IntPtr SendMessage(IntPtr window, uint message, IntPtr wp, IntPtr lp);
}
'@
$root = Split-Path -Parent $PSScriptRoot
$program = Join-Path $root 'dist\miniTask.exe'
$tempRoot = Join-Path $root 'dist\diagnostics\rotation'
$folder = Join-Path $tempRoot 'miniTask'
$null = New-Item -ItemType Directory -Path $folder -Force
$file = Join-Path $folder 'cpu-v2.csv'
$bytes = [byte[]]::new(2 * 1024 * 1024 - 100)
for ($i = 0; $i -lt $bytes.Length; $i++) { $bytes[$i] = 120 }
[IO.File]::WriteAllBytes($file, $bytes)
$priorAppData = $env:LOCALAPPDATA
$env:LOCALAPPDATA = $tempRoot
$app = Start-Process -FilePath $program -WindowStyle Hidden -PassThru
$window = [IntPtr]::Zero
$owner = [uint32]0
try {
    for ($i = 0; $i -lt 30; $i++) {
        Start-Sleep -Milliseconds 100
        $window = [RotationWindow]::FindWindow('miniTask.Tray', 'miniTask')
        if ($window -ne [IntPtr]::Zero) { break }
    }
    if ($window -eq [IntPtr]::Zero) { throw 'Tray window not found.' }
    $null = [RotationWindow]::GetWindowThreadProcessId($window, [ref]$owner)
    if ($owner -ne $app.Id) { throw 'Another instance owns the tray window.' }
    $null = [RotationWindow]::SendMessage($window, 0x111, [IntPtr]5, [IntPtr]::Zero)
    Start-Sleep -Seconds 4
    $null = [RotationWindow]::SendMessage($window, 0x111, [IntPtr]5, [IntPtr]::Zero)
    $old = "$file.old"
    if (-not (Test-Path -LiteralPath $old)) { throw 'Rotated file was not created.' }
    if (-not (Test-Path -LiteralPath $file)) { throw 'New log file was not created.' }
    $header = Get-Content -LiteralPath $file -TotalCount 1
    if ($header -ne 'time,cpu_percent,top_process,top_percent,peak_percent,peak_time,peak_process') {
        throw 'New log header is incorrect.'
    }
    [pscustomobject]@{
        RotatedBytes = (Get-Item -LiteralPath $old).Length
        NewBytes = (Get-Item -LiteralPath $file).Length
        NewHeader = $header
    }
} finally {
    if ($owner -eq $app.Id) {
        $null = [RotationWindow]::SendMessage($window, 0x111, [IntPtr]1, [IntPtr]::Zero)
    }
    if (-not $app.WaitForExit(3000)) { $app.Kill(); $app.WaitForExit() }
    $env:LOCALAPPDATA = $priorAppData
}
