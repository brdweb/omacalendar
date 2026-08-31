#pragma once

#include <QTimer>

#include "core/database.h"
#include "sync/provider.h"

namespace omacalendar {

// Device-only calendars still participate in the durable mutation pipeline.
// This provider acknowledges ready local operations without networking and
// exposes the same lifecycle/status contract as remote providers.
class LocalProvider final : public Provider {
  Q_OBJECT

 public:
  explicit LocalProvider(Database* database, QObject* parent = nullptr);

  [[nodiscard]] ProviderCapabilities capabilities() const override;
  void start() override;
  void syncAll() override;
  void syncAccount(const QString& accountId) override;
  [[nodiscard]] QJsonObject status(const QString& accountId = {}) const override;

 private:
  void drain(const QString& accountId = {});

  Database* m_database = nullptr;
  QTimer m_drainTimer;
};

}  // namespace omacalendar
