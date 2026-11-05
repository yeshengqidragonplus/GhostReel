using System.IO;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;

namespace AlphaClip.Tests
{
    /// <summary>
    /// Menu "AlphaClip > Create Demo Scene": copies the sample clip into StreamingAssets and builds a scene with a quad that
    /// shows it (AlphaClipPlayer + AlphaClipRendererBinder on an object, an orthographic camera). Press Play to watch it.
    /// </summary>
    public static class DemoScene
    {
        private const string ScenePath = "Assets/Scenes/Demo.unity";
        private const string MaterialPath = "Assets/Demo/DemoUnlit.mat";
        private const string ClipName = "opaque.webm";

        [MenuItem("AlphaClip/Create Demo Scene")]
        public static void Create()
        {
            string repoRoot = Path.GetFullPath(Path.Combine(Application.dataPath, "../../../.."));
            string sample = Path.Combine(repoRoot, "samples", "m0", ClipName);
            if (!File.Exists(sample))
            {
                Debug.LogError("AlphaClip demo: sample clip not found: " + sample);
                return;
            }
            Directory.CreateDirectory(Path.Combine(Application.dataPath, "StreamingAssets"));
            File.Copy(sample, Path.Combine(Application.dataPath, "StreamingAssets", ClipName), true);
            Directory.CreateDirectory(Path.Combine(Application.dataPath, "Scenes"));
            Directory.CreateDirectory(Path.Combine(Application.dataPath, "Demo"));

            var material = AssetDatabase.LoadAssetAtPath<Material>(MaterialPath);
            if (material == null)
            {
                material = new Material(Shader.Find("Unlit/Texture"));
                AssetDatabase.CreateAsset(material, MaterialPath);
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
            Object.DestroyImmediate(quad.GetComponent<Collider>());
            quad.transform.localScale = new Vector3(16f / 9f, 1f, 1f);   // the sample clip is 640x360
            quad.GetComponent<MeshRenderer>().sharedMaterial = material;

            var player = new GameObject("AlphaClip Player");
            var p = player.AddComponent<AlphaClipPlayer>();
            p.StreamingAssetsPath = ClipName;
            p.Loop = true;
            p.Matrix = ColorMatrix.BT601;   // the sample clip was made by ffmpeg from RGB: BT.601, limited range
            var binder = player.AddComponent<AlphaClipRendererBinder>();
            binder.Target = quad.GetComponent<MeshRenderer>();

            EditorSceneManager.SaveScene(scene, ScenePath);
            AssetDatabase.SaveAssets();
            Debug.Log("AlphaClip demo scene saved to " + ScenePath + ". Press Play.");
        }

        /// <summary>Batch-mode entry point.</summary>
        public static void CreateBatch()
        {
            Create();
            EditorApplication.Exit(File.Exists(Path.Combine(Application.dataPath, "Scenes", "Demo.unity")) ? 0 : 1);
        }

        /// <summary>Batch-mode entry point: opens the demo scene and enters play mode (the test driver checks it, see -acDemoCheck).</summary>
        public static void PlayDemoBatch()
        {
            EditorSceneManager.OpenScene(ScenePath);
            EditorApplication.isPlaying = true;
        }
    }
}
