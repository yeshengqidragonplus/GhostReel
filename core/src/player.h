// Internal. The threaded player: one decode thread feeding a bounded queue of ready frames.
//
// Threading rules (design doc 7.3, NFR-5):
//  * While the thread runs, ONLY the decode thread touches the Clip. A seek is a request the
//    decode thread carries out; the main thread never calls into the Clip.
//  * The main thread and the decode thread communicate only through the mutex-protected queue.
//  * A fixed pool of `depth + 2` frame buffers: up to `depth` queued, one held by the main thread
//    (acquired, not yet released), one being filled by the decode thread. Frame buffers are never
//    reallocated while the main thread holds one.
#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "alphaclip.h"
#include "clip.h"

namespace ac {

struct PlayerStats {
  int64_t frames_decoded = 0;
  int64_t frames_acquired = 0;
  int64_t frames_dropped = 0;
  int64_t decode_time_us = 0;
  int32_t frames_queued = 0;
  bool reached_end = false;
};

class Player {
 public:
  static constexpr int kDefaultDepth = 4;
  static constexpr int kMaxDepth = 64;

  Player(std::unique_ptr<Clip> clip, int depth);
  ~Player();  // stops and joins the decode thread
  Player(const Player&) = delete;
  Player& operator=(const Player&) = delete;

  void Start();  // throws Failure(AC_ERR_STATE) when already started
  void Seek(int64_t pts_us);  // asynchronous: flushes the queue and clears a failure

  // Main thread only. Never blocks. On a failure status, *error_text receives the message.
  ac_status Acquire(int64_t clock_us, const FrameBuffer** out, std::string* error_text);
  ac_status Release(const FrameBuffer* frame);

  PlayerStats Stats();

 private:
  void Run();

  std::unique_ptr<Clip> clip_;  // touched only by the decode thread once started
  const size_t depth_;

  std::mutex m_;
  std::condition_variable cv_decoder_;  // wakes the decode thread: space, seek or stop

  // Everything below is guarded by m_.
  std::vector<std::unique_ptr<FrameBuffer>> pool_;
  std::vector<FrameBuffer*> free_;
  std::deque<FrameBuffer*> queue_;  // ready frames, pts non-decreasing
  FrameBuffer* held_ = nullptr;     // acquired by the main thread, not yet released
  uint64_t generation_ = 0;         // bumped by Seek(): frames decoded before it are stale
  bool seek_pending_ = false;
  int64_t seek_target_ = 0;
  bool eos_ = false;                // the decode thread ran out of frames
  bool failed_ = false;             // the decode thread hit an error; Seek() recovers
  ac_status error_status_ = AC_OK;
  std::string error_text_;
  bool started_ = false;
  bool stop_ = false;
  PlayerStats stats_;

  std::thread thread_;
};

}  // namespace ac
