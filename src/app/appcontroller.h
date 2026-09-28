#pragma once

#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <functional>

#include "ipc/ipcclient.h"
#include "presentationlistmodel.h"

namespace omacalendar {

class AppController final : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool connected READ connected NOTIFY connectedChanged)
  Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
  Q_PROPERTY(QString statusText READ statusText NOTIFY statusTextChanged)
  Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
  Q_PROPERTY(QVariantList accounts READ accounts NOTIFY accountsChanged)
  Q_PROPERTY(bool canUndo READ canUndo NOTIFY historyChanged)
  Q_PROPERTY(bool canRedo READ canRedo NOTIFY historyChanged)
  // Account id -> {state, message, errorCode, lastSyncAt} for provider-backed
  // accounts, from sync.statusChanged and sync.status.
  Q_PROPERTY(QVariantMap accountSyncStates READ accountSyncStates NOTIFY
                 accountSyncStatesChanged)
  Q_PROPERTY(QVariantList calendars READ calendars NOTIFY calendarsChanged)
  Q_PROPERTY(QVariantList events READ events NOTIFY eventsChanged)
  Q_PROPERTY(QVariantList calendarSets READ calendarSets NOTIFY calendarSetsChanged)
  Q_PROPERTY(QVariantList invitations READ invitations NOTIFY invitationsChanged)
  Q_PROPERTY(QVariantList conflicts READ conflicts NOTIFY conflictsChanged)
  Q_PROPERTY(QVariantList operations READ operations NOTIFY operationsChanged)
  Q_PROPERTY(QVariantList searchResults READ searchResults NOTIFY searchResultsChanged)
  Q_PROPERTY(
      omacalendar::PresentationListModel* accountsModel READ accountsModel CONSTANT)
  Q_PROPERTY(
      omacalendar::PresentationListModel* calendarsModel READ calendarsModel CONSTANT)
  Q_PROPERTY(omacalendar::PresentationListModel* eventsModel READ eventsModel CONSTANT)
  Q_PROPERTY(omacalendar::PresentationListModel* calendarSetsModel READ
                 calendarSetsModel CONSTANT)
  Q_PROPERTY(omacalendar::PresentationListModel* invitationsModel READ invitationsModel
                 CONSTANT)
  Q_PROPERTY(
      omacalendar::PresentationListModel* conflictsModel READ conflictsModel CONSTANT)
  Q_PROPERTY(
      omacalendar::PresentationListModel* operationsModel READ operationsModel CONSTANT)
  Q_PROPERTY(omacalendar::PresentationListModel* searchResultsModel READ
                 searchResultsModel CONSTANT)
  Q_PROPERTY(QVariantMap preferences READ preferences NOTIFY preferencesChanged)
  Q_PROPERTY(bool widgetInstalled READ widgetInstalled NOTIFY widgetInstalledChanged)
  Q_PROPERTY(QString activeCalendarSetId READ activeCalendarSetId NOTIFY
                 activeCalendarSetIdChanged)
  Q_PROPERTY(
      bool preferencesLoaded READ preferencesLoaded NOTIFY preferencesLoadedChanged)
  Q_PROPERTY(QDateTime selectedDate READ selectedDate WRITE setSelectedDate NOTIFY
                 selectedDateChanged)
  Q_PROPERTY(QString systemTimeZoneId READ systemTimeZoneId CONSTANT)
  Q_PROPERTY(QStringList availableTimeZoneIds READ availableTimeZoneIds CONSTANT)
  Q_PROPERTY(bool bundledGoogleOAuthAvailable READ bundledGoogleOAuthAvailable CONSTANT)
  Q_PROPERTY(bool googleOAuthConfigured READ googleOAuthConfigured NOTIFY
                 googleOAuthConfiguredChanged)

 public:
  explicit AppController(QObject* parent = nullptr);

  [[nodiscard]] bool connected() const;
  [[nodiscard]] bool busy() const;
  [[nodiscard]] QString statusText() const;
  [[nodiscard]] QString lastError() const;
  [[nodiscard]] QVariantList accounts() const;
  [[nodiscard]] bool canUndo() const;
  [[nodiscard]] bool canRedo() const;
  [[nodiscard]] QVariantMap accountSyncStates() const;
  [[nodiscard]] QVariantList calendars() const;
  [[nodiscard]] QVariantList events() const;
  [[nodiscard]] QVariantList calendarSets() const;
  [[nodiscard]] QVariantList invitations() const;
  [[nodiscard]] QVariantList conflicts() const;
  [[nodiscard]] QVariantList operations() const;
  [[nodiscard]] QVariantList searchResults() const;
  [[nodiscard]] PresentationListModel* accountsModel();
  [[nodiscard]] PresentationListModel* calendarsModel();
  [[nodiscard]] PresentationListModel* eventsModel();
  [[nodiscard]] PresentationListModel* calendarSetsModel();
  [[nodiscard]] PresentationListModel* invitationsModel();
  [[nodiscard]] PresentationListModel* conflictsModel();
  [[nodiscard]] PresentationListModel* operationsModel();
  [[nodiscard]] PresentationListModel* searchResultsModel();
  [[nodiscard]] QVariantMap preferences() const;
  [[nodiscard]] bool widgetInstalled() const;
  [[nodiscard]] QString activeCalendarSetId() const;
  [[nodiscard]] bool preferencesLoaded() const;
  [[nodiscard]] QDateTime selectedDate() const;
  [[nodiscard]] QString systemTimeZoneId() const;
  [[nodiscard]] QStringList availableTimeZoneIds() const;
  [[nodiscard]] bool bundledGoogleOAuthAvailable() const;
  [[nodiscard]] bool googleOAuthConfigured() const;

  Q_INVOKABLE void refresh();
  Q_INVOKABLE void loadRange(const QDate& firstDate, const QDate& lastDate);
  Q_INVOKABLE void createEvent(const QVariantMap& event);
  Q_INVOKABLE void updateEvent(const QVariantMap& event);
  Q_INVOKABLE void removeEvent(const QString& eventId);
  Q_INVOKABLE void saveEvent(const QVariantMap& event,
                             const QVariantMap& mutationOptions = {});
  Q_INVOKABLE void requestDeleteEvent(const QString& eventId,
                                      const QVariantMap& mutationOptions = {});
  // Asks for guest addresses seen in cached events; answers with
  // contactSuggestionsReady. Quietly does nothing while disconnected.
  Q_INVOKABLE void suggestContacts(const QString& prefix);
  // Reads one line of quick-add text into an editor draft: title, location,
  // recurrenceRule, date and endDate (yyyy-MM-dd, empty when not given),
  // allDay, startMinute (-1 when not given) and durationMinutes (0 likewise).
  Q_INVOKABLE [[nodiscard]] QVariantMap parseQuickAdd(const QString& text) const;
  Q_INVOKABLE void searchEvents(const QString& query, const QVariantMap& filters = {});
  Q_INVOKABLE void respondToInvitation(const QString& eventId, const QString& response,
                                       const QString& recurrenceScope,
                                       const QString& recurrenceId,
                                       qint64 expectedLocalRevision);
  Q_INVOKABLE void markInvitationSeen(const QString& eventId);
  Q_INVOKABLE void resolveConflict(const QString& conflictId, const QString& strategy,
                                   const QVariantMap& mergedDraft = {});
  Q_INVOKABLE void retryOperation(const QString& operationId);
  Q_INVOKABLE void discardOperation(const QString& operationId);
  Q_INVOKABLE void addLocalCalendar(const QString& name, const QString& color,
                                    bool muteAlerts = false);
  Q_INVOKABLE void removeCalendar(const QString& calendarId);
  Q_INVOKABLE void removeLocalCalendar(const QString& calendarId);
  Q_INVOKABLE void addIcsSubscription(const QVariantMap& config);
  Q_INVOKABLE void previewIcsImport(const QUrl& file,
                                    const QString& destinationCalendarId);
  Q_INVOKABLE void commitIcsImport(const QUrl& file,
                                   const QString& destinationCalendarId,
                                   const QString& duplicatePolicy);
  Q_INVOKABLE void exportIcs(const QVariantMap& scope, const QUrl& destination);
  Q_INVOKABLE void connectGoogle(const QString& displayName = {});
  Q_INVOKABLE void connectGoogleConfigured(const QString& displayName = {});
  Q_INVOKABLE void connectGoogleWithClientId(const QString& clientId,
                                             const QString& displayName = {});
  Q_INVOKABLE void connectGoogleWithCredentials(const QUrl& credentialsFile,
                                                const QString& displayName = {});
  Q_INVOKABLE void addCalDavAccount(const QString& endpoint, const QString& username,
                                    const QString& password,
                                    const QString& displayName = {});
  Q_INVOKABLE void removeAccount(const QString& accountId);
  Q_INVOKABLE void removeAccountWithOptions(const QString& accountId,
                                            bool removeCachedData);
  Q_INVOKABLE void reauthorizeAccount(const QString& accountId);
  Q_INVOKABLE void updateAccountCredentials(const QString& accountId,
                                            const QString& username,
                                            const QString& password);
  Q_INVOKABLE void setCalendarPreference(const QString& calendarId, const QString& key,
                                         const QVariant& value);
  Q_INVOKABLE void setPreference(const QString& key, const QVariant& value);
  Q_INVOKABLE void setCalendarVisibility(const QString& calendarId, bool visible);
  Q_INVOKABLE void upsertCalendarSet(const QVariantMap& calendarSet);
  Q_INVOKABLE void removeCalendarSet(const QString& calendarSetId);
  Q_INVOKABLE void activateCalendarSet(const QString& setId);
  Q_INVOKABLE void setCurrentView(const QString& view);
  // Undo and redo walk a bounded history of event edits, creates and
  // deletes. undoLastMutation is kept for older callers.
  Q_INVOKABLE void undo();
  Q_INVOKABLE void redo();
  Q_INVOKABLE void undoLastMutation();
  Q_INVOKABLE bool canOpenExternalEventUrl(const QString& value) const;
  Q_INVOKABLE void openExternalEventUrl(const QString& value);
  Q_INVOKABLE void installWidget();
  Q_INVOKABLE void restoreOmarchyClock();
  Q_INVOKABLE void previewDiagnostics();
  Q_INVOKABLE void handleDeepLink(const QUrl& url);
  Q_INVOKABLE void handleIcsImportFile(const QUrl& file);
  Q_INVOKABLE QString wallTimeToUtc(const QString& dateText, const QString& timeText,
                                    const QString& timeZoneId) const;
  Q_INVOKABLE QString utcToWallTime(const QString& utcText,
                                    const QString& timeZoneId) const;
  Q_INVOKABLE bool isValidTimeZone(const QString& timeZoneId) const;
  Q_INVOKABLE void syncAll();
  Q_INVOKABLE void syncAccount(const QString& accountId);
  Q_INVOKABLE void probeThisAndFuture(const QString& calendarId);
  Q_INVOKABLE void setSelectedDate(const QDateTime& date);
  Q_INVOKABLE void reconnect();
  void activateWindow();

 signals:
  void connectedChanged();
  void busyChanged();
  void statusTextChanged();
  void lastErrorChanged();
  void accountsChanged();
  void accountSyncStatesChanged();
  void calendarsChanged();
  void eventsChanged();
  void calendarSetsChanged();
  void invitationsChanged();
  void conflictsChanged();
  void operationsChanged();
  void searchResultsChanged();
  void preferencesChanged();
  void widgetInstalledChanged();
  void activeCalendarSetIdChanged();
  void preferencesLoadedChanged();
  void selectedDateChanged();
  void googleOAuthConfiguredChanged();
  void eventSaved();
  void historyChanged();
  // After an interactive change the user may want to take back, for the
  // undo toast; undoable says whether undo() would reverse it.
  void mutationCompleted(const QString& message, bool undoable);
  void contactSuggestionsReady(const QString& prefix, const QVariantList& contacts);
  void accountSetupStarted();
  void icsImportPreviewReady(const QVariantMap& preview);
  void icsImportCompleted(const QVariantMap& result);
  void icsExportCompleted(const QVariantMap& result);
  void openEventRequested(const QVariantMap& event);
  void createEventRequested(const QVariantMap& draft);
  void openIcsImportRequested(const QUrl& file);
  void openSectionRequested(const QString& section);
  void windowActivationRequested();

 private:
  using ResultHandler = std::function<void(const QJsonValue&)>;
  // Where a mutation came from decides where its inverse is recorded.
  enum class HistoryMode { Record, Undo, Redo };
  // One reversible step: restore saves event with options, remove deletes
  // event, and undelete replays a delete's undo token before it expires.
  struct HistoryEntry {
    enum class Kind { Restore, Remove, Undelete };
    Kind kind = Kind::Restore;
    QVariantMap event;
    // For restore, the state being replaced, so the inverse does not depend
    // on the cached events having refreshed.
    QVariantMap previous;
    QVariantMap options;
    QString undoToken;
    QDateTime expiresAt;
  };
  static constexpr int kHistoryLimit = 20;
  // Returns true when it handled the error, which suppresses the generic
  // user-visible error message.
  using ErrorHandler = std::function<bool(const QJsonObject&)>;

  QString send(const QString& method, const QJsonObject& params,
               ResultHandler handler = {}, bool contributesToBusy = true,
               ErrorHandler errorHandler = {});
  void removeInvitation(const QString& eventId, const QString& recurrenceId);
  void setError(const QString& message);
  void setStatus(const QString& message);
  void setBusy(bool busy);
  void startDaemonIfNeeded();
  void refreshWidgetStatus();
  void processPendingDeepLink();
  void applyDisplayTimes(QVariantList* events) const;
  [[nodiscard]] QStringList visibleCalendarIds() const;
  void requestRangePage(quint64 generation, int offset, int limit);
  void refreshParts(int parts);
  void finishScopeRequest();
  void scheduleRefresh(int parts);
  [[nodiscard]] static int refreshPartsForNotification(const QString& event);
  void loadPreferences();
  void saveEventWithHistory(const QVariantMap& values,
                            const QVariantMap& mutationOptions, HistoryMode mode,
                            const QVariantMap& knownPrior = {});
  void deleteEventWithHistory(const QString& eventId,
                              const QVariantMap& mutationOptions, HistoryMode mode);
  void undeleteWithHistory(const HistoryEntry& entry, HistoryMode mode);
  void recordInverse(HistoryMode mode, const HistoryEntry& inverse);
  void applyHistoryEntry(const HistoryEntry& entry, HistoryMode mode);
  void setAccountSyncState(const QString& accountId, const QJsonObject& status);
  void refreshAccountSyncStates(bool icsOnly);
  void loadPreferencesIndividually();
  void markPreferencesLoaded();
  [[nodiscard]] static QStringList preferenceKeys();

  enum RefreshPart {
    RefreshSystemInfo = 1 << 0,
    RefreshAccounts = 1 << 1,
    RefreshCalendars = 1 << 2,
    RefreshCalendarSets = 1 << 3,
    RefreshInvitations = 1 << 4,
    RefreshPreferences = 1 << 5,
    RefreshEvents = 1 << 6,
    RefreshOperations = 1 << 7,
    RefreshConflicts = 1 << 8,
    RefreshAll = (1 << 9) - 1,
  };

  // The daemon's default events.list page size. Pages shrink when a response
  // would exceed the IPC frame limit.
  static constexpr int kEventPageLimit = 500;

  ipc::IpcClient m_client;
  QHash<QString, ResultHandler> m_pending;
  QHash<QString, ErrorHandler> m_pendingErrors;
  QSet<QString> m_backgroundRequests;
  QTimer m_refreshTimer;
  int m_pendingRefreshParts = 0;
  bool m_settingsGetManySupported = true;
  QVariantList m_accounts;
  QVariantMap m_accountSyncStates;
  QVariantList m_calendars;
  QVariantList m_events;
  QVariantList m_calendarSets;
  QVariantList m_invitations;
  QVariantList m_conflicts;
  QVariantList m_operations;
  QVariantList m_searchResults;
  PresentationListModel m_accountsModel;
  PresentationListModel m_calendarsModel;
  PresentationListModel m_eventsModel;
  PresentationListModel m_calendarSetsModel;
  PresentationListModel m_invitationsModel;
  PresentationListModel m_conflictsModel;
  PresentationListModel m_operationsModel;
  PresentationListModel m_searchResultsModel;
  QVariantMap m_preferences;
  QDate m_selectedDate = QDate::currentDate();
  QDate m_rangeStart;
  QDate m_rangeEnd;
  quint64 m_rangeGeneration = 0;
  QVariantList m_rangePages;
  QStringList m_visibleCalendarIds;
  bool m_calendarsReady = false;
  bool m_calendarSetsReady = false;
  bool m_rangeNeedsReload = false;
  int m_scopeRequestsInFlight = 0;
  QString m_statusText = QStringLiteral("Connecting to calendar service…");
  QString m_lastError;
  int m_activeRequests = 0;
  qint64 m_subscriptionRevision = -1;
  bool m_daemonStartAttempted = false;
  bool m_widgetInstalled = false;
  bool m_preferencesLoaded = false;
  bool m_googleOAuthConfigured = false;
  QString m_activeCalendarSetId = QStringLiteral("all-calendars");
  QUrl m_pendingDeepLink;
  QList<HistoryEntry> m_undoHistory;
  QList<HistoryEntry> m_redoHistory;
};

}  // namespace omacalendar
