#include "ipc/ipcprotocol.h"

#include <QJsonDocument>

namespace omacalendar::ipc {

QJsonObject successResponse(const QJsonValue& id, const QJsonValue& result) {
  return {{QStringLiteral("id"), id}, {QStringLiteral("result"), result}};
}

QJsonObject errorResponse(const QJsonValue& id, const Error& error) {
  return {
      {QStringLiteral("id"), id},
      {QStringLiteral("error"),
       QJsonObject{{QStringLiteral("code"), error.code},
                   {QStringLiteral("message"), error.message},
                   {QStringLiteral("retryable"), error.retryable}}},
  };
}

QJsonObject notification(const QString& event, const QJsonObject& data) {
  return {{QStringLiteral("event"), event}, {QStringLiteral("data"), data}};
}

QByteArray frame(const QJsonObject& object) {
  return QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
}

}  // namespace omacalendar::ipc
