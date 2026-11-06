using System;
using System.Collections;
using System.IO;
using System.Runtime.InteropServices;
using UnityEngine;
using UnityEngine.Networking;
using Object = UnityEngine.Object;
using Stopwatch = System.Diagnostics.Stopwatch;   // not `using System.Diagnostics`: its Debug clashes with UnityEngine.Debug

namespace AlphaClip
{
    /// <summary>
    /// How the YUV picture maps to RGB. A sidecar that names a matrix overrides the component's setting
    /// (<see cref="AlphaClipPlayer.EffectiveMatrix"/>); without one the setting is used.
    /// </summary>
    public enum ColorMatrix
    {
        BT601 = 0,
        BT709 = 1,
    }

    /// <summary>
    /// Where the half-resolution chroma samples sit relative to the luma samples. Left (MPEG-2, H.26x and VP9's default)
    /// is right for almost all video; Center is what a plain bilinear lookup assumes (JPEG/MPEG-1 style).
    /// </summary>
    public enum ChromaSiting
    {
        Left = 0,
        Center = 1,
    }

    /// <summary>Totals of the main-thread time spent in AlphaClipPlayer.Present, in microseconds (see GetPresentTimes).</summary>
    public readonly struct PresentTimes
    {
        /// <summary>Calls of Present (one per Unity frame while playing).</summary>
        public readonly long Calls;
        /// <summary>Calls that produced a new picture.</summary>
        public readonly long Frames;
        public readonly double AcquireUs, UploadUs, DrawUs;

        public PresentTimes(long calls, long frames, double acquireUs, double uploadUs, double drawUs)
        {
            Calls = calls;
            Frames = frames;
            AcquireUs = acquireUs;
            UploadUs = uploadUs;
            DrawUs = drawUs;
        }

        /// <summary>Upload + draw per new picture, in milliseconds.</summary>
        public double UploadDrawMsPerFrame => Frames > 0 ? (UploadUs + DrawUs) / Frames / 1000.0 : 0;
    }

    /// <summary>How a clip stores its transparency (the sidecar's "alpha_mode"; a clip without a sidecar is Opaque, or NativeAlpha when the file has an alpha stream).</summary>
    public enum AlphaClipAlphaMode
    {
        Opaque = 0,
        /// <summary>The picture on the left, its alpha as a grey image (luma) on the right.</summary>
        PackedRight = 1,
        /// <summary>The picture on top, its alpha as a grey image below.</summary>
        PackedBottom = 2,
        /// <summary>WebM alpha stream (BlockAdditional): the core returns it as a fourth plane.</summary>
        NativeAlpha = 3,
    }

    public readonly struct AlphaClipInfo
    {
        /// <summary>Size of the coded video: the frames the core returns (and the planes uploaded as textures).</summary>
        public readonly int Width, Height;
        /// <summary>Size of the picture: <see cref="AlphaClipPlayer.Output"/> has this size. Differs from Width/Height only in the packed modes.</summary>
        public readonly int ContentWidth, ContentHeight;
        public readonly double DurationSeconds;
        public readonly float Fps;
        /// <summary>True when <see cref="AlphaClipPlayer.Output"/> carries transparency (any mode except Opaque).</summary>
        public readonly bool HasAlpha;
        /// <summary>True when the core delivers a fourth (alpha) plane with every frame: NativeAlpha only.</summary>
        public readonly bool HasAlphaPlane;
        public readonly AlphaClipAlphaMode AlphaMode;
        /// <summary>The sidecar says the colour in the video is already multiplied by alpha.</summary>
        public readonly bool Premultiplied;
        /// <summary>What the sidecar says: 0 = not given (the component's setting applies), 1 = limited, 2 = full.</summary>
        public readonly int ColorRange;
        /// <summary>What the sidecar says: 0 = not given (the component's setting applies), 1 = BT.601, 2 = BT.709.</summary>
        public readonly int Matrix;
        /// <summary>The sidecar asks for looping (informational; <see cref="AlphaClipPlayer.Loop"/> decides).</summary>
        public readonly bool LoopHint;
        public readonly long PacketCount;
        public readonly long LoopPeriodUs;

        internal AlphaClipInfo(AcInfo i)
        {
            Width = i.Width;
            Height = i.Height;
            ContentWidth = i.ContentW;
            ContentHeight = i.ContentH;
            DurationSeconds = i.DurationUs / 1e6;
            Fps = i.Fps;
            HasAlpha = i.AlphaMode != 0;
            HasAlphaPlane = i.HasAlphaPlane != 0;
            AlphaMode = (AlphaClipAlphaMode)i.AlphaMode;
            Premultiplied = i.Premultiplied != 0;
            ColorRange = i.ColorRange;
            Matrix = i.Matrix;
            LoopHint = i.LoopHint != 0;
            PacketCount = i.PacketCount;
            LoopPeriodUs = i.LoopPeriodUs;
        }
    }

    public readonly struct AlphaClipStats
    {
        public readonly long FramesDecoded, FramesAcquired, FramesDropped, DecodeTimeUs;
        public readonly int FramesQueued;
        public readonly bool ReachedEnd;

        internal AlphaClipStats(AcStats s)
        {
            FramesDecoded = s.FramesDecoded;
            FramesAcquired = s.FramesAcquired;
            FramesDropped = s.FramesDropped;
            DecodeTimeUs = s.DecodeTimeUs;
            FramesQueued = s.FramesQueued;
            ReachedEnd = s.ReachedEnd != 0;
        }
    }

    /// <summary>
    /// Plays a VP9/WebM clip with the AlphaClip core. The core decodes on its own thread; every Unity frame this
    /// component asks it for the newest frame that is due, uploads the Y/U/V planes as R8 textures and converts
    /// them to RGB with a shader into <see cref="Output"/>, an ordinary RenderTexture that any material, RawImage
    /// or camera can use. A clip with transparency (native alpha stream, or a picture with its alpha packed beside it,
    /// described by the clip's sidecar) produces PREMULTIPLIED RGBA: show it with a premultiplied blend
    /// (Blend One OneMinusSrcAlpha), e.g. the AlphaClip/UnlitPremultiplied and AlphaClip/UIPremultiplied shaders
    /// (see <see cref="AlphaClipMaterials"/>), not with a straight-alpha shader.
    /// </summary>
    [DisallowMultipleComponent]
    public sealed class AlphaClipPlayer : MonoBehaviour
    {
        [Header("Source")]
        [Tooltip("WebM file under StreamingAssets (editor and standalone). Ignored when Clip Asset is set. " +
                 "A sidecar with the same name and the extension .acmeta.json next to it is picked up automatically.")]
        [SerializeField] private string streamingAssetsPath = "";
        [Tooltip("A TextAsset holding the WebM bytes (rename the file to .bytes). Works in asset bundles.")]
        [SerializeField] private TextAsset clipAsset;
        [Tooltip("The sidecar (clip.acmeta.json) for Clip Asset, as a TextAsset. Leave empty for a clip without a sidecar.")]
        [SerializeField] private TextAsset metaAsset;

        [Header("Playback")]
        [SerializeField] private bool playOnEnable = true;
        [SerializeField] private bool loop = true;
        [Tooltip("Decoded frames the core may keep ready (1..64).")]
        [SerializeField, Range(1, 64)] private int queueDepth = 4;

        [Header("Colour (used when the sidecar does not say; a sidecar's matrix and range win)")]
        [SerializeField] private ColorMatrix matrix = ColorMatrix.BT709;
        [Tooltip("Off: limited (TV) range 16-235, the usual case for video.")]
        [SerializeField] private bool fullRange = false;
        [Tooltip("Left is the video standard (MPEG-2 / H.26x / VP9). Center is for chroma that sits mid-block.")]
        [SerializeField] private ChromaSiting chromaSiting = ChromaSiting.Left;

        private static readonly int UTexId = Shader.PropertyToID("_UTex");
        private static readonly int VTexId = Shader.PropertyToID("_VTex");
        private static readonly int ATexId = Shader.PropertyToID("_ATex");
        private static readonly int AlphaModeId = Shader.PropertyToID("_AlphaMode");
        private static readonly int PremultipliedId = Shader.PropertyToID("_Premultiplied");
        private static readonly int ContentRectId = Shader.PropertyToID("_ContentRect");
        private static readonly int ChromaClampId = Shader.PropertyToID("_ChromaClamp");
        private static readonly int YuvRangeId = Shader.PropertyToID("_YuvRange");
        private static readonly int YuvCoeffId = Shader.PropertyToID("_YuvCoeff");
        private static readonly int ChromaShiftId = Shader.PropertyToID("_ChromaShift");
        private static readonly int LinearOutputId = Shader.PropertyToID("_LinearOutput");

        private IntPtr m_player;
        private IntPtr m_nativeData;   // the core does not copy the bytes of ac_open_memory: we own them until ac_close
        private AcInfo m_info;
        private Texture2D m_y, m_u, m_v, m_a;   // m_a: the alpha plane, native_alpha clips only
        private Vector4 m_contentRect, m_chromaClamp;
        private RenderTexture m_output;
        private Material m_material;
        private bool m_playing;
        private bool m_loading;
        private bool m_ended;
        private double m_startRealtime;
        // Play() and Seek() are usually called in the middle of a frame; the rest of that frame (rendering, a first-use
        // shader compile, scene loading) can take a long time. The clock therefore starts counting at the next Update
        // instead of at the call, so a start-up hitch does not make the first part of the clip fast-forward.
        private bool m_anchorPending;
        private long m_baseClockUs;
        private long m_lastPtsUs = -1;
        private long m_framesPresented;
        private long m_timeCalls, m_timeFrames, m_timeAcquireTicks, m_timeUploadTicks, m_timeDrawTicks;

        // ---- public API ----------------------------------------------------------------------------------------

        /// <summary>The RGB picture. Null until a clip is open.</summary>
        public RenderTexture Output => m_output;
        public bool IsOpen => m_player != IntPtr.Zero;
        public bool IsPlaying => m_playing;
        /// <summary>True once the stream finished (never when looping).</summary>
        public bool Ended => m_ended;
        public AlphaClipInfo Info => new AlphaClipInfo(m_info);
        /// <summary>Presentation time of the frame currently in <see cref="Output"/>, or -1.</summary>
        public long LastPtsUs => m_lastPtsUs;
        public long FramesPresented => m_framesPresented;

        /// <summary>Path of the WebM under StreamingAssets (an absolute path also works). Used by OnEnable when no clip is open.</summary>
        public string StreamingAssetsPath { get => streamingAssetsPath; set => streamingAssetsPath = value; }
        /// <summary>A TextAsset holding the WebM bytes; wins over <see cref="StreamingAssetsPath"/>.</summary>
        public TextAsset ClipAsset { get => clipAsset; set => clipAsset = value; }
        /// <summary>Open the configured source and start playing as soon as the component is enabled.</summary>
        public bool PlayOnEnable { get => playOnEnable; set => playOnEnable = value; }

        public bool Loop
        {
            get => loop;
            set
            {
                loop = value;
                if (IsOpen)
                {
                    AcNative.SetLoop(m_player, loop);
                }
            }
        }

        /// <summary>The sidecar (clip.acmeta.json) that goes with <see cref="ClipAsset"/>; used by <see cref="Open(TextAsset, string)"/> and <see cref="Open(byte[], string)"/> when no sidecar text is passed.</summary>
        public TextAsset MetaAsset { get => metaAsset; set => metaAsset = value; }

        public ColorMatrix Matrix { get => matrix; set => matrix = value; }
        public bool FullRange { get => fullRange; set => fullRange = value; }

        /// <summary>The matrix the shader uses: the sidecar's when it names one, otherwise <see cref="Matrix"/>.</summary>
        public ColorMatrix EffectiveMatrix => m_info.Matrix == 2 ? ColorMatrix.BT709 : m_info.Matrix == 1 ? ColorMatrix.BT601 : matrix;
        /// <summary>The range the shader uses: the sidecar's when it names one, otherwise <see cref="FullRange"/>.</summary>
        public bool EffectiveFullRange => m_info.ColorRange == 2 || (m_info.ColorRange != 1 && fullRange);
        public ChromaSiting ChromaSiting { get => chromaSiting; set => chromaSiting = value; }
        public int QueueDepth { get => queueDepth; set => queueDepth = Mathf.Clamp(value, 1, 64); }

        /// <summary>The media clock, in microseconds, that <see cref="Update"/> hands to the core.</summary>
        public long CurrentClockUs => m_playing && !m_anchorPending
            ? m_baseClockUs + (long)((Time.unscaledTimeAsDouble - m_startRealtime) * 1e6)
            : m_baseClockUs;

        /// <summary>Version string of the loaded native core. Throws if the plugin cannot be loaded.</summary>
        public static string NativeVersion()
        {
            AcNative.CheckLayout();
            return AcNative.Version();
        }

        /// <summary>Counters of native opens/closes: after every player is destroyed they must be equal.</summary>
        public static int NativeOpenCount => AcNative.OpenedCount;
        public static int NativeCloseCount => AcNative.ClosedCount;

        /// <summary>File extension of the sidecar that describes a clip: <c>clip.webm</c> goes with <c>clip.acmeta.json</c>.</summary>
        public const string SidecarExtension = ".acmeta.json";

        /// <summary>
        /// Opens a WebM file. <paramref name="metaJson"/> is the sidecar text; null looks for <c>&lt;path without extension&gt;.acmeta.json</c>
        /// next to the file and uses it when it exists; "" forces "no sidecar". Without a sidecar the clip plays as a plain
        /// picture, or with its alpha stream when the file has one (see the core's "auto" mode).
        /// Throws <see cref="AlphaClipException"/> (a malformed or unsupported sidecar is ErrFormat / ErrUnsupported).
        /// </summary>
        public void OpenFile(string path, string metaJson = null)
        {
            Close();
            try
            {
                if (metaJson == null)
                {
                    string sidecar = Path.ChangeExtension(path, SidecarExtension);
                    metaJson = File.Exists(sidecar) ? File.ReadAllText(sidecar, System.Text.Encoding.UTF8) : null;
                }
                Initialize(AcNative.OpenFile(path, metaJson));
            }
            catch
            {
                Close();
                throw;
            }
        }

        /// <summary>Opens WebM bytes. <paramref name="metaJson"/> is the sidecar text; null uses <see cref="MetaAsset"/> if one is set, "" forces none.</summary>
        public void Open(byte[] webm, string metaJson = null)
        {
            if (webm == null || webm.Length == 0)
            {
                throw new ArgumentException("empty clip data", nameof(webm));
            }
            Close();
            try
            {
                metaJson ??= metaAsset != null ? metaAsset.text : null;
                m_nativeData = Marshal.AllocHGlobal(webm.Length);
                Marshal.Copy(webm, 0, m_nativeData, webm.Length);
                Initialize(AcNative.OpenMemory(m_nativeData, webm.Length, metaJson));
            }
            catch
            {
                Close();
                throw;
            }
        }

        public void Open(TextAsset asset, string metaJson = null)
        {
            Open(asset.bytes, metaJson);
        }

        /// <summary>True while <see cref="OpenUriAsync"/> is still reading the clip.</summary>
        public bool IsLoading => m_loading;
        /// <summary>Why the last <see cref="OpenUriAsync"/> failed (null after a success).</summary>
        public string LoadError { get; private set; }

        /// <summary>
        /// Reads the clip through UnityWebRequest and opens it. This is how StreamingAssets are read on Android, where the
        /// files are inside the APK ("jar:file://...!/assets/...") and ordinary file APIs cannot see them; any URI that
        /// UnityWebRequest takes works (file://, jar:file://, http://). Use it as a coroutine
        /// (<c>yield return player.OpenUriAsync(uri)</c>) and check <see cref="IsOpen"/> or <see cref="LoadError"/> afterwards;
        /// a failure is logged and stored, not thrown, because exceptions cannot cross a coroutine.
        /// The whole file is read into memory first (the core reads from memory anyway), so memory briefly holds two
        /// copies; <see cref="Open(byte[])"/> keeps one native copy for the life of the clip.
        /// </summary>
        public IEnumerator OpenUriAsync(string uri, string metaJson = null)
        {
            LoadError = null;
            m_loading = true;
            byte[] data = null;
            using (UnityWebRequest request = UnityWebRequest.Get(uri))
            {
                yield return request.SendWebRequest();
                if (request.result == UnityWebRequest.Result.Success)
                {
                    data = request.downloadHandler.data;
                }
                else
                {
                    LoadError = "cannot read " + uri + ": " + request.error;
                }
            }
            if (data != null && metaJson == null)
            {
                // The sidecar next to the clip. A clip without one is fine: any failure to read it means "no sidecar".
                using (UnityWebRequest request = UnityWebRequest.Get(SidecarUri(uri)))
                {
                    yield return request.SendWebRequest();
                    if (request.result == UnityWebRequest.Result.Success)
                    {
                        metaJson = request.downloadHandler.text;
                    }
                }
            }
            m_loading = false;
            if (data != null)
            {
                try
                {
                    Open(data, metaJson);
                }
                catch (Exception e)
                {
                    LoadError = "cannot open " + uri + ": " + e.Message;
                }
            }
            if (LoadError != null)
            {
                Debug.LogError("AlphaClipPlayer: " + LoadError, this);
            }
        }

        /// <summary>
        /// <see cref="OpenUriAsync"/> for a path under StreamingAssets (works on every platform, including Android).
        /// A string that already contains "://" is used as the URI as it is.
        /// </summary>
        public IEnumerator OpenStreamingAssetsAsync(string relativePath)
        {
            return OpenUriAsync(ResolveStreamingAssets(relativePath));
        }

        private static string ResolveStreamingAssets(string relativePath)
        {
            return relativePath.Contains("://") ? relativePath : Path.Combine(Application.streamingAssetsPath, relativePath);
        }

        /// <summary>The URI of the sidecar that belongs to a clip URI: the extension of the last path segment becomes .acmeta.json (a query string or fragment stays).</summary>
        public static string SidecarUri(string uri)
        {
            int tail = uri.IndexOfAny(new[] { '?', '#' });
            string path = tail < 0 ? uri : uri.Substring(0, tail);
            string rest = tail < 0 ? "" : uri.Substring(tail);
            int slash = Mathf.Max(path.LastIndexOf('/'), path.LastIndexOf('\\'));
            int dot = path.LastIndexOf('.');
            if (dot > slash)
            {
                path = path.Substring(0, dot);
            }
            return path + SidecarExtension + rest;
        }

        public void Play()
        {
            if (!IsOpen)
            {
                throw new InvalidOperationException("AlphaClipPlayer: no clip is open");
            }
            if (m_playing)
            {
                return;   // already running: do not rewind the clock to the last base position
            }
            m_startRealtime = Time.unscaledTimeAsDouble;
            m_anchorPending = true;
            m_playing = true;
        }

        public void Pause()
        {
            m_baseClockUs = CurrentClockUs;
            m_playing = false;
        }

        /// <summary>Jumps to a position (seconds). The next presented frame is the one shown at that time.</summary>
        public void Seek(double seconds)
        {
            if (!IsOpen)
            {
                throw new InvalidOperationException("AlphaClipPlayer: no clip is open");
            }
            long us = (long)(seconds * 1e6);
            AcNative.Seek(m_player, us);
            m_baseClockUs = us;
            m_startRealtime = Time.unscaledTimeAsDouble;
            m_anchorPending = true;
            m_lastPtsUs = -1;
            m_ended = false;
        }

        /// <summary>
        /// Asks the core for the newest frame due at <paramref name="clockUs"/> and, if there is one, converts it into
        /// <see cref="Output"/>. Returns true when a new picture was produced. Update() calls this with the real-time
        /// clock; call it yourself to drive the player from another clock (audio, a test, a timeline).
        /// </summary>
        public bool Present(long clockUs)
        {
            if (!IsOpen)
            {
                return false;
            }
            long t0 = Stopwatch.GetTimestamp();
            AcStatus status = AcNative.AcquireFrame(m_player, clockUs, out AcFrame frame);
            long t1 = Stopwatch.GetTimestamp();
            m_timeCalls++;
            m_timeAcquireTicks += t1 - t0;
            if (status == AcStatus.End)
            {
                m_ended = true;
                return false;
            }
            if (status != AcStatus.Ok)
            {
                return false;   // nothing newer is due: keep showing the previous picture
            }
            try
            {
                Upload(ref frame);
                long t2 = Stopwatch.GetTimestamp();
                Draw();
                long t3 = Stopwatch.GetTimestamp();
                m_timeUploadTicks += t2 - t1;
                m_timeDrawTicks += t3 - t2;
                m_timeFrames++;
                m_lastPtsUs = frame.PtsUs;
                m_framesPresented++;
            }
            finally
            {
                AcNative.ReleaseFrame(m_player, ref frame);
            }
            return true;
        }

        /// <summary>
        /// Main-thread CPU time spent in <see cref="Present"/> since the last <see cref="ResetPresentTimes"/> (or since the
        /// clip was opened): taking a frame from the core, copying the three planes into textures, and submitting the
        /// YUV-to-RGB draw. It is time on the calling thread, not GPU time.
        /// </summary>
        public PresentTimes GetPresentTimes()
        {
            double us = 1e6 / Stopwatch.Frequency;
            return new PresentTimes(m_timeCalls, m_timeFrames, m_timeAcquireTicks * us, m_timeUploadTicks * us, m_timeDrawTicks * us);
        }

        public void ResetPresentTimes()
        {
            m_timeCalls = m_timeFrames = 0;
            m_timeAcquireTicks = m_timeUploadTicks = m_timeDrawTicks = 0;
        }

        public AlphaClipStats GetStats()
        {
            return new AlphaClipStats(AcNative.GetStats(m_player));
        }

        /// <summary>Closes the clip: stops the decode thread, frees the native player and every GPU object.</summary>
        public void Close()
        {
            m_playing = false;
            m_ended = false;
            if (m_player != IntPtr.Zero)
            {
                AcNative.Close(m_player);   // joins the decode thread: after this the core no longer reads m_nativeData
                m_player = IntPtr.Zero;
            }
            if (m_nativeData != IntPtr.Zero)
            {
                Marshal.FreeHGlobal(m_nativeData);
                m_nativeData = IntPtr.Zero;
            }
            SafeDestroy(ref m_y);
            SafeDestroy(ref m_u);
            SafeDestroy(ref m_v);
            SafeDestroy(ref m_a);
            SafeDestroy(ref m_material);
            m_info = default;   // the sidecar's matrix / range no longer apply
            if (m_output != null)
            {
                m_output.Release();
                SafeDestroy(ref m_output);
            }
            m_baseClockUs = 0;
            m_lastPtsUs = -1;
            m_framesPresented = 0;
            ResetPresentTimes();
        }

        // ---- Unity messages ------------------------------------------------------------------------------------

        private void OnEnable()
        {
            if (playOnEnable && !IsOpen && !m_loading && (clipAsset != null || !string.IsNullOrEmpty(streamingAssetsPath)))
            {
                try
                {
                    if (clipAsset != null)
                    {
                        Open(clipAsset);
                    }
                    else
                    {
                        string source = ResolveStreamingAssets(streamingAssetsPath);
                        if (source.Contains("://"))
                        {
                            // Android (jar:file://...) or an explicit URI: needs UnityWebRequest, so the clip opens a few frames later
                            StartCoroutine(OpenUriAndPlay(source));
                            return;
                        }
                        OpenFile(source);
                    }
                    Play();
                }
                catch (Exception e)
                {
                    Debug.LogError("AlphaClipPlayer: cannot open the clip: " + e.Message, this);
                }
            }
        }

        private void OnDisable()
        {
            m_loading = false;   // a coroutine that was still reading stops with the component being disabled
        }

        private IEnumerator OpenUriAndPlay(string uri)
        {
            yield return OpenUriAsync(uri);
            if (IsOpen)
            {
                Play();
            }
        }

        private void Update()
        {
            if (!m_playing)
            {
                return;
            }
            if (m_anchorPending)
            {
                m_startRealtime = Time.unscaledTimeAsDouble;
                m_anchorPending = false;
            }
            try
            {
                Present(CurrentClockUs);
            }
            catch (AlphaClipException e)
            {
                Debug.LogError("AlphaClipPlayer: " + e.Message, this);
                Pause();
            }
        }

        private void OnDestroy()
        {
            Close();
        }

        // ---- internals -----------------------------------------------------------------------------------------

        private void Initialize(IntPtr player)
        {
            AcNative.CheckLayout();
            m_player = player;
            m_info = AcNative.GetInfo(player);
            AcNative.SetQueueDepth(player, queueDepth);
            AcNative.SetLoop(player, loop);
            AcNative.Start(player);

            int w = m_info.Width, h = m_info.Height, cw = (w + 1) / 2, ch = (h + 1) / 2;
            m_y = NewPlane(w, h, FilterMode.Point, "AlphaClip Y");
            m_u = NewPlane(cw, ch, FilterMode.Bilinear, "AlphaClip U");
            m_v = NewPlane(cw, ch, FilterMode.Bilinear, "AlphaClip V");
            if (m_info.HasAlphaPlane != 0)
            {
                m_a = NewPlane(w, h, FilterMode.Point, "AlphaClip A");
            }

            // Where the picture (and, in the packed modes, its alpha image) lies in the coded frame, in texture coordinates,
            // and how far the chroma lookup may go: never into the chroma samples of the neighbouring alpha image.
            int pw = m_info.ContentW, ph = m_info.ContentH;
            m_contentRect = new Vector4((float)pw / w, (float)ph / h, (float)m_info.AlphaX / w, (float)m_info.AlphaY / h);
            int pcw = (pw + 1) / 2, pch = (ph + 1) / 2;
            m_chromaClamp = new Vector4(0.5f / cw, 0.5f / ch, (pcw - 0.5f) / cw, (pch - 0.5f) / ch);

            Shader shader = Resources.Load<Shader>("AlphaClip/YuvToRgb");
            if (shader == null)
            {
                throw new InvalidOperationException("AlphaClip: the YUV shader (Resources/AlphaClip/YuvToRgb) was not found");
            }
            m_material = new Material(shader) { hideFlags = HideFlags.HideAndDontSave, name = "AlphaClip YuvToRgb" };

            // Default read/write: an sRGB target in a Linear colour-space project (the shader then outputs linear values).
            // The size is the picture's, not the coded frame's (they differ in the packed modes).
            m_output = new RenderTexture(pw, ph, 0, RenderTextureFormat.ARGB32, RenderTextureReadWrite.Default)
            {
                name = "AlphaClip Output",
                hideFlags = HideFlags.HideAndDontSave,
                useMipMap = false,
                wrapMode = TextureWrapMode.Clamp,
            };
            m_output.Create();

            m_baseClockUs = 0;
            m_lastPtsUs = -1;
            m_framesPresented = 0;
            m_ended = false;
        }

        private static Texture2D NewPlane(int w, int h, FilterMode filter, string name)
        {
            // R8, no mipmaps, linear: the planes are data, not colours, and must never be sRGB-decoded.
            return new Texture2D(w, h, TextureFormat.R8, false, true)
            {
                name = name,
                hideFlags = HideFlags.HideAndDontSave,
                filterMode = filter,
                wrapMode = TextureWrapMode.Clamp,
            };
        }

        private void Upload(ref AcFrame f)
        {
            int w = m_info.Width, h = m_info.Height, cw = (w + 1) / 2, ch = (h + 1) / 2;
            if (f.Width != w || f.Height != h || f.Stride0 != w || f.Stride1 != cw || f.Stride2 != cw)
            {
                throw new AlphaClipException(AcStatus.ErrState, "AlphaClip: unexpected frame geometry from the core");
            }
            // The core hands out tightly packed planes, which is exactly the layout of an R8 texture.
            m_y.LoadRawTextureData(f.Plane0, w * h);
            m_y.Apply(false, false);
            m_u.LoadRawTextureData(f.Plane1, cw * ch);
            m_u.Apply(false, false);
            m_v.LoadRawTextureData(f.Plane2, cw * ch);
            m_v.Apply(false, false);
            if (m_a != null)
            {
                if (f.Plane3 == IntPtr.Zero || f.Stride3 != w)
                {
                    throw new AlphaClipException(AcStatus.ErrState, "AlphaClip: the core delivered no alpha plane for a native_alpha clip");
                }
                m_a.LoadRawTextureData(f.Plane3, w * h);
                m_a.Apply(false, false);
            }
        }

        private void Draw()
        {
            bool full = EffectiveFullRange;
            float offset = full ? 0f : 16f / 255f;
            float lumaScale = full ? 1f : 255f / 219f;
            float chromaScale = full ? 1f : 255f / 224f;
            Vector4 coeff = EffectiveMatrix == ColorMatrix.BT601
                ? new Vector4(1.402f, 0.344136f, 0.714136f, 1.772f)
                : new Vector4(1.5748f, 0.187324f, 0.468124f, 1.8556f);

            m_material.SetTexture(UTexId, m_u);
            m_material.SetTexture(VTexId, m_v);
            if (m_a != null)
            {
                m_material.SetTexture(ATexId, m_a);
            }
            m_material.SetFloat(AlphaModeId, m_info.AlphaMode);
            m_material.SetFloat(PremultipliedId, m_info.Premultiplied);
            m_material.SetVector(ContentRectId, m_contentRect);
            m_material.SetVector(ChromaClampId, m_chromaClamp);
            m_material.SetVector(YuvRangeId, new Vector4(offset, lumaScale, chromaScale, 0f));
            m_material.SetVector(YuvCoeffId, coeff);
            m_material.SetVector(ChromaShiftId, new Vector4(chromaSiting == ChromaSiting.Left ? 0.25f : 0f, 0f, 0f, 0f));
            m_material.SetFloat(LinearOutputId, QualitySettings.activeColorSpace == ColorSpace.Linear ? 1f : 0f);
            Graphics.Blit(m_y, m_output, m_material);   // _MainTex = the Y plane
        }

        private static void SafeDestroy<T>(ref T obj) where T : Object
        {
            if (obj == null)
            {
                return;
            }
            if (Application.isPlaying)
            {
                Destroy(obj);
            }
            else
            {
                DestroyImmediate(obj);
            }
            obj = null;
        }
    }
}
