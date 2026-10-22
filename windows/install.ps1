<#
  Per-user install for CursorLingo - no administrator rights needed (like VS Code's "User Installer").

    .\install.ps1               copy to %LOCALAPPDATA%\Programs\CursorLingo, add a Start Menu shortcut, launch
    .\install.ps1 -Startup      ...and start it with Windows
    .\install.ps1 -Uninstall    remove everything this script created (settings stay in HKCU\Software\CursorLingo)

  CursorLingo.exe is also fully portable: you can skip this script and run the exe from any folder.
#>
param(
    [switch]$Startup,
    [switch]$Uninstall
)
$ErrorActionPreference = 'Stop'

$name    = 'CursorLingo'
$dest    = Join-Path $env:LOCALAPPDATA "Programs\$name"
$exe     = Join-Path $dest "$name.exe"
$link    = Join-Path $env:APPDATA "Microsoft\Windows\Start Menu\Programs\$name.lnk"
$runKey  = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'

Get-Process -Name $name -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 300

if ($Uninstall) {
    Remove-Item $link -Force -ErrorAction SilentlyContinue
    Remove-ItemProperty -Path $runKey -Name $name -ErrorAction SilentlyContinue
    Remove-Item $dest -Recurse -Force -ErrorAction SilentlyContinue
    Write-Host "$name removed."
    return
}

$src = Join-Path $PSScriptRoot "$name.exe"
if (-not (Test-Path $src)) { throw "$name.exe must sit next to install.ps1." }

New-Item -ItemType Directory -Path $dest -Force | Out-Null
Copy-Item $src $exe -Force
Unblock-File $exe   # drop the "downloaded from the internet" mark so SmartScreen stays quiet

$shell = New-Object -ComObject WScript.Shell
$sc = $shell.CreateShortcut($link)
$sc.TargetPath = $exe
$sc.WorkingDirectory = $dest
$sc.Description = 'Shows the keyboard input language next to the mouse pointer'
$sc.Save()

if ($Startup) {
    New-Item -Path $runKey -Force | Out-Null
    Set-ItemProperty -Path $runKey -Name $name -Value "`"$exe`""
}

Start-Process $exe
Write-Host "Installed to $dest"
