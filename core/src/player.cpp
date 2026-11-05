#include "player.h"

#include <chrono>
#include <exception>
#include <new>
#include <utility>

#include "common.h"

namespace ac {
namespace {

struct Outcome {
  ac_status status = AC_OK;
  std::string text;
};

void SetOutcome(Outcome* o, ac_status status, const char* text) noexcept {
  o->status = status;
  try {
    o->text = text;
  } catch (...) {
  }
}

// Runs f on the decode thread and turns every exception into an Outcome: an exception that
// escapes a thread function would terminate the process.
template <class F>
Outcome Guarded(F&& f) noexcept {
  Outcome o;
  try {
    f();
  } catch (const Failure& e) {
    SetOutcome(&o, e.status(), e.what());
  } catch (const std::bad_alloc&) {
    SetOutcome(&o, AC_ERR_OOM, "out of memory");
  } catch (const std::exception&) {
    SetOutcome(&o, AC_ERR_STATE, "internal error in the decode thread");
  } catch (...) {
    SetOutcome(&o, AC_ERR_STATE, "internal error in the decode thread");
  }
  return o;
}

}  // namespace

Player::Player(std::unique_ptr<Clip> clip, int depth)
    : clip_(std::move(clip)), depth_(static_cast<size_t>(depth)) {
  pool_.resize(depth_ + 2);
  for (auto& slot : pool_) {
    slot.reset(new FrameBuffer());
    free_.push_back(slot.get());
  }
}

Player::~Player() {
  {
    std::lock_guard<std::mutex> g(m_);
    stop_ = true;
  }
  cv_decoder_.notify_all();
  if (thread_.joinable()) thread_.join();
}

void Player::Start() {
  std::lock_guard<std::mutex> g(m_);
  if (started_) throw Failure(AC_ERR_STATE, "the player is already started");
  thread_ = std::thread([this] { Run(); });  // may throw std::system_error: started_ stays false
  started_ = true;
}

void Player::Seek(int64_t pts_us) {
  {
    std::lock_guard<std::mutex> g(m_);
    ++generation_;  // anything the decode thread is working on right now is stale
    stats_.frames_dropped += static_cast<int64_t>(queue_.size());
    while (!queue_.empty()) {
      free_.push_back(queue_.front());
      queue_.pop_front();
    }
    seek_pending_ = true;
    seek_target_ = pts_us;
    eos_ = false;
    failed_ = false;
    error_status_ = AC_OK;
    error_text_.clear();
  }
  cv_decoder_.notify_one();
}

ac_status Player::Acquire(int64_t clock_us, const FrameBuffer** out, std::string* error_text) {
  std::lock_guard<std::mutex> g(m_);
  if (!started_) {
    if (error_text) *error_text = "ac_acquire_frame: the player is not started";
    return AC_ERR_STATE;
  }
  if (held_ != nullptr) {
    if (error_text) *error_text = "ac_acquire_frame: the previous frame has not been released";
    return AC_ERR_STATE;
  }
  // The queue is ordered by pts: count the frames that are due.
  size_t due = 0;
  while (due < queue_.size() && queue_[due]->pts_us <= clock_us) ++due;
  if (due > 0) {
    // Show the newest due frame; the older ones were never shown, so they are dropped.
    for (size_t i = 0; i + 1 < due; ++i) {
      free_.push_back(queue_.front());
      queue_.pop_front();
      ++stats_.frames_dropped;
    }
    held_ = queue_.front();
    queue_.pop_front();
    ++stats_.frames_acquired;
    *out = held_;
    cv_decoder_.notify_one();  // slots were freed
    return AC_OK;
  }
  if (queue_.empty()) {
    if (failed_) {
      if (error_text) *error_text = error_text_;
      return error_status_;
    }
    if (eos_) return AC_END;
  }
  return AC_AGAIN;
}

ac_status Player::Release(const FrameBuffer* frame) {
  {
    std::lock_guard<std::mutex> g(m_);
    if (frame == nullptr || frame != held_) return AC_ERR_ARG;
    free_.push_back(held_);
    held_ = nullptr;
  }
  cv_decoder_.notify_one();
  return AC_OK;
}

PlayerStats Player::Stats() {
  std::lock_guard<std::mutex> g(m_);
  PlayerStats s = stats_;
  s.frames_queued = static_cast<int32_t>(queue_.size());
  s.reached_end = eos_;
  return s;
}

void Player::Run() {
  try {
    std::unique_lock<std::mutex> lk(m_);
    for (;;) {
      cv_decoder_.wait(lk, [this] {
        return stop_ || seek_pending_ ||
               (!eos_ && !failed_ && !free_.empty() && queue_.size() < depth_);
      });
      if (stop_) return;

      if (seek_pending_) {
        const int64_t target = seek_target_;
        const uint64_t gen = generation_;
        seek_pending_ = false;
        lk.unlock();
        const Outcome o = Guarded([&] { clip_->Seek(target); });
        lk.lock();
        if (o.status != AC_OK && gen == generation_) {
          failed_ = true;
          error_status_ = o.status;
          error_text_ = o.text;
        }
        continue;
      }

      FrameBuffer* slot = free_.back();
      free_.pop_back();
      const uint64_t gen = generation_;
      lk.unlock();

      bool got = false;
      const auto t0 = std::chrono::steady_clock::now();
      const Outcome o = Guarded([&] { got = clip_->DecodeNext(slot); });
      const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                          std::chrono::steady_clock::now() - t0)
                          .count();

      lk.lock();
      stats_.decode_time_us += us;
      if (gen != generation_) {  // a seek happened while decoding: the frame is stale
        free_.push_back(slot);
        continue;
      }
      if (o.status != AC_OK) {
        free_.push_back(slot);
        failed_ = true;
        error_status_ = o.status;
        error_text_ = o.text;
      } else if (!got) {
        free_.push_back(slot);
        eos_ = true;
      } else {
        queue_.push_back(slot);
        ++stats_.frames_decoded;
      }
    }
  } catch (...) {
    // Locking or waiting failed: there is nothing safe left to do on this thread.
    // The main thread keeps seeing AC_AGAIN; ac_close still joins us.
  }
}

}  // namespace ac
