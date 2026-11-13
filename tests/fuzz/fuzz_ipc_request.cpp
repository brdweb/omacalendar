// Routes an untrusted IPC request frame, as omacalendard does for each line a
// local client sends, through handlers that echo their params.
#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <cstddef>
#include <cstdint>

#include "ipc/ipcprotocol.h"
#include "ipc/requestrouter.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  using omacalendar::ipc::Error;
  using omacalendar::ipc::RequestRouter;
  static RequestRouter* router = []() {
    auto* value = new RequestRouter();
    value->registerHandler(QStringLiteral("system.ping"),
                           [](const QJsonObject& params, Error*) { return params; });
    value->registerHandler(QStringLiteral("events.list"),
                           [](const QJsonObject& params, Error* error) {
                             if (!params.contains(QStringLiteral("start"))) {
                               error->code = QStringLiteral("invalid_params");
                             }
                             return QJsonValue(params);
                           });
    return value;
  }();
  const QByteArray line(reinterpret_cast<const char*>(data),
                        static_cast<qsizetype>(size));
  if (line.size() > omacalendar::ipc::kMaximumFrameBytes) {
    return 0;
  }
  const QJsonDocument document = QJsonDocument::fromJson(line);
  if (!document.isObject()) {
    return 0;
  }
  const QJsonObject response = router->route(document.object());
  (void)omacalendar::ipc::frame(response);
  return 0;
}
