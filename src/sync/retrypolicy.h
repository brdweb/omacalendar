#pragma once

#include <QtTypes>
#include <chrono>

#include "sync/provider.h"

namespace omacalendar {

enum class RetryAction {
  DoNotRetry,
  Retry,
  Reauthenticate,
  ResolveConflict,
  ResetSync,
  ReconcileBeforeRetry,
  Cancelled,
  AttemptsExhausted,
};

struct RetryOptions {
  // maximumAttempts includes the original request. With the default value, a
  // request can be made once and retried at most five times.
  int maximumAttempts = 6;
  std::chrono::milliseconds initialDelay{1000};
  std::chrono::milliseconds maximumDelay{5 * 60 * 1000};
  std::chrono::milliseconds maximumRetryAfter{24 * 60 * 60 * 1000};

  // Symmetric jitter in thousandths of the exponential delay. 200 means that
  // every computed delay stays within +/-20 percent of the unjittered delay.
  int jitterPermille = 200;
  quint64 randomSeed = UINT64_C(0x6f6d6163616c656e);
};

// These inputs deliberately separate transport knowledge from the provider's
// error category. That lets the outbox avoid blindly replaying a create whose
// response was lost after the remote server may already have accepted it.
struct RetryInput {
  ProviderError error;
  int completedAttempts = 1;
  bool operationIsIdempotent = true;
  bool requestMayHaveReachedServer = false;

  // A stable per-operation value (for example a persisted idempotency-key
  // hash). The same inputs always produce the same jittered delay.
  quint64 jitterKey = 0;
};

struct RetryDecision {
  RetryAction action = RetryAction::DoNotRetry;
  std::chrono::milliseconds delay{0};

  [[nodiscard]] bool shouldScheduleRetry() const noexcept {
    return action == RetryAction::Retry;
  }
};

// Pure, deterministic retry policy. It owns no timers and reads neither the
// clock nor global random state, so callers can test decisions without an
// event loop and schedule the returned delay with their own QTimer.
class RetryPolicy final {
 public:
  explicit RetryPolicy(RetryOptions options = {});

  [[nodiscard]] const RetryOptions& options() const noexcept { return m_options; }
  [[nodiscard]] RetryDecision evaluate(const RetryInput& input) const noexcept;

  // Exposed independently for table-driven tests. completedAttempts is
  // one-based: 1 returns a jittered form of initialDelay.
  [[nodiscard]] std::chrono::milliseconds delayForAttempt(
      int completedAttempts, quint64 jitterKey) const noexcept;

  // Classification does not consider maximumAttempts. evaluate() adds that
  // stateful boundary after classifying the supplied failure.
  [[nodiscard]] static RetryAction classify(const RetryInput& input) noexcept;

 private:
  RetryOptions m_options;
};

}  // namespace omacalendar
