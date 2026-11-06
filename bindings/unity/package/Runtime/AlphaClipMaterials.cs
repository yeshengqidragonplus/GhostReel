using System;
using UnityEngine;

namespace AlphaClip
{
    /// <summary>
    /// Materials for showing a transparent clip. <see cref="AlphaClipPlayer.Output"/> holds PREMULTIPLIED colour (colour already
    /// multiplied by alpha), so it must be drawn with Blend One OneMinusSrcAlpha; a straight-alpha shader (Unity's
    /// "Unlit/Transparent", "UI/Default", sprite shaders) multiplies by alpha a second time and leaves a dark rim around every
    /// soft edge. The two shaders live in Resources/AlphaClip, so they are always part of a player build.
    /// </summary>
    public static class AlphaClipMaterials
    {
        /// <summary>For a mesh, quad or sprite renderer in a scene (Built-in pipeline and URP).</summary>
        public const string UnlitShaderName = "AlphaClip/UnlitPremultiplied";

        /// <summary>For a UGUI RawImage (or any Graphic): keeps Mask / RectMask2D working.</summary>
        public const string UIShaderName = "AlphaClip/UIPremultiplied";

        /// <summary>A new material with the scene shader. The caller owns it (Destroy it when done). Set its texture to <see cref="AlphaClipPlayer.Output"/>, or add an <see cref="AlphaClipRendererBinder"/>.</summary>
        public static Material CreateUnlit() => Create("AlphaClip/Unlit", "AlphaClip Unlit Premultiplied");

        /// <summary>A new material with the UI shader. The caller owns it. Assign it to <c>RawImage.material</c> and set <c>RawImage.texture</c> to <see cref="AlphaClipPlayer.Output"/>.</summary>
        public static Material CreateUI() => Create("AlphaClip/UI", "AlphaClip UI Premultiplied");

        private static Material Create(string resource, string name)
        {
            Shader shader = Resources.Load<Shader>(resource);
            if (shader == null)
            {
                throw new InvalidOperationException("AlphaClip: the shader Resources/" + resource + " was not found");
            }
            return new Material(shader) { name = name };
        }
    }
}
