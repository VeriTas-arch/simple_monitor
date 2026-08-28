param(
    [string]$BuildDir = "build"
)

$ErrorActionPreference = "Stop"
$targetName = "simple_monitor_dev"

$repoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$resolvedBuildDir = Resolve-Path -LiteralPath (Join-Path $repoRoot $BuildDir)
$buildDirPath = $resolvedBuildDir.Path
$executablePath = Join-Path $buildDirPath "$targetName.exe"

$running = Get-Process -Name $targetName -ErrorAction SilentlyContinue |
    Where-Object { $_.Path -eq $executablePath }
if ($running) {
    $running | Stop-Process -Force
    $pids = $running.Id
    if ($pids) {
        Wait-Process -Id $pids -Timeout 3 -ErrorAction SilentlyContinue
    }
}

cmake --build $buildDirPath --target simple_monitor_dev

if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}
