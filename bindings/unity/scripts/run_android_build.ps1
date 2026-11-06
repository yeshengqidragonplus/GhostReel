# Builds the M3 on-device benchmark app (a development APK, arm64) from a COPY of bindings/unity.
#
#   powershell -File bindings/unity/scripts/run_android_build.ps1 -OfflineGuard            # dry run: nothing may be downloaded
#   powershell -File bindings/unity/scripts/run_android_build.ps1 -GradleHome E:\Tools\gradle-home   # real build
#
# The copy lives in <repo>\build\unity-android (git-ignored) so that the Android player settings and the Android
# import of the project never touch the committed TestProject. Run sync_plugin_android_arm64.sh first: the package's
# Plugins\Android\arm64-v8a\libalphaclip.so is what gets packaged. The first run imports the project for Android
# (several minutes); later runs reuse build\unity-android\TestProject\Library.
#
# -OfflineGuard: Gradle runs with org.gradle.offline=true and a dead proxy (a gradle.properties in a throw-away
# GRADLE_USER_HOME), so a dependency that is not cached makes the build fail with the list of what it wanted instead of
# downloading it. Without it, Gradle downloads the Android Gradle Plugin and its dependencies (ask first), into
# -GradleHome (default: the user's ~\.gradle on C:, which is nearly full; point it at E:).
# Exit code: 0 the APK was built, 1 the build failed, 3 the editor did not finish.
param(
  [string]$Unity = 'D:\Program Files\Unity\Hub\Editor\2022.3.62f2\Editor\Unity.exe',
  [string]$OutApk = '',
  [string]$GradleHome = '',
  [switch]$OfflineGuard,
  [int]$TimeoutMinutes = 45
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) '..\..\..')).Path
if (-not $OutApk) { $OutApk = Join-Path $repo 'build\android\m3test.apk' }
$work = Join-Path $repo 'build\unity-android'
$log = Join-Path $repo 'build\unity-android.log'

# 1. copy bindings/unity (without the generated folders of the committed TestProject) next to the previous copy
$src = Join-Path $repo 'bindings\unity'
New-Item -ItemType Directory -Force $work | Out-Null
robocopy $src $work /MIR /XD Library Temp Logs obj UserSettings MemoryCaptures Builds /XF *.csproj *.sln *.slnx /NFL /NDL /NJH /NJS /NP | Out-Null
if ($LASTEXITCODE -ge 8) { throw "robocopy failed ($LASTEXITCODE)" }
# /MIR above would delete the Library of an earlier copy; it is excluded from the compare via /XD, so it stays.

# 2. Gradle environment
$savedGradle = $env:GRADLE_USER_HOME; $savedHttp = $env:HTTP_PROXY; $savedHttps = $env:HTTPS_PROXY
if ($OfflineGuard) {
  $guardHome = Join-Path $repo 'build\gradle-offline-guard'
  New-Item -ItemType Directory -Force $guardHome | Out-Null
  $props = "org.gradle.offline=true`nsystemProp.http.proxyHost=127.0.0.1`nsystemProp.http.proxyPort=9`nsystemProp.https.proxyHost=127.0.0.1`nsystemProp.https.proxyPort=9`n"
  [IO.File]::WriteAllText((Join-Path $guardHome 'gradle.properties'), $props, (New-Object Text.UTF8Encoding($false)))
  $env:GRADLE_USER_HOME = $guardHome
  $env:HTTP_PROXY = 'http://127.0.0.1:9'; $env:HTTPS_PROXY = 'http://127.0.0.1:9'
} elseif ($GradleHome) {
  New-Item -ItemType Directory -Force $GradleHome | Out-Null
  $env:GRADLE_USER_HOME = $GradleHome
}

# 3. build
Remove-Item $log -ErrorAction SilentlyContinue
$args2 = @('-batchmode', '-buildTarget', 'Android', '-projectPath', "`"$(Join-Path $work 'TestProject')`"", '-logFile', "`"$log`"",
           '-executeMethod', 'AlphaClip.Tests.AndroidBuild.Build', '-acOut', "`"$OutApk`"")
try {
  $p = Start-Process -FilePath $Unity -ArgumentList $args2 -PassThru
  if (-not $p.WaitForExit($TimeoutMinutes * 60 * 1000)) { $p.Kill(); Write-Host "the editor did not finish in $TimeoutMinutes minutes"; exit 3 }
  $code = $p.ExitCode
} finally { $env:GRADLE_USER_HOME = $savedGradle; $env:HTTP_PROXY = $savedHttp; $env:HTTPS_PROXY = $savedHttps }
Select-String -Path $log -Pattern 'AlphaClip Android build|error CS|BuildFailedException|FAILURE:|Could not resolve|Could not GET|What went wrong' -ErrorAction SilentlyContinue |
  Select-Object -First 40 | ForEach-Object { $_.Line }
if ($code -eq 0 -and (Test-Path $OutApk)) { Get-Item $OutApk | ForEach-Object { "built {0} ({1:N1} MB)" -f $_.FullName, ($_.Length / 1MB) } }
exit $code
