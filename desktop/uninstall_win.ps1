<#
.SYNOPSIS
  Removes what desktop\install_win.ps1 created: the app folder and the Start Menu shortcut.

.DESCRIPTION
  Deletes only
    %LOCALAPPDATA%\Programs\LEGO Marvel Super Heroes (ARM 64)\   (if it holds the app exe)
    <Start Menu>\Programs\LEGO Marvel Super Heroes (ARM 64).lnk
  Saves and log.txt in %LOCALAPPDATA%\LEGO Marvel Super Heroes (ARM 64)\ are kept, and the
  game folder is never touched.
#>
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$AppName = 'LEGO Marvel Super Heroes (ARM 64)'
$Dest = Join-Path $env:LOCALAPPDATA "Programs\$AppName"
$Exe = Join-Path $Dest "$AppName.exe"
$Lnk = Join-Path ([Environment]::GetFolderPath('Programs')) "$AppName.lnk"

if (Get-Process | Where-Object { $_.Path -and $_.Path -ieq $Exe }) { throw "$AppName is running; close it first" }

if (Test-Path $Lnk) { Remove-Item $Lnk -Force; Write-Host "removed $Lnk" } else { Write-Host "no shortcut at $Lnk" }
if (Test-Path $Dest) {
    if (-not (Test-Path $Exe)) { throw "$Dest does not contain $AppName.exe; not deleting it" }
    Remove-Item $Dest -Recurse -Force
    Write-Host "removed $Dest"
} else { Write-Host "no app folder at $Dest" }
Write-Host "Saves kept in $(Join-Path $env:LOCALAPPDATA "$AppName\save")"
