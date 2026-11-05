# Generates synthetic M0 test clips. Our own content only (ffmpeg lavfi sources);
# no third-party assets are used or needed.
#
#   alpha_native.webm        VP9 yuva420p, alpha in BlockAdditional (ffmpeg writes the
#                            AlphaMode element automatically for yuva420p)
#   opaque.webm              VP9 yuv420p, no alpha (control)
#   alpha_truth.gray         raw 8-bit gray: exact alpha ground truth, 640x360 x 60 frames
#
# Alpha is a soft-edged disc (0 outside, 255 inside, a 40px ramp on the rim) that
# orbits the frame, so every frame contains fully transparent, fully opaque and
# partially transparent pixels. Keyframe every 30 frames (2 keyframes) for seek tests.
param(
  [string]$Out = $PSScriptRoot,
  [string]$Ffmpeg = 'E:\Tools\ffmpeg-9.0.2-full_build\bin\ffmpeg.exe'
)
$ErrorActionPreference = 'Stop'

$size = '640x360'; $fps = 30; $dur = 2
$alphaExpr = 'clip((140-hypot(X-320-120*sin(2*T),Y-180))*255/40,0,255)'
$colorSrc  = "testsrc2=s=${size}:r=${fps}:d=${dur},format=yuv420p"
$alphaSrc  = "nullsrc=s=${size}:r=${fps}:d=${dur},format=gray,geq=lum='${alphaExpr}'"

$enc = @('-c:v','libvpx-vp9','-b:v','0','-crf','28','-g','30','-auto-alt-ref','0','-lag-in-frames','0',
         '-deadline','good','-cpu-used','2','-threads','1','-an')

function Run([string[]]$ffArgs) {
  & $Ffmpeg -hide_banner -loglevel error -y @ffArgs
  if ($LASTEXITCODE -ne 0) { throw "ffmpeg failed: $($ffArgs -join ' ')" }
}

$graph = "${colorSrc}[c];${alphaSrc}[a];[c][a]alphamerge,format=yuva420p[v]"

Run (@('-lavfi',$graph,'-map','[v]') + $enc +
     @('-pix_fmt','yuva420p','-metadata:s:v:0','alpha_mode=1',(Join-Path $Out 'alpha_native.webm')))

Run (@('-f','lavfi','-i',$colorSrc) + $enc + @('-pix_fmt','yuv420p',(Join-Path $Out 'opaque.webm')))

Run @('-f','lavfi','-i',$alphaSrc,'-pix_fmt','gray','-f','rawvideo',(Join-Path $Out 'alpha_truth.gray'))

Get-ChildItem $Out | Where-Object { $_.Extension -in '.webm','.gray' } |
  Select-Object Name, @{n='KB';e={[math]::Round($_.Length/1KB,1)}} | Format-Table -AutoSize
