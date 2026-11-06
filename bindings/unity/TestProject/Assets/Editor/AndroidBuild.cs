using System;
using System.IO;
using UnityEditor;
using UnityEditor.Build;
using UnityEditor.Build.Reporting;
using UnityEditor.SceneManagement;
using UnityEngine;

namespace AlphaClip.Tests
{
    /// <summary>
    /// M3: builds the on-device benchmark app (the scene with AlphaClipBenchmark) as a development APK for arm64. Run it in
    /// batch mode on a COPY of the project (bindings/unity/scripts/run_android_build.ps1 does that), never on the committed
    /// TestProject: the Android player settings below would end up in its ProjectSettings. Arguments: -acOut app.apk.
    /// </summary>
    public static class AndroidBuild
    {
        public const string PackageId = "com.ghostreel.m3test";
        private const string ScenePath = "Assets/Scenes/Bench.unity";
        private static readonly string[] Clips = { "opaque.webm", "alpha_native.webm" };

        public static void CreateBenchScene()
        {
            string repoRoot = Path.GetFullPath(Path.Combine(Application.dataPath, "../../../.."));
            string streaming = Path.Combine(Application.dataPath, "StreamingAssets");
            Directory.CreateDirectory(streaming);
            foreach (string clip in Clips)
            {
                string src = Path.Combine(repoRoot, "samples", "m0", clip);
                if (!File.Exists(src))
                {
                    throw new FileNotFoundException("sample clip not found", src);
                }
                File.Copy(src, Path.Combine(streaming, clip), true);
            }
            Directory.CreateDirectory(Path.Combine(Application.dataPath, "Scenes"));
            Directory.CreateDirectory(Path.Combine(Application.dataPath, "Demo"));

            const string materialPath = "Assets/Demo/DemoUnlit.mat";
            var material = AssetDatabase.LoadAssetAtPath<Material>(materialPath);
            if (material == null)
            {
                material = new Material(Shader.Find("Unlit/Texture"));
                AssetDatabase.CreateAsset(material, materialPath);
            }

            var scene = EditorSceneManager.NewScene(NewSceneSetup.EmptyScene, NewSceneMode.Single);
            var cameraGo = new GameObject("Main Camera") { tag = "MainCamera" };
            var cam = cameraGo.AddComponent<Camera>();
            cam.orthographic = true;
            cam.orthographicSize = 0.5f;
            cam.clearFlags = CameraClearFlags.SolidColor;
            cam.backgroundColor = new Color(0.1f, 0.1f, 0.12f);
            cameraGo.transform.position = new Vector3(0, 0, -2);

            var quad = GameObject.CreatePrimitive(PrimitiveType.Quad);
            quad.name = "Video Quad";
            UnityEngine.Object.DestroyImmediate(quad.GetComponent<Collider>());
            quad.transform.localScale = new Vector3(16f / 9f, 1f, 1f);
            quad.GetComponent<MeshRenderer>().sharedMaterial = material;

            var bench = new GameObject("Benchmark").AddComponent<AlphaClipBenchmark>();
            bench.Quad = quad.GetComponent<MeshRenderer>();

            EditorSceneManager.SaveScene(scene, ScenePath);
            AssetDatabase.SaveAssets();
        }

        /// <summary>Batch-mode entry point; exit code 0 when the APK was built.</summary>
        public static void Build()
        {
            int code = 1;
            try
            {
                code = BuildApk();
            }
            catch (Exception e)
            {
                Debug.LogError("AlphaClip Android build failed: " + e);
            }
            EditorApplication.Exit(code);
        }

        private static int BuildApk()
        {
            string output = Arg("-acOut");
            if (string.IsNullOrEmpty(output))
            {
                throw new ArgumentException("-acOut <app.apk> is required");
            }
            if (EditorUserBuildSettings.activeBuildTarget != BuildTarget.Android)
            {
                throw new InvalidOperationException("the active build target is " + EditorUserBuildSettings.activeBuildTarget + "; start the editor with -buildTarget Android");
            }

            CreateBenchScene();

            // arm64 needs IL2CPP (Mono has no arm64 on Android). API 24 matches the NDK smoke value of the core build; the
            // minimum API level is still an open decision (design doc 13.2).
            PlayerSettings.SetScriptingBackend(NamedBuildTarget.Android, ScriptingImplementation.IL2CPP);
            PlayerSettings.Android.targetArchitectures = AndroidArchitecture.ARM64;
            PlayerSettings.Android.minSdkVersion = AndroidSdkVersions.AndroidApiLevel24;
            PlayerSettings.SetApplicationIdentifier(NamedBuildTarget.Android, PackageId);
            PlayerSettings.productName = "GhostReel M3 test";

            Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(output)) ?? ".");
            var options = new BuildPlayerOptions
            {
                scenes = new[] { ScenePath },
                locationPathName = Path.GetFullPath(output),
                target = BuildTarget.Android,
                options = BuildOptions.Development,   // debuggable: lets `adb shell run-as` reach the app's files and threads
            };
            BuildReport report = BuildPipeline.BuildPlayer(options);
            BuildSummary s = report.summary;
            Debug.Log("AlphaClip Android build: " + s.result + ", " + s.totalErrors + " errors, " + s.totalWarnings + " warnings, " +
                      (s.totalSize / 1048576.0).ToString("F1") + " MB, " + s.totalTime.TotalSeconds.ToString("F0") + " s -> " + s.outputPath);
            return s.result == BuildResult.Succeeded ? 0 : 1;
        }

        private static string Arg(string name)
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
    }
}
