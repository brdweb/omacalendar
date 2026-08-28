#include "ipc/requestrouter.h"

#include <algorithm>

#include "core/domain.h"

namespace omacalendar::ipc {

void RequestRouter::registerHandler(const QString& method, Handler handler) {
  m_handlers.insert(method, std::move(handler));
}

QJsonObject RequestRouter::route(const QJsonObject& request) const {
  const QJsonValue id = request.value(QStringLiteral("id"));
  if (id.isUndefined() || id.isNull()) {
    return errorResponse(QJsonValue::Null,
                         {QStringLiteral("invalid_request"),
                          QStringLiteral("Request id is required"), false});
  }
  if (request.value(QStringLiteral("protocolMajor")).toInt(-1) != kIpcProtocolMajor) {
    return errorResponse(id, {QStringLiteral("incompatible_protocol"),
                              QStringLiteral("Unsupported IPC protocol major"), false});
  }
  const QString method = request.value(QStringLiteral("method")).toString();
  if (method.isEmpty()) {
    return errorResponse(id, {QStringLiteral("invalid_request"),
                              QStringLiteral("Method is required"), false});
  }
  const auto iterator = m_handlers.constFind(method);
  if (iterator == m_handlers.constEnd()) {
    return errorResponse(id, {QStringLiteral("method_not_found"),
                              QStringLiteral("Unknown method: %1").arg(method), false});
  }
  const QJsonValue paramsValue = request.value(QStringLiteral("params"));
  if (!paramsValue.isUndefined() && !paramsValue.isObject()) {
    return errorResponse(id, {QStringLiteral("invalid_params"),
                              QStringLiteral("Params must be an object"), false});
  }

  Error handlerError;
  const QJsonValue result = iterator.value()(
      paramsValue.isObject() ? paramsValue.toObject() : QJsonObject{}, &handlerError);
  if (!handlerError.code.isEmpty()) {
    return errorResponse(id, handlerError);
  }
  return successResponse(id, result);
}

QStringList RequestRouter::methods() const {
  QStringList result = m_handlers.keys();
  std::sort(result.begin(), result.end());
  return result;
}

}  // namespace omacalendar::ipc
