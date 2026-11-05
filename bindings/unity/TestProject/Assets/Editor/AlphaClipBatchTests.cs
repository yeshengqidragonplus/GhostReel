using System;
using System.IO;
using System.Reflection;
using System.Text;
using UnityEditor;
using UnityEngine;
using UnityEngine.Rendering;
using Debug = UnityEngine.Debug;

namespace AlphaClip.Tests
{
    /// <summary>
    /// Editor tests run from the command line (Unity -batchmode -executeMethod ...). They exit with code 0 when
    /// everything passed. See bindings/unity/README.md for the exact command lines.
    /// </summary>
    public static class BatchTests
    {
        public static void SetGamma() => SetColorSpace(ColorSpace.Gamma);
        public static void SetLinear() => SetColorSpace(ColorSpace.Linear);

        private static void SetColorSpace(ColorSpace cs)
        {
            PlayerSettings.colorSpace = cs;
            AssetDatabase.SaveAssets();
            Debug.Log("AlphaClip tests: project colour space set to " + cs);
            EditorApplication.Exit(0);
        }

        public static void SetBuiltin() => SetPipeline(false);
        public static void SetUrp() => SetPipeline(true);

        /// <summary>
        /// Switches the project between the Built-in pipeline and URP (a default pipeline asset plus every quality level).
        /// URP is reached by reflection so that this project also compiles and runs without the URP package.
        /// </summary>
        private static void SetPipeline(bool urp)
        {
            RenderPipelineAsset asset = null;
            if (urp)
            {
                const string assetPath = "Assets/Demo/UrpPipeline.asset";
                const string rendererPath = "Assets/Demo/UrpRenderer.asset";
                Directory.CreateDirectory(Path.Combine(Application.dataPath, "Demo"));
                asset = AssetDatabase.LoadAssetAtPath<RenderPipelineAsset>(assetPath);
                if (asset == null)
                {
                    Type assetType = Type.GetType("UnityEngine.Rendering.Universal.UniversalRenderPipelineAsset, Unity.RenderPipelines.Universal.Runtime");
                    Type rendererType = Type.GetType("UnityEngine.Rendering.Universal.UniversalRendererData, Unity.RenderPipelines.Universal.Runtime");
                    if (assetType == null || rendererType == null)
                    {
                        Debug.LogError("AlphaClip tests: the URP package is not installed in this project");
                        EditorApplication.Exit(2);
                        return;
                    }
                    var renderer = (ScriptableObject)ScriptableObject.CreateInstance(rendererType);
                    AssetDatabase.CreateAsset(renderer, rendererPath);
                    Type rendererBase = Type.GetType("UnityEngine.Rendering.Universal.ScriptableRendererData, Unity.RenderPipelines.Universal.Runtime");
                    MethodInfo create = assetType.GetMethod("Create", BindingFlags.Public | BindingFlags.Static, null,
                        new[] { rendererBase }, null);
                    asset = (RenderPipelineAsset)create.Invoke(null, new object[] { renderer });
                    AssetDatabase.CreateAsset(asset, assetPath);
                }
            }
            GraphicsSettings.defaultRenderPipeline = asset;
            int levels = QualitySettings.names.Length;
            for (int i = 0; i < levels; i++)
            {
                QualitySettings.SetQualityLevel(i, false);
                QualitySettings.renderPipeline = asset;
            }
            AssetDatabase.SaveAssets();
            Debug.Log("AlphaClip tests: render pipeline set to " + (urp ? "URP" : "Built-in"));
            EditorApplication.Exit(0);
        }

        /// <summary>"URP" or "Builtin": what the running editor actually renders with.</summary>
        public static string ActivePipeline()
        {
            RenderPipelineAsset current = GraphicsSettings.currentRenderPipeline;
            return current == null ? "Builtin" : (current.GetType().Name.Contains("Universal") ? "URP" : current.GetType().Name);
        }

        /// <summary>Smoke test: the native plugin loads and reports its version.</summary>
        public static void Ping()
        {
            string version = AlphaClipPlayer.NativeVersion();
            Debug.Log("AlphaClip tests: native core version " + version + ", graphics " + SystemInfo.graphicsDeviceType +
                      ", colour space " + QualitySettings.activeColorSpace + ", pipeline " + ActivePipeline() + ", Unity " + Application.unityVersion);
            EditorApplication.Exit(0);
        }

        /// <summary>
        /// Enters play mode; the PlayModeTestDriver (started because of -acPlayTest) runs the tests there and exits the editor.
        /// Launch WITHOUT -quit, or the editor quits before play mode starts.
        /// </summary>
        public static void RunPlayMode()
        {
            EditorApplication.isPlaying = true;
        }

        public static string Arg(string name)
        {
            string[] args = Environment.GetCommandLineArgs();
            for (int i = 0; i < args.Length - 1; i++)
            {
                if (args[i] == name)
                {
                    return args[i + 1];
                }
            }
            return null;
        }

        public static void Finish(StringBuilder report, int code)
        {
            string path = Arg("-acReport");
            if (path != null)
            {
                File.WriteAllText(path, report.ToString());
            }
            Debug.Log("AlphaClip tests report:\n" + report);
            EditorApplication.Exit(code);
        }
    }
}
