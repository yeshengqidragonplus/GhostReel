// AlphaClip: unlit shader for putting the (premultiplied) AlphaClipPlayer.Output on a mesh or sprite in a scene.
// Blend One OneMinusSrcAlpha: the texture already holds colour * alpha, so the colour is NOT multiplied by alpha again
// (a straight-alpha shader such as "Unlit/Transparent" would darken every soft edge). _Color tints and fades:
// its alpha scales the whole premultiplied pixel. No lighting, no fog, no depth write; works in the Built-in pipeline and
// in URP (a pass without a LightMode is drawn by URP as SRPDefaultUnlit).
Shader "AlphaClip/UnlitPremultiplied"
{
    Properties
    {
        _MainTex ("Picture (premultiplied)", 2D) = "white" {}
        _Color ("Tint (alpha fades)", Color) = (1, 1, 1, 1)
    }
    SubShader
    {
        Tags { "Queue" = "Transparent" "RenderType" = "Transparent" "IgnoreProjector" = "True" }
        Cull Off
        ZWrite Off
        Blend One OneMinusSrcAlpha

        Pass
        {
            CGPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            #include "UnityCG.cginc"

            sampler2D _MainTex;
            float4 _MainTex_ST;
            float4 _Color;

            struct appdata
            {
                float4 vertex : POSITION;
                float2 uv : TEXCOORD0;
            };

            struct v2f
            {
                float4 pos : SV_POSITION;
                float2 uv : TEXCOORD0;
            };

            v2f vert(appdata v)
            {
                v2f o;
                o.pos = UnityObjectToClipPos(v.vertex);
                o.uv = TRANSFORM_TEX(v.uv, _MainTex);
                return o;
            }

            float4 frag(v2f i) : SV_Target
            {
                float4 c = tex2D(_MainTex, i.uv) * _Color;   // rgb * tint.rgb, a * tint.a
                c.rgb *= _Color.a;                           // the tint's alpha fades the premultiplied colour too
                return c;
            }
            ENDCG
        }
    }
    Fallback Off
}
