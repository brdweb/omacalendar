#pragma once

#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QTimeZone>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <functional>
#include <memory>

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
  // The latest free/busy answer: {requestId, start, end, self, attendees
  // (email -> [{start, end}]), pending (emails), unavailable ([{email,
  // reason}])}. Remote answers arrive after the request and merge in.
  Q_PROPERTY(QVariantMap freeBusy READ freeBusy NOTIFY freeBusyChanged)
  Q_PROPERTY(
      QVariantMap eventAttachments READ eventAttachments NOTIFY eventAttachmentsChanged)
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
  // Task lists and every task in them (open and completed). tasksSupported
  // is false while connected to a daemon without tasks.* methods.
  Q_PROPERTY(QVariantList taskLists READ taskLists NOTIFY tasksChanged)
  Q_PROPERTY(QVariantList tasks READ tasks NOTIFY tasksChanged)
  Q_PROPERTY(bool tasksSupported READ tasksSupported NOTIFY tasksChanged)
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
  [[nodiscard]] QVariantMap freeBusy() const;
  [[nodiscard]] QVariantMap eventAttachments() const;
  [[nodiscard]] bool canUndo() const;
  [[nodiscard]] bool canRedo() const;
  [[nodiscard]] QVariantMap accountSyncStates() const;
  [[nodiscard]] QVariantList calendars() const;
  [[nodiscard]] QVariantList events() const;
  [[nodiscard]] QVariantList calendarSets() const;
  [[nodiscard]] QVariantList invitations() const;
  [[nodiscard]] QVariantList conflicts() const;
  [[nodiscard]] QVariantList taskLists() const;
  [[nodiscard]] QVariantList tasks() const;
  [[nodiscard]] bool tasksSupported() const;
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
  // task holds title, notes, dueDate (yyyy-MM-dd or ""), completed and, for
  // a new task, listId (the device-only list when omitted).
  Q_INVOKABLE void createTask(const QVariantMap& task);
  // task holds id plus the fields to change.
  Q_INVOKABLE void updateTask(const QVariantMap& task);
  Q_INVOKABLE void setTaskCompleted(const QString& taskId, bool completed);
  Q_INVOKABLE void removeTask(const QString& taskId);
  Q_INVOKABLE void setTaskListEnabled(const QString& listId, bool enabled);
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
  // Saves the events of a date range as a print-ready PDF. options holds
  // firstDate and lastDate (yyyy-MM-dd), layout ("list" or "month"),
  // includeDetails, visibleOnly (only the calendars shown now), title,
  // timePattern (a QTime format) and firstDayOfWeek (1 = Monday … 7).
  Q_INVOKABLE void exportPdf(const QVariantMap& options, const QUrl& destination);
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
  // While the window is active the daemon polls providers more often; the
  // controller renews that before the daemon's lease runs out.
  Q_INVOKABLE void setInteractive(bool interactive);
  Q_INVOKABLE void queryFreeBusy(const QString& start, const QString& end,
                                 const QStringList& emails, const QString& calendarId,
                                 const QString& excludeEventId,
                                 const QString& excludeRecurrenceId);
  // The first start (ISO UTC) at or after earliest where durationMinutes
  // avoids every busy {start, end} and fits the working hours of its day in
  // timeZoneId (the display zone when empty); empty when nothing fits before
  // horizon.
  Q_INVOKABLE [[nodiscard]] QString nextFreeSlot(
      const QVariantList& busy, const QString& earliest, int durationMinutes,
      int workDayStartHour, int workDayEndHour, const QString& horizon,
      const QString& timeZoneId = {}) const;
  // Fetches one event's attachments into eventAttachments as {eventId,
  // recurrenceId, attachments: [{title, url, mimeType}]}.
  Q_INVOKABLE void loadEventAttachments(const QString& eventId,
                                        const QString& recurrenceId = {});
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
  // For a secondary time-zone gutter: each display-zone hour 0..24 of
  // dateText as {minute, dayOffset} in zoneId, plus a short label and UTC
  // offset for that day. Empty when either value is invalid.
  Q_INVOKABLE [[nodiscard]] QVariantMap secondaryTimeLabels(
      const QString& dateText, const QString& zoneId) const;
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
  void tasksChanged();
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
  void freeBusyChanged();
  void eventAttachmentsChanged();
  // After an interactive change the user may want to take back, for the
  // undo toast; undoable says whether undo() would reverse it.
  void mutationCompleted(const QString& message, bool undoable);
  void contactSuggestionsReady(const QString& prefix, const QVariantList& contacts);
  void accountSetupStarted();
  void icsImportPreviewReady(const QVariantMap& preview);
  void icsImportCompleted(const QVariantMap& result);
  void icsExportCompleted(const QVariantMap& result);
  void pdfExportCompleted(const QString& path, int pages);
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
    quint64 serial = 0;
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
  void startHistoryStep(HistoryMode mode);
  // The step in flight succeeded: drop its entry. Failure leaves the entry
  // in place so the same undo or redo can be tried again.
  void finishHistoryStep(HistoryMode mode);
  void dropHistoryStep(HistoryMode mode);
  [[nodiscard]] ErrorHandler historyFailureHandler(HistoryMode mode);
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
    RefreshTasks = 1 << 9,
    RefreshAll = (1 << 10) - 1,
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
  QVariantList m_taskLists;
  QVariantList m_tasks;
  bool m_tasksSupported = true;
  void loadTasks();
  void requestTaskPage(const QVariantList& lists, QVariantList tasks, int offset,
                       quint64 generation);
  // A newer load supersedes pages still in flight.
  quint64 m_taskGeneration = 0;
  static constexpr int kTaskPageLimit = 1000;
  void subscribe(bool includeTasks);
  void sendTaskMutation(const QString& method, const QJsonObject& params);
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
  bool m_interactive = false;
  QVariantMap m_freeBusy;
  QVariantMap m_eventAttachments;
  struct PdfExportJob;
  void requestPdfPage(const std::shared_ptr<PdfExportJob>& job, int offset, int limit);
  void finishPdfExport(const std::shared_ptr<PdfExportJob>& job);
  // Continues an export that waited for provider hydration: reads the range
  // again after events.changed, or prints what is cached after the wait.
  void resumePdfExport(bool refetch);
  std::shared_ptr<PdfExportJob> m_pendingPdfJob;
  QTimer m_pdfHydrationWait;
  [[nodiscard]] QTimeZone displayTimeZone() const;
  QTimer m_interactiveRenewal;
  void sendInteractive();
  quint64 m_historySerial = 0;
  // The serial of the entry an undo or redo request is applying, or 0.
  quint64 m_historyInFlight = 0;
  QList<HistoryEntry> m_undoHistory;
  QList<HistoryEntry> m_redoHistory;
};

}  // namespace omacalendar
