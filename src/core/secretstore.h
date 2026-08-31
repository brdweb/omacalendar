#pragma once

#include <QObject>
#include <QString>
#include <QtTypes>
#include <functional>

namespace omacalendar {

using SecretStoreOperationId = quint64;
inline constexpr SecretStoreOperationId kInvalidSecretStoreOperationId = 0;

struct SecretStoreResult {
  bool success = false;
  QString value;
  QString errorCode;
  QString errorMessage;
  bool cancelled = false;
  bool timedOut = false;
};

class SecretStore final {
 public:
  [[nodiscard]] bool isAvailable() const;

  bool store(const QString& accountId, const QString& kind, const QString& secret,
             QString* errorMessage = nullptr) const;
  [[nodiscard]] QString lookup(const QString& accountId, const QString& kind,
                               QString* errorMessage = nullptr) const;
  bool remove(const QString& accountId, const QString& kind,
              QString* errorMessage = nullptr) const;

 private:
  static QString executable();
};

// Event-loop-safe Secret Service access. Each accepted operation completes its
// callback exactly once unless this object is destroyed. No shell is involved,
// secrets are never placed in argv, and process stderr is deliberately not
// surfaced because helpers can echo credential material on failure.
class AsyncSecretStore final : public QObject {
  Q_OBJECT

 public:
  using Callback = std::function<void(SecretStoreResult)>;

  explicit AsyncSecretStore(QObject* parent = nullptr);
  ~AsyncSecretStore() override;

  AsyncSecretStore(const AsyncSecretStore&) = delete;
  AsyncSecretStore& operator=(const AsyncSecretStore&) = delete;

  [[nodiscard]] bool isAvailable() const;
  [[nodiscard]] int timeoutMs() const;
  void setTimeoutMs(int timeoutMs);

  [[nodiscard]] SecretStoreOperationId storeAsync(const QString& accountId,
                                                  const QString& kind,
                                                  const QString& secret,
                                                  Callback callback);
  [[nodiscard]] SecretStoreOperationId lookupAsync(const QString& accountId,
                                                   const QString& kind,
                                                   Callback callback);
  [[nodiscard]] SecretStoreOperationId removeAsync(const QString& accountId,
                                                   const QString& kind,
                                                   Callback callback);

  void cancel(SecretStoreOperationId operationId);
  void cancelAll();

 private:
  enum class Action {
    Store,
    Lookup,
    Remove,
  };
  struct Operation;

  [[nodiscard]] SecretStoreOperationId enqueue(Action action, const QString& accountId,
                                               const QString& kind,
                                               const QString& secret,
                                               Callback callback);
  void launch(SecretStoreOperationId operationId);
  void complete(SecretStoreOperationId operationId, SecretStoreResult result);

  int m_timeoutMs = 30000;
  SecretStoreOperationId m_nextOperationId = 1;
  class Private;
  Private* m_private = nullptr;
};

}  // namespace omacalendar
