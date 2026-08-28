#include "sync/retrypolicy.h"

#include <algorithm>
#include <limits>

namespace omacalendar {
namespace {

using Milliseconds = std::chrono::milliseconds;

constexpr qint64 kLargestSafeDelay = std::numeric_limits<qint64>::max() / INT64_C(4);

qint64 clampedDelayCount(const Milliseconds delay) noexcept {
  return std::clamp<qint64>(delay.count(), 0, kLargestSafeDelay);
}

quint64 mix64(quint64 value) noexcept {
  value += UINT64_C(0x9e3779b97f4a7c15);
  value = (value ^ (value >> 30U)) * UINT64_C(0xbf58476d1ce4e5b9);
  value = (value ^ (value >> 27U)) * UINT64_C(0x94d049bb133111eb);
  return value ^ (value >> 31U);
}

bool isRetryableHttpStatus(const int status) noexcept {
  return status == 408 || status == 425 || status == 429 ||
         (status >= 500 && status <= 599);
}

ProviderErrorKind kindFromHttpStatus(const int status) noexcept {
  switch (status) {
    case 400:
    case 405:
    case 406:
    case 411:
    case 413:
    case 414:
    case 415:
    case 422:
      return ProviderErrorKind::InvalidRequest;
    case 401:
      return ProviderErrorKind::Authentication;
    case 403:
      return ProviderErrorKind::Permission;
    case 404:
    case 410:
      return ProviderErrorKind::NotFound;
    case 409:
    case 412:
      return ProviderErrorKind::Conflict;
    case 408:
      return ProviderErrorKind::Timeout;
    case 425:
    case 429:
      return ProviderErrorKind::RateLimited;
    default:
      if (status >= 500 && status <= 599) {
        return ProviderErrorKind::ServiceUnavailable;
      }
      return ProviderErrorKind::Unknown;
  }
}

}  // namespace

RetryPolicy::RetryPolicy(RetryOptions options) : m_options(options) {
  m_options.maximumAttempts = std::max(1, m_options.maximumAttempts);
  m_options.jitterPermille = std::clamp(m_options.jitterPermille, 0, 1000);

  const qint64 maximumDelay = clampedDelayCount(m_options.maximumDelay);
  const qint64 initialDelay =
      std::min(clampedDelayCount(m_options.initialDelay), maximumDelay);
  const qint64 maximumRetryAfter = clampedDelayCount(m_options.maximumRetryAfter);

  m_options.initialDelay = Milliseconds(initialDelay);
  m_options.maximumDelay = Milliseconds(maximumDelay);
  m_options.maximumRetryAfter = Milliseconds(maximumRetryAfter);
}

RetryAction RetryPolicy::classify(const RetryInput& input) noexcept {
  ProviderErrorKind kind = input.error.kind;
  if ((kind == ProviderErrorKind::None || kind == ProviderErrorKind::Unknown) &&
      input.error.httpStatus >= 400) {
    kind = kindFromHttpStatus(input.error.httpStatus);
  }

  switch (kind) {
    case ProviderErrorKind::None:
      return RetryAction::DoNotRetry;
    case ProviderErrorKind::Cancelled:
      return RetryAction::Cancelled;
    case ProviderErrorKind::Authentication:
      return RetryAction::Reauthenticate;
    case ProviderErrorKind::Conflict:
      return RetryAction::ResolveConflict;
    case ProviderErrorKind::SyncTokenExpired:
      return RetryAction::ResetSync;
    case ProviderErrorKind::Permission:
    case ProviderErrorKind::NotFound:
    case ProviderErrorKind::InvalidRequest:
    case ProviderErrorKind::Unsupported:
      return RetryAction::DoNotRetry;
    case ProviderErrorKind::Network:
    case ProviderErrorKind::Timeout:
    case ProviderErrorKind::RateLimited:
    case ProviderErrorKind::ServiceUnavailable:
      if (input.requestMayHaveReachedServer && !input.operationIsIdempotent) {
        return RetryAction::ReconcileBeforeRetry;
      }
      return RetryAction::Retry;
    case ProviderErrorKind::Unknown:
      // An explicit unknown error remains terminal unless its HTTP status is a
      // well-known transient status. This keeps unclassified failures safe.
      if (isRetryableHttpStatus(input.error.httpStatus)) {
        if (input.requestMayHaveReachedServer && !input.operationIsIdempotent) {
          return RetryAction::ReconcileBeforeRetry;
        }
        return RetryAction::Retry;
      }
      return RetryAction::DoNotRetry;
  }

  return RetryAction::DoNotRetry;
}

Milliseconds RetryPolicy::delayForAttempt(const int completedAttempts,
                                          const quint64 jitterKey) const noexcept {
  const int normalizedAttempt = std::max(1, completedAttempts);
  const qint64 maximumDelay = m_options.maximumDelay.count();
  qint64 delay = m_options.initialDelay.count();

  // Saturating multiplication avoids shifts and remains bounded for arbitrary
  // caller input, including a corrupt attempts count read from storage.
  const int doublings = std::min(normalizedAttempt - 1, 63);
  for (int index = 0; index < doublings && delay < maximumDelay; ++index) {
    if (delay > maximumDelay / 2) {
      delay = maximumDelay;
    } else {
      delay *= 2;
    }
  }
  delay = std::min(delay, maximumDelay);

  if (delay == 0 || m_options.jitterPermille == 0) {
    return Milliseconds(delay);
  }

  const qint64 span = (delay * static_cast<qint64>(m_options.jitterPermille)) / 1000;
  if (span == 0) {
    return Milliseconds(delay);
  }

  const quint64 attemptSalt =
      static_cast<quint64>(normalizedAttempt) * UINT64_C(0xd6e8feb86659fd93);
  const quint64 random = mix64(m_options.randomSeed ^ jitterKey ^ attemptSalt);
  const quint64 width = static_cast<quint64>(span * 2 + 1);
  const qint64 offset = static_cast<qint64>(random % width) - span;

  return Milliseconds(std::clamp<qint64>(delay + offset, 0, maximumDelay));
}

RetryDecision RetryPolicy::evaluate(const RetryInput& input) const noexcept {
  RetryDecision decision;
  decision.action = classify(input);
  if (decision.action != RetryAction::Retry) {
    return decision;
  }

  const int completedAttempts = std::max(1, input.completedAttempts);
  if (completedAttempts >= m_options.maximumAttempts) {
    decision.action = RetryAction::AttemptsExhausted;
    return decision;
  }

  decision.delay = delayForAttempt(completedAttempts, input.jitterKey);

  if (input.error.retryAfterMs >= 0) {
    const qint64 retryAfter =
        std::min<qint64>(input.error.retryAfterMs,
                         static_cast<qint64>(m_options.maximumRetryAfter.count()));
    decision.delay =
        std::max(decision.delay, Milliseconds(std::max<qint64>(0, retryAfter)));
  }

  return decision;
}

}  // namespace omacalendar
