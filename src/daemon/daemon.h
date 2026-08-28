#pragma once

#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QStringList>

#include "core/database.h"
#include "ipc/ipcprotocol.h"
#include "ipc/ipcserver.h"
#include "ipc/requestrouter.h"
#include "providers/caldav/caldavsync.h"
#include "providers/google/googlesync.h"

namespace omacalendar {

class Daemon final : public QObject {
  Q_OBJECT

 public:
  explicit Daemon(QObject* parent = nullptr);

  bool start(QString* errorMessage = nullptr);
  [[nodiscard]] QString socketPath() const;
  [[nodiscard]] int connectedClients() const;
  [[nodiscard]] QStringList methods() const;

 private:
  void registerHandlers();
  [[nodiscard]] QJsonValue onSystemPing(const QJsonObject& params,
                                        ipc::Error* error) const;
  [[nodiscard]] QJsonValue onSystemInfo(const QJsonObject& params,
                                        ipc::Error* error) const;

  [[nodiscard]] QJsonValue onAccountsList(const QJsonObject& params, ipc::Error* error);
  [[nodiscard]] QJsonValue onAccountsCreateCalDav(const QJsonObject& params,
                                                  ipc::Error* error);
  [[nodiscard]] QJsonValue onAccountsRemove(const QJsonObject& params,
                                            ipc::Error* error);
  [[nodiscard]] QJsonValue onAccountsTest(const QJsonObject& params, ipc::Error* error);
  [[nodiscard]] QJsonValue onCalendarsList(const QJsonObject& params,
                                           ipc::Error* error);
  [[nodiscard]] QJsonValue onCalendarsUpsert(const QJsonObject& params,
                                             ipc::Error* error);
  [[nodiscard]] QJsonValue onEventsList(const QJsonObject& params, ipc::Error* error);
  [[nodiscard]] QJsonValue onEventsGet(const QJsonObject& params, ipc::Error* error);
  [[nodiscard]] QJsonValue onEventsCreate(const QJsonObject& params, ipc::Error* error);
  [[nodiscard]] QJsonValue onEventsUpdate(const QJsonObject& params, ipc::Error* error);
  [[nodiscard]] QJsonValue onEventsRemove(const QJsonObject& params, ipc::Error* error);
  [[nodiscard]] QJsonValue onEventsSearch(const QJsonObject& params, ipc::Error* error);
  [[nodiscard]] QJsonValue onSettingsGet(const QJsonObject& params, ipc::Error* error);
  [[nodiscard]] QJsonValue onSettingsSet(const QJsonObject& params, ipc::Error* error);
  [[nodiscard]] QJsonValue onOutboxList(const QJsonObject& params, ipc::Error* error);
  [[nodiscard]] QJsonValue onOutboxRetry(const QJsonObject& params, ipc::Error* error);
  [[nodiscard]] QJsonValue onGoogleConfigureClient(const QJsonObject& params,
                                                   ipc::Error* error);
  [[nodiscard]] QJsonValue onGoogleOauthStart(const QJsonObject& params,
                                              ipc::Error* error);
  [[nodiscard]] QJsonValue onGoogleOauthCancel(const QJsonObject& params,
                                               ipc::Error* error);
  [[nodiscard]] QJsonValue onGoogleDisconnect(const QJsonObject& params,
                                              ipc::Error* error);
  [[nodiscard]] QJsonValue onSyncAll(const QJsonObject& params, ipc::Error* error);
  [[nodiscard]] QJsonValue onSyncAccount(const QJsonObject& params, ipc::Error* error);
  [[nodiscard]] QJsonValue onSyncStatus(const QJsonObject& params,
                                        ipc::Error* error) const;

  static QStringList parseCalendarIds(const QJsonValue& value, ipc::Error* error);
  void emitEventsChanged(const QStringList& calendarIds);
  [[nodiscard]] bool validateRequiredString(const QJsonObject& params,
                                            const QString& key, QString* value,
                                            ipc::Error* error) const;

  Database m_database;
  ipc::RequestRouter m_router;
  ipc::IpcServer m_server;
  google::GoogleSync m_google;
  caldav::CalDavSync m_caldav;
};

}  // namespace omacalendar
