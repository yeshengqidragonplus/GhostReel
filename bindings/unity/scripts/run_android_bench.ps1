# Runs the M3 on-device benchmark app on the connected phone and collects its results.
#
#   powershell -File bindings/unity/scripts/run_android_bench.ps1 -Apk build\android\m3test.apk -OutDir build\m3-bench
#
# It installs the APK (adb install -r -t), clears logcat, starts the app, waits for ALPHACLIP_BENCH_DONE, saves the
# ALPHACLIP_BENCH lines, and uninstalls the app again. Before and after the run it records read-only device state
# (battery temperature, CPU frequencies, kernel thermal zones) next to the results, because on this phone timings depend
# on which core and what clock the work lands on (see docs/m1-conclusion.md).
# Ask the user before running it: it installs an app on their phone.
# Exit code: 0 results collected, 1 the app did not report, 2 setup problem.
param(
  [Parameter(Mandatory = $true)][string]$Apk,
  [string]$OutDir = 'build\m3-bench',
  [string]$Adb = 'E:\AndroidSDK\platform-tools\adb.exe',
  [string]$Package = 'com.ghostreel.m3test',
  [int]$TimeoutSeconds = 240,
  [switch]$KeepInstalled
)
$ErrorActionPreference = 'Stop'
if (-not (Test-Path $Apk)) { Write-Host "no APK at $Apk"; exit 2 }
New-Item -ItemType Directory -Force $OutDir | Out-Null
$devices = & $Adb devices | Select-String -Pattern "`tdevice$"
if (($devices | Measure-Object).Count -ne 1) { Write-Host "expected exactly one adb device, found $(($devices | Measure-Object).Count)"; exit 2 }

function Snapshot([string]$name) {
  $f = Join-Path $OutDir "$name.txt"
  $lines = @()
  $lines += '--- battery'
  $lines += (& $Adb shell dumpsys battery) -join "`n"
  $lines += '--- cpu frequencies (kHz): cur/max per core'
  $lines += (& $Adb shell 'for c in /sys/devices/system/cpu/cpu[0-7]; do echo $(basename $c) $(cat $c/cpufreq/scaling_cur_freq 2>/dev/null) $(cat $c/cpufreq/cpuinfo_max_freq 2>/dev/null); done') -join "`n"
  $lines += '--- thermal zones (type temp)'
  $lines += (& $Adb shell 'for z in /sys/class/thermal/thermal_zone*; do echo $(cat $z/type 2>/dev/null) $(cat $z/temp 2>/dev/null); done') -join "`n"
  $lines | Set-Content -Encoding UTF8 $f
}

$props = @{}
foreach ($k in 'ro.product.model', 'ro.build.version.release', 'ro.build.version.sdk', 'ro.product.cpu.abi', 'ro.build.fingerprint') {
  $props[$k] = (& $Adb shell getprop $k).Trim()
}
$props | ConvertTo-Json | Set-Content -Encoding UTF8 (Join-Path $OutDir 'device.json')

$installed = $false
try {
  & $Adb install -r -t $Apk | Write-Host
  $installed = $true
  Snapshot 'before'
  & $Adb logcat -c
  & $Adb shell am start -n "$Package/com.unity3d.player.UnityPlayerActivity" | Write-Host
  $logFile = Join-Path $OutDir 'logcat.txt'
  $done = $false
  $sw = [Diagnostics.Stopwatch]::StartNew()
  while ($sw.Elapsed.TotalSeconds -lt $TimeoutSeconds) {
    Start-Sleep -Seconds 5
    & $Adb logcat -d -s Unity:I | Set-Content -Encoding UTF8 $logFile
    if (Select-String -Path $logFile -Pattern 'ALPHACLIP_BENCH_DONE' -Quiet) { $done = $true; break }
    # the app died?
    $pid2 = (& $Adb shell pidof $Package).Trim()
    if (-not $pid2) { & $Adb logcat -d | Set-Content -Encoding UTF8 (Join-Path $OutDir 'logcat_full.txt'); Write-Host "the app is not running any more"; break }
  }
  Snapshot 'after'
  & $Adb logcat -d | Set-Content -Encoding UTF8 (Join-Path $OutDir 'logcat_full.txt')
  Select-String -Path $logFile -Pattern 'ALPHACLIP_BENCH' | ForEach-Object { $_.Line -replace '^.*(ALPHACLIP_BENCH)', '$1' } | Set-Content -Encoding UTF8 (Join-Path $OutDir 'results.txt')
  Get-Content (Join-Path $OutDir 'results.txt')
  if (-not $done) { exit 1 }
} finally {
  if ($installed -and -not $KeepInstalled) { & $Adb uninstall $Package | Write-Host }
}
exit 0
