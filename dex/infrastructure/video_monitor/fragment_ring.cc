#include "dex/infrastructure/video_monitor/fragment_ring.h"

#include <algorithm>

namespace dex::video_monitor {

FragmentRing::FragmentRing(size_t capacity) : capacity_(capacity) { ring_.resize(capacity); }

void FragmentRing::Push(Fragment fragment) {
  std::lock_guard lock(mutex_);

  ++head_sequence_;
  fragment.sequence = head_sequence_;

  if (fragment.contains_idr) {
    latest_idr_sequence_ = head_sequence_;
  }

  ring_[head_sequence_ % capacity_] = std::move(fragment);
  cv_.notify_all();
}

void FragmentRing::SetInitSegment(std::vector<uint8_t> init) {
  std::lock_guard lock(mutex_);
  init_segment_ = std::move(init);

  // Invalidate all existing fragments — they were encoded with the previous
  // encoder's SPS/PPS and are incompatible with the new init segment.
  // New clients will only receive fragments pushed after this point.
  for (auto& slot : ring_) {
    slot.data.reset();
    slot.sequence = 0;
  }
  latest_idr_sequence_ = 0;

  cv_.notify_all();
}

FragmentRing::ReadResult FragmentRing::ReadFrom(uint64_t after_sequence) const {
  std::lock_guard lock(mutex_);

  ReadResult result;
  result.last_sequence = after_sequence;

  if (head_sequence_ == 0) {
    return result;  // No fragments yet.
  }

  // Determine the start sequence for reading.
  uint64_t start_seq = after_sequence + 1;

  // If this is a new client (after_sequence == 0), include init segment.
  if (after_sequence == 0) {
    if (!init_segment_.empty()) {
      result.init_segment = init_segment_;
    }
    // Start from the latest IDR for a clean decode start.
    start_seq = (latest_idr_sequence_ > 0) ? latest_idr_sequence_ : 1;
  }

  // Check if the requested sequence is too old (already overwritten in the ring).
  uint64_t oldest_available = (head_sequence_ > capacity_) ? (head_sequence_ - capacity_ + 1) : 1;
  if (start_seq < oldest_available) {
    // Skip to the latest IDR if available, otherwise oldest available.
    start_seq = (latest_idr_sequence_ >= oldest_available) ? latest_idr_sequence_ : oldest_available;
  }

  // Collect fragments.
  for (uint64_t seq = start_seq; seq <= head_sequence_; ++seq) {
    const auto& frag = ring_[seq % capacity_];
    if (frag.sequence == seq && frag.data) {
      result.fragments.push_back(frag.data);
      result.last_sequence = seq;
    }
  }

  if (result.fragments.empty()) {
    result.last_sequence = head_sequence_;
  }

  return result;
}

bool FragmentRing::WaitForNew(uint64_t after_sequence, std::chrono::milliseconds timeout) const {
  std::unique_lock lock(mutex_);
  return cv_.wait_for(lock, timeout, [&] { return shutdown_ || head_sequence_ > after_sequence; });
}

void FragmentRing::NotifyAll() {
  std::lock_guard lock(mutex_);
  shutdown_ = true;
  cv_.notify_all();
}

uint64_t FragmentRing::HeadSequence() const {
  std::lock_guard lock(mutex_);
  return head_sequence_;
}

}  // namespace dex::video_monitor
