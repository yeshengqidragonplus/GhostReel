using System;
using System.Collections;
using System.Collections.Generic;
using System.IO;
using System.Text;
using UnityEngine;
using UnityEngine.Profiling;
using UnityEngine.Rendering;
#if UNITY_EDITOR
using UnityEditor;
#endif

namespace AlphaClip.Tests
{
    /// <summary>
    /// Play-mode tests of the real component lifecycle: OnEnable opens and starts the clip, Update presents frames on the
    /// real clock, the loop keeps the clock continuous, OnDestroy frees everything. Runs only when the editor is started
    /// with -acPlayTest (see BatchTests.RunPlayMode). Arguments: -acClip clip.webm [-acReport out.txt].
    /// </summary>
    public sealed class PlayModeTestDriver : MonoBehaviour
    {
        private readonly StringBuilder m_report = new StringBuilder();
        private int m_checks, m_failures;
        private string m_clip;

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

        [RuntimeInitializeOnLoadMethod(RuntimeInitializeLoadType.AfterSceneLoad)]
        private static void Boot()
        {
            string[] args = Environment.GetCommandLineArgs();
            if (Array.IndexOf(args, "-acPlayTest") < 0 && Array.IndexOf(args, "-acDemoCheck") < 0)
            {
                return;
            }
            DontDestroyOnLoad(new GameObject("alphaclip-playmode-tests", typeof(PlayModeTestDriver)));
        }

        private void Check(bool ok, string label, string detail = "")
        {
            m_checks++;
            if (!ok)
            {
                m_failures++;
            }
            m_report.AppendLine((ok ? "PASS  " : "FAIL  ") + label + (!ok && detail.Length > 0 ? "  -- " + detail : ""));
        }

        private IEnumerator Start()
        {
            m_clip = Arg("-acClip");
            int code = 0;
            var watchdog = StartCoroutine(Watchdog(150f));
            IEnumerator run = Array.IndexOf(Environment.GetCommandLineArgs(), "-acDemoCheck") >= 0 ? RunDemo() : RunAll();
            while (true)
            {
                try
                {
                    if (!run.MoveNext())
                    {
                        break;
                    }
                }
                catch (Exception e)
                {
                    m_report.AppendLine("EXCEPTION: " + e);
                    code = 2;
                    break;
                }
                yield return run.Current;
            }
            StopCoroutine(watchdog);
            if (code == 0 && m_failures > 0)
            {
                code = 1;
            }
            Finish(code);
        }

        private IEnumerator Watchdog(float seconds)
        {
            yield return new WaitForSecondsRealtime(seconds);
            m_report.AppendLine("FAIL  watchdog: the play-mode tests did not finish in " + seconds + " s");
            Finish(3);
        }

        private void Finish(int code)
        {
            m_report.AppendLine((m_checks - m_failures) + "/" + m_checks + " checks passed");
            string path = Arg("-acReport");
            if (path != null)
            {
                File.WriteAllText(path, m_report.ToString());
            }
            Debug.Log("AlphaClip play-mode report:\n" + m_report);
#if UNITY_EDITOR
            EditorApplication.Exit(code);
#else
            Application.Quit(code);
#endif
        }

        // ---------------------------------------------------------------------------------------------------------

        private AlphaClipPlayer NewPlayer(bool loop, int depth = 4)
        {
            // Inactive while configuring, so that OnEnable (open + play) sees the settings.
            var go = new GameObject("alphaclip-player");
            go.SetActive(false);
            var p = go.AddComponent<AlphaClipPlayer>();
            p.StreamingAssetsPath = m_clip;   // an absolute path: Path.Combine keeps it as is
            p.Loop = loop;
            p.QueueDepth = depth;
            p.PlayOnEnable = true;
            go.SetActive(true);
            return p;
        }

        private static string ActivePipeline()
        {
            RenderPipelineAsset current = GraphicsSettings.currentRenderPipeline;
            return current == null ? "Builtin" : (current.GetType().Name.Contains("Universal") ? "URP" : current.GetType().Name);
        }

        private void CheckPipeline()
        {
            string expect = Arg("-acExpectPipeline");
            Check(expect == null || ActivePipeline() == expect, "render pipeline is " + (expect ?? "unchecked") + " (is " + ActivePipeline() + ")");
        }

        private IEnumerator RunAll()
        {
            Check(Application.isPlaying, "running in play mode");
            CheckPipeline();
            int opened0 = AlphaClipPlayer.NativeOpenCount, closed0 = AlphaClipPlayer.NativeCloseCount;

            // 1. OnEnable opens the clip and starts playing; Update shows frames on the real clock
            double unscaled0 = Time.unscaledTimeAsDouble;
            AlphaClipPlayer p = NewPlayer(false);
            Check(p.IsOpen && p.IsPlaying, "OnEnable opened the clip and started playback");
            var seen = new List<long>();
            var lags = new List<long>();   // how far the picture on screen is behind the player's clock, once per Unity frame
            float t0 = Time.realtimeSinceStartup;
            while (!p.Ended && Time.realtimeSinceStartup - t0 < 8f)
            {
                if (p.LastPtsUs >= 0 && (seen.Count == 0 || seen[seen.Count - 1] != p.LastPtsUs))
                {
                    seen.Add(p.LastPtsUs);
                }
                if (p.LastPtsUs >= 0)
                {
                    lags.Add(p.CurrentClockUs - p.LastPtsUs);
                }
                yield return null;
            }
            double elapsed = Time.realtimeSinceStartup - t0;
            double elapsedUnscaled = Time.unscaledTimeAsDouble - unscaled0;
            Check(p.Ended, "a non-looping clip ends", "elapsed " + elapsed.ToString("F2") + " s");
            m_report.AppendLine("INFO  clocks over the run: Time.realtimeSinceStartup " + elapsed.ToString("F3") + " s, Time.unscaledTimeAsDouble " +
                                elapsedUnscaled.ToString("F3") + " s, clip " + p.Info.DurationSeconds.ToString("F3") + " s");
            Check(seen.Count > 10 && IsStrictlyIncreasing(seen), "frames arrive in order (" + seen.Count + " distinct frames seen by Update)");
            // Pacing: the picture on screen must keep up with the player's clock. Judged by the lag per frame (median and
            // 95th percentile), which a one-off start-up hitch (shader compilation right after a project setting changed,
            // seen once at 1.2 s) does not disturb, unlike the total elapsed time.
            lags.Sort();
            long medianLag = lags.Count > 0 ? lags[lags.Count / 2] : long.MaxValue;
            long p95Lag = lags.Count > 0 ? lags[Math.Min(lags.Count - 1, lags.Count * 95 / 100)] : long.MaxValue;
            Check(lags.Count > 20 && medianLag < 100_000 && p95Lag < 300_000,
                "pacing: the picture lags the clock by " + medianLag / 1000 + " ms (median), " + p95Lag / 1000 + " ms (95th percentile) over " + lags.Count + " frames");
            Check(p.LastPtsUs == 1967000 || p.LastPtsUs > 1_900_000, "the last picture on screen is the last frame (pts " + p.LastPtsUs + ")");
            Check(p.Output != null && p.Output.IsCreated(), "Output stays a valid RenderTexture after the end");
            Destroy(p.gameObject);
            yield return null;

            // 2. looping: the clock stays continuous across the loop point
            p = NewPlayer(true);
            seen.Clear();
            t0 = Time.realtimeSinceStartup;
            long target = p.Info.LoopPeriodUs + 600_000;   // a pass and a bit
            while (p.LastPtsUs < target && Time.realtimeSinceStartup - t0 < 8f)
            {
                if (p.LastPtsUs >= 0 && (seen.Count == 0 || seen[seen.Count - 1] != p.LastPtsUs))
                {
                    seen.Add(p.LastPtsUs);
                }
                yield return null;
            }
            Check(p.LastPtsUs >= target, "looping: playback ran past the end of the clip (pts " + p.LastPtsUs + " >= " + target + ")");
            Check(!p.Ended, "looping: the stream never ends");
            Check(IsStrictlyIncreasing(seen), "looping: pts keep increasing across the loop point");
            long maxGap = 0;
            for (int i = 1; i < seen.Count; i++)
            {
                maxGap = Math.Max(maxGap, seen[i] - seen[i - 1]);
            }
            Check(maxGap < 250_000, "looping: no gap in the pictures larger than 250 ms (largest " + maxGap / 1000 + " ms)");

            // 3. Pause / Seek / Play through the API
            p.Pause();
            long pausedAt = p.LastPtsUs;
            yield return new WaitForSeconds(0.3f);
            Check(p.LastPtsUs == pausedAt, "Pause: the picture does not change while paused");
            p.Seek(0.5);
            p.Play();
            t0 = Time.realtimeSinceStartup;
            while ((p.LastPtsUs < 0 || p.LastPtsUs > 800_000) && Time.realtimeSinceStartup - t0 < 5f)
            {
                yield return null;
            }
            Check(p.LastPtsUs >= 400_000 && p.LastPtsUs <= 800_000, "Seek(0.5) then Play shows pictures near 0.5 s (pts " + p.LastPtsUs + ")");
            AlphaClipStats stats = p.GetStats();
            Check(stats.FramesDecoded > 0 && stats.DecodeTimeUs > 0, "stats are available (" + stats.FramesDecoded + " decoded, " + stats.FramesDropped + " dropped)");
            Destroy(p.gameObject);
            yield return null;

            // 4. destroying releases everything: native player, textures, render texture, material
            yield return null;
            int tex0 = Resources.FindObjectsOfTypeAll<Texture2D>().Length, rt0 = Resources.FindObjectsOfTypeAll<RenderTexture>().Length;
            int mat0 = Resources.FindObjectsOfTypeAll<Material>().Length;
            long unity0 = Profiler.GetTotalAllocatedMemoryLong(), gfx0 = Profiler.GetAllocatedMemoryForGraphicsDriver();
            const int cycles = 150;
            for (int i = 0; i < cycles; i++)
            {
                AlphaClipPlayer q = NewPlayer(i % 2 == 0);
                float s = Time.realtimeSinceStartup;
                while (q.FramesPresented < 2 && Time.realtimeSinceStartup - s < 3f)
                {
                    yield return null;
                }
                Destroy(q.gameObject);   // OnDestroy -> Close, at the end of the frame
                yield return null;
            }
            yield return null;
            GC.Collect();
            yield return Resources.UnloadUnusedAssets();
            int tex1 = Resources.FindObjectsOfTypeAll<Texture2D>().Length, rt1 = Resources.FindObjectsOfTypeAll<RenderTexture>().Length;
            int mat1 = Resources.FindObjectsOfTypeAll<Material>().Length;
            Check(tex1 <= tex0 && rt1 <= rt0 && mat1 <= mat0,   // no growth (an object destroyed just before the baseline may be collected later)
                "destroy: Texture2D / RenderTexture / Material counts did not grow after " + cycles + " create/destroy cycles (" +
                tex0 + "/" + rt0 + "/" + mat0 + " -> " + tex1 + "/" + rt1 + "/" + mat1 + ")");
            double unityMb = (Profiler.GetTotalAllocatedMemoryLong() - unity0) / 1048576.0;
            double gfxMb = (Profiler.GetAllocatedMemoryForGraphicsDriver() - gfx0) / 1048576.0;
            Check(unityMb < 8 && gfxMb < 8, "destroy: Unity allocator grew " + unityMb.ToString("F1") + " MB, graphics driver memory " + gfxMb.ToString("F1") + " MB");
            int opened = AlphaClipPlayer.NativeOpenCount - opened0, closed = AlphaClipPlayer.NativeCloseCount - closed0;
            // opened: the non-looping player, the looping one and the `cycles` of the loop above
            Check(opened == cycles + 2 && closed == opened, "destroy: every native player that was opened was closed (" + opened + " opened, " + closed + " closed)");
        }

        /// <summary>Plays the demo scene (opened by DemoScene.PlayDemoBatch) for a moment and checks what is on screen.</summary>
        private IEnumerator RunDemo()
        {
            CheckPipeline();
            AlphaClipPlayer player = FindFirstObjectByType<AlphaClipPlayer>();
            Check(player != null, "the demo scene contains an AlphaClipPlayer");
            if (player == null)
            {
                yield break;
            }
            float t0 = Time.realtimeSinceStartup;
            while (player.FramesPresented < 20 && Time.realtimeSinceStartup - t0 < 8f)
            {
                yield return null;
            }
            Check(player.IsPlaying && player.FramesPresented >= 20, "the demo scene plays (" + player.FramesPresented + " pictures shown)");

            var binder = player.GetComponent<AlphaClipRendererBinder>();
            Check(binder != null && binder.Target != null, "the demo scene has a binder with a target renderer");
            if (binder != null && binder.Target != null)
            {
                var block = new MaterialPropertyBlock();
                binder.Target.GetPropertyBlock(block);
                // The binder picks _BaseMap (URP shaders) when the material has it, else _MainTex.
                Texture bound = block.GetTexture("_BaseMap") != null ? block.GetTexture("_BaseMap") : block.GetTexture("_MainTex");
                Check(bound == player.Output && player.Output != null, "the renderer shows the player's Output texture");
            }

            // What the camera sees: render it, check it is not a flat colour, keep it as a PNG
            Camera cam = Camera.main;
            Check(cam != null, "the demo scene has a main camera");
            if (cam != null)
            {
                yield return null;   // (WaitForEndOfFrame never resumes in batch mode)
                var rt = new RenderTexture(640, 360, 24, RenderTextureFormat.ARGB32, RenderTextureReadWrite.Default);
                cam.targetTexture = rt;
                cam.Render();
                cam.targetTexture = null;
                RenderTexture previous = RenderTexture.active;
                RenderTexture.active = rt;
                var tex = new Texture2D(640, 360, TextureFormat.RGBA32, false, !rt.sRGB);
                tex.ReadPixels(new Rect(0, 0, 640, 360), 0, 0, false);
                RenderTexture.active = previous;
                Color32[] px = tex.GetPixels32();
                int lo = 255, hi = 0;
                foreach (Color32 c in px)
                {
                    int l = (c.r + c.g + c.b) / 3;
                    lo = Math.Min(lo, l);
                    hi = Math.Max(hi, l);
                }
                Check(hi - lo > 100, "the camera image is not a flat colour (brightness range " + lo + ".." + hi + ")");
                string dir = Arg("-acSnapshotDir");
                if (dir != null)
                {
                    Directory.CreateDirectory(dir);
                    File.WriteAllBytes(Path.Combine(dir, "demo_camera.png"), tex.EncodeToPNG());
                }
                Destroy(tex);
                rt.Release();
                Destroy(rt);
            }
        }

        private static bool IsStrictlyIncreasing(List<long> v)
        {
            for (int i = 1; i < v.Count; i++)
            {
                if (v[i] <= v[i - 1])
                {
                    return false;
                }
            }
            return true;
        }
    }
}
