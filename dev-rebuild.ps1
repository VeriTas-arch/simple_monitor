param(
    [string]$BuildDir = "build",
    [switch]$NoLaunch
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$resolvedBuildDir = Resolve-Path -LiteralPath (Join-Path $repoRoot $BuildDir)
$buildDirPath = $resolvedBuildDir.Path
$exePath = Join-Path $buildDirPath "simple_monitor.exe"

$running = Get-Process -Name "simple_monitor" -ErrorAction SilentlyContinue
if ($running) {
    $running | Stop-Process -Force

    for ($i = 0; $i -lt 20; $i++) {
        Start-Sleep -Milliseconds 100
        if (-not (Get-Process -Name "simple_monitor" -ErrorAction SilentlyContinue)) {
            break
        }
    }
}

cmake --build $buildDirPath

if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

if (-not (Test-Path -LiteralPath $exePath)) {
    throw "Build succeeded but '$exePath' was not found."
}

if (-not $NoLaunch) {
    Start-Process -FilePath $exePath -WorkingDirectory $buildDirPath
}
