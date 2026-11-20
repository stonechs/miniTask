param([ValidateRange(1, 10)][int]$Runs = 3)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$source = Join-Path $PSScriptRoot 'memory-probe.c'
$probe = Join-Path $root 'dist\memory-probe.exe'
$null = New-Item -ItemType Directory -Path (Split-Path -Parent $probe) -Force
& gcc -m64 -std=c11 -Os -Wall -Wextra -fno-stack-protector -nostdlib -mwindows `
    '-Wl,-e,memoryProbeStart' -o $probe $source -lkernel32
if ($LASTEXITCODE -ne 0) { throw "Probe build failed: $LASTEXITCODE" }

$stages = @(
    'KERNEL32 only',
    'Load USER32',
    'Load GDI32',
    'Load SHELL32',
    'Create hidden window',
    'Create icon',
    'Add tray icon and set version',
    'Replace tray icon'
)

for ($stage = 0; $stage -lt $stages.Count; $stage++) {
    for ($run = 1; $run -le $Runs; $run++) {
        $process = Start-Process -FilePath $probe -ArgumentList "$stage" -WindowStyle Hidden -PassThru
        try {
            Start-Sleep -Milliseconds 800
            $process.Refresh()
            if ($process.HasExited) { throw "Probe stage $stage failed: $($process.ExitCode)" }
            $privateKiB = $process.PrivateMemorySize64 / 1KB
            $threads = $process.Threads.Count
            if (-not $process.WaitForExit(5000)) { throw "Probe stage $stage did not exit." }
            if ($process.ExitCode -ne 0) { throw "Probe stage $stage failed: $($process.ExitCode)" }
            [pscustomobject]@{
                Stage = $stage
                Operation = $stages[$stage]
                Run = $run
                PrivateKiB = $privateKiB
                Threads = $threads
            }
        } finally {
            if (-not $process.HasExited) { $process.Kill() }
            $process.Dispose()
        }
    }
}
