using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using UnityEditor;
using UnityEngine;
using UnityEngine.Profiling;
using Debug = UnityEngine.Debug;

namespace AlphaClip.Tests
{
    /// <summary>
    /// Deterministic editor tests of the AlphaClipPlayer (called with an explicit clock, no player loop):
    ///  * the picture in Output equals an independent CPU conversion of the decoded planes (the raw files come from
    ///    `alphaclip decode`, which test_vs_ffmpeg.py proves identical to ffmpeg),
    ///  * seeking, the colour matrix / range parameters, odd sizes,
    ///  * creating and destroying players does not leak.
    /// Arguments: -acPairs "clip.webm|clip.raw;..." -acExpectColorSpace Gamma|Linear -acReport out.txt
    /// </summary>
    public static class EditModeTests
    {
        private static readonly StringBuilder Report = new StringBuilder();
        private static int s_checks, s_failures;

        private static void Check(bool ok, string label, string detail = "")
        {
            s_checks++;
            if (!ok)
            {
                s_failures++;
            }
            Report.AppendLine((ok ? "PASS  " : "FAIL  ") + label + (!ok && detail.Length > 0 ? "  -- " + detail : ""));
        }

        public static void Run()
        {
            int code;
            try
            {
                RunAll();
                code = s_failures == 0 ? 0 : 1;
            }
            catch (Exception e)
            {
                Report.AppendLine("EXCEPTION: " + e);
                code = 2;
            }
            Report.AppendLine((s_checks - s_failures) + "/" + s_checks + " checks passed");
            BatchTests.Finish(Report, code);
        }

        private static void RunAll()
        {
            string expect = BatchTests.Arg("-acExpectColorSpace");
            Check(SystemInfo.graphicsDeviceType != UnityEngine.Rendering.GraphicsDeviceType.Null,
                "a real graphics device is available (" + SystemInfo.graphicsDeviceType + ")");
            Check(expect == null || QualitySettings.activeColorSpace.ToString() == expect,
                "project colour space is " + (expect ?? "unchecked") + " (is " + QualitySettings.activeColorSpace + ")");
            Check(AlphaClipPlayer.NativeVersion().StartsWith("0."), "native core loads: " + AlphaClipPlayer.NativeVersion());
            string expectPipeline = BatchTests.Arg("-acExpectPipeline");
            Check(expectPipeline == null || BatchTests.ActivePipeline() == expectPipeline,
                "render pipeline is " + (expectPipeline ?? "unchecked") + " (is " + BatchTests.ActivePipeline() + ")");

            string pairs = BatchTests.Arg("-acPairs");
            if (string.IsNullOrEmpty(pairs))
            {
                throw new InvalidOperationException("missing -acPairs");
            }
            string firstClip = null;
            foreach (string pair in pairs.Split(';'))
            {
                string[] parts = pair.Split('|');
                if (firstClip == null)
                {
                    firstClip = parts[0];
                }
                // The sample clips were made by ffmpeg from RGB sources: BT.601, limited range.
                PictureTest(parts[0], parts[1], ColorMatrix.BT601, false, ChromaSiting.Left, true);
            }
            // Other branches of the shader parameters, against the same independent reference.
            string[] first = pairs.Split(';')[0].Split('|');
            PictureTest(first[0], first[1], ColorMatrix.BT709, false, ChromaSiting.Left, false);
            PictureTest(first[0], first[1], ColorMatrix.BT601, true, ChromaSiting.Left, false);
            PictureTest(first[0], first[1], ColorMatrix.BT601, false, ChromaSiting.Center, false);

            SourceTests(first[0], first[1]);
            ConsumerTest(first[0], first[1], "Unlit/Texture");
            if (BatchTests.ActivePipeline() == "URP")
            {
                ConsumerTest(first[0], first[1], "Universal Render Pipeline/Unlit");
            }
            LeakTest(firstClip, 200, false);
        }

        /// <summary>
        /// The other ways to hand a clip to the player: raw bytes (copied into native memory that the core reads until it is
        /// closed) and a TextAsset (a ".bytes" file, which is how raw data travels in asset bundles). Frame 0 must equal the reference.
        /// </summary>
        private static void SourceTests(string clip, string rawPath)
        {
            byte[] raw = File.ReadAllBytes(rawPath);
            int opened0 = AlphaClipPlayer.NativeOpenCount, closed0 = AlphaClipPlayer.NativeCloseCount;
            string assetPath = "Assets/AlphaClipTempClip.bytes";
            try
            {
                File.Copy(clip, Path.Combine(Application.dataPath, "AlphaClipTempClip.bytes"), true);
                AssetDatabase.ImportAsset(assetPath, ImportAssetOptions.ForceSynchronousImport);
                var textAsset = AssetDatabase.LoadAssetAtPath<TextAsset>(assetPath);
                Check(textAsset != null && textAsset.bytes.Length == new FileInfo(clip).Length, "a .bytes file imports as a TextAsset with the clip's bytes");

                foreach (string how in new[] { "Open(byte[])", "Open(TextAsset)" })
                {
                    var go = new GameObject("alphaclip-source") { hideFlags = HideFlags.HideAndDontSave };
                    var player = go.AddComponent<AlphaClipPlayer>();
                    try
                    {
                        player.QueueDepth = 8;
                        player.Matrix = ColorMatrix.BT601;
                        if (how == "Open(byte[])")
                        {
                            player.Open(File.ReadAllBytes(clip));
                        }
                        else
                        {
                            player.Open(textAsset);
                        }
                        AlphaClipInfo info = player.Info;
                        int w = info.Width, h = info.Height;
                        bool got = false;
                        var sw = Stopwatch.StartNew();
                        while (sw.ElapsedMilliseconds < 5000 && !(got = player.Present(0)))
                        {
                            Thread.Sleep(1);
                        }
                        Diff d = default;
                        if (got)
                        {
                            CompareFrame(ReadBack(player.Output), raw, 0, w, h, ColorMatrix.BT601, false, ChromaSiting.Left, ref d);
                        }
                        Check(got && player.LastPtsUs == 0 && d.Total > 0 && d.Max <= 6 && d.Over2 * 1000 <= d.Total,
                            how + ": the first picture equals the reference (max diff " + d.Max + ")");
                        player.Close();
                    }
                    finally
                    {
                        UnityEngine.Object.DestroyImmediate(go);
                    }
                }
            }
            finally
            {
                AssetDatabase.DeleteAsset(assetPath);
            }
            Check(AlphaClipPlayer.NativeOpenCount - opened0 == 2 && AlphaClipPlayer.NativeCloseCount - closed0 == 2,
                "both sources opened and closed the native player (and freed the byte copy)");

            // Garbage must be refused with an exception, leaving no native player behind.
            var bad = new GameObject("alphaclip-bad") { hideFlags = HideFlags.HideAndDontSave }.AddComponent<AlphaClipPlayer>();
            try
            {
                bad.Open(new byte[] { 1, 2, 3, 4, 5, 6, 7, 8 });
                Check(false, "garbage bytes are refused");
            }
            catch (AlphaClipException e)
            {
                Check(e.Status == AcStatus.ErrFormat && !bad.IsOpen, "garbage bytes are refused with AcStatus.ErrFormat (" + e.Message + ")");
            }
            finally
            {
                UnityEngine.Object.DestroyImmediate(bad.gameObject);
            }
            Check(AlphaClipPlayer.NativeOpenCount - opened0 == 2 && AlphaClipPlayer.NativeCloseCount - closed0 == 2,
                "a refused clip leaves no native player open");
        }

        /// <summary>
        /// Diagnostic, not a pass/fail test: private-memory growth per create/destroy cycle, in eight phases of 200 cycles, for (a) the
        /// same Unity objects the player creates but without the native core and (b) the full player.
        /// </summary>
        public static void RunMemoryDiagnostics()
        {
            try
            {
                string[] first = BatchTests.Arg("-acPairs").Split(';')[0].Split('|');
                string clip = first[0];
                Shader shader = Resources.Load<Shader>("AlphaClip/YuvToRgb");

                Action unityOnly = () =>
                {
                    var y = new Texture2D(640, 360, TextureFormat.R8, false, true);
                    var u = new Texture2D(320, 180, TextureFormat.R8, false, true);
                    var v = new Texture2D(320, 180, TextureFormat.R8, false, true);
                    var rt = new RenderTexture(640, 360, 0, RenderTextureFormat.ARGB32, RenderTextureReadWrite.Default);
                    rt.Create();
                    var mat = new Material(shader);
                    mat.SetTexture("_UTex", u);
                    mat.SetTexture("_VTex", v);
                    y.Apply(false, false);
                    Graphics.Blit(y, rt, mat);
                    rt.Release();
                    UnityEngine.Object.DestroyImmediate(rt);
                    UnityEngine.Object.DestroyImmediate(mat);
                    UnityEngine.Object.DestroyImmediate(y);
                    UnityEngine.Object.DestroyImmediate(u);
                    UnityEngine.Object.DestroyImmediate(v);
                };
                Action fullPlayer = () =>
                {
                    var go = new GameObject("alphaclip-diag") { hideFlags = HideFlags.HideAndDontSave };
                    var p = go.AddComponent<AlphaClipPlayer>();
                    p.OpenFile(clip);
                    var sw = Stopwatch.StartNew();
                    while (sw.ElapsedMilliseconds < 3000 && !p.Present(0))
                    {
                        Thread.Sleep(1);
                    }
                    p.Close();
                    UnityEngine.Object.DestroyImmediate(go);
                };

                foreach (var pair in new[] { ("unity objects only", unityOnly), ("full player", fullPlayer) })
                {
                    for (int i = 0; i < 20; i++)
                    {
                        pair.Item2();
                    }
                    for (int phase = 1; phase <= 8; phase++)
                    {
                        GC.Collect();
                        long before = PrivateBytes();
                        for (int i = 0; i < 200; i++)
                        {
                            pair.Item2();
                        }
                        GC.Collect();
                        long after = PrivateBytes();
                        Report.AppendLine(pair.Item1 + ", phase " + phase + ": " + ((after - before) / 1048576.0).ToString("F1") +
                                          " MB over 200 cycles = " + ((after - before) / 200 / 1024.0).ToString("F0") + " KB per cycle");
                    }
                }
                BatchTests.Finish(Report, 0);
            }
            catch (Exception e)
            {
                Report.AppendLine("EXCEPTION: " + e);
                BatchTests.Finish(Report, 2);
            }
        }

        /// <summary>
        /// Negative control for the leak test: players that are never closed must make the leak checks FAIL.
        /// Exit code 0 means the checks did fail, as they should.
        /// </summary>
        public static void RunLeakControl()
        {
            int code;
            try
            {
                string[] first = BatchTests.Arg("-acPairs").Split(';')[0].Split('|');
                LeakTest(first[0], 40, true);
                code = s_failures >= 2 ? 0 : 1;
                Report.AppendLine(code == 0 ? "CONTROL OK: the leak checks detected the deliberate leak (" + s_failures + " failed)"
                                            : "CONTROL FAILED: the leak checks did not notice a deliberate leak");
            }
            catch (Exception e)
            {
                Report.AppendLine("EXCEPTION: " + e);
                code = 2;
            }
            BatchTests.Finish(Report, code);
        }

        /// <summary>
        /// Anything that can show a texture must be able to show Output: render it with a camera through the built-in
        /// Unlit/Texture shader into another target and compare with the reference. This goes through the sRGB read and
        /// write conversions of a Linear colour-space project.
        /// </summary>
        private static void ConsumerTest(string clip, string rawPath, string shaderName)
        {
            string name = Path.GetFileName(clip) + " via a camera and " + shaderName + " [" + BatchTests.ActivePipeline() + "]";
            var player = new GameObject("alphaclip-consumer").AddComponent<AlphaClipPlayer>();
            GameObject cameraGo = null, quad = null;
            RenderTexture target = null;
            Material material = null;
            try
            {
                player.QueueDepth = 64;
                player.Loop = false;
                player.Matrix = ColorMatrix.BT601;
                player.OpenFile(clip);
                AlphaClipInfo info = player.Info;
                int w = info.Width, h = info.Height, cw = (w + 1) / 2, ch = (h + 1) / 2;
                int frameBytes = w * h + 2 * cw * ch + (info.HasAlpha ? w * h : 0);
                byte[] raw = File.ReadAllBytes(rawPath);
                WaitFor(() => player.GetStats().ReachedEnd, 15000);
                int shown = 0;
                for (long clock = 0; shown < 13 && clock < 5_000_000; clock += 1000)
                {
                    if (player.Present(clock))
                    {
                        shown++;
                    }
                }

                cameraGo = new GameObject("alphaclip-camera");
                var cam = cameraGo.AddComponent<Camera>();
                cam.orthographic = true;
                cam.orthographicSize = 0.5f;           // the quad is 1 unit high and w/h wide: one texel per pixel
                cam.nearClipPlane = 0.1f;
                cam.farClipPlane = 10f;
                cam.clearFlags = CameraClearFlags.SolidColor;
                cam.backgroundColor = Color.black;
                cam.enabled = false;
                cameraGo.transform.position = new Vector3(0, 0, -1);

                quad = GameObject.CreatePrimitive(PrimitiveType.Quad);
                UnityEngine.Object.DestroyImmediate(quad.GetComponent<Collider>());
                quad.transform.localScale = new Vector3((float)w / h, 1f, 1f);
                Shader shader = Shader.Find(shaderName);
                Check(shader != null, name + ": shader found");
                material = new Material(shader) { mainTexture = player.Output };
                quad.GetComponent<MeshRenderer>().sharedMaterial = material;

                target = new RenderTexture(w, h, 24, RenderTextureFormat.ARGB32, RenderTextureReadWrite.Default);
                cam.targetTexture = target;
                cam.Render();

                Diff d = default;
                byte[] rgba = ReadBack(target);
                CompareFrame(rgba, raw, (shown - 1) * frameBytes, w, h, ColorMatrix.BT601, false, ChromaSiting.Left, ref d);
                ReportDiff(name + " (frame " + (shown - 1) + ")", d);
                string snapshotDir = BatchTests.Arg("-acSnapshotDir");
                if (snapshotDir != null)
                {
                    Directory.CreateDirectory(snapshotDir);
                    var png = new Texture2D(w, h, TextureFormat.RGBA32, false, true);
                    png.LoadRawTextureData(rgba);
                    png.Apply();
                    File.WriteAllBytes(Path.Combine(snapshotDir, "consumer_" + BatchTests.ActivePipeline() + "_" + shaderName.Replace("/", "_").Replace(" ", "") + "_" + Path.GetFileNameWithoutExtension(clip) + "_frame" + (shown - 1) + ".png"), png.EncodeToPNG());
                    UnityEngine.Object.DestroyImmediate(png);
                }
            }
            finally
            {
                player.Close();
                if (target != null) { target.Release(); UnityEngine.Object.DestroyImmediate(target); }
                if (material != null) UnityEngine.Object.DestroyImmediate(material);
                if (quad != null) UnityEngine.Object.DestroyImmediate(quad);
                if (cameraGo != null) UnityEngine.Object.DestroyImmediate(cameraGo);
                UnityEngine.Object.DestroyImmediate(player.gameObject);
            }
        }

        // ---------------------------------------------------------------------------------------------------------

        private struct Diff
        {
            public long Total, Over2, Over6;
            public int Max;
            public double Sum;
        }

        private static void PictureTest(string clip, string rawPath, ColorMatrix matrix, bool fullRange, ChromaSiting siting, bool alsoSeek)
        {
            string name = Path.GetFileName(clip) + " [" + matrix + (fullRange ? ", full range" : ", limited") + ", chroma " + siting + "]";
            var go = new GameObject("alphaclip-test") { hideFlags = HideFlags.HideAndDontSave };
            try
            {
                var player = go.AddComponent<AlphaClipPlayer>();
                player.QueueDepth = 64;
                player.Loop = false;
                player.Matrix = matrix;
                player.FullRange = fullRange;
                player.ChromaSiting = siting;
                player.OpenFile(clip);
                AlphaClipInfo info = player.Info;
                int w = info.Width, h = info.Height, n = (int)info.PacketCount;
                int cw = (w + 1) / 2, ch = (h + 1) / 2;
                int frameBytes = w * h + 2 * cw * ch + (info.HasAlpha ? w * h : 0);
                byte[] raw = File.ReadAllBytes(rawPath);
                Check(raw.Length == frameBytes * n, name + ": reference raw has " + n + " frames of " + frameBytes + " bytes",
                    raw.Length.ToString());
                Check(player.Output != null && player.Output.width == w && player.Output.height == h,
                    name + ": Output is a " + w + "x" + h + " RenderTexture");

                WaitFor(() => player.GetStats().ReachedEnd, 15000);
                var pts = new List<long>();
                Diff total = default;
                long presentTicks = 0;
                for (long clock = 0; pts.Count < n && clock < 20_000_000; clock += 1000)
                {
                    long t0 = Stopwatch.GetTimestamp();
                    bool shown = player.Present(clock);
                    if (!shown)
                    {
                        continue;
                    }
                    presentTicks += Stopwatch.GetTimestamp() - t0;
                    pts.Add(player.LastPtsUs);
                    CompareFrame(ReadBack(player.Output), raw, (pts.Count - 1) * frameBytes, w, h, matrix, fullRange, siting, ref total);
                }
                Check(pts.Count == n, name + ": " + n + " frames presented, each once", pts.Count.ToString());
                if (pts.Count > 0)
                {
                    double ms = presentTicks * 1000.0 / Stopwatch.Frequency / pts.Count;
                    Report.AppendLine("INFO  " + name + ": Present() (acquire, 3 plane uploads, YUV->RGB draw; CPU side) " + ms.ToString("F2") +
                                      " ms per frame at " + w + "x" + h + " on " + SystemInfo.graphicsDeviceName);
                }
                ReportDiff(name + ": picture vs independent CPU reference", total);

                if (alsoSeek && pts.Count == n)
                {
                    SeekTest(player, name, raw, frameBytes, w, h, pts, matrix, fullRange, siting);
                }
            }
            finally
            {
                UnityEngine.Object.DestroyImmediate(go);   // edit mode: OnDestroy is not sent, so the component is closed explicitly
            }
        }

        private static void ReportDiff(string label, Diff d)
        {
            double mean = d.Total > 0 ? d.Sum / d.Total : 0;
            // Tolerance: the GPU filters the chroma planes with 8-bit fixed-point weights and rounds once at the end, the
            // reference uses floats: a difference of 1-2 levels is expected. Larger ones must be rare.
            bool ok = d.Total > 0 && d.Max <= 6 && d.Over2 * 1000 <= d.Total;
            Check(ok, label + " (mean |diff| " + mean.ToString("F3") + ", max " + d.Max + ", >2: " + d.Over2 + " of " + d.Total + ", >6: " + d.Over6 + ")");
        }

        private static void SeekTest(AlphaClipPlayer player, string name, byte[] raw, int frameBytes, int w, int h,
            List<long> pts, ColorMatrix matrix, bool fullRange, ChromaSiting siting)
        {
            foreach (double seconds in new[] { 1.0, 0.5, 1.2, 0.0, 100.0 })
            {
                long target = (long)(seconds * 1e6);
                int j = 0;
                for (int i = 0; i < pts.Count; i++)
                {
                    if (pts[i] <= target)
                    {
                        j = i;
                    }
                }
                player.Seek(seconds);
                bool got = false;
                var sw = Stopwatch.StartNew();
                while (sw.ElapsedMilliseconds < 8000 && !(got = player.Present(target)))
                {
                    Thread.Sleep(2);
                }
                Diff d = default;
                if (got)
                {
                    CompareFrame(ReadBack(player.Output), raw, j * frameBytes, w, h, matrix, fullRange, siting, ref d);
                }
                Check(got && player.LastPtsUs == pts[j] && d.Total > 0 && d.Max <= 6 && d.Over2 * 1000 <= d.Total,
                    name + ": seek to " + seconds.ToString("F1") + " s shows packet " + j + " (pts " + pts[j] + ")",
                    got ? "pts " + player.LastPtsUs + ", max diff " + d.Max : "no frame arrived");
            }
        }

        // ---------------------------------------------------------------------------------------------------------

        private static void LeakTest(string clip, int cycles, bool skipClose)
        {
            Func<int> texCount = () => Resources.FindObjectsOfTypeAll<Texture2D>().Length;
            Func<int> rtCount = () => Resources.FindObjectsOfTypeAll<RenderTexture>().Length;
            Func<int> matCount = () => Resources.FindObjectsOfTypeAll<Material>().Length;

            Action cycle = () =>
            {
                var go = new GameObject("alphaclip-leak") { hideFlags = HideFlags.HideAndDontSave };
                var p = go.AddComponent<AlphaClipPlayer>();
                p.OpenFile(clip);
                var sw = Stopwatch.StartNew();
                while (sw.ElapsedMilliseconds < 3000 && !p.Present(0))
                {
                    Thread.Sleep(1);
                }
                if (!skipClose)
                {
                    p.Close();   // edit mode does not send OnDestroy: close explicitly (play mode is tested separately)
                }
                UnityEngine.Object.DestroyImmediate(go);
            };

            Action settle = () =>
            {
                GC.Collect();
                GL.Flush();
                EditorUtility.UnloadUnusedAssetsImmediate();
            };

            for (int i = 0; i < 30; i++)
            {
                cycle();   // warm-up: allocator pools, lazily created Unity objects, driver caches
            }
            settle();
            int opened0 = AlphaClipPlayer.NativeOpenCount, closed0 = AlphaClipPlayer.NativeCloseCount;
            int tex0 = texCount(), rt0 = rtCount(), mat0 = matCount();
            long unity0 = Profiler.GetTotalAllocatedMemoryLong();
            long gfx0 = Profiler.GetAllocatedMemoryForGraphicsDriver();
            // Two windows of `cycles` cycles; process private bytes are noisy (see below), so each window is measured on its own.
            var privateGrowthMb = new double[2];
            for (int window = 0; window < 2; window++)
            {
                long mem0 = PrivateBytes();
                for (int i = 0; i < cycles; i++)
                {
                    cycle();
                }
                settle();
                privateGrowthMb[window] = (PrivateBytes() - mem0) / 1048576.0;
            }
            int opened = AlphaClipPlayer.NativeOpenCount - opened0, closed = AlphaClipPlayer.NativeCloseCount - closed0;
            Check(opened == 2 * cycles && closed == 2 * cycles, "leak: " + 2 * cycles + " cycles opened and closed the native player " + opened + "/" + closed + " times");
            Check(texCount() == tex0 && rtCount() == rt0 && matCount() == mat0,
                "leak: Texture2D / RenderTexture / Material object counts unchanged (" + tex0 + "/" + rt0 + "/" + mat0 + " -> " +
                texCount() + "/" + rtCount() + "/" + matCount() + ")");

            // One cycle holds about 1.3 MB of Unity/GPU memory (three plane textures, the render texture): a leak of
            // even one plane texture per cycle would add up to 48 MB over 400 cycles.
            double unityMb = (Profiler.GetTotalAllocatedMemoryLong() - unity0) / 1048576.0;
            double gfxMb = (Profiler.GetAllocatedMemoryForGraphicsDriver() - gfx0) / 1048576.0;
            Check(unityMb < 8 && gfxMb < 8, "leak: Unity allocator grew " + unityMb.ToString("F1") + " MB, graphics driver memory " +
                                           gfxMb.ToString("F1") + " MB over " + 2 * cycles + " cycles");

            // Process private bytes include the native core's heap but also the graphics driver's deferred releases and
            // pooling, which swing by +-150 MB between windows with no trend (measured over 1600 cycles: sums to -111 MB),
            // so this is a coarse check only: a player that is never closed holds ~7 MB, and a real leak shows in BOTH windows.
            // The core's own heap is checked per cycle, without Unity, by core/tests/test_shared_lib.py.
            double smaller = Math.Min(privateGrowthMb[0], privateGrowthMb[1]);
            Check(smaller < 1.0 * cycles, "leak: process private bytes changed by " + privateGrowthMb[0].ToString("F1") + " and " +
                                         privateGrowthMb[1].ToString("F1") + " MB in two windows of " + cycles + " cycles (the smaller must stay under " + cycles + " MB)");
        }

        // ---------------------------------------------------------------------------------------------------------

        // Process.PrivateMemorySize64 does not move under Mono: ask Windows directly (private bytes of the whole editor
        // process, which includes the native core's heap).
        [StructLayout(LayoutKind.Sequential)]
        private struct ProcessMemoryCountersEx
        {
            public uint Cb, PageFaultCount;
            public UIntPtr PeakWorkingSetSize, WorkingSetSize, QuotaPeakPagedPoolUsage, QuotaPagedPoolUsage,
                QuotaPeakNonPagedPoolUsage, QuotaNonPagedPoolUsage, PagefileUsage, PeakPagefileUsage, PrivateUsage;
        }

        [DllImport("psapi.dll")]
        private static extern bool GetProcessMemoryInfo(IntPtr process, out ProcessMemoryCountersEx counters, uint size);

        [DllImport("kernel32.dll")]
        private static extern IntPtr GetCurrentProcess();

        private static long PrivateBytes()
        {
            var c = new ProcessMemoryCountersEx { Cb = (uint)Marshal.SizeOf<ProcessMemoryCountersEx>() };
            if (!GetProcessMemoryInfo(GetCurrentProcess(), out c, c.Cb))
            {
                throw new InvalidOperationException("GetProcessMemoryInfo failed");
            }
            return (long)c.PrivateUsage;
        }

        private static void WaitFor(Func<bool> condition, int timeoutMs)
        {
            var sw = Stopwatch.StartNew();
            while (!condition() && sw.ElapsedMilliseconds < timeoutMs)
            {
                Thread.Sleep(2);
            }
        }

        private static byte[] ReadBack(RenderTexture rt)
        {
            RenderTexture previous = RenderTexture.active;
            RenderTexture.active = rt;
            var tex = new Texture2D(rt.width, rt.height, TextureFormat.RGBA32, false, !rt.sRGB);
            tex.ReadPixels(new Rect(0, 0, rt.width, rt.height), 0, 0, false);
            RenderTexture.active = previous;
            byte[] data = tex.GetRawTextureData();
            UnityEngine.Object.DestroyImmediate(tex);
            return data;
        }

        /// <summary>
        /// Independent reference: planar YUV 4:2:0 -> RGB on the CPU with the same sampling the GPU does (luma point-sampled,
        /// chroma bilinear with texel centres and the chroma siting shift, clamp to edge). rgba is the read-back RenderTexture, rows bottom first.
        /// </summary>
        private static void CompareFrame(byte[] rgba, byte[] raw, int off, int w, int h, ColorMatrix matrix, bool full, ChromaSiting siting, ref Diff d)
        {
            int cw = (w + 1) / 2, ch = (h + 1) / 2;
            int uOff = off + w * h, vOff = uOff + cw * ch;
            float lo = full ? 0f : 16f / 255f, ys = full ? 1f : 255f / 219f, cs = full ? 1f : 255f / 224f;
            float shiftX = siting == ChromaSiting.Left ? 0.25f : 0f;   // in chroma texels, see the shader
            float rv, gu, gv, bu;
            if (matrix == ColorMatrix.BT601) { rv = 1.402f; gu = 0.344136f; gv = 0.714136f; bu = 1.772f; }
            else { rv = 1.5748f; gu = 0.187324f; gv = 0.468124f; bu = 1.8556f; }

            for (int j = 0; j < h; j++)
            {
                int r = h - 1 - j;   // RenderTexture row j counts from the bottom, the planes from the top
                float cyf = (r + 0.5f) / h * ch - 0.5f;
                int cy0 = Mathf.FloorToInt(cyf);
                float fy = cyf - cy0;
                int cy1 = Mathf.Clamp(cy0 + 1, 0, ch - 1);
                cy0 = Mathf.Clamp(cy0, 0, ch - 1);
                for (int x = 0; x < w; x++)
                {
                    float cxf = (x + 0.5f) / w * cw - 0.5f + shiftX;
                    int cx0 = Mathf.FloorToInt(cxf);
                    float fx = cxf - cx0;
                    int cx1 = Mathf.Clamp(cx0 + 1, 0, cw - 1);
                    cx0 = Mathf.Clamp(cx0, 0, cw - 1);

                    float yy = raw[off + r * w + x] / 255f;
                    float u = Bilinear(raw, uOff, cw, cx0, cx1, cy0, cy1, fx, fy);
                    float v = Bilinear(raw, vOff, cw, cx0, cx1, cy0, cy1, fx, fy);
                    yy = (yy - lo) * ys;
                    u = (u - 128f / 255f) * cs;
                    v = (v - 128f / 255f) * cs;
                    float rr = Mathf.Clamp01(yy + rv * v), gg = Mathf.Clamp01(yy - gu * u - gv * v), bb = Mathf.Clamp01(yy + bu * u);

                    int p = (j * w + x) * 4;
                    Tally(ref d, (int)Math.Round(rr * 255f), rgba[p]);
                    Tally(ref d, (int)Math.Round(gg * 255f), rgba[p + 1]);
                    Tally(ref d, (int)Math.Round(bb * 255f), rgba[p + 2]);
                }
            }
        }

        private static float Bilinear(byte[] raw, int plane, int stride, int x0, int x1, int y0, int y1, float fx, float fy)
        {
            float a = raw[plane + y0 * stride + x0], b = raw[plane + y0 * stride + x1];
            float c = raw[plane + y1 * stride + x0], e = raw[plane + y1 * stride + x1];
            return (a * (1 - fx) * (1 - fy) + b * fx * (1 - fy) + c * (1 - fx) * fy + e * fx * fy) / 255f;
        }

        private static void Tally(ref Diff d, int expected, int actual)
        {
            int diff = Math.Abs(expected - actual);
            d.Total++;
            d.Sum += diff;
            if (diff > d.Max) d.Max = diff;
            if (diff > 2) d.Over2++;
            if (diff > 6) d.Over6++;
        }
    }
}
