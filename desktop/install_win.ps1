<#
.SYNOPSIS
  Installs the native ARM64 build as a normal per-user app: "LEGO Marvel Super Heroes (ARM 64)".

.DESCRIPTION
  No admin rights, no registry writes. It:
    - copies desktop\build\win\lb.exe (as "LEGO Marvel Super Heroes (ARM 64).exe") and SDL2.dll
      to %LOCALAPPDATA%\Programs\LEGO Marvel Super Heroes (ARM 64)\
    - writes lb_paths.ini there (game folder + save folder), which the exe reads when it is
      started without arguments
    - creates the Start Menu shortcut, so Windows search finds the app by name
  Saves go to %LOCALAPPDATA%\LEGO Marvel Super Heroes (ARM 64)\save unless -SaveDir is given.
  The game folder is only read; this script never writes to it. It must contain the
  32-bit D3DX9_43.dll and D3DCompiler_43.dll (in the folder or its redist\ subfolder).
  Run desktop\uninstall_win.ps1 to remove the app folder and shortcut again (saves are kept).

.EXAMPLE
  .\desktop\install_win.ps1
.EXAMPLE
  .\desktop\install_win.ps1 -GameDir "E:\Games\LEGO Marvel Super Heroes"
#>
[CmdletBinding()]
param(
    [string]$GameDir = '',                # default: found in your Steam libraries
    [string]$SaveDir = '%LOCALAPPDATA%\LEGO Marvel Super Heroes (ARM 64)\save',
    [string]$BuildDir = ''
)
$ErrorActionPreference = 'Stop'

# Default game folder: look for the Steam install in every Steam library folder
# (Steam's libraryfolders.vdf lists them).
if (-not $GameDir) {
    $libs = @()
    $steam = Join-Path ${env:ProgramFiles(x86)} 'Steam'
    $vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
    if (Test-Path $vdf) {
        $libs += Select-String -Path $vdf -Pattern '"path"\s+"([^"]+)"' |
            ForEach-Object { $_.Matches[0].Groups[1].Value -replace '\\\\', '\' }
    }
    $libs += $steam
    foreach ($lib in ($libs | Select-Object -Unique)) {
        $cand = Join-Path $lib 'steamapps\common\LEGO Marvel Super Heroes'
        if (Test-Path (Join-Path $cand 'LEGOMARVEL.exe')) { $GameDir = $cand; break }
    }
    if (-not $GameDir) { throw "LEGO Marvel Super Heroes not found in your Steam libraries; pass -GameDir <folder>" }
}
$AppName = 'LEGO Marvel Super Heroes (ARM 64)'
$Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (-not $BuildDir) { $BuildDir = Join-Path $Root 'desktop\build\win' }
$Dest = Join-Path $env:LOCALAPPDATA "Programs\$AppName"
$ExeName = "$AppName.exe"
$Lnk = Join-Path ([Environment]::GetFolderPath('Programs')) "$AppName.lnk"

# --- checks: build output and game folder (read only)
foreach ($f in 'lb.exe', 'SDL2.dll') {
    if (-not (Test-Path (Join-Path $BuildDir $f))) { throw "missing $BuildDir\$f - run desktop\build_win.ps1 first" }
}
if (-not (Test-Path (Join-Path $GameDir 'LEGOMARVEL.exe'))) { throw "LEGOMARVEL.exe not found in $GameDir (pass -GameDir)" }
foreach ($dll in 'D3DX9_43.dll', 'D3DCompiler_43.dll') {
    if (-not ((Test-Path (Join-Path $GameDir $dll)) -or (Test-Path (Join-Path $GameDir "redist\$dll")))) {
        Write-Warning "$dll is not in $GameDir or its redist\ folder; the game will stop at startup until it is (see desktop\WINDOWS_ARM64.md)"
    }
}
if (Get-Process | Where-Object { $_.Path -and $_.Path -ieq (Join-Path $Dest $ExeName) }) {
    throw "$AppName is running; close it first"
}

# --- app folder
New-Item -ItemType Directory -Force $Dest | Out-Null
Copy-Item (Join-Path $BuildDir 'lb.exe') (Join-Path $Dest $ExeName) -Force
Copy-Item (Join-Path $BuildDir 'SDL2.dll') (Join-Path $Dest 'SDL2.dll') -Force
$ini = @"
; Read by "$ExeName" when it is started without arguments.
; %VARIABLES% are expanded. Command-line arguments (game dir, save dir) override these.
[paths]
game_dir=$GameDir
save_dir=$SaveDir
"@
Set-Content -Path (Join-Path $Dest 'lb_paths.ini') -Value $ini -Encoding ASCII

# --- Start Menu shortcut (a file in the per-user Programs folder; no registry)
$sh = New-Object -ComObject WScript.Shell
$s = $sh.CreateShortcut($Lnk)
$s.TargetPath = Join-Path $Dest $ExeName
$s.WorkingDirectory = $Dest
$s.IconLocation = "$(Join-Path $Dest $ExeName),0"
$s.Description = "$AppName - native Windows on ARM build"
$s.Save()

Write-Host "Installed $AppName" -ForegroundColor Green
Write-Host "  app:      $Dest"
Write-Host "  shortcut: $Lnk"
Write-Host "  game:     $GameDir"
Write-Host "  saves:    $([Environment]::ExpandEnvironmentVariables($SaveDir))  (log.txt is written there)"
