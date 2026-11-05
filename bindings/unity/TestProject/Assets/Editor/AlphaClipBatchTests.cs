using System;
using System.IO;
using System.Text;
using UnityEditor;
using UnityEngine;
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

        /// <summary>Smoke test: the native plugin loads and reports its version.</summary>
        public static void Ping()
        {
            string version = AlphaClipPlayer.NativeVersion();
            Debug.Log("AlphaClip tests: native core version " + version + ", graphics " + SystemInfo.graphicsDeviceType +
                      ", colour space " + QualitySettings.activeColorSpace + ", Unity " + Application.unityVersion);
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
