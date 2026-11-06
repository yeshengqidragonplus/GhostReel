# Generates the M4 test clips: ONE known shape (two soft-edged discs, a saturated colour that varies in space and
# time and is defined everywhere) stored four ways, plus exact RGBA ground truth for a few frames. Our own content
# only (ffmpeg lavfi sources); no third-party assets are used or needed.
#
#   native_straight.webm       VP9 yuva420p, alpha in BlockAdditional, colour NOT multiplied by alpha ("straight";
#                              the colour continues under the transparent pixels, so 4:2:0 cannot smear black into the edge)
#   native_premul.webm         same, colour already multiplied by alpha ("premultiplied": black where transparent)
#   packed_right_premul.webm   640x180 yuv420p: colour (premultiplied) on the left, alpha as a luma image on the right
#   packed_bottom_straight.webm  320x360 yuv420p: straight colour on top, alpha as a luma image below
#   truth_rgba_3f.raw          straight RGBA, 320x180, frames 0, 20 and 40, no chroma subsampling, no codec
#
# Colour is BT.601, limited range (what ffmpeg writes for RGB input); in the packed clips the alpha image is stored as
# LUMA in the same range as the colour (alpha 0 -> Y 16, alpha 255 -> Y 235; chroma 128), which is what a grey image
# becomes in a video; the sidecars say bt601 / limited. native_alpha stores alpha full range (0..255) in the second stream.
# CRF is low (10) so that codec noise stays small next to the effects the tests look for.
# Sidecars (*.acmeta.json) are hand-written files next to the clips; they are committed, not generated.
param(
  [string]$Out = $PSScriptRoot,
  [string]$Ffmpeg = 'E:\Tools\ffmpeg-9.0.2-full_build\bin\ffmpeg.exe'
)
$ErrorActionPreference = 'Stop'

$w = 320; $h = 180; $fps = 30; $dur = 2
$base = "nullsrc=s=${w}x${h}:r=${fps}:d=${dur}"

# Alpha: a big disc (radius 62, 14 px ramp) that orbits, and a small one (radius 22, 10 px ramp); 0 outside, 255 inside.
$d1 = 'clip((62-hypot(X-160-60*sin(2*T),Y-90-18*cos(2*T)))*255/14,0,255)'
$d2 = 'clip((22-hypot(X-70-30*cos(3*T),Y-135))*255/10,0,255)'
$A = "max($d1,$d2)"
# Colour (R, G, B in 0..255), saturated and varying, defined for the whole frame.
$R = '210+45*sin(X/17+3*T)'
$G = '70+60*sin(Y/23+T)'
$B = '40+40*sin((X+Y)/29)'

$colorStraight = "$base,format=gbrp,geq=r='$R':g='$G':b='$B'"
$colorPremul = "$base,format=gbrp,geq=r='($R)*($A)/255':g='($G)*($A)/255':b='($B)*($A)/255'"
$alphaGray = "$base,format=gray,geq=lum='$A'"
$alphaLuma = "$base,format=yuv420p,geq=lum='16+219*($A)/255':cb=128:cr=128"

$enc = @('-c:v', 'libvpx-vp9', '-b:v', '0', '-crf', '10', '-g', '30', '-auto-alt-ref', '0', '-lag-in-frames', '0',
         '-deadline', 'good', '-cpu-used', '2', '-threads', '1', '-an')

function Run([string[]]$ffArgs) {
  & $Ffmpeg -hide_banner -loglevel error -y @ffArgs
  if ($LASTEXITCODE -ne 0) { throw "ffmpeg failed: $($ffArgs -join ' ')" }
}

function Native([string]$color, [string]$name) {
  Run (@('-lavfi', "${color}[c];${alphaGray}[a];[c][a]alphamerge,format=yuva420p[v]", '-map', '[v]') + $enc +
       @('-pix_fmt', 'yuva420p', '-metadata:s:v:0', 'alpha_mode=1', (Join-Path $Out $name)))
}

Native $colorStraight 'native_straight.webm'
Native $colorPremul 'native_premul.webm'

Run (@('-lavfi', "${colorPremul},format=yuv420p[c];${alphaLuma}[a];[c][a]hstack[v]", '-map', '[v]') + $enc +
     @('-pix_fmt', 'yuv420p', (Join-Path $Out 'packed_right_premul.webm')))
Run (@('-lavfi', "${colorStraight},format=yuv420p[c];${alphaLuma}[a];[c][a]vstack[v]", '-map', '[v]') + $enc +
     @('-pix_fmt', 'yuv420p', (Join-Path $Out 'packed_bottom_straight.webm')))

# Ground truth: straight RGBA of frames 0, 20, 40, exactly as defined above.
Run @('-lavfi', "${colorStraight}[c];${alphaGray}[a];[c][a]alphamerge,format=rgba,select='eq(n,0)+eq(n,20)+eq(n,40)'[v]",
      '-map', '[v]', '-fps_mode', 'passthrough', '-f', 'rawvideo', '-pix_fmt', 'rgba', (Join-Path $Out 'truth_rgba_3f.raw'))

Get-ChildItem $Out | Where-Object { $_.Extension -in '.webm', '.raw' } |
  Select-Object Name, @{n = 'KB'; e = { [math]::Round($_.Length / 1KB, 1) } } | Format-Table -AutoSize
