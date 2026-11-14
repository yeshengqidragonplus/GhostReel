#version 300 es
// AlphaClip master shader (GLSL ES 3.0): planar YUV 4:2:0 (three single-channel R8 textures) plus optional alpha
// -> PREMULTIPLIED RGBA at the picture's size.
//
// This is the "one source" of design section 8.3. The WebGL preview (tools/preview) uses this file as it is. The Unity
// variant (bindings/unity/package/Runtime/Resources/AlphaClip/YuvToRgb.shader) is still written by hand: until the
// generator exists the two must be kept equal line for line, and core/tests/test_preview.py compares what both draw.
//
// Textures (all R8, never sRGB, clamp to edge, no mipmaps; rows top first):
//   uTexY  luma, coded w x h, NEAREST          uTexU / uTexV  chroma, ((w+1)/2) x ((h+1)/2), LINEAR
//   uTexA  alpha plane (native_alpha only), coded w x h, full range 0..255, NEAREST
// Uniforms:
//   uYuvRange      x: luma offset (16/255 or 0)   y: luma scale (255/219 or 1)   z: chroma scale (255/224 or 1)
//   uYuvCoeff      x: R from V   y: G from U   z: G from V   w: B from U        (BT.601 or BT.709)
//   uChromaShift   horizontal shift of the chroma lookup in chroma texels. Video 4:2:0 normally has the chroma samples
//                  co-sited with the even luma columns ("left", MPEG-2 / H.26x / VP9 default); a plain bilinear lookup
//                  assumes they sit in the middle of the 2x2 luma block, which is 0.25 chroma texel to the left.
//   uChromaTexel   1 / size of the chroma planes
//   uContentRect   xy: size of the picture relative to the coded frame (1,1 unless the clip is packed)
//                  zw: where the alpha image starts relative to the coded frame (packed modes)
//   uChromaClamp   min u, min v, max u, max v of the chroma lookup: keeps the bilinear filter inside the picture's own
//                  chroma samples, so that in the packed modes the grey alpha image never bleeds into the colour
//   uAlphaMode     0 opaque, 1 packed_right, 2 packed_bottom (alpha = luma of the second image, same range as the
//                  colour), 3 native_alpha (alpha = uTexA)
//   uPremultiplied 1 when the colour in the file is already multiplied by alpha, 0 for straight colour
//   uLinearOutput  1 when the render target is sRGB-encoded by the hardware (a Linear colour-space project): the
//                  picture is display-referred sRGB data, so the shader must output linear values.
// Input:  vUv, picture coordinates: (0,0) is the top-left corner of the picture, (1,1) the bottom-right one.
// Output: PREMULTIPLIED RGBA (blend One OneMinusSrcAlpha). With uLinearOutput the premultiplication is done on the
// linear value: straight colour -> linear * alpha; already-premultiplied colour -> divide alpha out, convert,
// multiply it back (so the same picture stored either way gives the same result, and blending in a linear
// framebuffer is correct). Otherwise straight colour is multiplied by alpha and premultiplied colour is passed
// through, limited to <= alpha (a colour above its own alpha would add light where the picture is transparent).
precision highp float;
precision highp sampler2D;

uniform sampler2D uTexY;
uniform sampler2D uTexU;
uniform sampler2D uTexV;
uniform sampler2D uTexA;
uniform vec4 uYuvRange;
uniform vec4 uYuvCoeff;
uniform float uChromaShift;
uniform vec2 uChromaTexel;
uniform vec4 uContentRect;
uniform vec4 uChromaClamp;
uniform float uAlphaMode;
uniform float uPremultiplied;
uniform float uLinearOutput;

in vec2 vUv;
out vec4 oColor;

vec3 SrgbToLinear(vec3 c) {
  vec3 lo = c / 12.92;
  vec3 hi = pow((c + 0.055) / 1.055, vec3(2.4));
  return mix(hi, lo, step(c, vec3(0.04045)));
}

void main() {
  vec2 cuv0 = vUv * uContentRect.xy;  // the same point in the coded frame
  float y = (texture(uTexY, cuv0).r - uYuvRange.x) * uYuvRange.y;
  vec2 cuv = clamp(cuv0 + vec2(uChromaShift * uChromaTexel.x, 0.0), uChromaClamp.xy, uChromaClamp.zw);
  float u = (texture(uTexU, cuv).r - 128.0 / 255.0) * uYuvRange.z;
  float v = (texture(uTexV, cuv).r - 128.0 / 255.0) * uYuvRange.z;
  vec3 rgb = vec3(y + uYuvCoeff.x * v,
                  y - uYuvCoeff.y * u - uYuvCoeff.z * v,
                  y + uYuvCoeff.w * u);
  rgb = clamp(rgb, 0.0, 1.0);

  float a = 1.0;
  if (uAlphaMode > 2.5) {
    a = textureLod(uTexA, cuv0, 0.0).r;
  } else if (uAlphaMode > 0.5) {
    float luma = textureLod(uTexY, cuv0 + uContentRect.zw, 0.0).r;
    a = clamp((luma - uYuvRange.x) * uYuvRange.y, 0.0, 1.0);
  }

  if (uAlphaMode > 0.5) {
    if (uLinearOutput > 0.5) {
      vec3 straight = uPremultiplied > 0.5 ? clamp(rgb / max(a, 1e-4), 0.0, 1.0) : rgb;
      rgb = SrgbToLinear(straight) * a;
    } else {
      rgb = uPremultiplied > 0.5 ? min(rgb, vec3(a)) : rgb * a;
    }
  } else if (uLinearOutput > 0.5) {
    rgb = SrgbToLinear(rgb);
  }
  oColor = vec4(rgb, a);
}
