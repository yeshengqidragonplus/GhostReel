using System;
using System.IO;
using System.Runtime.InteropServices;
using UnityEngine;
using Object = UnityEngine.Object;

namespace AlphaClip
{
    /// <summary>How the YUV picture maps to RGB. M4 moves this into the sidecar metadata; until then it is a setting.</summary>
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

    public readonly struct AlphaClipInfo
    {
        public readonly int Width, Height;
        public readonly double DurationSeconds;
        public readonly float Fps;
        public readonly bool HasAlpha;
        public readonly long PacketCount;
        public readonly long LoopPeriodUs;

        internal AlphaClipInfo(AcInfo i)
        {
            Width = i.Width;
            Height = i.Height;
            DurationSeconds = i.DurationUs / 1e6;
            Fps = i.Fps;
            HasAlpha = i.HasAlphaPlane != 0;
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
    /// or camera can use. M2: opaque video; alpha comes with M4.
    /// </summary>
    [DisallowMultipleComponent]
    public sealed class AlphaClipPlayer : MonoBehaviour
    {
        [Header("Source")]
        [Tooltip("WebM file under StreamingAssets (editor and standalone). Ignored when Clip Asset is set.")]
        [SerializeField] private string streamingAssetsPath = "";
        [Tooltip("A TextAsset holding the WebM bytes (rename the file to .bytes). Works in asset bundles.")]
        [SerializeField] private TextAsset clipAsset;

        [Header("Playback")]
        [SerializeField] private bool playOnEnable = true;
        [SerializeField] private bool loop = true;
        [Tooltip("Decoded frames the core may keep ready (1..64).")]
        [SerializeField, Range(1, 64)] private int queueDepth = 4;

        [Header("Colour (moves into the sidecar in M4)")]
        [SerializeField] private ColorMatrix matrix = ColorMatrix.BT709;
        [Tooltip("Off: limited (TV) range 16-235, the usual case for video.")]
        [SerializeField] private bool fullRange = false;
        [Tooltip("Left is the video standard (MPEG-2 / H.26x / VP9). Center is for chroma that sits mid-block.")]
        [SerializeField] private ChromaSiting chromaSiting = ChromaSiting.Left;

        private static readonly int UTexId = Shader.PropertyToID("_UTex");
        private static readonly int VTexId = Shader.PropertyToID("_VTex");
        private static readonly int YuvRangeId = Shader.PropertyToID("_YuvRange");
        private static readonly int YuvCoeffId = Shader.PropertyToID("_YuvCoeff");
        private static readonly int ChromaShiftId = Shader.PropertyToID("_ChromaShift");
        private static readonly int LinearOutputId = Shader.PropertyToID("_LinearOutput");

        private IntPtr m_player;
        private IntPtr m_nativeData;   // the core does not copy the bytes of ac_open_memory: we own them until ac_close
        private AcInfo m_info;
        private Texture2D m_y, m_u, m_v;
        private RenderTexture m_output;
        private Material m_material;
        private bool m_playing;
        private bool m_ended;
        private double m_startRealtime;
        private long m_baseClockUs;
        private long m_lastPtsUs = -1;
        private long m_framesPresented;

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

        public ColorMatrix Matrix { get => matrix; set => matrix = value; }
        public bool FullRange { get => fullRange; set => fullRange = value; }
        public ChromaSiting ChromaSiting { get => chromaSiting; set => chromaSiting = value; }
        public int QueueDepth { get => queueDepth; set => queueDepth = Mathf.Clamp(value, 1, 64); }

        /// <summary>The media clock, in microseconds, that <see cref="Update"/> hands to the core.</summary>
        public long CurrentClockUs => m_playing
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

        public void OpenFile(string path)
        {
            Close();
            try
            {
                Initialize(AcNative.OpenFile(path));
            }
            catch
            {
                Close();
                throw;
            }
        }

        public void Open(byte[] webm)
        {
            if (webm == null || webm.Length == 0)
            {
                throw new ArgumentException("empty clip data", nameof(webm));
            }
            Close();
            try
            {
                m_nativeData = Marshal.AllocHGlobal(webm.Length);
                Marshal.Copy(webm, 0, m_nativeData, webm.Length);
                Initialize(AcNative.OpenMemory(m_nativeData, webm.Length));
            }
            catch
            {
                Close();
                throw;
            }
        }

        public void Open(TextAsset asset)
        {
            Open(asset.bytes);
        }

        public void Play()
        {
            if (!IsOpen)
            {
                throw new InvalidOperationException("AlphaClipPlayer: no clip is open");
            }
            m_startRealtime = Time.unscaledTimeAsDouble;
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
            AcStatus status = AcNative.AcquireFrame(m_player, clockUs, out AcFrame frame);
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
                Draw();
                m_lastPtsUs = frame.PtsUs;
                m_framesPresented++;
            }
            finally
            {
                AcNative.ReleaseFrame(m_player, ref frame);
            }
            return true;
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
            SafeDestroy(ref m_material);
            if (m_output != null)
            {
                m_output.Release();
                SafeDestroy(ref m_output);
            }
            m_baseClockUs = 0;
            m_lastPtsUs = -1;
            m_framesPresented = 0;
        }

        // ---- Unity messages ------------------------------------------------------------------------------------

        private void OnEnable()
        {
            if (playOnEnable && !IsOpen && (clipAsset != null || !string.IsNullOrEmpty(streamingAssetsPath)))
            {
                try
                {
                    if (clipAsset != null)
                    {
                        Open(clipAsset);
                    }
                    else
                    {
                        OpenFile(Path.Combine(Application.streamingAssetsPath, streamingAssetsPath));
                    }
                    Play();
                }
                catch (Exception e)
                {
                    Debug.LogError("AlphaClipPlayer: cannot open the clip: " + e.Message, this);
                }
            }
        }

        private void Update()
        {
            if (!m_playing)
            {
                return;
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

            Shader shader = Resources.Load<Shader>("AlphaClip/YuvToRgb");
            if (shader == null)
            {
                throw new InvalidOperationException("AlphaClip: the YUV shader (Resources/AlphaClip/YuvToRgb) was not found");
            }
            m_material = new Material(shader) { hideFlags = HideFlags.HideAndDontSave, name = "AlphaClip YuvToRgb" };

            // Default read/write: an sRGB target in a Linear colour-space project (the shader then outputs linear values).
            m_output = new RenderTexture(w, h, 0, RenderTextureFormat.ARGB32, RenderTextureReadWrite.Default)
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
        }

        private void Draw()
        {
            float offset = fullRange ? 0f : 16f / 255f;
            float lumaScale = fullRange ? 1f : 255f / 219f;
            float chromaScale = fullRange ? 1f : 255f / 224f;
            Vector4 coeff = matrix == ColorMatrix.BT601
                ? new Vector4(1.402f, 0.344136f, 0.714136f, 1.772f)
                : new Vector4(1.5748f, 0.187324f, 0.468124f, 1.8556f);

            m_material.SetTexture(UTexId, m_u);
            m_material.SetTexture(VTexId, m_v);
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
