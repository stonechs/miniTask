param(
    [string]$GccPath = 'gcc',
    [string]$ClangClPath = 'clang-cl',
    [string]$CrinklerPath = 'D:\2026work\Crinkler.exe',
    [string]$UpxPath = 'D:\2026work\Compress\tools\upx-5.3.0\upx.exe'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$dist = Join-Path $root 'dist'
New-Item -ItemType Directory -Path $dist -Force | Out-Null

function Resolve-Tool([string]$Path) {
    if (Test-Path -LiteralPath $Path -PathType Leaf) {
        return (Resolve-Path -LiteralPath $Path).Path
    }
    $command = Get-Command $Path -ErrorAction Stop
    return $command.Source
}

function Invoke-Checked([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code ${LASTEXITCODE}: $Program"
    }
}

$gcc = Resolve-Tool $GccPath
$clangCl = Resolve-Tool $ClangClPath
$crinkler = Resolve-Tool $CrinklerPath
$upx = Resolve-Tool $UpxPath

$windowsKitLibRoot = 'C:\Program Files (x86)\Windows Kits\10\Lib'
$windowsKit = Get-ChildItem -LiteralPath $windowsKitLibRoot -Directory |
    Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
if (-not $windowsKit) { throw 'Windows SDK libraries were not found.' }
$sdkX86 = Join-Path $windowsKit.FullName 'um\x86'

$msvcLibRoot = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC'
$msvcToolset = Get-ChildItem -LiteralPath $msvcLibRoot -Directory |
    Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
if (-not $msvcToolset) { throw 'MSVC x86 libraries were not found.' }
$msvcX86 = Join-Path $msvcToolset.FullName 'lib\x86'

$source = Join-Path $root 'src\minitask.c'
$x64Exe = Join-Path $dist 'miniTask-x64.exe'
$x64Map = Join-Path $dist 'miniTask-x64.map'
$x86Exe = Join-Path $dist 'miniTask-x86.exe'
$x86Object = Join-Path $dist 'miniTask-x86-release.obj'
$x86Crinkled = Join-Path $dist 'miniTask-x86-crinkler.exe'
$x64Upx = Join-Path $dist 'miniTask-x64-upx.exe'

Push-Location $root
try {
    Invoke-Checked $gcc @(
        '-m64', '-std=c11', '-Os', '-Wall', '-Wextra', '-ffunction-sections',
        '-fno-stack-protector', '-nostdlib', '-mwindows',
        '-Wl,--gc-sections,-e,miniTaskStart', "-Wl,-Map,$x64Map", '-s',
        '-o', $x64Exe, $source,
        '-lkernel32', '-luser32', '-lshell32', '-lgdi32', '-ladvapi32'
    )

    Invoke-Checked $clangCl @(
        '--target=i686-pc-windows-msvc', '/c', '/O2', '/clang:-Oz', '/GS-', '/Zl',
        '/std:c11', $source, "/Fo:$x86Object"
    )
    Invoke-Checked $clangCl @(
        '--target=i686-pc-windows-msvc', '/O2', '/clang:-Oz', '/GS-', '/Zl', '/std:c11',
        "/Fe:$x86Exe", $source, '/link', '/NODEFAULTLIB', '/ENTRY:miniTaskStart',
        '/SUBSYSTEM:WINDOWS', "/LIBPATH:$sdkX86", "/LIBPATH:$msvcX86",
        'kernel32.lib', 'user32.lib', 'shell32.lib', 'gdi32.lib', 'advapi32.lib', 'libcmt.lib'
    )

    Invoke-Checked $crinkler @(
        "/OUT:$x86Crinkled", '/ENTRY:miniTaskStart', '/SUBSYSTEM:WINDOWS',
        '/NODEFAULTLIB', '/CRINKLER', '/COMPMODE:FAST', "/LIBPATH:$sdkX86",
        "/LIBPATH:$msvcX86", $x86Object, 'kernel32.lib', 'user32.lib',
        'shell32.lib', 'gdi32.lib', 'advapi32.lib', 'libcmt.lib'
    )

    Copy-Item -LiteralPath $x64Exe -Destination $x64Upx -Force
    Invoke-Checked $upx @('--best', $x64Upx)
    Invoke-Checked $upx @('-t', $x64Upx)

    $outputs = @($x86Exe, $x64Exe, $x86Crinkled, $x64Upx)
    foreach ($output in $outputs) {
        if (-not (Test-Path -LiteralPath $output -PathType Leaf)) {
            throw "Expected release executable is missing: $output"
        }
    }
    Get-Item -LiteralPath $outputs | Select-Object Name, Length
}
finally {
    Pop-Location
}
