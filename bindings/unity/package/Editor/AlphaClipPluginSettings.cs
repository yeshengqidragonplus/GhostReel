using System;
using UnityEditor;

namespace AlphaClip.Editor
{
    /// <summary>
    /// Applies the import settings of the AlphaClip core when it is imported: alphaclip.dll becomes an Editor +
    /// Windows-64 standalone plugin, libalphaclip.so an Android arm64 plugin. The native libraries are not committed
    /// (they are built from core/ and copied in by bindings/unity/scripts/sync_plugin_*.sh), so the settings are applied
    /// by code instead of by .meta files that would point at files that may not exist.
    /// </summary>
    internal sealed class AlphaClipPluginSettings : AssetPostprocessor
    {
        private void OnPreprocessAsset()
        {
            if (!(assetImporter is PluginImporter importer))
            {
                return;
            }
            if (assetPath.EndsWith("/alphaclip.dll", StringComparison.OrdinalIgnoreCase))
            {
                importer.SetCompatibleWithAnyPlatform(false);
                importer.SetCompatibleWithEditor(true);
                importer.SetCompatibleWithPlatform(BuildTarget.StandaloneWindows64, true);
            }
            else if (assetPath.EndsWith("/libalphaclip.so", StringComparison.Ordinal))
            {
                // Android arm64-v8a only. The "CPU" platform-data key is what the Inspector's CPU drop-down writes
                // (the docs do not list it: the APK contents are checked in M3 to prove it took effect).
                importer.SetCompatibleWithAnyPlatform(false);
                importer.SetCompatibleWithEditor(false);
                importer.SetCompatibleWithPlatform(BuildTarget.Android, true);
                importer.SetPlatformData(BuildTarget.Android, "CPU", "ARM64");
            }
        }
    }
}
