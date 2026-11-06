using System;
using System.Diagnostics;
using System.Threading;
using UnityEngine;

namespace AlphaClip.Tests
{
    /// <summary>Error of a rendered composite against the analytic one (see <see cref="AlphaClipTruth.Measure"/>).</summary>
    public struct CompositeStats
    {
        public long Count, EdgeCount;
        public double SumAbs, EdgeSum;
        public int Max;
        public double MeanAbs => Count > 0 ? SumAbs / Count : 0;
        /// <summary>Signed mean error over the soft edge (truth alpha 5%..95%): a dark rim is negative, a bright one positive.</summary>
        public double EdgeMean => EdgeCount > 0 ? EdgeSum / EdgeCount : 0;

        public void Add(CompositeStats o)
        {
            Count += o.Count;
            EdgeCount += o.EdgeCount;
            SumAbs += o.SumAbs;
            EdgeSum += o.EdgeSum;
            Max = Math.Max(Max, o.Max);
        }
    }

    /// <summary>
    /// The M4 ground truth (samples/m4/truth_rgba_3f.raw: straight RGBA of frames 0, 20 and 40 of the sample shape) and the
    /// metric used by the edit-mode (camera + quad) and the play-mode (UI RawImage) tests: a clip is shown over a solid
    /// background and the result is compared with  truth.rgb * a + background * (1 - a)  (in linear light in a Linear project).
    /// Shared by both test assemblies, so both judge with the same numbers.
    /// </summary>
    public static class AlphaClipTruth
    {
        public const int Width = 320, Height = 180;
        public static readonly int[] Frames = { 0, 20, 40 };

        // Measured on an RTX 4060 (Built-in pipeline and URP agree to a few hundredths):
        //   Gamma  project: the right pipeline gives a mean |error| of 0.13..0.28 and a soft-edge signed mean within +-0.06;
        //                   the controls (wrong premultiplied flag either way, straight-alpha scene shader, default UI material) give -19.2 .. +21.2.
        //   Linear project: 0.12..0.31 and 0.04..0.15 (largest single error 38); the controls give -6.0 (straight-alpha shader, UI material),
        //                   +13.8 and -9.7 (wrong flag). They are smaller because the error is made in linear light and then sRGB-encoded
        //                   into bytes, where the same double multiplication moves the value less.
        // The limits sit between the two groups in each colour space (at least 39x the right pipeline's number, at most half the smallest control's).
        public const double MaxMeanAbs = 1.0;           // all pixels, all channels
        public const double MaxEdgeMean = 1.0;          // |signed mean| over the soft edge
        /// <summary>A control must be at least this far out (signed soft-edge mean, in bytes).</summary>
        public static double MinControlEdgeMean => QualitySettings.activeColorSpace == ColorSpace.Linear ? 3.0 : 10.0;

        public static float SrgbToLinear(float c) => c <= 0.04045f ? c / 12.92f : Mathf.Pow((c + 0.055f) / 1.055f, 2.4f);
        public static float LinearToSrgb(float c) => c <= 0.0031308f ? c * 12.92f : 1.055f * Mathf.Pow(c, 1f / 2.4f) - 0.055f;

        /// <summary>Seeks to frame n of a 30 fps clip and presents it (the clock is the target time, so the frame at that time is the newest due).</summary>
        public static bool ShowFrame(AlphaClipPlayer player, int n)
        {
            double seconds = (Math.Round(n * 1000.0 / 30.0) + 0.5) / 1000.0;
            player.Pause();
            player.Seek(seconds);
            long target = (long)(seconds * 1e6);
            bool got = false;
            var sw = Stopwatch.StartNew();
            while (sw.ElapsedMilliseconds < 8000 && !(got = player.Present(target)))
            {
                Thread.Sleep(2);
            }
            return got && Math.Abs(player.LastPtsUs - n * 1_000_000L / 30) < 20_000;
        }

        /// <summary>Reads a render texture back as RGBA bytes, rows bottom first.</summary>
        public static byte[] ReadBack(RenderTexture rt)
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

        /// <summary>The composite of one truth pixel over a background colour, as bytes (Gamma: arithmetic on the encoded values; Linear: in linear light).</summary>
        public static int Expected(byte[] truth, int truthFrame, int x, int r, int channel, Color32 background)
        {
            int t = truthFrame * Width * Height * 4 + (r * Width + x) * 4;
            float a = truth[t + 3] / 255f;
            float tc = truth[t + channel] / 255f;
            float b = (channel == 0 ? background.r : channel == 1 ? background.g : background.b) / 255f;
            bool linear = QualitySettings.activeColorSpace == ColorSpace.Linear;
            float e = linear ? LinearToSrgb(SrgbToLinear(tc) * a + SrgbToLinear(b) * (1f - a)) : tc * a + b * (1f - a);
            return (int)Math.Round(e * 255f);
        }

        /// <summary>
        /// Compares a rendered composite (rows bottom first) with the analytic one from the straight RGBA truth (rows top first),
        /// for the columns xMin..xMax-1 (the whole picture by default).
        /// </summary>
        public static CompositeStats Measure(byte[] rgba, byte[] truth, int truthFrame, Color32 background, int xMin = 0, int xMax = Width)
        {
            var s = new CompositeStats();
            int baseOff = truthFrame * Width * Height * 4;
            for (int j = 0; j < Height; j++)
            {
                int r = Height - 1 - j;
                for (int x = xMin; x < xMax; x++)
                {
                    int alphaByte = truth[baseOff + (r * Width + x) * 4 + 3];
                    bool edge = alphaByte >= 13 && alphaByte <= 242;
                    for (int c = 0; c < 3; c++)
                    {
                        int err = rgba[(j * Width + x) * 4 + c] - Expected(truth, truthFrame, x, r, c, background);
                        s.Count++;
                        s.SumAbs += Math.Abs(err);
                        s.Max = Math.Max(s.Max, Math.Abs(err));
                        if (edge)
                        {
                            s.EdgeCount++;
                            s.EdgeSum += err;
                        }
                    }
                }
            }
            return s;
        }
    }
}
