#pragma once

#include <QJsonObject>
#include <QString>

namespace omacalendar::ipc {

inline constexpr qsizetype kMaximumFrameBytes = 1024 * 1024;

struct Error {
  QString code;
  QString message;
  bool retryable = false;
};

QJsonObject successResponse(const QJsonValue& id, const QJsonValue& result);
QJsonObject errorResponse(const QJsonValue& id, const Error& error);
QJsonObject notification(const QString& event, const QJsonObject& data);
QByteArray frame(const QJsonObject& object);

}  // namespace omacalendar::ipc
