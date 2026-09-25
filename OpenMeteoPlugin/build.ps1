# Builds OpenMeteo.dll and SmokeTest.exe (x64).
#
#   .\build.ps1             — build Release
#   .\build.ps1 -Test       — build and run SmokeTest (makes a real request to Open-Meteo)
#   .\build.ps1 -Install    — build and copy the DLL into TrafficMonitor's plugins folder
param(
    [ValidateSet('Release', 'Debug')] [string]$Configuration = 'Release',
    [switch]$Test,
    [switch]$Install,
    [string]$TrafficMonitorDir = (Split-Path $PSScriptRoot -Parent)
)
$ErrorActionPreference = 'Stop'
$name = 'OpenMeteo'

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw 'vswhere.exe not found — install Visual Studio Build Tools.' }
$msbuild = & $vswhere -latest -products * -prerelease -requires Microsoft.Component.MSBuild `
    -find 'MSBuild\**\Bin\amd64\MSBuild.exe' | Select-Object -First 1
if (-not $msbuild) { throw 'MSBuild not found. Install Build Tools with the "Desktop development with C++" workload.' }

foreach ($project in "$name.vcxproj", 'tools\SmokeTest.vcxproj') {
    & $msbuild (Join-Path $PSScriptRoot $project) /nologo /verbosity:minimal /m `
        "/p:Configuration=$Configuration" /p:Platform=x64
    if ($LASTEXITCODE -ne 0) { throw "Failed to build $project" }
}

$bin = Join-Path $PSScriptRoot "bin\$Configuration"
$dll = Join-Path $bin "$name.dll"
Write-Host "Built: $dll"

if ($Test) {
    [Console]::OutputEncoding = [Text.Encoding]::UTF8
    & (Join-Path $bin 'SmokeTest.exe') $dll
    if ($LASTEXITCODE -ne 0) { throw 'SmokeTest failed' }
}

if ($Install) {
    $plugins = Join-Path $TrafficMonitorDir 'plugins'
    if (-not (Test-Path (Join-Path $TrafficMonitorDir 'TrafficMonitor.exe'))) {
        throw "No TrafficMonitor.exe in $TrafficMonitorDir — pass -TrafficMonitorDir."
    }
    $target = Join-Path $plugins "$name.dll"
    if (Test-Path $target) {
        # A DLL loaded by TrafficMonitor can't be overwritten or deleted, but it can be moved within
        # the same drive: the running process keeps using the old copy.
        $previous = Join-Path $PSScriptRoot 'obj\previous'
        New-Item -ItemType Directory -Force $previous | Out-Null
        Get-ChildItem $previous -Filter *.dll | Remove-Item -ErrorAction SilentlyContinue   # ones no longer loaded
        Move-Item $target (Join-Path $previous "$name.$(Get-Date -Format yyyyMMdd-HHmmss).dll")
    }
    Copy-Item $dll $target
    Write-Host "Installed: $target. Restart TrafficMonitor to load the new version."
}
