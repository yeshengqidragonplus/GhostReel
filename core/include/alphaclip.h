/*
 * AlphaClip core: C ABI. ("AlphaClip" is only a placeholder project name.)
 *
 * Bytes in, YUV(A) frames out. This library never touches a graphics API.
 * Every function returns an ac_status; no exception ever crosses this boundary,
 * and malformed input must produce an error, never a crash.
 *
 * Status: open / info / seek / synchronous decode / threaded player / loop (M1),
 * sidecar metadata with the opaque, packed_right, packed_bottom and native_alpha
 * modes (M4). See docs/m1-notes.md, docs/m4-notes.md and the design document,
 * sections 6 and 7.
 */
#ifndef ALPHACLIP_H
#define ALPHACLIP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) && defined(AC_BUILDING_SHARED)
#define AC_API __declspec(dllexport)
#elif defined(_WIN32) && defined(AC_USING_SHARED)
#define AC_API __declspec(dllimport)
#elif defined(_WIN32)
#define AC_API
#else
#define AC_API __attribute__((visibility("default")))
#endif

typedef struct ac_player ac_player;

/* Numeric values are part of the ABI: append new values, never renumber. */
typedef enum ac_status {
  AC_OK = 0,
  AC_AGAIN = 1,           /* no frame is ready right now (threaded player) */
  AC_ERR_IO = 2,          /* file could not be opened or read */
  AC_ERR_FORMAT = 3,      /* malformed or truncated container / stream */
  AC_ERR_UNSUPPORTED = 4, /* well-formed but outside what the core handles */
  AC_ERR_DECODE = 5,      /* the video decoder rejected a packet */
  AC_ERR_STATE = 6,       /* call not valid in the current state, or internal error */
  AC_ERR_OOM = 7,         /* allocation failed or a size limit was exceeded */
  AC_ERR_ARG = 8,         /* invalid argument (null pointer, empty buffer, ...) */
  AC_END = 9              /* end of stream: no more frames */
} ac_status;

/* How transparency is stored. Numeric values are part of the ABI (4 is reserved for "keyed", which is rejected). */
typedef enum ac_alpha_mode {
  AC_ALPHA_OPAQUE = 0,         /* no transparency */
  AC_ALPHA_PACKED_RIGHT = 1,   /* the picture is on the left, its alpha as a grey image on the right */
  AC_ALPHA_PACKED_BOTTOM = 2,  /* the picture is on top, its alpha as a grey image below */
  AC_ALPHA_NATIVE = 3          /* WebM BlockAdditional alpha: frames carry plane[3] */
} ac_alpha_mode;

/* ac_info.color_range / ac_info.matrix: what the sidecar says; UNSPECIFIED = the binding's own default applies. */
enum {
  AC_RANGE_UNSPECIFIED = 0,
  AC_RANGE_LIMITED = 1,
  AC_RANGE_FULL = 2,
  AC_MATRIX_UNSPECIFIED = 0,
  AC_MATRIX_BT601 = 1,
  AC_MATRIX_BT709 = 2
};

typedef struct ac_info {
  int32_t width, height;         /* coded video size = size of the frames ac_decode_next / ac_acquire_frame return */
  int32_t content_w, content_h;  /* visible picture size: == width/height except in the packed modes */
  int64_t duration_us;
  float fps;                     /* 0 when it cannot be determined */
  int32_t has_alpha_plane;       /* 1 when frames carry a 4th (A) plane (alpha_mode == AC_ALPHA_NATIVE) */
  int64_t packet_count;          /* video packets in the file (hidden frames produce no output) */
  int64_t loop_period_us;        /* pts added per loop pass: (last pts - first pts) + one frame */
  int32_t alpha_mode;            /* ac_alpha_mode */
  int32_t premultiplied;         /* 1: the colour in the video is already multiplied by alpha (sidecar "premultiplied") */
  int32_t color_range;           /* AC_RANGE_* */
  int32_t matrix;                /* AC_MATRIX_* */
  int32_t loop_hint;             /* 1: the sidecar asks for looping (informational) */
  int32_t alpha_x, alpha_y;      /* packed modes: top-left corner of the alpha image inside the coded frame (its size is
                                    content_w x content_h; the alpha value is the Y plane there); 0 otherwise */
  int32_t reserved;              /* 0 */
} ac_info;

typedef struct ac_frame {
  const uint8_t* plane[4];       /* Y, U, V, A; plane[3] is NULL without alpha */
  int32_t stride[4];             /* bytes per row of each plane */
  int32_t width, height;         /* Y and A are width x height; U and V are ((w+1)/2) x ((h+1)/2) */
  int64_t pts_us;
  uint32_t flags;                /* reserved, 0 */
} ac_frame;

/* Version string of the core, e.g. "0.1.0-m4". Never NULL. */
AC_API const char* ac_version(void);

/*
 * Opens a WebM clip. `data` is NOT copied: it must stay valid until ac_close.
 * `meta_json` is the sidecar metadata (clip.acmeta.json, UTF-8, at most 64 KiB), or NULL / "" for none.
 * Without a sidecar the clip is "auto": native_alpha when the file carries an alpha stream, otherwise
 * opaque; colour range and matrix stay UNSPECIFIED. With one the sidecar is authoritative:
 *   AC_ERR_FORMAT       the JSON is malformed, a value has the wrong type or is out of range, or the metadata
 *                       contradicts the file (native_alpha without an alpha stream, a packed layout that does not fit)
 *   AC_ERR_UNSUPPORTED  a well-formed sidecar this core cannot honour: schema other than 1, alpha_mode "keyed",
 *                       non-zero edge post-processing values
 * Unknown extra keys are ignored. opaque never decodes the alpha stream, even if the file has one; packed_*
 * with an alpha stream in the file is rejected as contradictory. See docs/m4-notes.md for the schema.
 */
AC_API ac_status ac_open_memory(const uint8_t* data, size_t len, const char* meta_json,
                                ac_player** out);
/* Reads the whole file into memory (UTF-8 path), then behaves like ac_open_memory. */
AC_API ac_status ac_open_file(const char* path, const char* meta_json, ac_player** out);

AC_API ac_status ac_get_info(ac_player* player, ac_info* out);

/*
 * Positions the stream so that the next frame is the one shown at `pts_us`: the
 * latest packet with pts <= pts_us (the first packet when pts_us is earlier). Decoding
 * restarts at the closest earlier keyframe and the frames before the target are
 * discarded. In synchronous mode it takes effect immediately; with the threaded player it
 * returns at once, flushes the queue (those frames count as dropped) and clears a decode
 * failure; new frames appear as the decode thread catches up.
 */
AC_API ac_status ac_seek(ac_player* player, int64_t pts_us);

/*
 * Synchronous decode for tools and tests: decodes the next shown frame in stream
 * order, with no thread and no clock. Returns AC_END after the last frame.
 * The frame stays valid until the next ac_decode_next / ac_seek / ac_close on
 * this player. Not thread-safe. Returns AC_ERR_STATE once ac_start was called.
 */
AC_API ac_status ac_decode_next(ac_player* player, ac_frame* out);

/* ---- Threaded player ------------------------------------------------------
 *
 * ac_start launches one decode thread that demuxes + decodes into a bounded
 * queue of ready frames. The caller's thread (the "main" thread; all calls
 * below must come from one thread at a time) polls with ac_acquire_frame.
 */

/* Number of decoded frames the queue may hold. 1..64, default 4. Only before ac_start. */
AC_API ac_status ac_set_queue_depth(ac_player* player, int32_t depth);

AC_API ac_status ac_start(ac_player* player);

/*
 * Loop playback (default off). When the decode thread reaches the end of the clip it
 * restarts at the first frame and adds ac_info.loop_period_us to every following pts, so
 * the caller's clock never has to jump back and AC_END is never returned. Takes effect at
 * the next end of stream (it does not restart a stream that already ended: use ac_seek).
 * A seek addresses a position inside one pass: it resets the pts offset to 0.
 * May be called before or after ac_start.
 */
AC_API ac_status ac_set_loop(ac_player* player, int32_t enable);

/*
 * Returns the newest queued frame with pts <= clock_us; older queued frames are
 * dropped. The clock is supplied by the caller (engine time, audio clock...).
 *   AC_OK     a frame is returned; give it back with ac_release_frame before the next acquire
 *   AC_AGAIN  nothing newer is due yet: keep showing the previous frame
 *   AC_END    the stream is finished and every frame has been consumed
 *   other     the decode thread failed (AC_ERR_DECODE ...); ac_last_error has the text.
 *             ac_seek clears the failure.
 * Never blocks.
 */
AC_API ac_status ac_acquire_frame(ac_player* player, int64_t clock_us, ac_frame* out);
AC_API ac_status ac_release_frame(ac_player* player, ac_frame* frame);

typedef struct ac_stats {
  int64_t frames_decoded;      /* frames the decode thread produced */
  int64_t frames_acquired;     /* frames handed out by ac_acquire_frame */
  int64_t frames_dropped;      /* produced but never shown: older than the clock, or flushed by a seek */
  int64_t decode_time_us;      /* total time the decode thread spent decoding (excludes waiting) */
  int32_t frames_queued;       /* frames waiting in the queue right now */
  int32_t reached_end;         /* 1 once the decode thread has no more frames to produce */
} ac_stats;

AC_API ac_status ac_get_stats(ac_player* player, ac_stats* out);

/*
 * Human-readable text of the most recent failure. With a player: that player's
 * last failure. With NULL: the last failure on the calling thread (use this
 * when an open call failed and there is no player). The pointer stays valid
 * until the next call into the library on the same player / thread.
 */
AC_API const char* ac_last_error(ac_player* player);

AC_API void ac_close(ac_player* player); /* NULL is allowed */

#ifdef __cplusplus
}
#endif

#endif /* ALPHACLIP_H */
