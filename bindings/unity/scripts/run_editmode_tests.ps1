# Runs the AlphaClip editor tests of bindings/unity/TestProject in batch mode and prints the report.
#
#   powershell -File bindings/unity/scripts/run_editmode_tests.ps1 -ColorSpace Linear `
#       -Pairs "C:\clips\opaque.webm|C:\clips\opaque.raw;C:\clips\alpha_native.webm|C:\clips\alpha_native.raw"
#
# The .raw file next to each clip is `alphaclip decode clip.webm --out clip.raw` (planar Y,U,V[,A]); the
# tests compare the rendered picture with an independent CPU conversion of it.
# The project colour space (-ColorSpace) and render pipeline (-Pipeline Builtin|URP) are switched first when they
# differ (a separate editor start: the change only takes effect after a restart). Close any editor that has this
# project open first.
# With -PlayClip <clip.webm> the play-mode tests run instead (the real component lifecycle, Update-driven
# playback, OnDestroy): the editor enters play mode, so it is started without -quit and the test driver exits it.
# With -Demo the demo scene is (re)created from the menu code and played for a moment; -SnapshotDir <dir> makes the
# tests save what they rendered as PNG files there.
# -OfflineGuard starts the editor with a dead HTTP(S) proxy: if the package manager wanted to download something it
# fails (visible in the log) instead of fetching. Use it whenever nothing may be downloaded.
# Exit code: 0 all checks passed, 1 a check failed, 2 an exception, 3 the editor did not finish.
param(
  [string]$Unity = 'D:\Program Files\Unity\Hub\Editor\2022.3.62f2\Editor\Unity.exe',
  [string]$Project = '',
  [ValidateSet('Gamma', 'Linear')][string]$ColorSpace = 'Gamma',
  [ValidateSet('Builtin', 'URP')][string]$Pipeline = 'Builtin',
  [string]$Pairs = '',
  [string]$PlayClip = '',
  [switch]$Demo,
  [switch]$OfflineGuard,
  [string]$SnapshotDir = '',
  [string]$Method = 'AlphaClip.Tests.EditModeTests.Run',
  [string]$WorkDir = $env:TEMP,
  [int]$TimeoutMinutes = 9
)
$ErrorActionPreference = 'Stop'
if (-not $Project) { $Project = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) '..\TestProject' }
$Project = (Resolve-Path $Project).Path
New-Item -ItemType Directory -Force $WorkDir | Out-Null

function Invoke-Unity([string[]]$UnityArgs, [string]$Log) {
  Remove-Item $Log -ErrorAction SilentlyContinue
  $all = @('-batchmode', '-projectPath', "`"$Project`"", '-logFile', "`"$Log`"") + $UnityArgs
  $savedHttp = $env:HTTP_PROXY; $savedHttps = $env:HTTPS_PROXY
  if ($OfflineGuard) { $env:HTTP_PROXY = 'http://127.0.0.1:9'; $env:HTTPS_PROXY = 'http://127.0.0.1:9' }
  try {
    $p = Start-Process -FilePath $Unity -ArgumentList $all -PassThru
    if (-not $p.WaitForExit($TimeoutMinutes * 60 * 1000)) { $p.Kill(); return 3 }
    return $p.ExitCode
  } finally { $env:HTTP_PROXY = $savedHttp; $env:HTTPS_PROXY = $savedHttps }
}

# 1. colour space (ProjectSettings.asset: m_ActiveColorSpace 0 = Gamma, 1 = Linear)
$settings = Join-Path $Project 'ProjectSettings\ProjectSettings.asset'
$want = if ($ColorSpace -eq 'Linear') { 1 } else { 0 }
$have = $null
if (Test-Path $settings) {
  $m = Select-String -Path $settings -Pattern '^\s*m_ActiveColorSpace:\s*(\d+)' | Select-Object -First 1
  if ($m) { $have = [int]$m.Matches[0].Groups[1].Value }
}
if ($have -ne $want) {
  Write-Host "switching the project colour space to $ColorSpace (was $have)"
  $rc = Invoke-Unity @('-executeMethod', "AlphaClip.Tests.BatchTests.Set$ColorSpace", '-quit') (Join-Path $WorkDir 'unity_colorspace.log')
  if ($rc -ne 0) { Write-Host "colour space switch failed ($rc), see unity_colorspace.log"; exit 3 }
}

# 1b. render pipeline (GraphicsSettings.asset: m_CustomRenderPipeline is {fileID: 0} when there is none)
$graphics = Join-Path $Project 'ProjectSettings\GraphicsSettings.asset'
$haveUrp = $false
if (Test-Path $graphics) {
  $g = Select-String -Path $graphics -Pattern '^\s*m_CustomRenderPipeline:\s*(.*)$' | Select-Object -First 1
  if ($g) { $haveUrp = $g.Matches[0].Groups[1].Value -notmatch '^\{fileID: 0\}' }
}
if (($Pipeline -eq 'URP') -ne $haveUrp) {
  Write-Host "switching the project render pipeline to $Pipeline"
  # (not named $method: PowerShell variables are case-insensitive and that would overwrite the -Method parameter)
  $switchMethod = if ($Pipeline -eq 'URP') { 'SetUrp' } else { 'SetBuiltin' }
  $rc = Invoke-Unity @('-executeMethod', "AlphaClip.Tests.BatchTests.$switchMethod", '-quit') (Join-Path $WorkDir 'unity_pipeline.log')
  if ($rc -ne 0) { Write-Host "render pipeline switch failed ($rc), see unity_pipeline.log"; exit 3 }
}

# 2. the tests
$report = Join-Path $WorkDir 'unity_report.txt'
Remove-Item $report -ErrorAction SilentlyContinue
$log = Join-Path $WorkDir 'unity_tests.log'
$extra = @('-acExpectPipeline', $Pipeline)
if ($SnapshotDir) { New-Item -ItemType Directory -Force $SnapshotDir | Out-Null; $extra += @('-acSnapshotDir', "`"$SnapshotDir`"") }
if ($Demo) {
  $rc = Invoke-Unity @('-executeMethod', 'AlphaClip.Tests.DemoScene.CreateBatch') (Join-Path $WorkDir 'unity_demo_create.log')
  if ($rc -ne 0) { Write-Host "creating the demo scene failed ($rc), see unity_demo_create.log"; exit 3 }
  $rc = Invoke-Unity (@('-executeMethod', 'AlphaClip.Tests.DemoScene.PlayDemoBatch', '-acDemoCheck', '-acReport', "`"$report`"") + $extra) $log
} elseif ($PlayClip) {
  $rc = Invoke-Unity (@('-executeMethod', 'AlphaClip.Tests.BatchTests.RunPlayMode', '-acPlayTest', '-acClip', "`"$PlayClip`"", '-acReport', "`"$report`"") + $extra) $log
} else {
  if (-not $Pairs) { Write-Host 'give -Pairs (edit-mode tests), -PlayClip (play-mode tests) or -Demo'; exit 2 }
  $rc = Invoke-Unity (@('-executeMethod', $Method, '-acPairs', "`"$Pairs`"", '-acExpectColorSpace', $ColorSpace, '-acReport', "`"$report`"") + $extra) $log
}
if (Test-Path $report) { Get-Content $report } else { Write-Host "no report was written; see $log" }
Write-Host "unity exit code: $rc"
exit $rc
