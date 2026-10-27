<#
  Per-user install for CursorLingo - no administrator rights needed (like VS Code's "User Installer").

    .\install.ps1               copy to %LOCALAPPDATA%\Programs\CursorLingo, add a Start Menu shortcut, launch
    .\install.ps1 -Startup      ...and start it with Windows
    .\install.ps1 -Uninstall    remove everything this script created (settings stay in %APPDATA%\CursorLingo)

  CursorLingo.exe is also fully portable: you can skip this script and run the exe from any folder.
  Nothing here writes to the registry: "start with Windows" is a shortcut in your Startup folder.
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
$startup = Join-Path ([Environment]::GetFolderPath('Startup')) "$name.lnk"

function New-Shortcut($path) {
    $sc = (New-Object -ComObject WScript.Shell).CreateShortcut($path)
    $sc.TargetPath = $exe
    $sc.WorkingDirectory = $dest
    $sc.Description = 'Shows the keyboard input language next to the mouse pointer'
    $sc.Save()
}

Get-Process -Name $name -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 300

if ($Uninstall) {
    Remove-Item $link, $startup -Force -ErrorAction SilentlyContinue
    Remove-Item $dest -Recurse -Force -ErrorAction SilentlyContinue
    Write-Host "$name removed."
    return
}

$src = Join-Path $PSScriptRoot "$name.exe"
if (-not (Test-Path $src)) { throw "$name.exe must sit next to install.ps1." }

New-Item -ItemType Directory -Path $dest -Force | Out-Null
Copy-Item $src $exe -Force
Unblock-File $exe   # drop the "downloaded from the internet" mark so SmartScreen stays quiet

New-Shortcut $link
if ($Startup) { New-Shortcut $startup }

Start-Process $exe
Write-Host "Installed to $dest"
