// AlphaClip: planar YUV 4:2:0 (three single-channel R8 textures) plus optional alpha -> premultiplied RGBA, drawn into a
// RenderTexture with Graphics.Blit. Hand-written Unity variant; the design calls for one master source plus a generator for
// the engine variants (docs/m2-notes.md records this as a deviation).
//
// Inputs (set by AlphaClipPlayer):
//   _MainTex  Y plane (coded w x h)     _UTex / _VTex  chroma planes ((w+1)/2 x (h+1)/2), bilinear
//   _ATex     alpha plane (native_alpha only, coded w x h, full range 0..255, point sampled)
//   _YuvRange x: luma offset (16/255 or 0)   y: luma scale (255/219 or 1)   z: chroma scale (255/224 or 1)
//   _YuvCoeff x: R from V   y: G from U   z: G from V   w: B from U      (BT.601 or BT.709)
//   _ChromaShift x: horizontal shift of the chroma lookup in chroma texels. Video 4:2:0 normally has the chroma samples
//                 co-sited with the even luma columns ("left", MPEG-2 / H.26x / VP9 default); a plain bilinear lookup
//                 assumes they sit in the middle of the 2x2 luma block, which is 0.25 chroma texel to the left.
//   _ContentRect  xy: size of the picture relative to the coded frame (1,1 unless the clip is packed)
//                 zw: where the alpha image starts relative to the coded frame (packed modes)
//   _ChromaClamp  min u, min v, max u, max v of the chroma lookup: keeps the bilinear filter inside the picture's own
//                 chroma samples, so that in the packed modes the grey alpha image never bleeds into the colour
//   _AlphaMode    0 opaque, 1 packed_right, 2 packed_bottom (alpha = luma of the second image, same range as the colour),
//                 3 native_alpha (alpha = _ATex)
//   _Premultiplied 1 when the colour in the file is already multiplied by alpha, 0 for straight colour
//   _LinearOutput 1 in a Linear colour-space project: the picture is display-referred sRGB-encoded
//                 data, but the hardware encodes linear -> sRGB when it writes the render texture,
//                 so the shader must output linear values.
//
// Output: PREMULTIPLIED RGBA (blend One OneMinusSrcAlpha). In a Linear project the premultiplication is done on the linear
// value: straight colour -> linear * alpha; already-premultiplied colour -> divide alpha out, convert, multiply it back
// (so the same picture stored either way gives the same result, and blending in the linear framebuffer is correct).
// In a Gamma project straight colour is multiplied by alpha and premultiplied colour is passed through, limited to
// <= alpha (a colour above its own alpha would add light where the picture is transparent).
//
// The planes are uploaded top row first, which Unity treats as the bottom row of a texture, so the
// shader flips V to produce an upright image.
Shader "Hidden/AlphaClip/YuvToRgb"
{
    Properties
    {
        _MainTex ("Y", 2D) = "black" {}
        _UTex ("U", 2D) = "gray" {}
        _VTex ("V", 2D) = "gray" {}
        _ATex ("A", 2D) = "white" {}
    }
    SubShader
    {
        Cull Off
        ZWrite Off
        ZTest Always

        Pass
        {
            CGPROGRAM
            #pragma vertex vert_img
            #pragma fragment frag
            #include "UnityCG.cginc"

            sampler2D _MainTex;
            sampler2D _UTex;
            sampler2D _VTex;
            sampler2D _ATex;
            float4 _YuvRange;
            float4 _YuvCoeff;
            float4 _ChromaShift;
            float4 _ContentRect;
            float4 _ChromaClamp;
            float4 _UTex_TexelSize;
            float _AlphaMode;
            float _Premultiplied;
            float _LinearOutput;

            float3 SrgbToLinear(float3 c)
            {
                float3 lo = c / 12.92;
                float3 hi = pow((c + 0.055) / 1.055, 2.4);
                return lerp(hi, lo, step(c, 0.04045));
            }

            float4 frag(v2f_img i) : SV_Target
            {
                float2 uv = float2(i.uv.x, 1.0 - i.uv.y);          // picture coordinates, v counted from the top
                float2 cuv0 = uv * _ContentRect.xy;                // the same point in the coded frame
                float y = (tex2D(_MainTex, cuv0).r - _YuvRange.x) * _YuvRange.y;
                float2 cuv = clamp(cuv0 + float2(_ChromaShift.x * _UTex_TexelSize.x, 0.0), _ChromaClamp.xy, _ChromaClamp.zw);
                float u = (tex2D(_UTex, cuv).r - 128.0 / 255.0) * _YuvRange.z;
                float v = (tex2D(_VTex, cuv).r - 128.0 / 255.0) * _YuvRange.z;
                float3 rgb = float3(y + _YuvCoeff.x * v,
                                    y - _YuvCoeff.y * u - _YuvCoeff.z * v,
                                    y + _YuvCoeff.w * u);
                rgb = saturate(rgb);

                float a = 1.0;
                if (_AlphaMode > 2.5)
                {
                    a = tex2Dlod(_ATex, float4(cuv0, 0, 0)).r;
                }
                else if (_AlphaMode > 0.5)
                {
                    float luma = tex2Dlod(_MainTex, float4(cuv0 + _ContentRect.zw, 0, 0)).r;
                    a = saturate((luma - _YuvRange.x) * _YuvRange.y);
                }

                if (_AlphaMode > 0.5)
                {
                    if (_LinearOutput > 0.5)
                    {
                        float3 straight = _Premultiplied > 0.5 ? saturate(rgb / max(a, 1e-4)) : rgb;
                        rgb = SrgbToLinear(straight) * a;
                    }
                    else
                    {
                        rgb = _Premultiplied > 0.5 ? min(rgb, a) : rgb * a;
                    }
                }
                else if (_LinearOutput > 0.5)
                {
                    rgb = SrgbToLinear(rgb);
                }
                return float4(rgb, a);
            }
            ENDCG
        }
    }
    Fallback Off
}
