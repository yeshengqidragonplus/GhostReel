using System;
using System.Collections;
using System.Collections.Generic;
using System.IO;
using System.Text;
using UnityEngine;
using UnityEngine.Profiling;
using UnityEngine.Rendering;
using UnityEngine.UI;
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
            var watchdog = StartCoroutine(Watchdog(300f));
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

        private AlphaClipPlayer NewPlayer(bool loop, int depth = 4, string source = null)
        {
            // Inactive while configuring, so that OnEnable (open + play) sees the settings.
            var go = new GameObject("alphaclip-player");
            go.SetActive(false);
            var p = go.AddComponent<AlphaClipPlayer>();
            p.StreamingAssetsPath = source ?? m_clip;   // an absolute path: Path.Combine keeps it as is
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
            long firstLag = lags.Count > 0 ? lags[0] : long.MaxValue;
            int caughtUp = lags.FindIndex(l => l < 100_000);
            long maxLag = 0;
            int maxLagFrame = -1;
            for (int i = 0; i < lags.Count; i++)
            {
                if (lags[i] > maxLag)
                {
                    maxLag = lags[i];
                    maxLagFrame = i;
                }
            }
            m_report.AppendLine("INFO  lag timeline: " + lags.Count + " frames; first/last lag " + (lags.Count > 0 ? lags[0] / 1000 : -1) + "/" +
                                (lags.Count > 0 ? lags[lags.Count - 1] / 1000 : -1) + " ms; below 100 ms from frame " + caughtUp +
                                "; max " + maxLag / 1000 + " ms at frame " + maxLagFrame);
            // The clock starts at the first Update after Play(), so a hitch before it (loading, a first-use shader compile)
            // must not make the start of the clip fast-forward: the first picture is shown on time.
            // Measured 140-220 ms on Unity 6 (the decode thread needs that long for its first frame on the plain-C Windows
            // build); the hitch this guards against was 1.3 s.
            Check(firstLag < 500_000, "the clock starts with playback: the first picture is " + firstLag / 1000 + " ms behind it");
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

            // 5. Loading through UnityWebRequest, the way StreamingAssets are read on Android (there the URI is
            //    jar:file://...!/assets/...; here a file:// URI exercises the same code path on any platform)
            string uri = new Uri(Path.GetFullPath(m_clip)).AbsoluteUri;
            opened0 = AlphaClipPlayer.NativeOpenCount;
            closed0 = AlphaClipPlayer.NativeCloseCount;
            var go5 = new GameObject("alphaclip-uri");   // no source configured: OnEnable does nothing
            AlphaClipPlayer p5 = go5.AddComponent<AlphaClipPlayer>();
            Check(!p5.IsOpen && !p5.IsLoading, "a player without a source stays closed");
            IEnumerator load = p5.OpenUriAsync(uri);
            while (load.MoveNext())
            {
                yield return load.Current;
            }
            Check(p5.IsOpen && !p5.IsLoading && p5.LoadError == null && p5.Info.Width > 0, "OpenUriAsync(file://) opens the clip (" + p5.Info.Width + "x" + p5.Info.Height + ")");
            p5.Play();
            t0 = Time.realtimeSinceStartup;
            while (p5.FramesPresented < 5 && Time.realtimeSinceStartup - t0 < 5f)
            {
                yield return null;
            }
            Check(p5.FramesPresented >= 5, "a clip opened through UnityWebRequest plays (" + p5.FramesPresented + " pictures)");
            Destroy(go5);
            yield return null;

            var go6 = new GameObject("alphaclip-missing");
            AlphaClipPlayer p6 = go6.AddComponent<AlphaClipPlayer>();
            load = p6.OpenUriAsync(new Uri(Path.GetFullPath(m_clip) + ".does-not-exist").AbsoluteUri);   // logs an error on purpose
            while (load.MoveNext())
            {
                yield return load.Current;
            }
            Check(!p6.IsOpen && !p6.IsLoading && !string.IsNullOrEmpty(p6.LoadError), "a missing file is reported through LoadError, not thrown (" + p6.LoadError + ")");
            Destroy(go6);
            yield return null;

            // the component path: PlayOnEnable with a URI as the source opens a few frames later and then plays
            AlphaClipPlayer p7 = NewPlayer(true, 4, uri);
            Check(!p7.IsOpen && p7.IsLoading, "PlayOnEnable with a URI starts loading and is not open yet");
            t0 = Time.realtimeSinceStartup;
            while (p7.FramesPresented < 5 && Time.realtimeSinceStartup - t0 < 5f)
            {
                yield return null;
            }
            Check(p7.IsOpen && p7.IsPlaying && p7.FramesPresented >= 5, "PlayOnEnable with a URI ends up playing (" + p7.FramesPresented + " pictures)");
            Destroy(p7.gameObject);
            yield return null;

            // destroyed while the request is still in flight: no exception, no native player left behind
            AlphaClipPlayer p8 = NewPlayer(true, 4, uri);
            Destroy(p8.gameObject);
            yield return null;
            yield return new WaitForSeconds(0.3f);
            opened = AlphaClipPlayer.NativeOpenCount - opened0;
            closed = AlphaClipPlayer.NativeCloseCount - closed0;
            Check(closed == opened, "UnityWebRequest loading: every native player that was opened was closed (" + opened + " opened, " + closed + " closed)");

            // 6. Picture samples for the device comparison (M3): the same routine runs in the benchmark app on the phone;
            //    compare_picture_samples.py compares the two. Here: the samples exist and the picture is not flat.
            AlphaClipPlayer p9 = NewPlayer(false);
            p9.Matrix = ColorMatrix.BT601;   // as the benchmark app sets it for the sample clips
            string clipName = Path.GetFileName(m_clip);
            bool produced = true;
            foreach (double t in new[] { 0.0, 0.5, 1.0 })
            {
                string json = AlphaClipBenchmark.SamplePicture(p9, clipName, t);
                m_report.AppendLine("INFO  verify " + json);
                produced &= !json.Contains("\"pts_ms\":-");   // a picture was shown at that time
            }
            Check(produced, "picture samples for the device comparison were produced for t = 0, 0.5, 1.0 s");
            Destroy(p9.gameObject);
            yield return null;

            // 7. M4: sidecars through UnityWebRequest, then transparent clips through a UGUI RawImage (only with -acAlphaClips / -acTruth)
            yield return UriSidecarTests();
            yield return UiCompositeTests();
        }

        /// <summary>
        /// The sidecar travels with the clip when it is read through UnityWebRequest (this is the Android StreamingAssets path; a file:// URI
        /// exercises the same code): the clip native_premul.webm says "premultiplied", which "auto" mode would not know.
        /// </summary>
        private IEnumerator UriSidecarTests()
        {
            string clipsArg = Arg("-acAlphaClips");
            string premul = string.IsNullOrEmpty(clipsArg) ? null : Array.Find(clipsArg.Split(';'), c => Path.GetFileName(c).Contains("native_premul"));
            if (premul == null)
            {
                yield break;
            }
            string uri = new Uri(Path.GetFullPath(premul)).AbsoluteUri;
            int opened0 = AlphaClipPlayer.NativeOpenCount, closed0 = AlphaClipPlayer.NativeCloseCount;

            var go = new GameObject("alphaclip-uri-sidecar");
            AlphaClipPlayer p = go.AddComponent<AlphaClipPlayer>();
            IEnumerator load = p.OpenUriAsync(uri);
            while (load.MoveNext())
            {
                yield return load.Current;
            }
            AlphaClipInfo info = p.Info;
            Check(p.IsOpen && p.LoadError == null && info.AlphaMode == AlphaClipAlphaMode.NativeAlpha && info.Premultiplied &&
                  info.ContentWidth == AlphaClipTruth.Width && info.Matrix == 1 && info.ColorRange == 1,
                "OpenUriAsync(file://) finds the sidecar next to the clip (" + info.AlphaMode + ", premultiplied " + info.Premultiplied + ", error " + p.LoadError + ")");
            Destroy(go);
            yield return null;

            go = new GameObject("alphaclip-uri-sidecar");
            p = go.AddComponent<AlphaClipPlayer>();
            load = p.OpenUriAsync(uri, "{\"schema\":1,\"alpha_mode\":\"opaque\"}");
            while (load.MoveNext())
            {
                yield return load.Current;
            }
            Check(p.IsOpen && p.Info.AlphaMode == AlphaClipAlphaMode.Opaque && !p.Info.Premultiplied,
                "OpenUriAsync with an explicit sidecar text uses that text, not the file next to the clip");
            Destroy(go);
            yield return null;

            go = new GameObject("alphaclip-uri-sidecar");
            p = go.AddComponent<AlphaClipPlayer>();
            load = p.OpenUriAsync(uri, "{\"schema\":1,");   // logs an error on purpose
            while (load.MoveNext())
            {
                yield return load.Current;
            }
            Check(!p.IsOpen && !p.IsLoading && p.LoadError != null && p.LoadError.Contains("sidecar"),
                "a bad sidecar given to OpenUriAsync is reported through LoadError, not thrown (" + p.LoadError + ")");
            Destroy(go);
            yield return null;

            AlphaClipPlayer p4 = NewPlayer(true, 4, uri);   // PlayOnEnable with a URI source
            float t0 = Time.realtimeSinceStartup;
            while (p4.FramesPresented < 3 && Time.realtimeSinceStartup - t0 < 5f)
            {
                yield return null;
            }
            Check(p4.IsPlaying && p4.FramesPresented >= 3 && p4.Info.Premultiplied && p4.Info.AlphaMode == AlphaClipAlphaMode.NativeAlpha,
                "PlayOnEnable with a URI source picks up the sidecar and plays (" + p4.FramesPresented + " pictures)");
            Destroy(p4.gameObject);
            yield return null;
            yield return new WaitForSeconds(0.2f);
            int opened = AlphaClipPlayer.NativeOpenCount - opened0, closed = AlphaClipPlayer.NativeCloseCount - closed0;
            Check(opened == 3 && closed == opened, "sidecar loading through UnityWebRequest: every native player that was opened was closed (" + opened + " opened, " + closed + " closed)");
        }

        /// <summary>
        /// Transparent clips on the UI path: each clip is shown by a RawImage with the premultiplied UI shader (AlphaClipMaterials.CreateUI)
        /// in a world-space canvas, drawn by a camera over solid backgrounds, and compared with the exact truth like the camera + quad
        /// test of the edit-mode suite. Controls: the default UI material (straight-alpha blend) must show a dark rim; a RectMask2D over the
        /// left half must clip the premultiplied picture cleanly (the right half stays the plain background).
        /// </summary>
        private IEnumerator UiCompositeTests()
        {
            string clipsArg = Arg("-acAlphaClips"), truthPath = Arg("-acTruth");
            if (string.IsNullOrEmpty(clipsArg) || string.IsNullOrEmpty(truthPath))
            {
                yield break;
            }
            byte[] truth = File.ReadAllBytes(truthPath);
            int w = AlphaClipTruth.Width, h = AlphaClipTruth.Height;
            var backgrounds = new[] { new Color32(255, 255, 255, 255), new Color32(128, 128, 128, 255), new Color32(0, 200, 230, 255) };
            string space = QualitySettings.activeColorSpace + ", " + ActivePipeline();

            Material uiMaterial = AlphaClipMaterials.CreateUI();
            var camGo = new GameObject("alphaclip-ui-camera");
            var cam = camGo.AddComponent<Camera>();
            cam.orthographic = true;
            cam.orthographicSize = 0.5f;
            cam.nearClipPlane = 0.1f;
            cam.farClipPlane = 10f;
            cam.clearFlags = CameraClearFlags.SolidColor;
            cam.enabled = false;
            camGo.transform.position = new Vector3(1000f, 0f, -1f);   // away from anything else in the scene

            // A world-space canvas of w x h pixels, 1 unit high, so that one canvas pixel is one pixel of the camera's target.
            var canvasGo = new GameObject("alphaclip-ui-canvas", typeof(RectTransform));
            var canvas = canvasGo.AddComponent<Canvas>();
            canvas.renderMode = RenderMode.WorldSpace;
            canvas.worldCamera = cam;
            var canvasRect = canvasGo.GetComponent<RectTransform>();
            canvasRect.sizeDelta = new Vector2(w, h);
            canvasRect.localScale = Vector3.one / h;
            canvasRect.position = new Vector3(1000f, 0f, 0f);

            var maskGo = new GameObject("mask", typeof(RectTransform));
            maskGo.transform.SetParent(canvasGo.transform, false);
            var maskRect = maskGo.GetComponent<RectTransform>();
            maskRect.anchorMin = Vector2.zero;
            maskRect.anchorMax = Vector2.one;
            maskRect.offsetMin = maskRect.offsetMax = Vector2.zero;

            // The picture keeps its full width however narrow the mask is: pinned to the mask's left edge, w pixels wide.
            var rawGo = new GameObject("raw", typeof(RectTransform));
            rawGo.transform.SetParent(maskGo.transform, false);
            var raw = rawGo.AddComponent<RawImage>();
            raw.raycastTarget = false;
            var rawRect = rawGo.GetComponent<RectTransform>();
            rawRect.anchorMin = Vector2.zero;
            rawRect.anchorMax = new Vector2(0f, 1f);
            rawRect.pivot = new Vector2(0f, 0.5f);
            rawRect.sizeDelta = new Vector2(w, 0f);
            rawRect.anchoredPosition = Vector2.zero;

            var target = new RenderTexture(w, h, 24, RenderTextureFormat.ARGB32, RenderTextureReadWrite.Default);
            cam.targetTexture = target;

            bool maskTested = false;
            try
            {
                foreach (string clip in clipsArg.Split(';'))
                {
                    string name = Path.GetFileName(clip);
                    AlphaClipPlayer p = NewPlayer(false, 64, clip);
                    p.Pause();
                    Check(p.IsOpen && p.Info.HasAlpha && p.Output != null && p.Output.width == w && p.Output.height == h,
                        name + " (UI): opened as a transparent clip with a " + w + "x" + h + " picture");
                    raw.texture = p.Output;

                    raw.material = uiMaterial;
                    for (int b = 0; b < backgrounds.Length; b++)
                    {
                        CompositeStats all = default;
                        for (int f = 0; f < AlphaClipTruth.Frames.Length; f++)
                        {
                            Check(AlphaClipTruth.ShowFrame(p, AlphaClipTruth.Frames[f]), name + " (UI): frame " + AlphaClipTruth.Frames[f] + " is shown after a seek");
                            yield return null;
                            all.Add(AlphaClipTruth.Measure(RenderUi(cam, target, backgrounds[b]), truth, f, backgrounds[b]));
                        }
                        m_report.AppendLine("INFO  " + name + " (UI) over (" + backgrounds[b].r + "," + backgrounds[b].g + "," + backgrounds[b].b + ") [" + space +
                                            "]: mean |error| " + all.MeanAbs.ToString("F3") + ", max " + all.Max + ", soft-edge signed mean " + all.EdgeMean.ToString("F3"));
                        Check(all.EdgeCount > 100 && all.MeanAbs <= AlphaClipTruth.MaxMeanAbs && Math.Abs(all.EdgeMean) <= AlphaClipTruth.MaxEdgeMean,
                            name + " (UI) over (" + backgrounds[b].r + "," + backgrounds[b].g + "," + backgrounds[b].b + "): RawImage + AlphaClip/UIPremultiplied matches the truth, no rim (mean |error| " +
                            all.MeanAbs.ToString("F2") + ", soft-edge signed mean " + all.EdgeMean.ToString("F2") + ")");
                    }

                    // control: Unity's default UI material blends straight alpha, so premultiplied colour is multiplied twice: a dark rim
                    raw.material = null;
                    CompositeStats control = default;
                    for (int f = 0; f < AlphaClipTruth.Frames.Length; f++)
                    {
                        AlphaClipTruth.ShowFrame(p, AlphaClipTruth.Frames[f]);
                        yield return null;
                        control.Add(AlphaClipTruth.Measure(RenderUi(cam, target, backgrounds[0]), truth, f, backgrounds[0]));
                    }
                    m_report.AppendLine("INFO  " + name + " (UI) CONTROL default UI material over white: soft-edge signed mean " + control.EdgeMean.ToString("F2"));
                    Check(control.EdgeMean < -AlphaClipTruth.MinControlEdgeMean,
                        name + " (UI): control, the default UI material gives a dark rim (soft-edge signed mean " + control.EdgeMean.ToString("F1") + " < -" + AlphaClipTruth.MinControlEdgeMean + ")");

                    if (!maskTested)
                    {
                        maskTested = true;
                        raw.material = uiMaterial;
                        var rectMask = maskGo.AddComponent<RectMask2D>();
                        maskRect.anchorMax = new Vector2(0.5f, 1f);
                        Color32 bg = backgrounds[2];
                        AlphaClipTruth.ShowFrame(p, AlphaClipTruth.Frames[1]);
                        yield return null;
                        byte[] rgba = RenderUi(cam, target, bg);
                        int outside = 0;
                        for (int j = 0; j < h; j++)
                        {
                            for (int x = w / 2 + 2; x < w; x++)
                            {
                                int p0 = (j * w + x) * 4;
                                if (Math.Abs(rgba[p0] - bg.r) > 1 || Math.Abs(rgba[p0 + 1] - bg.g) > 1 || Math.Abs(rgba[p0 + 2] - bg.b) > 1)
                                {
                                    outside++;
                                }
                            }
                        }
                        CompositeStats inside = AlphaClipTruth.Measure(rgba, truth, 1, bg, 0, w / 2 - 2);
                        Check(outside == 0, name + " (UI): RectMask2D over the left half leaves the right half as the plain background (" + outside + " pixels differ)");
                        Check(inside.Count > 0 && inside.MeanAbs <= AlphaClipTruth.MaxMeanAbs,
                            name + " (UI): RectMask2D keeps the left half equal to the truth (mean |error| " + inside.MeanAbs.ToString("F3") + ")");
                        Destroy(rectMask);
                        maskRect.anchorMax = Vector2.one;
                        yield return null;
                    }
                    Destroy(p.gameObject);
                    yield return null;
                }
            }
            finally
            {
                cam.targetTexture = null;
                target.Release();
                Destroy(target);
                Destroy(uiMaterial);
                Destroy(canvasGo);
                Destroy(camGo);
            }
        }

        private static byte[] RenderUi(Camera cam, RenderTexture target, Color32 background)
        {
            cam.backgroundColor = background;
            Canvas.ForceUpdateCanvases();
            cam.Render();
            return AlphaClipTruth.ReadBack(target);
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
