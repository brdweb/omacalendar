#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSqlError>
#include <QSqlQuery>
#include <algorithm>
#include <cerrno>
#include <cstring>

#include "core/database.h"

namespace omacalendar {

namespace {

const QRegularExpression& backupNamePattern() {
  static const QRegularExpression pattern(
      QStringLiteral(R"(^calendar-(\d{8}T\d{6}Z)(?:-(\d+))?\.sqlite3$)"));
  return pattern;
}

void setError(QString* errorMessage, const QString& message) {
  if (errorMessage != nullptr) {
    *errorMessage = message;
  }
}

QString systemError() { return QString::fromLocal8Bit(std::strerror(errno)); }

// Creates `directory` if needed and makes sure it is a real directory owned by
// this user with mode 0700.
bool preparePrivateDirectory(const QString& directory, QString* errorMessage) {
  if (!QDir().mkpath(directory)) {
    setError(errorMessage,
             QStringLiteral("Could not create backup directory %1").arg(directory));
    return false;
  }
  const QByteArray path = QFile::encodeName(directory);
  struct stat info{};
  if (::lstat(path.constData(), &info) != 0 || !S_ISDIR(info.st_mode) ||
      info.st_uid != ::getuid()) {
    setError(
        errorMessage,
        QStringLiteral("Backup directory is not a directory owned by this user: %1")
            .arg(directory));
    return false;
  }
  if (::chmod(path.constData(), 0700) != 0) {
    setError(errorMessage, QStringLiteral("Could not restrict permissions on %1: %2")
                               .arg(directory, systemError()));
    return false;
  }
  return true;
}

bool syncPath(const QString& path, int flags) {
  const int fd = ::open(QFile::encodeName(path).constData(), flags | O_CLOEXEC);
  if (fd < 0) {
    return false;
  }
  const bool ok = ::fsync(fd) == 0;
  ::close(fd);
  return ok;
}

struct BackupEntry {
  QString name;
  QString stamp;
  int sequence = 1;
};

}  // namespace

bool Database::createBackup(const QString& directory, int keep, const QDateTime& now,
                            DatabaseBackup* backup, QString* errorMessage) {
  if (!isOpen()) {
    setError(errorMessage, QStringLiteral("The database is not open"));
    return false;
  }
  if (!preparePrivateDirectory(directory, errorMessage)) {
    return false;
  }
  const QDir dir(directory);
  const QDateTime createdAt = now.toUTC();
  const QString stamp = createdAt.toString(QStringLiteral("yyyyMMdd'T'HHmmss'Z'"));
  // Two backups within the same second get a -2, -3, ... suffix.
  QString name;
  for (int sequence = 1; sequence < 1000; ++sequence) {
    const QString candidate =
        sequence == 1
            ? QStringLiteral("calendar-%1.sqlite3").arg(stamp)
            : QStringLiteral("calendar-%1-%2.sqlite3").arg(stamp).arg(sequence);
    if (!QFileInfo::exists(dir.filePath(candidate)) &&
        !QFileInfo(dir.filePath(candidate)).isSymLink()) {
      name = candidate;
      break;
    }
  }
  if (name.isEmpty()) {
    setError(errorMessage, QStringLiteral("Too many backups in one second"));
    return false;
  }
  const QString finalPath = dir.filePath(name);
  const QString partialPath = dir.filePath(QStringLiteral(".%1.partial").arg(name));
  const QByteArray partial = QFile::encodeName(partialPath);
  const QByteArray finalName = QFile::encodeName(finalPath);

  // The copy is created empty and owner-only before SQLite writes to it, so
  // no other user can open it at any point. VACUUM INTO accepts an empty file.
  ::unlink(partial.constData());
  const int fd = ::open(partial.constData(),
                        O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd < 0) {
    setError(errorMessage, QStringLiteral("Could not create backup file %1: %2")
                               .arg(partialPath, systemError()));
    return false;
  }
  ::fchmod(fd, 0600);
  ::close(fd);

  QSqlQuery query(m_database);
  if (!query.prepare(QStringLiteral("VACUUM INTO ?"))) {
    ::unlink(partial.constData());
    setError(errorMessage, query.lastError().text());
    return false;
  }
  query.addBindValue(partialPath);
  if (!query.exec()) {
    ::unlink(partial.constData());
    setError(errorMessage,
             QStringLiteral("Backup failed: %1").arg(query.lastError().text()));
    return false;
  }
  if (!syncPath(partialPath, O_RDONLY)) {
    ::unlink(partial.constData());
    setError(errorMessage, QStringLiteral("Could not flush backup file %1: %2")
                               .arg(partialPath, systemError()));
    return false;
  }
  // link() never replaces an existing file, unlike rename().
  if (::link(partial.constData(), finalName.constData()) != 0) {
    const QString reason = systemError();
    ::unlink(partial.constData());
    setError(errorMessage,
             QStringLiteral("Could not finish backup %1: %2").arg(finalPath, reason));
    return false;
  }
  ::unlink(partial.constData());
  syncPath(directory, O_RDONLY | O_DIRECTORY);

  // Retention: newest first by timestamp, then by same-second sequence. The
  // backup just written is always kept, even if the clock went backwards.
  QList<BackupEntry> entries;
  const QStringList names = dir.entryList(QDir::Files | QDir::Hidden | QDir::System);
  for (const QString& entryName : names) {
    const QRegularExpressionMatch match = backupNamePattern().match(entryName);
    if (!match.hasMatch() || entryName == name) {
      continue;
    }
    BackupEntry entry;
    entry.name = entryName;
    entry.stamp = match.captured(1);
    entry.sequence = match.captured(2).isEmpty() ? 1 : match.captured(2).toInt();
    entries.append(entry);
  }
  std::sort(entries.begin(), entries.end(),
            [](const BackupEntry& left, const BackupEntry& right) {
              if (left.stamp != right.stamp) {
                return left.stamp > right.stamp;
              }
              return left.sequence > right.sequence;
            });
  const qsizetype othersToKeep = std::max(keep, 1) - 1;
  for (qsizetype index = othersToKeep; index < entries.size(); ++index) {
    QFile::remove(dir.filePath(entries.at(index).name));
  }

  if (backup != nullptr) {
    backup->path = finalPath;
    backup->sizeBytes = QFileInfo(finalPath).size();
    backup->createdAt = createdAt;
  }
  return true;
}

}  // namespace omacalendar
