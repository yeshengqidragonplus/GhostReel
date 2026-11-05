# Generates the M1 edge-case test clips. Our own content only (ffmpeg lavfi sources).
#
#   opaque_altref.webm   VP9 yuv420p with alt-ref frames enabled: hidden frames travel inside
#                        superframes (a packet with a hidden + a shown frame)
#   opaque_odd.webm      321x181, yuv420p: odd sizes round the chroma planes up
#   alpha_odd.webm       321x181, native alpha (yuva420p)
#   alpha_scenecut.webm  640x360 native alpha; the picture hard-cuts between two different
#                        sources while the alpha (a still disc) never changes, so the colour
#                        stream gets a scene-cut keyframe that the alpha stream does not: the
#                        keyframes of the two streams are NOT aligned.
#
# Alpha clips need -auto-alt-ref 0 (libvpx cannot combine alt-ref with the alpha encoder).
param(
  [string]$Out = $PSScriptRoot,
  [string]$Ffmpeg = 'E:\Tools\ffmpeg-9.0.2-full_build\bin\ffmpeg.exe'
)
$ErrorActionPreference = 'Stop'

function Run([string[]]$ffArgs) {
  & $Ffmpeg -hide_banner -loglevel error -y @ffArgs
  if ($LASTEXITCODE -ne 0) { throw "ffmpeg failed: $($ffArgs -join ' ')" }
}
$common = @('-c:v','libvpx-vp9','-b:v','0','-deadline','good','-cpu-used','2','-threads','1','-an')

# 1. alt-ref (hidden frames in superframes), 60 frames (the player tests need <= 64). One-pass encodes did not produce any
#    alt-ref frame; a two-pass encode with lag does.
$altSrc = @('-f','lavfi','-i','testsrc2=s=640x360:r=30:d=2,format=yuv420p')
$altEnc = $common + @('-b:v','400k','-g','45','-auto-alt-ref','1','-lag-in-frames','25','-pix_fmt','yuv420p')
$passLog = Join-Path $Out 'altref_pass'
Run ($altSrc + $altEnc + @('-pass','1','-passlogfile',$passLog,'-f','null','NUL'))
Run ($altSrc + $altEnc + @('-pass','2','-passlogfile',$passLog,(Join-Path $Out 'opaque_altref.webm')))
Remove-Item "$passLog*" -ErrorAction SilentlyContinue

# 2./3. odd size 321x181, 45 frames, keyframe every 15
# (testsrc2 silently crops an odd size to even; rgbtestsrc keeps it)
$oddColor = 'rgbtestsrc=s=321x181:r=30:d=1.5,format=yuv420p'
$oddAlpha = "nullsrc=s=321x181:r=30:d=1.5,format=gray,geq=lum='clip((70-hypot(X-160-60*sin(2*T),Y-90))*255/20,0,255)'"
$oddEnc = $common + @('-crf','28','-g','15','-auto-alt-ref','0','-lag-in-frames','0')
Run (@('-f','lavfi','-i',$oddColor) + $oddEnc + @('-pix_fmt','yuv420p',(Join-Path $Out 'opaque_odd.webm')))
Run (@('-lavfi',"${oddColor}[c];${oddAlpha}[a];[c][a]alphamerge,format=yuva420p[v]",'-map','[v]') + $oddEnc +
     @('-pix_fmt','yuva420p','-metadata:s:v:0','alpha_mode=1',(Join-Path $Out 'alpha_odd.webm')))

# 4. scene cut in the picture, still alpha: 60 frames, cut at frame 30, long GOP so only scene cuts add keyframes
$cut = 'testsrc2=s=640x360:r=30:d=1,format=yuv420p[a1];rgbtestsrc=s=640x360:r=30:d=1,format=yuv420p[a2];[a1][a2]concat=n=2:v=1:a=0[c]'
$still = "nullsrc=s=640x360:r=30:d=2,format=gray,geq=lum='clip((140-hypot(X-320,Y-180))*255/40,0,255)'[a]"
Run (@('-lavfi',"${cut};${still};[c][a]alphamerge,format=yuva420p[v]",'-map','[v]') + $common +
     @('-crf','28','-g','120','-auto-alt-ref','0','-lag-in-frames','0','-pix_fmt','yuva420p',
       '-metadata:s:v:0','alpha_mode=1',(Join-Path $Out 'alpha_scenecut.webm')))

Get-ChildItem $Out | Where-Object { $_.Extension -eq '.webm' } |
  Select-Object Name, @{n='KB';e={[math]::Round($_.Length/1KB,1)}} | Format-Table -AutoSize
