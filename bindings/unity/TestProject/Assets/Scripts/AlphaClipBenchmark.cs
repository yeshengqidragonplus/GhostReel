using System;
using System.Collections;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text;
using UnityEngine;
using UnityEngine.Profiling;

namespace AlphaClip.Tests
{
    /// <summary>
    /// The M3 performance baseline, run inside the player on the device: plays each clip in a loop through the real
    /// component (UnityWebRequest load, upload, draw to a quad on screen), and after a warm-up logs one line per run,
    /// "ALPHACLIP_BENCH {json}", for logcat. Metrics: the main thread's frame time and its time inside Present (acquire /
    /// upload / draw, CPU side), the decode thread's time per frame (from the core's counters), dropped frames, how far the
    /// picture lags the clock, and memory. It is test code (the package ships no benchmark); the scene is built by
    /// AndroidBuild.CreateBenchScene.
    /// </summary>
    public sealed class AlphaClipBenchmark : MonoBehaviour
    {
        [SerializeField] private string[] clips = { "opaque.webm", "alpha_native.webm" };
        [SerializeField] private int rounds = 3;
        [SerializeField] private float warmupSeconds = 2f;
        [SerializeField] private float measureSeconds = 10f;
        [SerializeField] private Renderer quad;

        public Renderer Quad { set => quad = value; }

        private IEnumerator Start()
        {
            // Mobile defaults would cap the frame rate (30 fps) and hide the main thread's real cost: ask for 60 and no vsync wait.
            QualitySettings.vSyncCount = 0;
            Application.targetFrameRate = 60;
            Screen.sleepTimeout = SleepTimeout.NeverSleep;

            yield return null;
            Log("device", DeviceJson());
            yield return VerifyAll();
            for (int round = 0; round < rounds; round++)
            {
                foreach (string clip in clips)
                {
                    yield return RunOne(clip, round);
                }
            }
            Debug.Log("ALPHACLIP_BENCH_DONE");
            yield return new WaitForSecondsRealtime(1f);
            Application.Quit(0);
        }

        private IEnumerator RunOne(string clip, int round)
        {
            var go = new GameObject("bench-" + clip);
            go.SetActive(false);
            var player = go.AddComponent<AlphaClipPlayer>();
            player.StreamingAssetsPath = clip;
            player.Loop = true;
            player.Matrix = ColorMatrix.BT601;   // the sample clips come from ffmpeg RGB input: BT.601
            var binder = go.AddComponent<AlphaClipRendererBinder>();
            binder.Target = quad;
            float opened = Time.realtimeSinceStartup;
            go.SetActive(true);
            while (player.FramesPresented < 1 && Time.realtimeSinceStartup - opened < 30f)
            {
                yield return null;
            }
            if (player.FramesPresented < 1)
            {
                Log("run", "{\"clip\":\"" + clip + "\",\"round\":" + round + ",\"error\":\"no picture after 30 s: " + player.LoadError + "\"}");
                Destroy(go);
                yield return null;
                yield break;
            }
            double firstPictureMs = (Time.realtimeSinceStartup - opened) * 1000.0;

            for (float t = Time.realtimeSinceStartup; Time.realtimeSinceStartup - t < warmupSeconds;)
            {
                yield return null;
            }

            player.ResetPresentTimes();
            AlphaClipStats s0 = player.GetStats();
            long shown0 = player.FramesPresented;
            var frameMs = new List<float>(2048);
            var lagMs = new List<float>(2048);
            float t0 = Time.realtimeSinceStartup;
            while (Time.realtimeSinceStartup - t0 < measureSeconds)
            {
                yield return null;
                frameMs.Add(Time.unscaledDeltaTime * 1000f);
                if (player.LastPtsUs >= 0)
                {
                    lagMs.Add((player.CurrentClockUs - player.LastPtsUs) / 1000f);
                }
            }
            double wall = Time.realtimeSinceStartup - t0;
            PresentTimes pt = player.GetPresentTimes();
            AlphaClipStats s1 = player.GetStats();
            long decoded = s1.FramesDecoded - s0.FramesDecoded;
            long dropped = s1.FramesDropped - s0.FramesDropped;
            long shown = player.FramesPresented - shown0;

            frameMs.Sort();
            lagMs.Sort();
            var sb = new StringBuilder();
            sb.Append('{');
            Field(sb, "clip", clip, true);
            Field(sb, "round", round);
            Field(sb, "size", player.Info.Width + "x" + player.Info.Height, true);
            Field(sb, "alpha", player.Info.HasAlpha ? 1 : 0);
            Field(sb, "first_picture_ms", firstPictureMs);
            Field(sb, "wall_s", wall);
            Field(sb, "frames_unity", frameMs.Count);
            Field(sb, "fps_unity", frameMs.Count / wall);
            Field(sb, "frame_ms_p50", Percentile(frameMs, 50));
            Field(sb, "frame_ms_p95", Percentile(frameMs, 95));
            Field(sb, "frame_ms_p99", Percentile(frameMs, 99));
            Field(sb, "frame_ms_max", frameMs.Count > 0 ? frameMs[frameMs.Count - 1] : 0);
            Field(sb, "pictures", shown);
            Field(sb, "decoded", decoded);
            Field(sb, "dropped", dropped);
            Field(sb, "decode_ms_per_frame", decoded > 0 ? (s1.DecodeTimeUs - s0.DecodeTimeUs) / 1000.0 / decoded : 0);
            Field(sb, "present_calls", pt.Calls);
            Field(sb, "acquire_us_per_call", pt.Calls > 0 ? pt.AcquireUs / pt.Calls : 0);
            Field(sb, "upload_ms_per_picture", pt.Frames > 0 ? pt.UploadUs / pt.Frames / 1000.0 : 0);
            Field(sb, "draw_ms_per_picture", pt.Frames > 0 ? pt.DrawUs / pt.Frames / 1000.0 : 0);
            Field(sb, "main_present_ms_per_unity_frame", pt.Calls > 0 ? (pt.AcquireUs + pt.UploadUs + pt.DrawUs) / pt.Calls / 1000.0 : 0);
            Field(sb, "lag_ms_p50", Percentile(lagMs, 50));
            Field(sb, "lag_ms_p95", Percentile(lagMs, 95));
            Field(sb, "mono_used_mb", Profiler.GetMonoUsedSizeLong() / 1048576.0);
            Field(sb, "unity_alloc_mb", Profiler.GetTotalAllocatedMemoryLong() / 1048576.0);
            Field(sb, "gfx_driver_mb", Profiler.GetAllocatedMemoryForGraphicsDriver() / 1048576.0);
            Field(sb, "rss_mb", ProcStatusKb("VmRSS") / 1024.0);
            Field(sb, "rss_peak_mb", ProcStatusKb("VmHWM") / 1024.0, false, true);
            sb.Append('}');
            Log("run", sb.ToString());

            Destroy(go);
            yield return null;
            yield return new WaitForSecondsRealtime(1f);   // let the device settle between runs
        }

        /// <summary>
        /// Shows the frame at <paramref name="seconds"/> (Seek, then Present on that exact clock), reads the finished
        /// RenderTexture back and describes it: the mean colour of the whole picture and a grid of 8x6 sample pixels. The same
        /// routine runs in the Windows editor (whose picture the M2 tests compared with a CPU reference to within 1 level) and
        /// on the phone, so comparing the two outputs proves the picture on the device (decode through the .so, upload, GPU
        /// conversion) matches. compare_picture_samples.py does the comparison. Leaves the player paused.
        /// </summary>
        public static string SamplePicture(AlphaClipPlayer player, string clipName, double seconds)
        {
            player.Pause();
            player.Seek(seconds);
            long target = (long)(seconds * 1e6);
            float start = Time.realtimeSinceStartup;
            while ((player.LastPtsUs < 0 || player.LastPtsUs < target - 20_000) && Time.realtimeSinceStartup - start < 10f)
            {
                if (!player.Present(target))
                {
                    System.Threading.Thread.Sleep(2);   // the decode thread is still catching up from the keyframe
                }
            }
            int w = player.Info.Width, h = player.Info.Height;
            RenderTexture previous = RenderTexture.active;
            RenderTexture.active = player.Output;
            var tex = new Texture2D(w, h, TextureFormat.RGBA32, false, true);
            tex.ReadPixels(new Rect(0, 0, w, h), 0, 0, false);
            RenderTexture.active = previous;
            Color32[] px = tex.GetPixels32();
            Destroy(tex);

            double r = 0, g = 0, b = 0;
            foreach (Color32 c in px)
            {
                r += c.r;
                g += c.g;
                b += c.b;
            }
            var samples = new StringBuilder();
            for (int j = 0; j < 6; j++)
            {
                for (int i = 0; i < 8; i++)
                {
                    int x = (int)((i + 0.5) * w / 8), y = (int)((j + 0.5) * h / 6);
                    Color32 c = px[y * w + x];
                    samples.Append(samples.Length > 0 ? "," : "").Append(c.r.ToString("x2")).Append(c.g.ToString("x2")).Append(c.b.ToString("x2"));
                }
            }
            double n = px.Length;
            return "{\"clip\":\"" + clipName + "\",\"t\":" + seconds.ToString("0.###", CultureInfo.InvariantCulture) +
                   ",\"pts_ms\":" + (player.LastPtsUs / 1000).ToString(CultureInfo.InvariantCulture) + ",\"w\":" + w + ",\"h\":" + h +
                   ",\"mean\":[" + (r / n).ToString("0.###", CultureInfo.InvariantCulture) + "," + (g / n).ToString("0.###", CultureInfo.InvariantCulture) + "," +
                   (b / n).ToString("0.###", CultureInfo.InvariantCulture) + "],\"samples\":\"" + samples + "\"}";
        }

        private IEnumerator VerifyAll()
        {
            foreach (string clip in clips)
            {
                var go = new GameObject("verify-" + clip);
                go.SetActive(false);
                var player = go.AddComponent<AlphaClipPlayer>();
                player.StreamingAssetsPath = clip;
                player.Loop = false;
                player.Matrix = ColorMatrix.BT601;
                go.SetActive(true);
                float t0 = Time.realtimeSinceStartup;
                while (!player.IsOpen && Time.realtimeSinceStartup - t0 < 30f)
                {
                    yield return null;
                }
                if (!player.IsOpen)
                {
                    Log("verify", "{\"clip\":\"" + clip + "\",\"error\":\"not open: " + player.LoadError + "\"}");
                }
                else
                {
                    foreach (double t in new[] { 0.0, 0.5, 1.0 })
                    {
                        Log("verify", SamplePicture(player, clip, t));
                        yield return null;
                    }
                }
                Destroy(go);
                yield return null;
            }
        }

        private static string DeviceJson()
        {
            var sb = new StringBuilder();
            sb.Append('{');
            Field(sb, "model", SystemInfo.deviceModel, true);
            Field(sb, "os", SystemInfo.operatingSystem, true);
            Field(sb, "cpu", SystemInfo.processorType, true);
            Field(sb, "cpu_count", SystemInfo.processorCount);
            Field(sb, "gpu", SystemInfo.graphicsDeviceName, true);
            Field(sb, "graphics_api", SystemInfo.graphicsDeviceType.ToString(), true);
            Field(sb, "refresh_hz", Screen.currentResolution.refreshRateRatio.value);
            Field(sb, "screen", Screen.width + "x" + Screen.height, true);
            Field(sb, "development_build", Debug.isDebugBuild ? 1 : 0);
            Field(sb, "native_core", AlphaClipPlayer.NativeVersion(), true, true);
            sb.Append('}');
            return sb.ToString();
        }

        private static void Log(string kind, string json)
        {
            Debug.Log("ALPHACLIP_BENCH " + kind + " " + json);
        }

        private static float Percentile(List<float> sorted, int p)
        {
            return sorted.Count == 0 ? 0 : sorted[Math.Min(sorted.Count - 1, sorted.Count * p / 100)];
        }

        private static long ProcStatusKb(string key)
        {
            try
            {
                foreach (string line in File.ReadAllLines("/proc/self/status"))
                {
                    if (line.StartsWith(key + ":", StringComparison.Ordinal))
                    {
                        string digits = line.Substring(key.Length + 1).Replace("kB", "").Trim();
                        return long.Parse(digits, CultureInfo.InvariantCulture);
                    }
                }
            }
            catch (Exception)
            {
                // not Linux/Android: no /proc
            }
            return 0;
        }

        private static void Field(StringBuilder sb, string name, string value, bool quoted, bool last = false)
        {
            sb.Append('"').Append(name).Append("\":");
            sb.Append(quoted ? "\"" + value.Replace("\"", "'") + "\"" : value);
            if (!last)
            {
                sb.Append(',');
            }
        }

        private static void Field(StringBuilder sb, string name, double value, bool unused = false, bool last = false)
        {
            Field(sb, name, value.ToString("0.###", CultureInfo.InvariantCulture), false, last);
        }

        private static void Field(StringBuilder sb, string name, long value)
        {
            Field(sb, name, value.ToString(CultureInfo.InvariantCulture), false, false);
        }
    }
}
