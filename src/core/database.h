#pragma once

#include <QJsonValue>
#include <QList>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QString>

#include "core/domain.h"

namespace omacalendar {

class Database final {
 public:
  Database();
  ~Database();

  Database(const Database&) = delete;
  Database& operator=(const Database&) = delete;

  bool open(const QString& path, QString* errorMessage = nullptr);
  void close();
  [[nodiscard]] bool isOpen() const;
  [[nodiscard]] int schemaVersion() const;

  bool upsertAccount(Account account, QString* errorMessage = nullptr);
  [[nodiscard]] QList<Account> accounts(QString* errorMessage = nullptr) const;
  [[nodiscard]] Account account(const QString& accountId,
                                QString* errorMessage = nullptr) const;
  bool removeAccount(const QString& accountId, QString* errorMessage = nullptr);

  bool upsertCalendar(Calendar calendar, QString* errorMessage = nullptr);
  [[nodiscard]] QList<Calendar> calendars(const QString& accountId = {},
                                          QString* errorMessage = nullptr) const;
  [[nodiscard]] Calendar calendar(const QString& calendarId,
                                  QString* errorMessage = nullptr) const;
  [[nodiscard]] Calendar calendarByRemoteId(const QString& accountId,
                                            const QString& remoteId,
                                            QString* errorMessage = nullptr) const;

  bool applyRemoteEvent(Event event, QString* errorMessage = nullptr,
                        bool* conflicted = nullptr);
  bool removeRemoteEvent(const QString& calendarId, const QString& remoteId,
                         const QString& remotePayload = {},
                         QString* errorMessage = nullptr, bool* conflicted = nullptr);
  bool clearCleanRemoteEvents(const QString& calendarId,
                              QString* errorMessage = nullptr);
  bool saveLocalEvent(Event* event, OutboxOperation operation,
                      QString* errorMessage = nullptr);
  bool markLocalEventRemoved(const QString& eventId, QString* errorMessage = nullptr);
  [[nodiscard]] Event event(const QString& eventId,
                            QString* errorMessage = nullptr) const;
  [[nodiscard]] Event eventByRemoteId(const QString& calendarId,
                                      const QString& remoteId,
                                      QString* errorMessage = nullptr) const;
  [[nodiscard]] QList<Event> eventsBetween(const QDateTime& startUtc,
                                           const QDateTime& endUtc,
                                           const QStringList& calendarIds = {},
                                           QString* errorMessage = nullptr) const;

  [[nodiscard]] QList<OutboxItem> readyOutbox(int limit = 100,
                                              QString* errorMessage = nullptr) const;
  bool updateOutboxState(qint64 id, OutboxState state, int attempts,
                         const QDateTime& nextAttemptAt, const QString& errorCode = {},
                         const QString& errorMessage = {},
                         QString* databaseError = nullptr);
  bool completeOutbox(qint64 id, const Event* remoteEvent = nullptr,
                      QString* errorMessage = nullptr);

  [[nodiscard]] QJsonValue providerState(const QString& accountId,
                                         const QString& calendarId, const QString& key,
                                         const QJsonValue& fallback = {},
                                         QString* errorMessage = nullptr) const;
  bool setProviderState(const QString& accountId, const QString& calendarId,
                        const QString& key, const QJsonValue& value,
                        QString* errorMessage = nullptr);
  bool clearProviderState(const QString& accountId, const QString& calendarId = {},
                          QString* errorMessage = nullptr);

  [[nodiscard]] QJsonValue setting(const QString& key, const QJsonValue& fallback = {},
                                   QString* errorMessage = nullptr) const;
  bool setSetting(const QString& key, const QJsonValue& value,
                  QString* errorMessage = nullptr);

 private:
  bool migrate(QString* errorMessage);
  bool execute(const QString& sql, QString* errorMessage) const;
  bool upsertEventRecord(const Event& event, QString* errorMessage);
  [[nodiscard]] Event eventFromQuery(const QSqlQuery& query) const;
  [[nodiscard]] OutboxItem outboxFromQuery(const QSqlQuery& query) const;

  QString m_connectionName;
  QSqlDatabase m_database;
};

}  // namespace omacalendar
