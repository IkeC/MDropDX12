# build.ps1 — Invoke from any shell (Git Bash, cmd, Claude's Bash tool, etc.)
# Usage:  powershell -ExecutionPolicy Bypass -File build.ps1 [Debug|Release] [Win32|x64] [Clean]
#
# Examples:
#   powershell -ExecutionPolicy Bypass -File build.ps1                  # Debug x64 build
#   powershell -ExecutionPolicy Bypass -File build.ps1 Release          # Release x64 build
#   powershell -ExecutionPolicy Bypass -File build.ps1 Debug Win32      # Debug Win32 build
#   powershell -ExecutionPolicy Bypass -File build.ps1 Release Win32    # Release Win32 build
#   powershell -ExecutionPolicy Bypass -File build.ps1 Debug x64 Clean  # Clean then build

param(
    [string]$Configuration = "Debug",
    [string]$Platform      = "x64",  # "x64" or "Win32"
    [string]$Target        = "Build"   # "Build" or "Clean"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# ── 1. Locate vswhere ──────────────────────────────────────────────────────────
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    $vswhere = "${env:ProgramFiles}\Microsoft Visual Studio\Installer\vswhere.exe"
}
if (-not (Test-Path $vswhere)) {
    Write-Error "vswhere.exe not found. Install Visual Studio 2022 or later."
    exit 1
}

# ── 2. Find MSBuild ────────────────────────────────────────────────────────────
$msbuild = & $vswhere -latest -requires Microsoft.Component.MSBuild `
           -find "MSBuild\**\Bin\MSBuild.exe" | Select-Object -First 1

# Fallback: vswhere without -requires (works after fresh VS install where component
# registration may not be complete yet)
if (-not $msbuild -or -not (Test-Path $msbuild)) {
    $msbuild = & $vswhere -latest -find "MSBuild\**\Bin\MSBuild.exe" | Select-Object -First 1
}

# Fallback: known installation paths
if (-not $msbuild -or -not (Test-Path $msbuild)) {
    $paths = @(
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe",
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe",
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe",
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"
    )
    foreach ($p in $paths) { if (Test-Path $p) { $msbuild = $p; break } }
}

if (-not $msbuild -or -not (Test-Path $msbuild)) {
    Write-Error "MSBuild not found. Ensure Visual Studio 2022 with C++ workload is installed."
    exit 1
}

Write-Host "MSBuild  : $msbuild"
Write-Host "Config   : $Configuration"
Write-Host "Platform : $Platform"
Write-Host "Target   : $Target"
Write-Host ""

# ── 3. Fetch Spout2 SDK if needed ─────────────────────────────────────────────
$spoutDir = Join-Path $PSScriptRoot "external\Spout2"
if (-not (Test-Path (Join-Path $spoutDir "SPOUTSDK"))) {
    Write-Host "Fetching Spout2 SDK..."
    & git clone --depth 1 https://github.com/leadedge/Spout2.git $spoutDir
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Failed to clone Spout2 SDK."
        exit 1
    }
}

# ── 3b. Fetch Scintilla if needed ─────────────────────────────────────────────
# Statically linked into engine.vcxproj for the Preset Editor's syntax
# highlighting.  Lexilla is deliberately NOT fetched: we lex in-process with
# SCLEX_CONTAINER (see src/mDropDX12/preset_code_lexer.cpp).
$sciDir = Join-Path $PSScriptRoot "external\scintilla"
if (-not (Test-Path (Join-Path $sciDir "src\Editor.cxx"))) {
    Write-Host "Fetching Scintilla 5.6.6..."
    $sciZip = Join-Path $env:TEMP "scintilla566.zip"
    if (-not (Test-Path $sciZip)) {
        Invoke-WebRequest -Uri "https://www.scintilla.org/scintilla566.zip" -OutFile $sciZip
    }
    $sciParent = Join-Path $PSScriptRoot "external"
    if (-not (Test-Path $sciParent)) { New-Item -ItemType Directory -Force $sciParent | Out-Null }
    Expand-Archive -Path $sciZip -DestinationPath $sciParent -Force
    if (-not (Test-Path (Join-Path $sciDir "src\Editor.cxx"))) {
        Write-Error "Scintilla extraction failed: $sciDir\src\Editor.cxx not found."
        exit 1
    }
}

# ── 4. Kill running exe (async, non-blocking) ─────────────────────────────────
# Only the instances the linker is about to overwrite, i.e. those running from
# THIS configuration's output directory. A copy of the exe kept elsewhere so it
# can survive rebuilds -- Shane runs one from MilkAssets\bin -- carries the same
# process name, so matching on the name alone would kill it on every build.
# The OutDir values come from engine.vcxproj: "<Config>_x64" for x64, and a bare
# "<Config>" for Win32.
# NB: this script runs under Windows PowerShell 5.1 (powershell.exe), so no
# PS7-only syntax here -- no null-conditional "?.", no try/catch as an expression.
$exeName   = "MDropDX12"
$outSuffix = ""
if ($Platform -eq "x64") { $outSuffix = "_x64" }
$outDir = Join-Path $PSScriptRoot "src\mDropDX12\$Configuration$outSuffix"

# Resolve so the comparison is against a canonical path; if the directory does
# not exist yet there is nothing running from it to kill.
$outDirFull = $null
if (Test-Path -LiteralPath $outDir) {
    $outDirFull = (Resolve-Path -LiteralPath $outDir).Path
}
if ($outDirFull) {
    $prefix = $outDirFull.TrimEnd('\') + '\'
    Get-Process -Name $exeName -ErrorAction SilentlyContinue | ForEach-Object {
        # .Path throws for processes we cannot open (elevated / exiting), and a
        # process we cannot inspect is not one we should be killing.
        $path = $null
        try { $path = $_.Path } catch { $path = $null }
        if ($path -and $path.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
            Write-Host "Killing $exeName.exe (PID $($_.Id)) from $($prefix)..."
            Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue
        }
        elseif ($path) {
            Write-Host "Leaving $exeName.exe (PID $($_.Id)) alone -- runs from $path"
        }
    }
}

# ── 5. Run the build ───────────────────────────────────────────────────────────
$project = Join-Path $PSScriptRoot "src\mDropDX12\engine.vcxproj"

# "Clean" target means clean first, then build (full rebuild)
if ($Target -eq "Clean") {
    Write-Host "Cleaning..."
    & $msbuild $project `
        /t:Clean `
        /p:Configuration=$Configuration `
        /p:Platform=$Platform `
        /p:PlatformToolset=v143 `
        /m `
        /nologo `
        /clp:Summary
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    Write-Host ""
    Write-Host "Building..."
    $Target = "Build"
}

& $msbuild $project `
    /t:$Target `
    /p:Configuration=$Configuration `
    /p:Platform=$Platform `
    /p:PlatformToolset=v143 `
    /m `
    /nologo `
    /clp:Summary

exit $LASTEXITCODE
