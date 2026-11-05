using UnityEditor;

namespace AlphaClip.Editor
{
    /// <summary>
    /// Marks the AlphaClip core DLL as an Editor + Windows-64 standalone plugin when it is imported. The DLL itself is
    /// not committed (it is built from core/ and copied in by bindings/unity/scripts/sync_plugin_win64.sh), so the
    /// import settings are applied by code instead of by a .meta file that would point at a file that may not exist.
    /// </summary>
    internal sealed class AlphaClipPluginSettings : AssetPostprocessor
    {
        private void OnPreprocessAsset()
        {
            if (!assetPath.EndsWith("/alphaclip.dll", System.StringComparison.OrdinalIgnoreCase))
            {
                return;
            }
            if (assetImporter is PluginImporter importer)
            {
                importer.SetCompatibleWithAnyPlatform(false);
                importer.SetCompatibleWithEditor(true);
                importer.SetCompatibleWithPlatform(BuildTarget.StandaloneWindows64, true);
            }
        }
    }
}
