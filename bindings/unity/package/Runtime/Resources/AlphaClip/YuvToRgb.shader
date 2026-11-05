// AlphaClip: planar YUV 4:2:0 (three single-channel R8 textures) -> RGB, drawn into a RenderTexture
// with Graphics.Blit. M2 hand-written Unity variant; the design calls for one master source plus a
// generator for the engine variants (docs/m2-notes.md records this as a deviation).
//
// Inputs (set by AlphaClipPlayer):
//   _MainTex  Y plane (w x h)           _UTex / _VTex  chroma planes ((w+1)/2 x (h+1)/2), bilinear
//   _YuvRange x: luma offset (16/255 or 0)   y: luma scale (255/219 or 1)   z: chroma scale (255/224 or 1)
//   _YuvCoeff x: R from V   y: G from U   z: G from V   w: B from U      (BT.601 or BT.709)
//   _ChromaShift x: horizontal shift of the chroma lookup in chroma texels. Video 4:2:0 normally has the chroma samples
//                 co-sited with the even luma columns ("left", MPEG-2 / H.26x / VP9 default); a plain bilinear lookup
//                 assumes they sit in the middle of the 2x2 luma block, which is 0.25 chroma texel to the left.
//   _LinearOutput 1 in a Linear colour-space project: the picture is display-referred sRGB-encoded
//                 data, but the hardware encodes linear -> sRGB when it writes the render texture,
//                 so the shader must output linear values.
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
            float4 _YuvRange;
            float4 _YuvCoeff;
            float4 _ChromaShift;
            float4 _UTex_TexelSize;
            float _LinearOutput;

            float3 SrgbToLinear(float3 c)
            {
                float3 lo = c / 12.92;
                float3 hi = pow((c + 0.055) / 1.055, 2.4);
                return lerp(hi, lo, step(c, 0.04045));
            }

            float4 frag(v2f_img i) : SV_Target
            {
                float2 uv = float2(i.uv.x, 1.0 - i.uv.y);
                float y = (tex2D(_MainTex, uv).r - _YuvRange.x) * _YuvRange.y;
                float2 cuv = uv + float2(_ChromaShift.x * _UTex_TexelSize.x, 0.0);
                float u = (tex2D(_UTex, cuv).r - 128.0 / 255.0) * _YuvRange.z;
                float v = (tex2D(_VTex, cuv).r - 128.0 / 255.0) * _YuvRange.z;
                float3 rgb = float3(y + _YuvCoeff.x * v,
                                    y - _YuvCoeff.y * u - _YuvCoeff.z * v,
                                    y + _YuvCoeff.w * u);
                rgb = saturate(rgb);
                if (_LinearOutput > 0.5)
                {
                    rgb = SrgbToLinear(rgb);
                }
                return float4(rgb, 1.0);
            }
            ENDCG
        }
    }
    Fallback Off
}
