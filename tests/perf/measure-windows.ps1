# Measures CursorLingo on Windows: CPU over 20 s and memory (working set, private bytes), in several situations.
#
#   powershell -ExecutionPolicy Bypass -File tests\perf\measure-windows.ps1 [-Exe path\to\CursorLingo.exe] [-Cases baseline,moving,huge,garbage]
#
# The settings folder (%APPDATA%\CursorLingo, or the exe's folder when a CursorLingo.ini is there) is copied before
# and put back after, so the stress cases leave your settings as they were. "moving" moves the pointer by itself for
# 20 s. Run it from a normal (not administrator) PowerShell; works with Windows PowerShell 5.1 and PowerShell 7.
param(
    [string]$Exe = "",
    [string[]]$Cases = @("baseline", "moving", "huge", "garbage")
)
$ErrorActionPreference = "Stop"
$repo = Resolve-Path (Join-Path $PSScriptRoot "..\..")
if (-not $Exe) {
    foreach ($c in @("build\windows\Release\CursorLingo.exe", "build\windows-mingw\CursorLingo.exe", "build\windows\CursorLingo.exe")) {
        $p = Join-Path $repo $c
        if (Test-Path $p) { $Exe = $p; break }
    }
}
if (-not $Exe -or -not (Test-Path $Exe)) { Write-Host "CursorLingo.exe not found; pass -Exe"; exit 1 }
$Exe = (Resolve-Path $Exe).Path

$exeDir = Split-Path $Exe
$dir = if (Test-Path (Join-Path $exeDir "CursorLingo.ini")) { $exeDir } else { Join-Path $env:APPDATA "CursorLingo" }
$ini = Join-Path $dir "CursorLingo.ini"
$words = Join-Path $dir "CursorLingo.words"
$backup = Join-Path $env:TEMP ("CursorLingo-perf-" + [guid]::NewGuid())
New-Item -ItemType Directory -Force -Path $backup | Out-Null
foreach ($f in @($ini, $words)) { if (Test-Path $f) { Copy-Item $f $backup } }
$wasRunning = [bool](Get-Process CursorLingo -ErrorAction SilentlyContinue)

Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class Pointer {
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
    public struct POINT { public int X, Y; }
}
"@

function Stop-App {
    Get-Process CursorLingo -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 800
}

function Restore-Settings {
    Stop-App
    foreach ($f in @($ini, $words)) { if (Test-Path $f) { Remove-Item $f -Force } }
    foreach ($f in Get-ChildItem $backup) { Copy-Item $f.FullName $dir }
    if (Test-Path "$ini.old") { Remove-Item "$ini.old" -Force }
}

function Measure-App([string]$name, [bool]$move) {
    Stop-App
    $p = Start-Process -FilePath $Exe -PassThru
    Start-Sleep -Seconds 5
    $p.Refresh()
    if ($p.HasExited) { Write-Host ("{0,-10} QUIT OR CRASHED at launch (exit code {1})" -f $name, $p.ExitCode); return }
    $c0 = $p.TotalProcessorTime.TotalSeconds
    $start = Get-Date
    if ($move) {
        $origin = New-Object Pointer+POINT
        [Pointer]::GetCursorPos([ref]$origin) | Out-Null
        while (((Get-Date) - $start).TotalSeconds -lt 20) {
            $a = ((Get-Date) - $start).TotalSeconds * 3
            [Pointer]::SetCursorPos([int](700 + 250 * [math]::Cos($a)), [int](450 + 200 * [math]::Sin($a))) | Out-Null
            Start-Sleep -Milliseconds 8
        }
        [Pointer]::SetCursorPos($origin.X, $origin.Y) | Out-Null
    } else {
        Start-Sleep -Seconds 20
    }
    $p.Refresh()
    if ($p.HasExited) { Write-Host ("{0,-10} CRASHED while running (exit code {1})" -f $name, $p.ExitCode); return }
    $cpu = ($p.TotalProcessorTime.TotalSeconds - $c0) / ((Get-Date) - $start).TotalSeconds / [Environment]::ProcessorCount * 100
    Write-Host ("{0,-10} cpu {1,5:N2} %   working set {2,5:N1} MB   private {3,5:N1} MB" -f $name, $cpu, ($p.WorkingSet64 / 1MB), ($p.PrivateMemorySize64 / 1MB))
}

# A settings file far bigger than anything the app writes (100,000 app layouts, 50,000 colours, broken lines),
# values out of range, and a word list with 200,000 entries and damaged ones.
function Write-Huge {
    Stop-App
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $sb = New-Object System.Text.StringBuilder
    [void]$sb.AppendLine("[Settings]")
    foreach ($kv in @("Style=99", "Mode=-1", "Size=abc", "AutoFix=1", "PerAppLanguage=1", "UILanguage=7", "SpotlightRadius=999999",
                      "ZoomLevel=0", "ShortcutSpotlight=4294967295", "ShortcutZoom=-5", "ShortcutDraw=65536", "BadgeBorder=2")) {
        [void]$sb.AppendLine($kv)
    }
    [void]$sb.AppendLine("Hidden=" + (",XX" * 5000) + ",")
    [void]$sb.AppendLine("[Colors]")
    for ($i = 0; $i -lt 50000; $i++) { [void]$sb.AppendLine("C$i=12AB34") }
    [void]$sb.AppendLine("EN=nothex"); [void]$sb.AppendLine("=FFFFFF"); [void]$sb.AppendLine("TOOLONG=123456")
    [void]$sb.AppendLine("[AppLanguages]")
    for ($i = 0; $i -lt 100000; $i++) { [void]$sb.AppendLine("app$i.exe=41e041e") }
    [void]$sb.AppendLine(("x" * 5000) + ".exe=409")
    [System.IO.File]::WriteAllText($ini, $sb.ToString(), [System.Text.Encoding]::Unicode)  # UTF-16 with BOM, as the app writes it
    $rnd = New-Object System.Random 1
    $ms = New-Object System.IO.MemoryStream
    for ($i = 0; $i -lt 200000; $i++) {
        $n = $rnd.Next(2, 11)
        $ms.WriteByte($n); $ms.WriteByte($rnd.Next(0, 4))
        for ($j = 0; $j -lt $n; $j++) { $ms.WriteByte($rnd.Next(0, 47)) }
        if ($i % 1000 -eq 0) { $ms.WriteByte(200); $ms.WriteByte(3); $ms.WriteByte(255) }  # a damaged entry
    }
    [System.IO.File]::WriteAllBytes($words, $ms.ToArray())
}

# Not a settings file at all, and a word list of random bytes.
function Write-Garbage {
    Stop-App
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $bytes = New-Object byte[] 300000
    (New-Object System.Random 2).NextBytes($bytes)
    [System.IO.File]::WriteAllBytes($ini, $bytes)
    [System.IO.File]::WriteAllBytes($words, $bytes)
}

try {
    foreach ($c in $Cases) {
        switch ($c) {
            "baseline" { Measure-App "baseline" $false }
            "moving" { Measure-App "moving" $true }
            "huge" {
                Write-Huge
                Write-Host ("huge       settings file {0:N1} MB, word list {1:N1} MB" -f ((Get-Item $ini).Length / 1MB), ((Get-Item $words).Length / 1MB))
                Measure-App "huge-1st" $false
                Write-Host ("           after the first launch: settings file {0:N0} KB" -f ((Get-Item $ini).Length / 1KB))
                Measure-App "huge-2nd" $false
                Restore-Settings
            }
            "garbage" { Write-Garbage; Measure-App "garbage" $false; Restore-Settings }
            default { Write-Host "unknown case: $c" }
        }
    }
} finally {
    Restore-Settings
    Remove-Item $backup -Recurse -Force
    if ($wasRunning) { Start-Process -FilePath $Exe }
}
