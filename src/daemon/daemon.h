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
#include "providers/ics/icsservice.h"
#include "reminders/reminderscheduler.h"
#include "sync/synccoordinator.h"

namespace omacalendar {

class Daemon final : public QObject {
  Q_OBJECT

 public:
  explicit Daemon(QObject* parent = nullptr);

  bool start(QString* errorMessage = nullptr, qintptr socketDescriptor = -1);
  [[nodiscard]] QString socketPath() const;
  [[nodiscard]] int connectedClients() const;
  [[nodiscard]] QStringList methods() const;

 private:
  void registerHandlers();
  [[nodiscard]] QJsonValue onSystemPing(const QJsonObject& params,
                                        ipc::Error* error) const;
  [[nodiscard]] QJsonValue onSystemInfo(const QJsonObject& params,
                                        ipc::Error* error) const;
  [[nodiscard]] QJsonValue onSystemHealth(const QJsonObject& params,
                                          ipc::Error* error) const;
  [[nodiscard]] QJsonValue onSystemSubscribe(const QJsonObject& params,
                                             ipc::Error* error) const;

  [[nodiscard]] QJsonValue onAccountsList(const QJsonObject& params, ipc::Error* error);
  [[nodiscard]] QJsonValue onAccountsReauthorize(const QJsonObject& params,
                                                 ipc::Error* error);
  [[nodiscard]] QJsonValue onAccountsUpdate(const QJsonObject& params,
                                            ipc::Error* error);
  [[nodiscard]] QJsonValue onAccountsDisconnect(const QJsonObject& params,
                                                ipc::Error* error);
  [[nodiscard]] QJsonValue onAccountsCreateCalDav(const QJsonObject& params,
                                                  ipc::Error* error);
  [[nodiscard]] QJsonValue onAccountsAddIcs(const QJsonObject& params,
                                            ipc::Error* error);
  [[nodiscard]] QJsonValue onAccountsRemove(const QJsonObject& params,
                                            ipc::Error* error);
  [[nodiscard]] QJsonValue onAccountsTest(const QJsonObject& params, ipc::Error* error);
  [[nodiscard]] QJsonValue onCalendarsList(const QJsonObject& params,
                                           ipc::Error* error);
  [[nodiscard]] QJsonValue onCalendarsUpsert(const QJsonObject& params,
                                             ipc::Error* error);
  [[nodiscard]] QJsonValue onCalendarsRemove(const QJsonObject& params,
                                             ipc::Error* error);
  [[nodiscard]] QJsonValue onCalendarsUpdatePreferences(const QJsonObject& params,
                                                        ipc::Error* error);
  [[nodiscard]] QJsonValue onCalendarSetsList(const QJsonObject& params,
                                              ipc::Error* error);
  [[nodiscard]] QJsonValue onCalendarSetsUpsert(const QJsonObject& params,
                                                ipc::Error* error);
  [[nodiscard]] QJsonValue onCalendarSetsRemove(const QJsonObject& params,
                                                ipc::Error* error);
  [[nodiscard]] QJsonValue onCalendarSetsActivate(const QJsonObject& params,
                                                  ipc::Error* error);
  [[nodiscard]] QJsonValue onEventsList(const QJsonObject& params, ipc::Error* error);
  [[nodiscard]] QJsonValue onEventsGet(const QJsonObject& params, ipc::Error* error);
  [[nodiscard]] QJsonValue onEventsCreate(const QJsonObject& params, ipc::Error* error);
  [[nodiscard]] QJsonValue onEventsUpdate(const QJsonObject& params, ipc::Error* error);
  [[nodiscard]] QJsonValue onEventsRemove(const QJsonObject& params, ipc::Error* error);
  [[nodiscard]] QJsonValue onEventsUndo(const QJsonObject& params, ipc::Error* error);
  [[nodiscard]] QJsonValue onEventsSearch(const QJsonObject& params, ipc::Error* error);
  [[nodiscard]] QJsonValue onEventsMove(const QJsonObject& params, ipc::Error* error);
  [[nodiscard]] QJsonValue onEventsRespond(const QJsonObject& params,
                                           ipc::Error* error);
  [[nodiscard]] QJsonValue onInvitationsList(const QJsonObject& params,
                                             ipc::Error* error);
  [[nodiscard]] QJsonValue onInvitationsMarkSeen(const QJsonObject& params,
                                                 ipc::Error* error);
  [[nodiscard]] QJsonValue onConflictsList(const QJsonObject& params,
                                           ipc::Error* error);
  [[nodiscard]] QJsonValue onConflictsResolve(const QJsonObject& params,
                                              ipc::Error* error);
  [[nodiscard]] QJsonValue onSettingsGet(const QJsonObject& params, ipc::Error* error);
  [[nodiscard]] QJsonValue onSettingsSet(const QJsonObject& params, ipc::Error* error);
  [[nodiscard]] QJsonValue onOutboxList(const QJsonObject& params, ipc::Error* error);
  [[nodiscard]] QJsonValue onOutboxRetry(const QJsonObject& params, ipc::Error* error);
  [[nodiscard]] QJsonValue onOperationsDiscard(const QJsonObject& params,
                                               ipc::Error* error);
  [[nodiscard]] QJsonValue onRemindersList(const QJsonObject& params,
                                           ipc::Error* error);
  [[nodiscard]] QJsonValue onRemindersSnooze(const QJsonObject& params,
                                             ipc::Error* error);
  [[nodiscard]] QJsonValue onRemindersDismiss(const QJsonObject& params,
                                              ipc::Error* error);
  [[nodiscard]] QJsonValue onIcsRefresh(const QJsonObject& params, ipc::Error* error);
  [[nodiscard]] QJsonValue onIcsStatus(const QJsonObject& params,
                                       ipc::Error* error) const;
  [[nodiscard]] QJsonValue onImportPreview(const QJsonObject& params,
                                           ipc::Error* error) const;
  [[nodiscard]] QJsonValue onImportCommit(const QJsonObject& params, ipc::Error* error);
  [[nodiscard]] QJsonValue onExport(const QJsonObject& params, ipc::Error* error) const;
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
  [[nodiscard]] QJsonValue onSyncCalendar(const QJsonObject& params, ipc::Error* error);
  [[nodiscard]] QJsonValue onWidgetSnapshot(const QJsonObject& params,
                                            ipc::Error* error);

  static QStringList parseCalendarIds(const QJsonValue& value, ipc::Error* error);
  void emitEventsChanged(const QStringList& calendarIds);
  bool finishCalendarRemoval(const Calendar& calendar, QString* errorMessage = nullptr);
  [[nodiscard]] bool validateRequiredString(const QJsonObject& params,
                                            const QString& key, QString* value,
                                            ipc::Error* error) const;

  Database m_database;
  ipc::RequestRouter m_router;
  ipc::IpcServer m_server;
  google::GoogleSync m_google;
  caldav::CalDavSync m_caldav;
  ics::IcsService m_ics;
  SyncCoordinator m_sync;
  ReminderScheduler m_reminders;
  QList<Event> m_invitationReadCache;
  qint64 m_invitationReadCacheRevision = -1;
  QDateTime m_invitationReadCacheExpiresAt;
};

}  // namespace omacalendar
