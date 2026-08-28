param(
    [string]$BuildDir = "build",
    [switch]$Restart
)

$ErrorActionPreference = "Stop"
$targetName = "simple_monitor_dev"

$repoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$resolvedBuildDir = Resolve-Path -LiteralPath (Join-Path $repoRoot $BuildDir)
$buildDirPath = $resolvedBuildDir.Path
$executablePath = Join-Path $buildDirPath "$targetName.exe"
if ($Restart) {
    if (-not ("SimpleMonitorDevRebuild.NativeWindow" -as [type])) {
        Add-Type `
            -Namespace SimpleMonitorDevRebuild `
            -Name NativeWindow `
            -MemberDefinition '[System.Runtime.InteropServices.DllImport("user32.dll", CharSet=System.Runtime.InteropServices.CharSet.Unicode)] public static extern System.IntPtr FindWindow(string className, string windowName);'
    }
    if ([SimpleMonitorDevRebuild.NativeWindow]::FindWindow("Shell_TrayWnd", $null) -eq [IntPtr]::Zero) {
        throw "Restart requires access to the interactive Windows desktop (Shell_TrayWnd was not visible)."
    }
}

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

if ($Restart) {
    if (-not (Test-Path -LiteralPath $executablePath -PathType Leaf)) {
        throw "Built executable was not found: $executablePath"
    }

    $started = Start-Process `
        -FilePath $executablePath `
        -WorkingDirectory $buildDirPath `
        -WindowStyle Hidden `
        -PassThru
    Start-Sleep -Milliseconds 500
    $started.Refresh()
    if ($started.HasExited) {
        throw "Restarted process exited with code $($started.ExitCode)"
    }
    Write-Output "Started $targetName (PID $($started.Id))"
}
