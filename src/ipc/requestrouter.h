#pragma once

#include <QHash>
#include <QJsonObject>
#include <QString>
#include <functional>

#include "ipc/ipcprotocol.h"

namespace omacalendar::ipc {

class RequestRouter final {
 public:
  using Handler = std::function<QJsonValue(const QJsonObject& params, Error* error)>;

  void registerHandler(const QString& method, Handler handler);
  [[nodiscard]] QJsonObject route(const QJsonObject& request) const;
  [[nodiscard]] QStringList methods() const;

 private:
  QHash<QString, Handler> m_handlers;
};

}  // namespace omacalendar::ipc
