using System;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

namespace AlphaClip
{
    /// <summary>Mirror of ac_status in include/alphaclip.h. The numeric values are part of the ABI.</summary>
    public enum AcStatus
    {
        Ok = 0,
        Again = 1,
        ErrIo = 2,
        ErrFormat = 3,
        ErrUnsupported = 4,
        ErrDecode = 5,
        ErrState = 6,
        ErrOom = 7,
        ErrArg = 8,
        End = 9,
    }

    public sealed class AlphaClipException : Exception
    {
        public AcStatus Status { get; }

        public AlphaClipException(AcStatus status, string message) : base(message)
        {
            Status = status;
        }
    }

    // The three structs cross the ABI by layout. The C++ side locks it with static_asserts
    // (core/src/api.cpp: 80 / 72 / 40 bytes on 64-bit targets); AcNative.CheckLayout() verifies it again at runtime.
    [StructLayout(LayoutKind.Sequential)]
    internal struct AcInfo
    {
        public int Width, Height, ContentW, ContentH;
        public long DurationUs;
        public float Fps;
        public int HasAlphaPlane;
        public long PacketCount;
        public long LoopPeriodUs;
        public int AlphaMode;       // ac_alpha_mode: 0 opaque, 1 packed_right, 2 packed_bottom, 3 native_alpha
        public int Premultiplied;
        public int ColorRange;      // 0 unspecified, 1 limited, 2 full
        public int Matrix;          // 0 unspecified, 1 bt601, 2 bt709
        public int LoopHint;
        public int AlphaX, AlphaY;  // packed modes: top-left of the alpha image in the coded frame
        public int Reserved;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct AcFrame
    {
        public IntPtr Plane0, Plane1, Plane2, Plane3;   // Y, U, V, A (Plane3 is null without alpha)
        public int Stride0, Stride1, Stride2, Stride3;
        public int Width, Height;
        public long PtsUs;
        public uint Flags;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct AcStats
    {
        public long FramesDecoded, FramesAcquired, FramesDropped, DecodeTimeUs;
        public int FramesQueued, ReachedEnd;
    }

    /// <summary>P/Invoke declarations for the AlphaClip core (alphaclip.dll) and thin checked wrappers.</summary>
    internal static class AcNative
    {
        private const string Lib = "alphaclip";

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] private static extern IntPtr ac_version();
        // metaJson: a NUL-terminated UTF-8 byte array, or null for "no sidecar" (marshalled as a pointer to the array / NULL).
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] private static extern int ac_open_memory(IntPtr data, UIntPtr len, byte[] metaJson, out IntPtr player);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] private static extern int ac_open_file(byte[] utf8Path, byte[] metaJson, out IntPtr player);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] private static extern int ac_get_info(IntPtr player, out AcInfo info);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] private static extern int ac_seek(IntPtr player, long ptsUs);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] private static extern int ac_set_queue_depth(IntPtr player, int depth);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] private static extern int ac_start(IntPtr player);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] private static extern int ac_set_loop(IntPtr player, int enable);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] private static extern int ac_acquire_frame(IntPtr player, long clockUs, out AcFrame frame);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] private static extern int ac_release_frame(IntPtr player, ref AcFrame frame);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] private static extern int ac_get_stats(IntPtr player, out AcStats stats);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] private static extern IntPtr ac_last_error(IntPtr player);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] private static extern void ac_close(IntPtr player);

        // Bookkeeping for the leak tests: every successful open must be matched by a close.
        private static int s_opened;
        private static int s_closed;
        public static int OpenedCount => Volatile.Read(ref s_opened);
        public static int ClosedCount => Volatile.Read(ref s_closed);

        public static void CheckLayout()
        {
            if (Marshal.SizeOf<AcInfo>() != 80 || Marshal.SizeOf<AcFrame>() != 72 || Marshal.SizeOf<AcStats>() != 40)
            {
                throw new InvalidOperationException(
                    "AlphaClip: the managed struct layout does not match the native ABI (expected 80/72/40 bytes, got " +
                    Marshal.SizeOf<AcInfo>() + "/" + Marshal.SizeOf<AcFrame>() + "/" + Marshal.SizeOf<AcStats>() + ").");
            }
        }

        public static string Version()
        {
            return Marshal.PtrToStringAnsi(ac_version());
        }

        private static string LastError(IntPtr player)
        {
            IntPtr p = ac_last_error(player);
            return p == IntPtr.Zero ? "(no message)" : Marshal.PtrToStringAnsi(p);
        }

        private static void Check(int status, IntPtr player, string what)
        {
            if (status != (int)AcStatus.Ok)
            {
                throw new AlphaClipException((AcStatus)status, what + ": " + LastError(player));
            }
        }

        // The sidecar travels as NUL-terminated UTF-8; null or "" means no sidecar.
        private static byte[] MetaBytes(string metaJson)
        {
            return string.IsNullOrEmpty(metaJson) ? null : Encoding.UTF8.GetBytes(metaJson + "\0");
        }

        /// <summary>Opens a clip from native memory. The memory is NOT copied by the core: it must stay valid until Close.</summary>
        public static IntPtr OpenMemory(IntPtr data, int length, string metaJson = null)
        {
            Check(ac_open_memory(data, (UIntPtr)(uint)length, MetaBytes(metaJson), out IntPtr player), IntPtr.Zero, "ac_open_memory");
            Interlocked.Increment(ref s_opened);
            return player;
        }

        public static IntPtr OpenFile(string path, string metaJson = null)
        {
            byte[] utf8 = Encoding.UTF8.GetBytes(path + "\0");
            Check(ac_open_file(utf8, MetaBytes(metaJson), out IntPtr player), IntPtr.Zero, "ac_open_file");
            Interlocked.Increment(ref s_opened);
            return player;
        }

        public static AcInfo GetInfo(IntPtr player)
        {
            Check(ac_get_info(player, out AcInfo info), player, "ac_get_info");
            return info;
        }

        public static void Seek(IntPtr player, long ptsUs) => Check(ac_seek(player, ptsUs), player, "ac_seek");
        public static void SetQueueDepth(IntPtr player, int depth) => Check(ac_set_queue_depth(player, depth), player, "ac_set_queue_depth");
        public static void Start(IntPtr player) => Check(ac_start(player), player, "ac_start");
        public static void SetLoop(IntPtr player, bool loop) => Check(ac_set_loop(player, loop ? 1 : 0), player, "ac_set_loop");

        /// <summary>Returns Ok (frame filled in), Again or End; throws for a real failure.</summary>
        public static AcStatus AcquireFrame(IntPtr player, long clockUs, out AcFrame frame)
        {
            int s = ac_acquire_frame(player, clockUs, out frame);
            if (s == (int)AcStatus.Ok || s == (int)AcStatus.Again || s == (int)AcStatus.End)
            {
                return (AcStatus)s;
            }
            throw new AlphaClipException((AcStatus)s, "ac_acquire_frame: " + LastError(player));
        }

        public static void ReleaseFrame(IntPtr player, ref AcFrame frame) => Check(ac_release_frame(player, ref frame), player, "ac_release_frame");

        public static AcStats GetStats(IntPtr player)
        {
            Check(ac_get_stats(player, out AcStats stats), player, "ac_get_stats");
            return stats;
        }

        public static void Close(IntPtr player)
        {
            if (player == IntPtr.Zero)
            {
                return;
            }
            ac_close(player);
            Interlocked.Increment(ref s_closed);
        }
    }
}
