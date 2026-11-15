# Parallel version of tools/regen_gen.ps1: the exe chain (discover + lift) and
# the DLL chain (D3DX9 then D3DCompiler, discover + lift each) run side by side;
# each module's lift only reads its own analysis/functions*.json. Every Python
# step runs through tools/lowprio.py (Windows background mode).
#
# Usage: powershell -File tools\regen_parallel.ps1 -GameDir GAME
#        [-ExeDiscoverPid N]   adopt an exe discovery that is already running:
#                              wait for that process instead of starting one.
#        [-ReuseExeDiscovery]  keep an existing analysis\functions.json (exe
#                              discovery is the slowest step, ~30 min).
param(
    [Parameter(Mandatory = $true)][string]$GameDir,
    [int]$ExeDiscoverPid = 0,
    [switch]$ReuseExeDiscovery,
    [string]$Python = ''
)
$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path (Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) '..')).Path
if (-not $Python) { $Python = Join-Path $Root '.venv-x64\Scripts\python.exe' }
$G = (Resolve-Path $GameDir).Path
$Py = (Resolve-Path $Python).Path
$Low = Join-Path $Root 'tools\lowprio.py'
Set-Location $Root
New-Item -ItemType Directory -Force analysis, gen | Out-Null
$env:LIFT_GAME = 'marvel'; $env:PYTHONUNBUFFERED = '1'; $env:PYTHONUTF8 = '1'

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
function Log($m) { Write-Host "[$((Get-Date).ToString('HH:mm:ss'))] $m" }

# Runs a list of steps in its own hidden process, logging to analysis\<name>.log.
function Start-Chain([string]$name, [object[]]$steps) {
    $lines = @('$ErrorActionPreference = "Stop"')
    foreach ($s in $steps) {
        $argList = ($s.Args | ForEach-Object { "'" + ($_ -replace "'", "''") + "'" }) -join ' '
        $lines += "Write-Host ('[' + (Get-Date).ToString('HH:mm:ss') + '] START $($s.Label)')"
        $lines += "Push-Location '$($s.Cwd)'; & '$Py' '$Low' $argList; `$rc = `$LASTEXITCODE; Pop-Location"
        $lines += "if (`$rc -ne 0) { Write-Host ('FAILED $($s.Label) exit ' + `$rc); exit `$rc }"
        $lines += "Write-Host ('[' + (Get-Date).ToString('HH:mm:ss') + '] DONE  $($s.Label)')"
    }
    $script = Join-Path $Root "analysis\chain_$name.ps1"
    Set-Content -Path $script -Value $lines -Encoding UTF8
    $p = Start-Process powershell.exe -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$script`"") `
        -WorkingDirectory $Root -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput (Join-Path $Root "analysis\chain_$name.log") `
        -RedirectStandardError (Join-Path $Root "analysis\chain_$name.err.log")
    $null = $p.Handle  # PS 5.1: without caching the handle now, ExitCode is blank later
    $p.PriorityClass = 'BelowNormal'
    Log "chain $name started (pid $($p.Id))"
    return $p
}
function Step($label, $cwd, [string[]]$a) { [pscustomobject]@{ Label = $label; Cwd = $cwd; Args = $a } }
$T = Join-Path $Root 'tools'

$dll = Start-Chain 'dll' @(
    (Step 'discover d3dx' $T @('discover.py', $D3dx, '..\analysis\functions_d3dx.json', '08000000')),
    (Step 'lift d3dx' $Root @('tools\lift.py', $D3dx, 'analysis\functions_d3dx.json', 'gen', 'd3dx9', '08000000')),
    (Step 'discover d3dc' $T @('discover.py', $D3dc, '..\analysis\functions_d3dc.json', '09000000')),
    (Step 'lift d3dc' $Root @('tools\lift.py', $D3dc, 'analysis\functions_d3dc.json', 'gen', 'd3dc', '09000000')))

$liftExe = Step 'lift exe' $Root @('tools\lift.py', $Exe, 'analysis\functions.json', 'gen', 'exe')
if ($ExeDiscoverPid) {
    $d = Get-Process -Id $ExeDiscoverPid -ErrorAction SilentlyContinue
    if ($d) { Log "waiting for running exe discovery (pid $ExeDiscoverPid)"; $d.WaitForExit() }
    if (-not (Test-Path analysis\functions.json)) { throw 'exe discovery ended without analysis\functions.json' }
    Log 'DONE  discover exe (adopted)'
    $exe = Start-Chain 'exe' @($liftExe)
} elseif ($ReuseExeDiscovery -and (Test-Path analysis\functions.json)) {
    Log 'reusing analysis\functions.json'
    $exe = Start-Chain 'exe' @($liftExe)
} else {
    $exe = Start-Chain 'exe' @((Step 'discover exe' $T @('discover.py', $Exe, '..\analysis\functions.json')), $liftExe)
}

foreach ($c in @(@{n = 'exe'; p = $exe }, @{n = 'dll'; p = $dll })) {
    $c.p.WaitForExit()
    if ($c.p.ExitCode -ne 0) { throw "chain $($c.n) failed (exit $($c.p.ExitCode)); see analysis\chain_$($c.n).log" }
    Log "chain $($c.n) finished"
}
& $Py $Low tools\patch_gen.py gen; if ($LASTEXITCODE) { throw 'patch_gen failed' }
& $Py $Low tools\gen_d3d9_bridge.py runtime; if ($LASTEXITCODE) { throw 'gen_d3d9_bridge failed' }
Log 'gen/ is ready'
