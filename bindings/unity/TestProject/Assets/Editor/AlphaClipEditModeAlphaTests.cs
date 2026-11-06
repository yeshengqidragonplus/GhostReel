using System;
using System.Diagnostics;
using System.IO;
using System.Threading;
using UnityEngine;

namespace AlphaClip.Tests
{
    /// <summary>
    /// M4 tests of transparency, against EXACT ground truth instead of a decode of the same file: samples/m4 holds one
    /// known shape stored four ways (native alpha / packed right / packed bottom, straight and premultiplied colour) plus the
    /// straight RGBA truth of three frames. Each clip is shown in a camera over three backgrounds with the premultiplied
    /// scene shader and compared with the analytic composite  truth.rgb * a + background * (1 - a)  (in linear light in a Linear
    /// project). The number that matters is the SIGNED error on the soft edge (alpha 5%..95%): a dark rim is negative, a bright one
    /// positive. Three negative controls must push that number far out of range, so the check is known to see a rim:
    /// the wrong "premultiplied" flag in the sidecar (both ways), and a straight-alpha shader drawing premultiplied colour.
    /// Arguments: -acAlphaClips "a.webm;b.webm;..." -acTruth truth_rgba_3f.raw   (the sidecars sit next to the clips)
    /// </summary>
    public static partial class EditModeTests
    {
        // The ground truth, the metric and its limits are shared with the play-mode tests: Assets/Scripts/AlphaClipTruth.cs
        private const double MaxMeanAbs = AlphaClipTruth.MaxMeanAbs, MaxEdgeMean = AlphaClipTruth.MaxEdgeMean;
        private static double MinControlEdgeMean => AlphaClipTruth.MinControlEdgeMean;   // depends on the colour space

        private static void AlphaTests(string[] clips, string truthPath)
        {
            if (string.IsNullOrEmpty(truthPath))
            {
                throw new InvalidOperationException("missing -acTruth");
            }
            byte[] truth = File.ReadAllBytes(truthPath);
            Check(truth.Length == AlphaClipTruth.Width * AlphaClipTruth.Height * 4 * AlphaClipTruth.Frames.Length,
                "ground truth: " + AlphaClipTruth.Frames.Length + " frames of " + AlphaClipTruth.Width + "x" + AlphaClipTruth.Height + " RGBA", truth.Length.ToString());

            var backgrounds = new[] { new Color32(255, 255, 255, 255), new Color32(128, 128, 128, 255), new Color32(0, 200, 230, 255) };
            Material scene = AlphaClipMaterials.CreateUnlit();
            Shader straightShader = Shader.Find("Unlit/Transparent");
            Check(straightShader != null, "the straight-alpha control shader Unlit/Transparent exists");
            var straightControl = new Material(straightShader);
            string space = QualitySettings.activeColorSpace + ", " + BatchTests.ActivePipeline();
            try
            {
                foreach (string clip in clips)
                {
                    string name = Path.GetFileName(clip);
                    string sidecar = Path.ChangeExtension(clip, AlphaClipPlayer.SidecarExtension);
                    string meta = File.ReadAllText(sidecar);

                    // 1. what the sidecar says arrives in Info, and Output is the picture, not the coded frame
                    AlphaClipAlphaMode wantMode = name.Contains("packed_right") ? AlphaClipAlphaMode.PackedRight
                        : name.Contains("packed_bottom") ? AlphaClipAlphaMode.PackedBottom : AlphaClipAlphaMode.NativeAlpha;
                    bool wantPremul = name.Contains("premul");
                    var player = NewAlphaPlayer(clip, null);
                    try
                    {
                        AlphaClipInfo info = player.Info;
                        Check(info.AlphaMode == wantMode && info.Premultiplied == wantPremul && info.HasAlpha &&
                              info.HasAlphaPlane == (wantMode == AlphaClipAlphaMode.NativeAlpha) &&
                              info.ContentWidth == AlphaClipTruth.Width && info.ContentHeight == AlphaClipTruth.Height &&
                              player.Output.width == AlphaClipTruth.Width && player.Output.height == AlphaClipTruth.Height &&
                              info.ColorRange == 1 && info.Matrix == 1 && info.LoopHint,
                            name + ": the sidecar arrives (" + info.AlphaMode + ", premultiplied " + info.Premultiplied + ", coded " +
                            info.Width + "x" + info.Height + ", picture " + info.ContentWidth + "x" + info.ContentHeight + ")");
                        Check(player.EffectiveMatrix == ColorMatrix.BT601 && !player.EffectiveFullRange,
                            name + ": the sidecar's matrix and range win over the component's settings (set to BT709)");

                        // 2. the real pipeline against the truth, three frames x three backgrounds
                        foreach (Color32 bg in backgrounds)
                        {
                            CompositeStats all = default;
                            for (int f = 0; f < AlphaClipTruth.Frames.Length; f++)
                            {
                                Check(AlphaClipTruth.ShowFrame(player, AlphaClipTruth.Frames[f]), name + ": frame " + AlphaClipTruth.Frames[f] + " is shown after a seek",
                                    "last pts " + player.LastPtsUs);
                                all.Add(AlphaClipTruth.Measure(RenderComposite(player.Output, scene, bg), truth, f, bg));
                            }
                            Report.AppendLine("INFO  " + name + " over (" + bg.r + "," + bg.g + "," + bg.b + ") [" + space + "]: mean |error| " +
                                              all.MeanAbs.ToString("F3") + ", max " + all.Max + ", soft-edge signed mean " + all.EdgeMean.ToString("F3") +
                                              " over " + all.EdgeCount + " samples");
                            Check(all.Count > 0 && all.EdgeCount > 100 && all.MeanAbs <= MaxMeanAbs && Math.Abs(all.EdgeMean) <= MaxEdgeMean,
                                name + " over (" + bg.r + "," + bg.g + "," + bg.b + "): composite matches the truth, no rim (mean |error| " +
                                all.MeanAbs.ToString("F2") + " <= " + MaxMeanAbs + ", soft-edge signed mean " + all.EdgeMean.ToString("F2") +
                                ", limit +-" + MaxEdgeMean + ")");
                        }

                        // 3. control: the right clip through a straight-alpha shader (colour multiplied by alpha twice) must show a dark rim
                        CompositeStats control = default;
                        for (int f = 0; f < AlphaClipTruth.Frames.Length; f++)
                        {
                            AlphaClipTruth.ShowFrame(player, AlphaClipTruth.Frames[f]);
                            control.Add(AlphaClipTruth.Measure(RenderComposite(player.Output, straightControl, backgrounds[0]), truth, f, backgrounds[0]));
                        }
                        Report.AppendLine("INFO  " + name + " CONTROL straight-alpha shader over white: soft-edge signed mean " + control.EdgeMean.ToString("F2"));
                        Check(control.EdgeMean < -MinControlEdgeMean,
                            name + ": control, a straight-alpha shader gives a dark rim (soft-edge signed mean " + control.EdgeMean.ToString("F1") + " < -" + MinControlEdgeMean + ")");
                    }
                    finally
                    {
                        DestroyPlayer(player);
                    }

                    // 4. control: the sidecar's premultiplied flag the wrong way round must be seen too
                    string flipped = wantPremul ? meta.Replace("\"premultiplied\": true", "\"premultiplied\": false")
                                                : meta.Replace("\"premultiplied\": false", "\"premultiplied\": true");
                    Check(flipped != meta, name + ": control sidecar differs from the real one");
                    var wrong = NewAlphaPlayer(clip, flipped);
                    try
                    {
                        CompositeStats control = default;
                        for (int f = 0; f < AlphaClipTruth.Frames.Length; f++)
                        {
                            AlphaClipTruth.ShowFrame(wrong, AlphaClipTruth.Frames[f]);
                            control.Add(AlphaClipTruth.Measure(RenderComposite(wrong.Output, scene, backgrounds[0]), truth, f, backgrounds[0]));
                        }
                        Report.AppendLine("INFO  " + name + " CONTROL wrong premultiplied flag over white: soft-edge signed mean " + control.EdgeMean.ToString("F2"));
                        Check(Math.Abs(control.EdgeMean) > MinControlEdgeMean,
                            name + ": control, the wrong premultiplied flag is seen (soft-edge signed mean " + control.EdgeMean.ToString("F1") + ", |.| > " + MinControlEdgeMean + ")");
                    }
                    finally
                    {
                        DestroyPlayer(wrong);
                    }
                }
                SidecarApiTests(Array.Find(clips, c => Path.GetFileName(c).Contains("native")) ?? clips[0]);
            }
            finally
            {
                UnityEngine.Object.DestroyImmediate(scene);
                UnityEngine.Object.DestroyImmediate(straightControl);
            }
        }

        /// <summary>Sidecar handling of the component: errors surface as exceptions with the right status and leave nothing open; an explicit sidecar wins.</summary>
        private static void SidecarApiTests(string clip)
        {
            int opened0 = AlphaClipPlayer.NativeOpenCount, closed0 = AlphaClipPlayer.NativeCloseCount;
            var go = new GameObject("alphaclip-sidecar") { hideFlags = HideFlags.HideAndDontSave };
            var p = go.AddComponent<AlphaClipPlayer>();
            try
            {
                foreach (var c in new[] { ("{\"schema\":1,", AcStatus.ErrFormat), ("{\"schema\":2,\"alpha_mode\":\"opaque\"}", AcStatus.ErrUnsupported),
                                          ("{\"schema\":1,\"alpha_mode\":\"keyed\"}", AcStatus.ErrUnsupported),
                                          ("{\"schema\":1,\"alpha_mode\":\"packed_right\"}", AcStatus.ErrFormat) })
                {
                    try
                    {
                        p.OpenFile(clip, c.Item1);
                        Check(false, "a bad sidecar is refused: " + c.Item1);
                    }
                    catch (AlphaClipException e)
                    {
                        Check(e.Status == c.Item2 && !p.IsOpen, "a bad sidecar is refused with " + c.Item2 + ", nothing stays open (" + e.Message + ")");
                    }
                }
                p.OpenFile(clip, "{\"schema\":1,\"alpha_mode\":\"opaque\"}");
                AlphaClipInfo info = p.Info;
                Check(info.AlphaMode == AlphaClipAlphaMode.Opaque && !info.HasAlpha && !info.HasAlphaPlane && info.ContentWidth == info.Width,
                    "an explicit opaque sidecar wins over the sidecar file and turns the alpha off");
                p.OpenFile(clip, "");
                Check(p.Info.AlphaMode == AlphaClipAlphaMode.NativeAlpha || p.Info.AlphaMode == AlphaClipAlphaMode.Opaque,
                    "an empty sidecar string means none: the core's auto mode (" + p.Info.AlphaMode + ")");
                p.Close();
            }
            finally
            {
                UnityEngine.Object.DestroyImmediate(go);
            }
            Check(AlphaClipPlayer.NativeOpenCount - opened0 == AlphaClipPlayer.NativeCloseCount - closed0,
                "sidecar tests: every successful open was closed");
            Check(AlphaClipPlayer.SidecarUri("jar:file:///data/app/x/base.apk!/assets/clips/a.b.webm") == "jar:file:///data/app/x/base.apk!/assets/clips/a.b.acmeta.json" &&
                  AlphaClipPlayer.SidecarUri("http://h.example/dir.v2/clip.webm?token=1#x") == "http://h.example/dir.v2/clip.acmeta.json?token=1#x" &&
                  AlphaClipPlayer.SidecarUri("C:\\clips\\noext") == "C:\\clips\\noext.acmeta.json",
                "the sidecar URI replaces the extension of the last path segment and keeps a query string");
        }

        private static AlphaClipPlayer NewAlphaPlayer(string clip, string metaJson)
        {
            var go = new GameObject("alphaclip-alpha-test") { hideFlags = HideFlags.HideAndDontSave };
            var player = go.AddComponent<AlphaClipPlayer>();
            player.QueueDepth = 64;
            player.Loop = false;
            player.Matrix = ColorMatrix.BT709;   // the sidecars say BT.601 and must win
            player.FullRange = true;             // ... and limited range
            player.OpenFile(clip, metaJson);
            return player;
        }

        private static void DestroyPlayer(AlphaClipPlayer player)
        {
            player.Close();   // edit mode: OnDestroy is not sent
            UnityEngine.Object.DestroyImmediate(player.gameObject);
        }

        /// <summary>Draws the picture on a full-frame quad in front of a solid background colour, as an ordinary scene would, and reads the camera's target back.</summary>
        private static byte[] RenderComposite(RenderTexture picture, Material material, Color32 background)
        {
            int w = picture.width, h = picture.height;
            GameObject cameraGo = null, quad = null;
            RenderTexture target = null;
            try
            {
                cameraGo = new GameObject("alphaclip-camera");
                var cam = cameraGo.AddComponent<Camera>();
                cam.orthographic = true;
                cam.orthographicSize = 0.5f;          // the quad is 1 unit high and w/h wide: one texel per pixel
                cam.nearClipPlane = 0.1f;
                cam.farClipPlane = 10f;
                cam.clearFlags = CameraClearFlags.SolidColor;
                cam.backgroundColor = background;
                cam.enabled = false;
                cameraGo.transform.position = new Vector3(0, 0, -1);

                quad = GameObject.CreatePrimitive(PrimitiveType.Quad);
                UnityEngine.Object.DestroyImmediate(quad.GetComponent<Collider>());
                quad.transform.localScale = new Vector3((float)w / h, 1f, 1f);
                material.mainTexture = picture;
                quad.GetComponent<MeshRenderer>().sharedMaterial = material;

                target = new RenderTexture(w, h, 24, RenderTextureFormat.ARGB32, RenderTextureReadWrite.Default);
                cam.targetTexture = target;
                cam.Render();
                return ReadBack(target);
            }
            finally
            {
                if (target != null) { target.Release(); UnityEngine.Object.DestroyImmediate(target); }
                if (quad != null) UnityEngine.Object.DestroyImmediate(quad);
                if (cameraGo != null) UnityEngine.Object.DestroyImmediate(cameraGo);
            }
        }
    }
}
