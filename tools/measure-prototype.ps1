param([ValidateRange(5, 300)][int]$Seconds = 15)

$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Threading;

public static class MiniTaskProbe
{
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr FindWindow(string className, string title);
    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);
    [DllImport("user32.dll")]
    public static extern IntPtr SendMessageTimeout(IntPtr window, uint message,
        IntPtr wParam, IntPtr lParam, uint flags, uint timeout, out UIntPtr result);
    [DllImport("user32.dll")]
    public static extern bool PostMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);

    [DllImport("kernel32.dll")]
    private static extern bool GetSystemTimes(out long idle, out long kernel, out long user);

    public static long TotalTime()
    {
        long idle, kernel, user;
        if (!GetSystemTimes(out idle, out kernel, out user))
            throw new InvalidOperationException("GetSystemTimes failed.");
        return kernel + user;
    }

    private static volatile bool running;
    public static Thread StartLoad()
    {
        running = true;
        Thread worker = new Thread(() => {
            while (running)
                Thread.SpinWait(256);
        });
        worker.IsBackground = true;
        worker.Start();
        return worker;
    }

    public static void StopLoad(Thread worker)
    {
        running = false;
        worker.Join();
    }
}
'@

function Read-Diagnostic([uint32]$Message) {
    [UIntPtr]$value = [UIntPtr]::Zero
    $ok = [MiniTaskProbe]::SendMessageTimeout($window, $Message,
        [IntPtr]::Zero, [IntPtr]::Zero, 2, 1000, [ref]$value)
    if ($ok -eq [IntPtr]::Zero) { throw 'The prototype did not answer a diagnostic query.' }
    $value.ToUInt64()
}

function Measure-Phase([string]$Name) {
    $intervals = [Collections.Generic.List[int]]::new()
    $loads = [Collections.Generic.List[int]]::new()
    $app.Refresh()
    $privatePeak = $app.PrivateMemorySize64
    $cpuStart = $app.TotalProcessorTime.Ticks
    $totalStart = [MiniTaskProbe]::TotalTime()
    $initialCount = $last = Read-Diagnostic 0x8002
    $clock = [Diagnostics.Stopwatch]::StartNew()
    while ($clock.Elapsed.TotalSeconds -lt $Seconds) {
        Start-Sleep -Milliseconds 210
        $count = Read-Diagnostic 0x8002
        if ($count -ne $last) {
            $last = $count
            $sample = Read-Diagnostic 0x8003
            $intervals.Add([int]($sample -shr 8))
            $loads.Add([int]($sample -band 255))
            $app.Refresh()
            $privatePeak = [Math]::Max($privatePeak, $app.PrivateMemorySize64)
        }
    }
    $app.Refresh()
    $cpuUsed = $app.TotalProcessorTime.Ticks - $cpuStart
    $totalUsed = [MiniTaskProbe]::TotalTime() - $totalStart
    $sorted = @($intervals | Sort-Object)
    if (-not $sorted.Length) { throw 'No CPU samples were observed.' }
    [pscustomobject]@{
        Phase = $Name
        Seconds = [Math]::Round($clock.Elapsed.TotalSeconds, 2)
        Samples = $last - $initialCount
        ObservedIntervals = $sorted.Length
        SystemCpuMeanPct = [Math]::Round(($loads | Measure-Object -Average).Average, 2)
        AppCpuPct = [Math]::Round(100 * $cpuUsed / $totalUsed, 4)
        PrivateBytesEnd = $app.PrivateMemorySize64
        PrivateBytesObservedPeak = $privatePeak
        IntervalMedianMs = $sorted[[int][Math]::Floor($sorted.Length / 2)]
        IntervalP95Ms = $sorted[[int][Math]::Ceiling($sorted.Length * 0.95) - 1]
        IntervalMaxMs = $sorted[-1]
    }
}

$program = Join-Path (Split-Path -Parent $PSScriptRoot) 'dist\miniTask-debug.exe'
$app = Start-Process -FilePath $program -WindowStyle Hidden -PassThru
$window = [IntPtr]::Zero
$owned = $false
try {
    for ($attempt = 0; $attempt -lt 30; ++$attempt) {
        $window = [MiniTaskProbe]::FindWindow('miniTask.Tray', 'miniTask')
        if ($window -ne [IntPtr]::Zero) { break }
        Start-Sleep -Milliseconds 100
    }
    if ($window -eq [IntPtr]::Zero) { throw 'The tray window was not found.' }
    [uint32]$ownerId = 0
    $null = [MiniTaskProbe]::GetWindowThreadProcessId($window, [ref]$ownerId)
    if ($ownerId -ne $app.Id) { throw 'Another miniTask instance is already running.' }
    $owned = $true
    Start-Sleep -Seconds 2
    if ((Read-Diagnostic 0x8004) -ne 1) { throw 'Explorer did not accept the tray icon.' }
    if ($app.PriorityClass -ne 'High') { throw 'The prototype is not running at high priority.' }
    Measure-Phase 'Desktop baseline'
    $worker = [MiniTaskProbe]::StartLoad()
    try {
        Start-Sleep -Seconds 2
        Measure-Phase 'One busy thread'
    } finally {
        [MiniTaskProbe]::StopLoad($worker)
    }
} finally {
    if ($owned) {
        $null = [MiniTaskProbe]::PostMessage($window, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
    }
    if (-not $app.WaitForExit(3000)) {
        $app.Kill()
        $app.WaitForExit()
    }
}
