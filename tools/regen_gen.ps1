# Windows equivalent of tools/regen_gen.sh: regenerates gen/ (recompiled C)
# from your own LEGO Marvel Super Heroes files.
# Usage: powershell -File tools\regen_gen.ps1 -GameDir GAME [-Python path\to\python.exe]
# GAME_DIR must contain LEGOMARVEL.exe, D3DX9_43.dll and D3DCompiler_43.dll
# (the DLLs may be in GAME_DIR\redist).
# Needs Python 3.10+ with capstone==5.0.9 and pefile==2024.8.26. On Windows on
# ARM, capstone only ships an x64 wheel, so use an x64 Python (runs emulated).
param(
    [Parameter(Mandatory = $true)][string]$GameDir,
    [string]$Python = ''
)
$ErrorActionPreference = 'Stop'
$G = (Resolve-Path $GameDir).Path
# $PSScriptRoot is empty in param() defaults on Windows PowerShell 5.1
$Root = (Resolve-Path (Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) '..')).Path
if (-not $Python) { $Python = Join-Path $Root '.venv-x64\Scripts\python.exe' }
$Py = (Resolve-Path $Python).Path
Set-Location $Root

function Find-GameFile([string]$name) {
    foreach ($d in @($G, "$G\redist", "$G\Redist")) {
        if (Test-Path $d) {
            $f = Get-ChildItem $d -File | Where-Object { $_.Name -ieq $name } | Select-Object -First 1
            if ($f) { return $f.FullName }
        }
    }
    throw "missing $name in $G"
}
$Exe = Find-GameFile 'legomarvel.exe'
$D3dx = Find-GameFile 'd3dx9_43.dll'
$D3dc = Find-GameFile 'd3dcompiler_43.dll'

New-Item -ItemType Directory -Force analysis, gen | Out-Null
$env:LIFT_GAME = 'marvel'
$env:PYTHONUNBUFFERED = '1'
$env:PYTHONUTF8 = '1'  # lifter writes text with the default encoding; avoid cp1252

function Step([string]$label, [string]$cwd, [string[]]$pyArgs) {
    $t = Get-Date
    Write-Host "[$($t.ToString('HH:mm:ss'))] START $label"
    Push-Location $cwd
    try {
        # lowprio.py runs each step in Windows background mode (low CPU/IO/memory priority)
        & $Py (Join-Path $Root 'tools\lowprio.py') @pyArgs
        if ($LASTEXITCODE -ne 0) { throw "$label failed (exit $LASTEXITCODE)" }
    } finally { Pop-Location }
    $m = [int]((Get-Date) - $t).TotalMinutes
    Write-Host "[$((Get-Date).ToString('HH:mm:ss'))] DONE  $label ($m min)"
}

# discover.py imports peimg.py from tools/, so run it from there.
Step 'discover exe'  "$Root\tools" @('discover.py', $Exe,  '..\analysis\functions.json')
Step 'discover d3dx' "$Root\tools" @('discover.py', $D3dx, '..\analysis\functions_d3dx.json', '08000000')
Step 'discover d3dc' "$Root\tools" @('discover.py', $D3dc, '..\analysis\functions_d3dc.json', '09000000')
Step 'lift exe'      $Root @('tools\lift.py', $Exe,  'analysis\functions.json', 'gen', 'exe')
Step 'lift d3dx'     $Root @('tools\lift.py', $D3dx, 'analysis\functions_d3dx.json', 'gen', 'd3dx9', '08000000')
Step 'lift d3dc'     $Root @('tools\lift.py', $D3dc, 'analysis\functions_d3dc.json', 'gen', 'd3dc', '09000000')
Step 'patch_gen'     $Root @('tools\patch_gen.py', 'gen')
Step 'd3d9 bridge'   $Root @('tools\gen_d3d9_bridge.py', 'runtime')
Write-Host 'gen/ is ready'
