// The C ABI. This is the only place exceptions are caught: nothing may escape through it.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "alphaclip.h"
#include "clip.h"
#include "common.h"
#include "meta.h"
#include "player.h"

// The structs below cross the ABI by value layout: the engine bindings (C# P/Invoke, GDExtension,
// Cocos) declare the same layout by hand. Lock it here so that a change breaks the build instead
// of silently corrupting memory. (64-bit targets only: Windows x86_64, Android arm64.)
#if UINTPTR_MAX == 0xFFFFFFFFFFFFFFFFu
static_assert(sizeof(ac_info) == 80 && offsetof(ac_info, width) == 0 && offsetof(ac_info, height) == 4 &&
                  offsetof(ac_info, content_w) == 8 && offsetof(ac_info, content_h) == 12 &&
                  offsetof(ac_info, duration_us) == 16 && offsetof(ac_info, fps) == 24 &&
                  offsetof(ac_info, has_alpha_plane) == 28 && offsetof(ac_info, packet_count) == 32 &&
                  offsetof(ac_info, loop_period_us) == 40 && offsetof(ac_info, alpha_mode) == 48 &&
                  offsetof(ac_info, premultiplied) == 52 && offsetof(ac_info, color_range) == 56 &&
                  offsetof(ac_info, matrix) == 60 && offsetof(ac_info, loop_hint) == 64 &&
                  offsetof(ac_info, alpha_x) == 68 && offsetof(ac_info, alpha_y) == 72 &&
                  offsetof(ac_info, reserved) == 76,
              "ac_info layout changed: update every binding");
static_assert(sizeof(ac_frame) == 72 && offsetof(ac_frame, plane) == 0 && offsetof(ac_frame, stride) == 32 &&
                  offsetof(ac_frame, width) == 48 && offsetof(ac_frame, height) == 52 &&
                  offsetof(ac_frame, pts_us) == 56 && offsetof(ac_frame, flags) == 64,
              "ac_frame layout changed: update every binding");
static_assert(sizeof(ac_stats) == 40 && offsetof(ac_stats, frames_decoded) == 0 &&
                  offsetof(ac_stats, frames_acquired) == 8 && offsetof(ac_stats, frames_dropped) == 16 &&
                  offsetof(ac_stats, decode_time_us) == 24 && offsetof(ac_stats, frames_queued) == 32 &&
                  offsetof(ac_stats, reached_end) == 36,
              "ac_stats layout changed: update every binding");
#endif

// Member order matters: `player` is declared last so it is destroyed first (its destructor joins
// the decode thread, which reads `owned` through the clip it owns).
struct ac_player {
  std::vector<uint8_t> owned;  // file contents when opened with ac_open_file
  std::unique_ptr<ac::Clip> clip;  // moved into `player` by ac_start
  ac::ClipInfo info;
  ac::Meta meta;               // the sidecar (present == false: none was given)
  ac::Layout layout;           // what the sidecar means for this file
  ac::FrameBuffer frame;       // the frame handed out by ac_decode_next
  std::string last_error;
  int depth = ac::Player::kDefaultDepth;
  bool loop = false;           // applied when the player is created by ac_start
  const ac::FrameBuffer* acquired = nullptr;  // the frame the main thread holds (threaded mode)
  std::unique_ptr<ac::Player> player;
};

namespace {

thread_local std::string g_last_error;

// Records the failure for ac_last_error. Never throws, even when out of memory.
ac_status Fail(ac_player* p, ac_status status, const char* a, const char* b = "") noexcept {
  try {
    std::string text = a;
    text += b;
    g_last_error = text;
    if (p != nullptr) p->last_error = std::move(text);
  } catch (...) {
  }
  return status;
}

// Runs f and converts every exception into a status.
template <class F>
ac_status Guard(ac_player* p, F&& f) noexcept {
  try {
    return f();
  } catch (const ac::Failure& e) {
    return Fail(p, e.status(), e.what());
  } catch (const std::bad_alloc&) {
    return Fail(p, AC_ERR_OOM, "out of memory");
  } catch (const std::exception& e) {
    return Fail(p, AC_ERR_STATE, "internal error: ", e.what());
  } catch (...) {
    return Fail(p, AC_ERR_STATE, "internal error");
  }
}

std::FILE* OpenUtf8(const char* path) {
#ifdef _WIN32
  const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, nullptr, 0);
  if (n <= 0) return nullptr;
  std::wstring wide(static_cast<size_t>(n), L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, &wide[0], n) <= 0) return nullptr;
  return _wfopen(wide.c_str(), L"rb");
#else
  return std::fopen(path, "rb");
#endif
}

void ReadAll(std::FILE* f, std::vector<uint8_t>* out) {
  constexpr size_t kChunk = 1u << 20;
  size_t used = 0;
  for (;;) {
    if (used > ac::kMaxFileBytes) {
      throw ac::Failure(AC_ERR_OOM, "the file is larger than the supported maximum");
    }
    out->resize(used + kChunk);
    const size_t n = std::fread(out->data() + used, 1, kChunk, f);
    used += n;
    if (n < kChunk) {
      if (std::ferror(f)) throw ac::Failure(AC_ERR_IO, "read error");
      break;
    }
  }
  out->resize(used);
}

void FillFrame(const ac::FrameBuffer& f, ac_frame* out) {
  for (int i = 0; i < 4; ++i) {
    out->plane[i] = f.plane[i].empty() ? nullptr : f.plane[i].data();
    out->stride[i] = f.stride[i];
  }
  out->width = f.width;
  out->height = f.height;
  out->pts_us = f.pts_us;
  out->flags = 0;
}

// Parses the clip and applies the (already parsed) sidecar to it. `data` must stay valid for the player's life.
void OpenClip(ac_player* p, const uint8_t* data, size_t len, const ac::Meta& meta) {
  p->clip = ac::Clip::Open(data, len, ac::MetaIgnoresAlphaStream(meta));
  p->info = p->clip->info();
  p->meta = meta;
  p->layout = ac::ResolveLayout(meta, p->info.width, p->info.height, p->info.stream_has_alpha);
}

}  // namespace

extern "C" {

const char* ac_version(void) { return "0.1.0-m4"; }

ac_status ac_open_memory(const uint8_t* data, size_t len, const char* meta_json, ac_player** out) {
  if (out == nullptr) return Fail(nullptr, AC_ERR_ARG, "ac_open_memory: out is null");
  *out = nullptr;
  if (data == nullptr || len == 0) return Fail(nullptr, AC_ERR_ARG, "ac_open_memory: data is empty");
  return Guard(nullptr, [&]() -> ac_status {
    const ac::Meta meta = ac::ParseMeta(meta_json);
    std::unique_ptr<ac_player> p(new ac_player());
    OpenClip(p.get(), data, len, meta);
    *out = p.release();
    return AC_OK;
  });
}

ac_status ac_open_file(const char* path, const char* meta_json, ac_player** out) {
  if (out == nullptr) return Fail(nullptr, AC_ERR_ARG, "ac_open_file: out is null");
  *out = nullptr;
  if (path == nullptr || path[0] == '\0') return Fail(nullptr, AC_ERR_ARG, "ac_open_file: path is empty");
  return Guard(nullptr, [&]() -> ac_status {
    const ac::Meta meta = ac::ParseMeta(meta_json);  // before reading a possibly large file
    std::FILE* f = OpenUtf8(path);
    if (f == nullptr) throw ac::Failure(AC_ERR_IO, std::string("cannot open '") + path + "'");
    std::unique_ptr<ac_player> p(new ac_player());
    try {
      ReadAll(f, &p->owned);
    } catch (...) {
      std::fclose(f);
      throw;
    }
    std::fclose(f);
    if (p->owned.empty()) throw ac::Failure(AC_ERR_FORMAT, "the file is empty");
    OpenClip(p.get(), p->owned.data(), p->owned.size(), meta);
    *out = p.release();
    return AC_OK;
  });
}

ac_status ac_get_info(ac_player* player, ac_info* out) {
  if (player == nullptr || out == nullptr) return Fail(player, AC_ERR_ARG, "ac_get_info: null argument");
  return Guard(player, [&]() -> ac_status {
    const ac::ClipInfo& i = player->info;
    const ac::Layout& l = player->layout;
    const ac::Meta& m = player->meta;
    std::memset(out, 0, sizeof(*out));
    out->width = i.width;
    out->height = i.height;
    out->content_w = l.content_w;
    out->content_h = l.content_h;
    out->duration_us = i.duration_us;
    out->fps = static_cast<float>(i.fps);
    out->has_alpha_plane = i.has_alpha ? 1 : 0;
    out->packet_count = static_cast<int64_t>(i.packet_count);
    out->loop_period_us = i.loop_period_us;
    out->alpha_mode = l.alpha_mode;
    out->premultiplied = m.premultiplied ? 1 : 0;
    out->color_range = m.color_range;
    out->matrix = m.matrix;
    out->loop_hint = m.loop_hint ? 1 : 0;
    out->alpha_x = l.alpha_x;
    out->alpha_y = l.alpha_y;
    return AC_OK;
  });
}

ac_status ac_seek(ac_player* player, int64_t pts_us) {
  if (player == nullptr) return Fail(nullptr, AC_ERR_ARG, "ac_seek: player is null");
  return Guard(player, [&]() -> ac_status {
    if (player->player) {
      player->player->Seek(pts_us);  // asynchronous: the decode thread performs it
    } else {
      player->clip->Seek(pts_us);
    }
    return AC_OK;
  });
}

ac_status ac_decode_next(ac_player* player, ac_frame* out) {
  if (player == nullptr || out == nullptr) {
    return Fail(player, AC_ERR_ARG, "ac_decode_next: null argument");
  }
  return Guard(player, [&]() -> ac_status {
    if (player->player) {
      throw ac::Failure(AC_ERR_STATE, "ac_decode_next is not available after ac_start");
    }
    if (!player->clip->DecodeNext(&player->frame)) return AC_END;
    FillFrame(player->frame, out);
    return AC_OK;
  });
}

ac_status ac_set_queue_depth(ac_player* player, int32_t depth) {
  if (player == nullptr) return Fail(nullptr, AC_ERR_ARG, "ac_set_queue_depth: player is null");
  if (depth < 1 || depth > ac::Player::kMaxDepth) {
    return Fail(player, AC_ERR_ARG, "ac_set_queue_depth: depth must be 1..64");
  }
  if (player->player) {
    return Fail(player, AC_ERR_STATE, "ac_set_queue_depth: only allowed before ac_start");
  }
  player->depth = depth;
  return AC_OK;
}

ac_status ac_start(ac_player* player) {
  if (player == nullptr) return Fail(nullptr, AC_ERR_ARG, "ac_start: player is null");
  return Guard(player, [&]() -> ac_status {
    if (!player->player) {
      player->player.reset(new ac::Player(std::move(player->clip), player->depth));
      player->player->SetLoop(player->loop);
    }
    player->player->Start();  // throws AC_ERR_STATE if already started
    return AC_OK;
  });
}

ac_status ac_set_loop(ac_player* player, int32_t enable) {
  if (player == nullptr) return Fail(nullptr, AC_ERR_ARG, "ac_set_loop: player is null");
  return Guard(player, [&]() -> ac_status {
    player->loop = enable != 0;
    if (player->player) player->player->SetLoop(player->loop);
    return AC_OK;
  });
}

ac_status ac_acquire_frame(ac_player* player, int64_t clock_us, ac_frame* out) {
  if (player == nullptr || out == nullptr) {
    return Fail(player, AC_ERR_ARG, "ac_acquire_frame: null argument");
  }
  return Guard(player, [&]() -> ac_status {
    if (!player->player) {
      throw ac::Failure(AC_ERR_STATE, "ac_acquire_frame: the player is not started");
    }
    const ac::FrameBuffer* f = nullptr;
    std::string error_text;
    const ac_status s = player->player->Acquire(clock_us, &f, &error_text);
    if (s == AC_OK) {
      player->acquired = f;
      FillFrame(*f, out);
    } else if (s != AC_AGAIN && s != AC_END) {
      return Fail(player, s, error_text.c_str());
    }
    return s;
  });
}

ac_status ac_release_frame(ac_player* player, ac_frame* frame) {
  if (player == nullptr || frame == nullptr) {
    return Fail(player, AC_ERR_ARG, "ac_release_frame: null argument");
  }
  return Guard(player, [&]() -> ac_status {
    const ac::FrameBuffer* held = player->acquired;
    if (!player->player || held == nullptr || frame->plane[0] != held->plane[0].data()) {
      return Fail(player, AC_ERR_ARG, "ac_release_frame: this is not the acquired frame");
    }
    player->acquired = nullptr;
    return player->player->Release(held);
  });
}

ac_status ac_get_stats(ac_player* player, ac_stats* out) {
  if (player == nullptr || out == nullptr) return Fail(player, AC_ERR_ARG, "ac_get_stats: null argument");
  return Guard(player, [&]() -> ac_status {
    std::memset(out, 0, sizeof(*out));
    if (player->player) {
      const ac::PlayerStats s = player->player->Stats();
      out->frames_decoded = s.frames_decoded;
      out->frames_acquired = s.frames_acquired;
      out->frames_dropped = s.frames_dropped;
      out->decode_time_us = s.decode_time_us;
      out->frames_queued = s.frames_queued;
      out->reached_end = s.reached_end ? 1 : 0;
    }
    return AC_OK;
  });
}

const char* ac_last_error(ac_player* player) {
  return player != nullptr ? player->last_error.c_str() : g_last_error.c_str();
}

void ac_close(ac_player* player) {
  try {
    delete player;
  } catch (...) {
  }
}

}  // extern "C"
