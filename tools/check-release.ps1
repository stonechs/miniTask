param([ValidateRange(4, 300)][int]$Seconds = 30)
$ErrorActionPreference = 'Stop'
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class MiniTaskWindow {
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr FindWindow(string name, string title);
    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);
    [DllImport("user32.dll")]
    public static extern IntPtr SendMessage(IntPtr window, uint message, IntPtr wp, IntPtr lp);
    [DllImport("kernel32.dll")]
    private static extern bool GetSystemTimes(out long idle, out long kernel, out long user);
    public static long TotalCpuTime() {
        long idle, kernel, user;
        if (!GetSystemTimes(out idle, out kernel, out user)) throw new Exception("GetSystemTimes failed");
        return kernel + user;
    }
}
'@
$program = Join-Path (Split-Path -Parent $PSScriptRoot) 'dist\miniTask.exe'
$log = Join-Path $env:LOCALAPPDATA 'miniTask\cpu-v2.csv'
$oldLogBytes = if (Test-Path -LiteralPath $log) { (Get-Item -LiteralPath $log).Length } else { 0 }
$settings = Get-ItemProperty -Path 'HKCU:\Software\miniTask' -ErrorAction SilentlyContinue
$rankWasEnabled = $settings -and $settings.Rank -eq 1
$logWasEnabled = $settings -and $settings.Log -eq 1
$app = Start-Process -FilePath $program -WindowStyle Hidden -PassThru
$owner = [uint32]0
$window = [IntPtr]::Zero
try {
    for ($i = 0; $i -lt 30; $i++) {
        Start-Sleep -Milliseconds 100
        $window = [MiniTaskWindow]::FindWindow('miniTask.Tray', 'miniTask')
        if ($window -ne [IntPtr]::Zero) { break }
    }
    if ($window -eq [IntPtr]::Zero) { throw 'Tray window not found.' }
    $null = [MiniTaskWindow]::GetWindowThreadProcessId($window, [ref]$owner)
    if ($owner -ne $app.Id) { throw 'Another instance owns the tray window.' }
    if ($app.PriorityClass -ne 'High') { throw 'Priority is not High.' }
    $null = [MiniTaskWindow]::SendMessage($window, 0x111, [IntPtr]2, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 400
    $history = [MiniTaskWindow]::FindWindow('miniTask.History', 'miniTask - 最近 60 秒 CPU')
    if ($history -eq [IntPtr]::Zero) { throw 'History window was not created.' }
    if (-not $rankWasEnabled) {
        $null = [MiniTaskWindow]::SendMessage($window, 0x111, [IntPtr]4, [IntPtr]::Zero)
    }
    if (-not $logWasEnabled) {
        $null = [MiniTaskWindow]::SendMessage($window, 0x111, [IntPtr]5, [IntPtr]::Zero)
    }
    $app.Refresh()
    $appCpuStart = $app.TotalProcessorTime.Ticks
    $systemCpuStart = [MiniTaskWindow]::TotalCpuTime()
    Start-Sleep -Seconds $Seconds
    $app.Refresh()
    $memory = $app.PrivateMemorySize64
    $appCpuPct = 100 * ($app.TotalProcessorTime.Ticks - $appCpuStart) /
                 ([MiniTaskWindow]::TotalCpuTime() - $systemCpuStart)
    if (-not $logWasEnabled) {
        $null = [MiniTaskWindow]::SendMessage($window, 0x111, [IntPtr]5, [IntPtr]::Zero)
    }
    if (-not (Test-Path -LiteralPath $log)) { throw 'CSV log was not created.' }
    $size = (Get-Item -LiteralPath $log).Length
    if ($size -le $oldLogBytes) { throw 'CSV log did not grow.' }
    $tail = Get-Content -LiteralPath $log -Tail 3
    [pscustomobject]@{
        ExeBytes = (Get-Item -LiteralPath $program).Length
        PrivateBytesWithHistoryRankLog = $memory
        AppCpuPercentOfSystem = [Math]::Round($appCpuPct, 4)
        LogBytesAdded = $size - $oldLogBytes
        LogTail = ($tail -join ' | ')
    }
} finally {
    if ($owner -eq $app.Id) {
        if (-not $rankWasEnabled) {
            $null = [MiniTaskWindow]::SendMessage($window, 0x111, [IntPtr]4, [IntPtr]::Zero)
        }
        if (-not $logWasEnabled -and (Get-ItemProperty -Path 'HKCU:\Software\miniTask').Log -eq 1) {
            $null = [MiniTaskWindow]::SendMessage($window, 0x111, [IntPtr]5, [IntPtr]::Zero)
        }
        $null = [MiniTaskWindow]::SendMessage($window, 0x111, [IntPtr]1, [IntPtr]::Zero)
    }
    if (-not $app.WaitForExit(3000)) {
        $app.Kill()
        $app.WaitForExit()
    }
}
