# Runs rechan's scripted --autotest mode over a set of levels and frame rates, then
# compares the screenshots with a baseline.
#
#   scripts\run-autotests.ps1                  compare against scripts\autotest-baseline
#   scripts\run-autotests.ps1 -UpdateBaseline  record the current output as the baseline
#   scripts\run-autotests.ps1 -Exe bin\rechan-asan.exe -NoCompare   crash/ASan sweep only
#
# The baseline is local (it is rendered from the user's own game data), so it is not
# committed. Record it on a known-good build, then re-run after renderer changes.
param(
    [string]$Exe = "bin\rechan.exe",
    [string]$OutDir = "build\autotest",
    [string]$BaselineDir = "scripts\autotest-baseline",
    [double]$Threshold = 2.0,
    [switch]$UpdateBaseline,
    [switch]$NoCompare
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$exePath = Join-Path $root $Exe
$workDir = Split-Path -Parent $exePath
$outPath = Join-Path $root $OutDir
$basePath = Join-Path $root $BaselineDir

# name, level index, petal, frames, held buttons, fps
$cases = @(
    @("l0_start_30",   0, 0,  240, "",      30),
    @("l0_start_60",   0, 0,  480, "",      60),
    @("l0_start_144",  0, 0, 1152, "",     144),
    @("l0_enemy_30",   0, 0,  750, "right", 30),
    @("l0_enemy_60",   0, 0, 1500, "right", 60),
    @("l1_start_60",   1, 0,  480, "",      60),
    @("l2_start_60",   2, 0,  480, "",      60),
    @("l3_start_60",   3, 0,  480, "",      60),
    @("l4_start_60",   4, 0,  480, "",      60),
    @("l5_start_60",   5, 0,  480, "",      60),
    @("l6_hub_60",     6, 0,  480, "",      60)
)

Add-Type -AssemblyName System.Drawing

function Get-MeanDiff([string]$a, [string]$b) {
    $ia = [System.Drawing.Bitmap]::FromFile($a)
    $ib = [System.Drawing.Bitmap]::FromFile($b)
    try {
        if ($ia.Width -ne $ib.Width -or $ia.Height -ne $ib.Height) { return 255.0 }
        $rect = New-Object System.Drawing.Rectangle 0, 0, $ia.Width, $ia.Height
        $fmt = [System.Drawing.Imaging.PixelFormat]::Format24bppRgb
        $da = $ia.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly, $fmt)
        $db = $ib.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly, $fmt)
        $len = [Math]::Abs($da.Stride) * $ia.Height
        $ba = New-Object byte[] $len
        $bb = New-Object byte[] $len
        [System.Runtime.InteropServices.Marshal]::Copy($da.Scan0, $ba, 0, $len)
        [System.Runtime.InteropServices.Marshal]::Copy($db.Scan0, $bb, 0, $len)
        $ia.UnlockBits($da)
        $ib.UnlockBits($db)
        [long]$sum = 0
        # Sample every 4th byte: plenty for a mean and keeps this fast in PowerShell.
        for ($i = 0; $i -lt $len; $i += 4) { $sum += [Math]::Abs([int]$ba[$i] - [int]$bb[$i]) }
        return [double]$sum / ($len / 4)
    }
    finally {
        $ia.Dispose()
        $ib.Dispose()
    }
}

New-Item -ItemType Directory -Force $outPath | Out-Null
if ($UpdateBaseline) { New-Item -ItemType Directory -Force $basePath | Out-Null }
$env:RECHAN_CRASH_REPORTER_NO_DIALOG = "1"

$failed = 0
foreach ($c in $cases) {
    $name, $level, $petal, $frames, $hold, $fps = $c
    $shot = Join-Path $outPath "$name.png"
    Remove-Item $shot -ErrorAction SilentlyContinue

    $argLine = "--autotest $level --petal $petal --frames $frames --fps $fps --screenshot `"$shot`""
    if ($hold) { $argLine += " --hold $hold" }

    $errFile = Join-Path $outPath "$name.stderr.txt"
    $p = Start-Process -FilePath $exePath -ArgumentList $argLine -WorkingDirectory $workDir -PassThru `
        -RedirectStandardError $errFile -RedirectStandardOutput (Join-Path $outPath "stdout.txt")
    if (-not $p.WaitForExit(600000)) {
        $p.Kill()
        Write-Host "FAIL  $name  timed out"
        $failed++
        continue
    }

    $status = "ok"
    if ($p.ExitCode -ne 0) { $status = "exit code $($p.ExitCode)" }
    elseif ((Get-Item $errFile).Length -gt 0 -and (Select-String -Path $errFile -Pattern "AddressSanitizer" -Quiet)) { $status = "ASan report in $errFile" }
    elseif (-not (Test-Path $shot)) { $status = "no screenshot" }

    if ($status -ne "ok") {
        Write-Host "FAIL  $name  $status"
        $failed++
        continue
    }

    $baseShot = Join-Path $basePath "$name.png"
    if ($UpdateBaseline) {
        Copy-Item $shot $baseShot -Force
        Write-Host "BASE  $name"
    }
    elseif ($NoCompare) {
        Write-Host "RAN   $name"
    }
    elseif (-not (Test-Path $baseShot)) {
        Write-Host "NEW   $name  (no baseline)"
    }
    else {
        $diff = Get-MeanDiff $shot $baseShot
        if ($diff -gt $Threshold) {
            Write-Host ("DIFF  {0}  mean abs diff {1:N2} > {2}" -f $name, $diff, $Threshold)
            $failed++
        }
        else {
            Write-Host ("PASS  {0}  mean abs diff {1:N2}" -f $name, $diff)
        }
    }
}

Write-Host "$failed failed of $($cases.Count)"
exit $failed
