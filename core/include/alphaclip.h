/*
 * AlphaClip core: C ABI. ("AlphaClip" is only a placeholder project name.)
 *
 * Bytes in, YUV(A) frames out. This library never touches a graphics API.
 * Every function returns an ac_status; no exception ever crosses this boundary,
 * and malformed input must produce an error, never a crash.
 *
 * M1 status: open / info / seek / synchronous decode are implemented.
 * The threaded player (ac_start / ac_acquire_frame / ac_release_frame) arrives
 * later in M1; see docs/ and the design document, section 7.
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

typedef struct ac_info {
  int32_t width, height;         /* coded video size */
  int32_t content_w, content_h;  /* visible picture size (== width/height until packed modes exist) */
  int64_t duration_us;
  float fps;                     /* 0 when it cannot be determined */
  int32_t has_alpha_plane;       /* 1 when frames carry a 4th (A) plane */
  int64_t packet_count;          /* video packets in the file (hidden frames produce no output) */
} ac_info;

typedef struct ac_frame {
  const uint8_t* plane[4];       /* Y, U, V, A; plane[3] is NULL without alpha */
  int32_t stride[4];             /* bytes per row of each plane */
  int32_t width, height;         /* Y and A are width x height; U and V are ((w+1)/2) x ((h+1)/2) */
  int64_t pts_us;
  uint32_t flags;                /* reserved, 0 */
} ac_frame;

/* Version string of the core, e.g. "0.1.0-m1". Never NULL. */
AC_API const char* ac_version(void);

/*
 * Opens a WebM clip. `data` is NOT copied: it must stay valid until ac_close.
 * `meta_json` is the sidecar metadata; sidecars are not supported yet, so pass
 * NULL (a non-empty string returns AC_ERR_UNSUPPORTED).
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
 * discarded. Valid in synchronous mode.
 */
AC_API ac_status ac_seek(ac_player* player, int64_t pts_us);

/*
 * Synchronous decode for tools and tests: decodes the next shown frame in stream
 * order, with no thread and no clock. Returns AC_END after the last frame.
 * The frame stays valid until the next ac_decode_next / ac_seek / ac_close on
 * this player. Not thread-safe; do not mix with the threaded player.
 */
AC_API ac_status ac_decode_next(ac_player* player, ac_frame* out);

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
