<#
.SYNOPSIS
  Native Windows ARM64 build of lb.exe (configure with CMake + Ninja, then build).

.DESCRIPTION
  Configures desktop/win into desktop/build/win with desktop/win-arm64-toolchain.cmake
  (standalone LLVM + xwin CRT/SDK) and builds it with Ninja. The configure and build run
  at Idle priority (compilers inherit it) so the machine stays responsive. Each generated
  gen/*.c needs 1-2 GB of RAM to compile, so keep -Jobs low on machines with 16 GB or less.
  SDL2 is not rebuilt: lb.exe links the existing build in desktop/build/win-sdl2.

.EXAMPLE
  .\desktop\build_win.ps1 -Target lb_runtime -Jobs 2   # runtime only, no gen/ needed
.EXAMPLE
  .\desktop\build_win.ps1                              # everything -> desktop\build\win\lb.exe
.EXAMPLE
  .\desktop\build_win.ps1 -Fresh -CMakeArgs '-DLB_EXTRA_LINK_OPTIONS=LINKER:/FORCE:UNRESOLVED'
#>
[CmdletBinding()]
param(
    [ValidateRange(1, 64)][int]$Jobs = 4,
    [string]$Target = '',                 # '' = default target (lb); e.g. lb_runtime, lb_gen
    [switch]$Fresh,                       # wipe the CMake cache and configure from scratch
    [switch]$Reconfigure,                 # re-run the configure step even if build.ninja exists
    [switch]$ConfigureOnly,
    [string[]]$CMakeArgs = @(),           # extra -D... arguments for the configure step
    [string]$CMake = 'cmake',
    [string]$Ninja = '',                  # default: ninja on PATH
    [string]$GameExe = '',                # your LEGOMARVEL.exe (icon source); default GAME\LEGOMARVEL.exe
    [string]$Python = ''                  # Python with pefile; default .venv-x64
)
$ErrorActionPreference = 'Stop'

$Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (-not $GameExe) { $GameExe = Join-Path $Root 'GAME\LEGOMARVEL.exe' }
if (-not $Python) { $Python = Join-Path $Root '.venv-x64\Scripts\python.exe' }

# The exe icon is the game's own resource: extract it from your copy into the build dir
# (never committed). CMake embeds desktop\build\win-icon\lb.ico when it exists.
$Icon = Join-Path $Root 'desktop\build\win-icon\lb.ico'
if ((Test-Path $GameExe) -and (Test-Path $Python) -and
    (-not (Test-Path $Icon) -or (Get-Item $GameExe).LastWriteTime -gt (Get-Item $Icon).LastWriteTime)) {
    New-Item -ItemType Directory -Force (Split-Path $Icon) | Out-Null
    $extract = @'
import struct, sys, pefile
RT_ICON, RT_GROUP_ICON = 3, 14
pe = pefile.PE(sys.argv[1], fast_load=True)
pe.parse_data_directories([pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_RESOURCE"]])
icons, group = {}, None
for typ in pe.DIRECTORY_ENTRY_RESOURCE.entries:
    for name in typ.directory.entries:
        lang = name.directory.entries[0]
        data = pe.get_data(lang.data.struct.OffsetToData, lang.data.struct.Size)
        if typ.id == RT_ICON: icons[name.id] = data
        elif typ.id == RT_GROUP_ICON and group is None: group = data
if group is None: sys.exit("no icon in " + sys.argv[1])
count = struct.unpack_from("<H", group, 4)[0]
entries = [struct.unpack_from("<BBBBHHIH", group, 6 + 14 * i) for i in range(count)]
out = bytearray(struct.pack("<HHH", 0, 1, count)); offset = 6 + 16 * count; images = []
for w, h, colors, res, planes, bits, size, icon_id in entries:
    img = icons[icon_id]
    out += struct.pack("<BBBBHHII", w, h, colors, res, planes, bits, len(img), offset)
    offset += len(img); images.append(img)
for img in images: out += img
open(sys.argv[2], "wb").write(bytes(out))
print("icon: %d images -> %s" % (count, sys.argv[2]))
'@
    $eap = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    $extract | & $Python - $GameExe $Icon
    $ErrorActionPreference = $eap
    if ($LASTEXITCODE -ne 0) { Write-Warning "icon extraction failed; building without an icon" }
} elseif (-not (Test-Path $Icon)) {
    Write-Warning "no icon: need $GameExe and $Python (with pefile); building without one"
}
$Src = Join-Path $Root 'desktop\win'
$BuildDir = Join-Path $Root 'desktop\build\win'
# Absolute, forward-slashed: a relative toolchain path would resolve against the build dir.
$Toolchain = (Resolve-Path (Join-Path $Root 'desktop\win-arm64-toolchain.cmake')).Path -replace '\\', '/'

if (-not $Ninja) { $Ninja = (Get-Command ninja -ErrorAction SilentlyContinue).Source }
if (-not $Ninja -or -not (Test-Path $Ninja)) { throw "ninja not found on PATH (pass -Ninja <path>)" }
if (-not (Get-Command $CMake -ErrorAction SilentlyContinue)) { throw "cmake not found (pass -CMake <path>)" }

function Invoke-Native([string]$exe, [string[]]$argv) {
    Write-Host ">> $exe $($argv -join ' ')" -ForegroundColor Cyan
    # Windows PowerShell 5.1 turns native stderr (CMake warnings) into ErrorRecords when the
    # caller redirects it; under 'Stop' that would abort, so judge by the exit code only.
    $eap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { & $exe @argv } finally { $ErrorActionPreference = $eap }
    if ($LASTEXITCODE -ne 0) { throw "$exe failed with exit code $LASTEXITCODE" }
}

# Child processes inherit an Idle/BelowNormal priority class, so lower ours before starting
# cmake/ninja (setting it on ninja afterwards would race with its first compiler spawns).
$self = [System.Diagnostics.Process]::GetCurrentProcess()
$oldPriority = $self.PriorityClass
$self.PriorityClass = [System.Diagnostics.ProcessPriorityClass]::Idle
try {
    $needConfigure = $Fresh -or $Reconfigure -or $ConfigureOnly -or
                     -not (Test-Path (Join-Path $BuildDir 'build.ninja'))
    if ($needConfigure) {
        $cfg = @('-G', 'Ninja', '-S', $Src, '-B', $BuildDir,
                 "-DCMAKE_MAKE_PROGRAM=$($Ninja -replace '\\', '/')",
                 "-DCMAKE_TOOLCHAIN_FILE=$Toolchain",
                 '-DCMAKE_BUILD_TYPE=Release')
        if ($Fresh) { $cfg = @('--fresh') + $cfg }
        Invoke-Native $CMake ($cfg + $CMakeArgs)
    }
    if (-not $ConfigureOnly) {
        $b = @('-C', $BuildDir, "-j$Jobs")
        if ($Target) { $b += $Target }
        Invoke-Native $Ninja $b
        $exe = Join-Path $BuildDir 'lb.exe'
        if (-not $Target -or $Target -eq 'lb') { if (Test-Path $exe) { Write-Host "Built $exe" -ForegroundColor Green } }
    }
}
finally {
    $self.PriorityClass = $oldPriority
}
