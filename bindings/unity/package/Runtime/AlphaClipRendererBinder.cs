using UnityEngine;

namespace AlphaClip
{
    /// <summary>
    /// Shows the picture of the <see cref="AlphaClipPlayer"/> on a Renderer: it sets the player's Output RenderTexture on the
    /// renderer's material through a MaterialPropertyBlock, so the material asset itself is never modified. Any shader with a
    /// texture property works. By default the property is found automatically: _BaseMap (URP shaders) if the material has
    /// it, otherwise _MainTex (Built-in "Unlit/Texture", "Standard", ...). Name one explicitly for custom shaders.
    /// For other targets (UI RawImage, your own code) read <see cref="AlphaClipPlayer.Output"/> directly.
    /// </summary>
    [DisallowMultipleComponent]
    [RequireComponent(typeof(AlphaClipPlayer))]
    public sealed class AlphaClipRendererBinder : MonoBehaviour
    {
        [SerializeField] private Renderer target;
        [Tooltip("Leave empty to use _BaseMap when the material has it, else _MainTex.")]
        [SerializeField] private string textureProperty = "";

        private AlphaClipPlayer m_player;
        private MaterialPropertyBlock m_block;
        private Texture m_bound;

        public Renderer Target { get => target; set { target = value; m_bound = null; } }
        public string TextureProperty { get => textureProperty; set { textureProperty = value; m_bound = null; } }

        private void Awake()
        {
            m_player = GetComponent<AlphaClipPlayer>();
        }

        private void LateUpdate()
        {
            Texture output = m_player.Output;
            if (target == null || output == m_bound)
            {
                return;
            }
            m_block ??= new MaterialPropertyBlock();
            target.GetPropertyBlock(m_block);
            m_block.SetTexture(ResolveProperty(), output);   // a null Output (clip closed) clears the texture
            target.SetPropertyBlock(m_block);
            m_bound = output;
        }

        private string ResolveProperty()
        {
            if (!string.IsNullOrEmpty(textureProperty))
            {
                return textureProperty;
            }
            Material material = target.sharedMaterial;
            return material != null && material.HasProperty("_BaseMap") ? "_BaseMap" : "_MainTex";
        }

        private void OnDestroy()
        {
            if (target != null && m_block != null)
            {
                target.GetPropertyBlock(m_block);
                m_block.SetTexture(ResolveProperty(), Texture2D.blackTexture);
                target.SetPropertyBlock(m_block);
            }
        }
    }
}
