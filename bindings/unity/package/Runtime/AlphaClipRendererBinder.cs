using UnityEngine;

namespace AlphaClip
{
    /// <summary>
    /// Shows the picture of the <see cref="AlphaClipPlayer"/> on a Renderer: it sets the player's Output RenderTexture on the
    /// renderer's material through a MaterialPropertyBlock, so the material asset itself is never modified. Any shader with a
    /// texture property works (Built-in "Unlit/Texture" and "Standard" use _MainTex; URP/Lit uses _BaseMap).
    /// For other targets (UI RawImage, your own code) read <see cref="AlphaClipPlayer.Output"/> directly.
    /// </summary>
    [DisallowMultipleComponent]
    [RequireComponent(typeof(AlphaClipPlayer))]
    public sealed class AlphaClipRendererBinder : MonoBehaviour
    {
        [SerializeField] private Renderer target;
        [SerializeField] private string textureProperty = "_MainTex";

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
            m_block.SetTexture(textureProperty, output);   // a null Output (clip closed) clears the texture
            target.SetPropertyBlock(m_block);
            m_bound = output;
        }

        private void OnDestroy()
        {
            if (target != null && m_block != null)
            {
                target.GetPropertyBlock(m_block);
                m_block.SetTexture(textureProperty, Texture2D.blackTexture);
                target.SetPropertyBlock(m_block);
            }
        }
    }
}
