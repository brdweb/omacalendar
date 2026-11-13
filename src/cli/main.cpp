#include <fcntl.h>
#include <unistd.h>

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QTextStream>
#include <QTimeZone>
#include <QTimer>
#include <QUuid>
#include <algorithm>
#include <cerrno>
#include <optional>

#include "core/paths.h"
#include "core/quickadd.h"
#include "ipc/ipcclient.h"

using omacalendar::ipc::IpcClient;

namespace {

constexpr int kDefaultTimeoutSeconds = 5;
constexpr int kSyncTimeoutSeconds = 120;
constexpr int kBackupTimeoutSeconds = 120;
constexpr int kAgendaLimit = 500;

const char* const kUsage =
    "Usage:\n"
    "  omacalendarctl [options] <method> [json_params | -]\n"
    "  omacalendarctl [options] today | agenda [--days N] | add TEXT | sync | methods\n"
    "  omacalendarctl [options] backup [--copy-to PATH]\n"
    "\n"
    "Raw mode sends one IPC method (see docs/IPC.md) and prints the response\n"
    "as JSON on stdout. Pass '-' (or --params-stdin) to read the JSON params\n"
    "from stdin, which keeps passwords and client secrets out of the process\n"
    "list and shell history.\n"
    "\n"
    "Commands:\n"
    "  today            List today's events.\n"
    "  agenda           List events from today for --days days (default 7).\n"
    "  add TEXT         Create an event from quick-add text, e.g.\n"
    "                   \"Lunch with Sam fri 12:30 @ Cafe\".\n"
    "  sync             Sync every account and wait for the result.\n"
    "  methods          List the IPC methods the service supports.\n"
    "  backup           Save a copy of the calendar database in the data\n"
    "                   directory's backups folder (the newest 5 are kept);\n"
    "                   --copy-to PATH also copies it to PATH.\n"
    "\n"
    "Examples:\n"
    "  omacalendarctl system.info\n"
    "  omacalendarctl events.list "
    "'{\"start\":\"2026-08-01T00:00:00Z\",\"end\":\"2026-08-02T00:00:00Z\"}'\n"
    "  printf '%s' \"$params\" | omacalendarctl accounts.addCalDav -\n";

void printJson(const QJsonValue& value, QTextStream& stream) {
  const QJsonDocument document = value.isObject() ? QJsonDocument(value.toObject())
                                                  : QJsonDocument(value.toArray());
  stream << document.toJson(QJsonDocument::Compact) << Qt::endl;
}

// One connection to omacalendard with blocking request helpers.
class Session {
 public:
  Session(QTextStream& out, QTextStream& err) : m_out(out), m_err(err) {
    m_client.setAutoReconnect(false);
    QObject::connect(&m_client, &IpcClient::responseReceived,
                     [this](const QString& id, const QJsonValue& result) {
                       if (id == m_pendingId) {
                         m_result = result;
                         m_done = true;
                         m_loop.quit();
                       }
                     });
    QObject::connect(&m_client, &IpcClient::errorReceived,
                     [this](const QString& id, const QJsonObject& error) {
                       if (id == m_pendingId) {
                         m_error = error;
                         m_done = true;
                         m_loop.quit();
                       }
                     });
    QObject::connect(&m_client, &IpcClient::protocolError,
                     [this](const QString& message) {
                       m_err << "Protocol error: " << message << Qt::endl;
                       m_protocolFailed = true;
                       m_loop.quit();
                     });
    QObject::connect(&m_client, &IpcClient::notificationReceived,
                     [this](const QString& event, const QJsonObject& data) {
                       // Notifications go to stderr unless --watch asked for
                       // them, so stdout carries only the response.
                       printJson(QJsonObject{{QStringLiteral("event"), event},
                                             {QStringLiteral("data"), data}},
                                 m_watch ? m_out : m_err);
                     });
  }

  void setWatch(const bool watch) { m_watch = watch; }

  bool connect(const int timeoutMs) {
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&m_client, &IpcClient::connectedChanged, &loop, [&]() {
      if (m_client.isConnected()) {
        loop.quit();
      }
    });
    m_client.connectTo(omacalendar::paths::socketFile());
    timer.start(timeoutMs);
    if (!m_client.isConnected()) {
      loop.exec();
    }
    if (!m_client.isConnected()) {
      m_err << "Unable to connect to the calendar service at "
            << omacalendar::paths::socketFile()
            << "\nIs omacalendard running? Try: systemctl --user start omacalendard"
            << Qt::endl;
      return false;
    }
    return true;
  }

  // Sends one request and waits. Returns the result, or nullopt after
  // printing the error (when printErrors) or a timeout.
  std::optional<QJsonValue> call(const QString& method, const QJsonObject& params,
                                 const int timeoutMs, QString* requestId = nullptr,
                                 QJsonObject* errorOut = nullptr) {
    m_result = {};
    m_error = {};
    m_done = false;
    m_pendingId = m_client.request(method, params);
    if (requestId != nullptr) {
      *requestId = m_pendingId;
    }
    if (m_pendingId.isEmpty()) {
      m_err << "Could not send the request" << Qt::endl;
      return std::nullopt;
    }
    QTimer timer;
    timer.setSingleShot(true);
    bool timedOut = false;
    QObject::connect(&timer, &QTimer::timeout, &m_loop, [&]() {
      timedOut = true;
      m_loop.quit();
    });
    timer.start(timeoutMs);
    if (!m_done && !m_protocolFailed) {
      m_loop.exec();
    }
    if (timedOut && !m_done) {
      m_err << "Request timed out after " << timeoutMs / 1000
            << " s (use --timeout to wait longer)" << Qt::endl;
      return std::nullopt;
    }
    if (m_protocolFailed) {
      return std::nullopt;
    }
    if (!m_error.isEmpty()) {
      if (errorOut != nullptr) {
        *errorOut = m_error;
      }
      return std::nullopt;
    }
    return m_result;
  }

  // Keeps printing notifications until interrupted.
  void watchForever() { m_loop.exec(); }

  void disconnect() { m_client.disconnectFromServer(); }

 private:
  IpcClient m_client;
  QTextStream& m_out;
  QTextStream& m_err;
  QEventLoop m_loop;
  QString m_pendingId;
  QJsonValue m_result;
  QJsonObject m_error;
  bool m_done = false;
  bool m_protocolFailed = false;
  bool m_watch = false;
};

void printError(const QJsonObject& error, QTextStream& err) {
  const QString message = error.value(QStringLiteral("message")).toString();
  const QString code = error.value(QStringLiteral("code")).toString();
  err << "Error";
  if (!code.isEmpty()) {
    err << " (" << code << ")";
  }
  err << ": " << (message.isEmpty() ? QStringLiteral("request failed") : message)
      << Qt::endl;
}

QString isoUtc(const QDateTime& value) {
  return value.toUTC().toString(Qt::ISODateWithMs);
}

QString displayZoneId(Session& session, const int timeoutMs) {
  const auto result =
      session.call(QStringLiteral("settings.get"),
                   {{QStringLiteral("key"), QStringLiteral("displayTimeZone")},
                    {QStringLiteral("fallback"), QString()}},
                   timeoutMs);
  const QString configured =
      result ? result->toObject().value(QStringLiteral("value")).toString() : QString();
  if (!configured.isEmpty() && QTimeZone(configured.toUtf8()).isValid()) {
    return configured;
  }
  const QByteArray system = QTimeZone::systemTimeZoneId();
  // Without a resolvable system zone (a bare container), fall back to UTC.
  return QTimeZone(system).isValid() ? QString::fromUtf8(system)
                                     : QStringLiteral("UTC");
}

// Prints events grouped by day in the display zone.
void printAgenda(const QJsonArray& events, const QDate& first, const int days,
                 const QTimeZone& zone, QTextStream& out) {
  struct Line {
    QDate day;
    bool allDay = false;
    QDateTime start;
    QDateTime end;
    QString text;
  };
  QList<Line> lines;
  const QDate last = first.addDays(days - 1);
  for (const QJsonValue& value : events) {
    const QJsonObject event = value.toObject();
    QString text = event.value(QStringLiteral("summary")).toString();
    if (text.isEmpty()) {
      text = QStringLiteral("(no title)");
    }
    const QString location = event.value(QStringLiteral("location")).toString();
    if (!location.isEmpty()) {
      text += QStringLiteral(" @ ") + location;
    }
    if (event.value(QStringLiteral("allDay")).toBool()) {
      const QDate start = QDate::fromString(
          event.value(QStringLiteral("startDate")).toString(), Qt::ISODate);
      const QDate end = QDate::fromString(
          event.value(QStringLiteral("endDate")).toString(), Qt::ISODate);
      for (QDate day = std::max(start, first);
           day.isValid() && day < end && day <= last; day = day.addDays(1)) {
        lines.append({day, true, {}, {}, text});
      }
      continue;
    }
    const QDateTime start =
        QDateTime::fromString(event.value(QStringLiteral("startUtc")).toString(),
                              Qt::ISODateWithMs)
            .toTimeZone(zone);
    const QDateTime end =
        QDateTime::fromString(event.value(QStringLiteral("endUtc")).toString(),
                              Qt::ISODateWithMs)
            .toTimeZone(zone);
    if (!start.isValid()) {
      continue;
    }
    const QDate day = std::max(start.date(), first);
    if (day <= last) {
      lines.append({day, false, start, end, text});
    }
  }
  std::stable_sort(lines.begin(), lines.end(), [](const Line& a, const Line& b) {
    if (a.day != b.day) {
      return a.day < b.day;
    }
    if (a.allDay != b.allDay) {
      return a.allDay;
    }
    return a.start < b.start;
  });
  if (lines.isEmpty()) {
    out << (days == 1 ? "No events today." : "No events.") << Qt::endl;
    return;
  }
  QDate current;
  for (const Line& line : std::as_const(lines)) {
    if (line.day != current) {
      if (current.isValid()) {
        out << Qt::endl;
      }
      current = line.day;
      out << line.day.toString(QStringLiteral("dddd, d MMMM yyyy")) << Qt::endl;
    }
    if (line.allDay) {
      out << QStringLiteral("  all day      ");
    } else {
      out << "  " << line.start.toString(QStringLiteral("HH:mm")) << "–"
          << (line.end.isValid() ? line.end.toString(QStringLiteral("HH:mm"))
                                 : QStringLiteral("     "))
          << "  ";
    }
    out << line.text << Qt::endl;
  }
}

int runAgenda(Session& session, const int days, const bool json, const int timeoutMs,
              QTextStream& out, QTextStream& err) {
  const QString zoneId = displayZoneId(session, timeoutMs);
  const QTimeZone zone(zoneId.toUtf8());
  const QDate first = QDateTime::currentDateTime(zone).date();
  const QDateTime start(first, QTime(0, 0), zone);
  const QDateTime end(first.addDays(days), QTime(0, 0), zone);
  QJsonArray events;
  for (int offset = 0;; offset += kAgendaLimit) {
    QJsonObject error;
    const auto result = session.call(QStringLiteral("events.list"),
                                     {{QStringLiteral("start"), isoUtc(start)},
                                      {QStringLiteral("end"), isoUtc(end)},
                                      {QStringLiteral("offset"), offset},
                                      {QStringLiteral("limit"), kAgendaLimit}},
                                     timeoutMs, nullptr, &error);
    if (!result) {
      if (!error.isEmpty()) {
        printError(error, err);
      }
      return 1;
    }
    const QJsonArray page =
        result->toObject().value(QStringLiteral("events")).toArray();
    for (const QJsonValue& value : page) {
      events.append(value);
    }
    if (page.size() < kAgendaLimit) {
      break;
    }
  }
  if (json) {
    printJson(QJsonObject{{QStringLiteral("timeZone"), zoneId},
                          {QStringLiteral("start"), first.toString(Qt::ISODate)},
                          {QStringLiteral("days"), days},
                          {QStringLiteral("events"), events}},
              out);
  } else {
    printAgenda(events, first, days, zone, out);
  }
  return 0;
}

int runAdd(Session& session, const QString& text, const QString& calendarOption,
           const bool json, const int timeoutMs, QTextStream& out, QTextStream& err) {
  const QString zoneId = displayZoneId(session, timeoutMs);
  const QTimeZone zone(zoneId.toUtf8());
  const QDate today = QDateTime::currentDateTime(zone).date();
  const omacalendar::QuickAddDraft draft = omacalendar::parseQuickAdd(text, today);
  if (draft.title.trimmed().isEmpty()) {
    err << "The text needs a title, for example: omacalendarctl add \"Lunch fri 12:30\""
        << Qt::endl;
    return 1;
  }
  QString calendarId = calendarOption;
  if (calendarId.isEmpty()) {
    const auto result =
        session.call(QStringLiteral("settings.get"),
                     {{QStringLiteral("key"), QStringLiteral("defaultCalendarId")},
                      {QStringLiteral("fallback"), QStringLiteral("local-default")}},
                     timeoutMs);
    calendarId = result ? result->toObject().value(QStringLiteral("value")).toString()
                        : QStringLiteral("local-default");
  }
  const QDate date = draft.date.isValid() ? draft.date : today;
  QJsonObject event{
      {QStringLiteral("calendarId"), calendarId},
      {QStringLiteral("summary"), draft.title.trimmed()},
      {QStringLiteral("location"), draft.location},
      {QStringLiteral("recurrenceRule"), draft.recurrenceRule},
  };
  if (draft.allDay || draft.startMinute < 0) {
    const QDate endDate =
        draft.allDay && draft.endDate.isValid() ? draft.endDate : date.addDays(1);
    event.insert(QStringLiteral("allDay"), true);
    event.insert(QStringLiteral("timeKind"), QStringLiteral("all_day"));
    event.insert(QStringLiteral("startDate"), date.toString(Qt::ISODate));
    event.insert(QStringLiteral("endDate"), endDate.toString(Qt::ISODate));
  } else {
    const QDateTime start(date, QTime(0, 0).addSecs(draft.startMinute * 60), zone);
    const int duration = draft.durationMinutes > 0 ? draft.durationMinutes : 60;
    event.insert(QStringLiteral("allDay"), false);
    event.insert(QStringLiteral("timeKind"), QStringLiteral("zoned"));
    event.insert(QStringLiteral("startTimeZone"), zoneId);
    event.insert(QStringLiteral("endTimeZone"), zoneId);
    event.insert(QStringLiteral("startUtc"), isoUtc(start));
    event.insert(QStringLiteral("endUtc"), isoUtc(start.addSecs(duration * 60LL)));
  }
  QJsonObject error;
  const auto result = session.call(
      QStringLiteral("events.create"),
      {{QStringLiteral("event"), event},
       {QStringLiteral("clientMutationId"),
        QUuid::createUuid().toString(QUuid::WithoutBraces)},
       {QStringLiteral("expectedLocalRevision"), 0},
       {QStringLiteral("recurrenceScope"), QStringLiteral("series")},
       {QStringLiteral("guestNotificationPolicy"), QStringLiteral("none")}},
      timeoutMs, nullptr, &error);
  if (!result) {
    if (!error.isEmpty()) {
      printError(error, err);
    }
    return 1;
  }
  if (json) {
    printJson(*result, out);
    return 0;
  }
  out << "Added \"" << draft.title.trimmed() << "\" on "
      << date.toString(QStringLiteral("dddd, d MMMM yyyy"));
  if (!event.value(QStringLiteral("allDay")).toBool()) {
    out << " at "
        << QTime(0, 0)
               .addSecs(draft.startMinute * 60)
               .toString(QStringLiteral("HH:mm"));
  }
  out << Qt::endl;
  return 0;
}

int runSync(Session& session, const bool json, const int timeoutMs, QTextStream& out,
            QTextStream& err) {
  QJsonObject error;
  const auto result =
      session.call(QStringLiteral("sync.all"), {}, timeoutMs, nullptr, &error);
  if (!result) {
    if (!error.isEmpty()) {
      printError(error, err);
    }
    return 1;
  }
  if (json) {
    printJson(*result, out);
  } else {
    out << "Sync finished." << Qt::endl;
  }
  return 0;
}

// Copies `source` to `target` (or into it, when it is a directory) as a new
// owner-only file. An existing file is never replaced.
bool copyBackup(const QString& source, const QString& target, QString* copiedTo,
                QTextStream& err) {
  QString destination = target;
  if (QFileInfo(target).isDir()) {
    destination = QDir(target).filePath(QFileInfo(source).fileName());
  }
  QFile input(source);
  if (!input.open(QIODevice::ReadOnly)) {
    err << "Could not read the backup " << source << ": " << input.errorString()
        << Qt::endl;
    return false;
  }
  const int fd = ::open(QFile::encodeName(destination).constData(),
                        O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd < 0) {
    err << "Could not create " << destination << ": " << qt_error_string(errno)
        << Qt::endl;
    return false;
  }
  QFile output;
  if (!output.open(fd, QIODevice::WriteOnly, QFileDevice::AutoCloseHandle)) {
    ::close(fd);
    err << "Could not write " << destination << Qt::endl;
    return false;
  }
  bool ok = true;
  while (ok && !input.atEnd()) {
    const QByteArray chunk = input.read(1 << 20);
    ok = !chunk.isEmpty() && output.write(chunk) == chunk.size();
  }
  ok = ok && output.flush() && ::fsync(fd) == 0;
  output.close();
  if (!ok) {
    QFile::remove(destination);
    err << "Could not write " << destination << Qt::endl;
    return false;
  }
  *copiedTo = QFileInfo(destination).absoluteFilePath();
  return true;
}

int runBackup(Session& session, const QString& copyTo, const bool json,
              const int timeoutMs, QTextStream& out, QTextStream& err) {
  QJsonObject error;
  const auto result =
      session.call(QStringLiteral("system.backup"), {}, timeoutMs, nullptr, &error);
  if (!result) {
    if (!error.isEmpty()) {
      printError(error, err);
    }
    return 1;
  }
  QJsonObject backup = result->toObject();
  const QString path = backup.value(QStringLiteral("path")).toString();
  QString copiedTo;
  if (!copyTo.isEmpty()) {
    if (!copyBackup(path, copyTo, &copiedTo, err)) {
      return 1;
    }
    backup.insert(QStringLiteral("copiedTo"), copiedTo);
  }
  if (json) {
    printJson(backup, out);
    return 0;
  }
  out << "Backup saved to " << path << " ("
      << backup.value(QStringLiteral("sizeBytes")).toInteger() << " bytes)."
      << Qt::endl;
  if (!copiedTo.isEmpty()) {
    out << "Copied to " << copiedTo << "." << Qt::endl;
  }
  return 0;
}

int runMethods(Session& session, const bool json, const int timeoutMs, QTextStream& out,
               QTextStream& err) {
  QJsonObject error;
  const auto result =
      session.call(QStringLiteral("system.info"), {}, timeoutMs, nullptr, &error);
  if (!result) {
    if (!error.isEmpty()) {
      printError(error, err);
    }
    return 1;
  }
  const QJsonArray methods =
      result->toObject().value(QStringLiteral("methods")).toArray();
  if (json) {
    printJson(methods, out);
    return 0;
  }
  for (const QJsonValue& method : methods) {
    out << method.toString() << Qt::endl;
  }
  return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
  QCoreApplication app(argc, argv);
  QCoreApplication::setApplicationName(QStringLiteral("omacalendarctl"));
  QCoreApplication::setApplicationVersion(QStringLiteral(OMACALENDAR_VERSION));
  QTextStream out(stdout);
  QTextStream err(stderr);

  QCommandLineParser parser;
  parser.setApplicationDescription(QString::fromUtf8(kUsage));
  parser.setOptionsAfterPositionalArgumentsMode(QCommandLineParser::ParseAsOptions);
  const QCommandLineOption helpOption({QStringLiteral("h"), QStringLiteral("help")},
                                      QStringLiteral("Show this help."));
  const QCommandLineOption versionOption(QStringLiteral("version"),
                                         QStringLiteral("Show the version."));
  const QCommandLineOption timeoutOption(
      QStringLiteral("timeout"),
      QStringLiteral(
          "Seconds to wait for a response (default 5; 120 for sync and backup)."),
      QStringLiteral("seconds"));
  const QCommandLineOption stdinOption(
      QStringLiteral("params-stdin"),
      QStringLiteral("Read the JSON params from stdin."));
  const QCommandLineOption watchOption(
      QStringLiteral("watch"),
      QStringLiteral("After the response, keep printing notifications on stdout "
                     "(use with system.subscribe)."));
  const QCommandLineOption jsonOption(
      QStringLiteral("json"), QStringLiteral("Print commands' results as JSON."));
  const QCommandLineOption daysOption(QStringLiteral("days"),
                                      QStringLiteral("Days for agenda (default 7)."),
                                      QStringLiteral("n"), QStringLiteral("7"));
  const QCommandLineOption calendarOption(
      QStringLiteral("calendar"),
      QStringLiteral("Calendar ID for add (default: the default calendar)."),
      QStringLiteral("id"));
  const QCommandLineOption copyToOption(
      QStringLiteral("copy-to"),
      QStringLiteral("For backup: also copy the backup to this file or directory."),
      QStringLiteral("path"));
  parser.addOptions({helpOption, versionOption, timeoutOption, stdinOption, watchOption,
                     jsonOption, daysOption, calendarOption, copyToOption});
  // JSON params may start with '-' only as the lone '-'; anything else that
  // looks like an option is rejected rather than silently sent.
  if (!parser.parse(app.arguments())) {
    err << parser.errorText() << Qt::endl << Qt::endl << kUsage;
    return 1;
  }
  if (parser.isSet(versionOption)) {
    out << "omacalendarctl " << OMACALENDAR_VERSION << Qt::endl;
    return 0;
  }
  const QStringList positional = parser.positionalArguments();
  if (parser.isSet(helpOption) || positional.isEmpty()) {
    (positional.isEmpty() && !parser.isSet(helpOption) ? err : out)
        << parser.helpText();
    return parser.isSet(helpOption) ? 0 : 1;
  }

  const QString command = positional.constFirst();
  const bool json = parser.isSet(jsonOption);
  int timeoutSeconds = kDefaultTimeoutSeconds;
  if (command == QStringLiteral("sync")) {
    timeoutSeconds = kSyncTimeoutSeconds;
  } else if (command == QStringLiteral("backup")) {
    timeoutSeconds = kBackupTimeoutSeconds;
  }
  if (parser.isSet(timeoutOption)) {
    bool ok = false;
    timeoutSeconds = parser.value(timeoutOption).toInt(&ok);
    if (!ok || timeoutSeconds <= 0 || timeoutSeconds > 3600) {
      err << "--timeout must be a number of seconds from 1 to 3600" << Qt::endl;
      return 1;
    }
  }
  const int timeoutMs = timeoutSeconds * 1000;

  // Commands that are not IPC method names (methods contain a dot).
  const bool humanCommand = !command.contains(QLatin1Char('.'));
  QJsonObject params;
  if (!humanCommand) {
    QByteArray payload;
    const bool fromStdin =
        parser.isSet(stdinOption) ||
        (positional.size() == 2 && positional.at(1) == QStringLiteral("-"));
    if (fromStdin) {
      if (positional.size() > 2 ||
          (positional.size() == 2 && positional.at(1) != QStringLiteral("-"))) {
        err << "Give the params either on the command line or on stdin, not both"
            << Qt::endl;
        return 1;
      }
      QFile input;
      if (!input.open(stdin, QIODevice::ReadOnly)) {
        err << "Could not read stdin" << Qt::endl;
        return 1;
      }
      payload = input.readAll();
    } else if (positional.size() >= 2) {
      payload = positional.mid(1).join(QLatin1Char(' ')).toUtf8();
    }
    if (!payload.trimmed().isEmpty()) {
      QJsonParseError parseError;
      const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
      if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        err << "Invalid JSON params: " << parseError.errorString() << Qt::endl;
        return 1;
      }
      params = document.object();
    }
  } else if (command != QStringLiteral("today") &&
             command != QStringLiteral("agenda") && command != QStringLiteral("add") &&
             command != QStringLiteral("sync") &&
             command != QStringLiteral("methods") &&
             command != QStringLiteral("backup")) {
    err << "Unknown command: " << command << Qt::endl << Qt::endl << kUsage;
    return 1;
  }

  int days = 1;
  if (command == QStringLiteral("agenda")) {
    bool ok = false;
    days = parser.value(daysOption).toInt(&ok);
    if (!ok || days < 1 || days > 366) {
      err << "--days must be from 1 to 366" << Qt::endl;
      return 1;
    }
  }
  const QString addText = positional.mid(1).join(QLatin1Char(' '));
  if (command == QStringLiteral("add") && addText.trimmed().isEmpty()) {
    err << "Usage: omacalendarctl add \"Lunch with Sam fri 12:30 @ Cafe\"" << Qt::endl;
    return 1;
  }

  const QString copyTo = parser.value(copyToOption);
  if (parser.isSet(copyToOption) &&
      (command != QStringLiteral("backup") || copyTo.isEmpty())) {
    err << "--copy-to takes a path and works only with backup" << Qt::endl;
    return 1;
  }

  Session session(out, err);
  session.setWatch(parser.isSet(watchOption));
  if (!session.connect(3000)) {
    return 1;
  }

  int exitCode = 1;
  if (command == QStringLiteral("today") || command == QStringLiteral("agenda")) {
    exitCode = runAgenda(session, days, json, timeoutMs, out, err);
  } else if (command == QStringLiteral("add")) {
    exitCode = runAdd(session, addText, parser.value(calendarOption), json, timeoutMs,
                      out, err);
  } else if (command == QStringLiteral("sync")) {
    exitCode = runSync(session, json, timeoutMs, out, err);
  } else if (command == QStringLiteral("backup")) {
    exitCode = runBackup(session, copyTo, json, timeoutMs, out, err);
  } else if (command == QStringLiteral("methods")) {
    exitCode = runMethods(session, json, timeoutMs, out, err);
  } else {
    QString requestId;
    QJsonObject error;
    const auto result = session.call(command, params, timeoutMs, &requestId, &error);
    if (result) {
      printJson(QJsonObject{{QStringLiteral("id"), requestId},
                            {QStringLiteral("result"), *result}},
                out);
      exitCode = 0;
      if (parser.isSet(watchOption)) {
        session.watchForever();
      }
    } else if (!error.isEmpty()) {
      printJson(QJsonObject{{QStringLiteral("id"), requestId},
                            {QStringLiteral("error"), error}},
                err);
    }
  }
  session.disconnect();
  return exitCode;
}
