// Fabric Compatibility Registry - Summon Software Labs
// Cooperative cancellation shared by the library, the store and the server.
#pragma once

#include <atomic>
#include <memory>

namespace fcr {

// A cancellation token is a handle to shared state. Copying a token shares the
// flag, so a shutdown signal reaches every operation that took a copy.
//
// Cancellation is cooperative and is only observed at defined checkpoints
// (before a commit point, between bounded units of work). An operation that
// observes cancellation returns ErrorCode::Cancelled and must not have
// published anything.
class CancellationToken {
 public:
  CancellationToken() : flag_(std::make_shared<std::atomic<bool>>(false)) {}

  void Cancel() const { flag_->store(true, std::memory_order_release); }
  bool IsCancelled() const { return flag_->load(std::memory_order_acquire); }

 private:
  std::shared_ptr<std::atomic<bool>> flag_;
};

}  // namespace fcr
