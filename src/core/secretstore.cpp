#include "core/secretstore.h"

#include <QProcess>
#include <QStandardPaths>

namespace omacalendar {
namespace {

constexpr int kSecretToolTimeoutMs = 30000;

QString safeProcessError(QProcess& process, const QString& action) {
  const QString stderrText =
      QString::fromUtf8(process.readAllStandardError()).trimmed().left(300);
  if (!stderrText.isEmpty()) {
    return QStringLiteral("Secret Service %1 failed: %2").arg(action, stderrText);
  }
  return QStringLiteral("Secret Service %1 failed with exit code %2")
      .arg(action)
      .arg(process.exitCode());
}

QStringList attributes(const QString& accountId, const QString& kind) {
  return {QStringLiteral("application"), QStringLiteral("omacalendar"),
          QStringLiteral("account"),     accountId,
          QStringLiteral("kind"),        kind};
}

}  // namespace

QString SecretStore::executable() {
  return QStandardPaths::findExecutable(QStringLiteral("secret-tool"));
}

bool SecretStore::isAvailable() const { return !executable().isEmpty(); }

bool SecretStore::store(const QString& accountId, const QString& kind,
                        const QString& secret, QString* errorMessage) const {
  if (accountId.isEmpty() || kind.isEmpty()) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("Secret account and kind are required");
    }
    return false;
  }
  const QString command = executable();
  if (command.isEmpty()) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("secret-tool is not installed");
    }
    return false;
  }

  QProcess process;
  QStringList arguments = {
      QStringLiteral("store"),
      QStringLiteral("--label=OmaCalendar %1").arg(kind),
  };
  arguments.append(attributes(accountId, kind));
  process.start(command, arguments, QIODevice::ReadWrite);
  if (!process.waitForStarted(kSecretToolTimeoutMs)) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("Could not start secret-tool");
    }
    return false;
  }
  process.write(secret.toUtf8());
  process.write("\n");
  process.closeWriteChannel();
  if (!process.waitForFinished(kSecretToolTimeoutMs)) {
    process.kill();
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("Secret Service store timed out");
    }
    return false;
  }
  if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
    if (errorMessage != nullptr) {
      *errorMessage = safeProcessError(process, QStringLiteral("store"));
    }
    return false;
  }
  return true;
}

QString SecretStore::lookup(const QString& accountId, const QString& kind,
                            QString* errorMessage) const {
  const QString command = executable();
  if (command.isEmpty()) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("secret-tool is not installed");
    }
    return {};
  }
  QProcess process;
  QStringList arguments = {QStringLiteral("lookup")};
  arguments.append(attributes(accountId, kind));
  process.start(command, arguments, QIODevice::ReadOnly);
  if (!process.waitForFinished(kSecretToolTimeoutMs)) {
    process.kill();
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("Secret Service lookup timed out");
    }
    return {};
  }
  if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
    if (errorMessage != nullptr) {
      *errorMessage = safeProcessError(process, QStringLiteral("lookup"));
    }
    return {};
  }
  QByteArray value = process.readAllStandardOutput();
  while (value.endsWith('\n') || value.endsWith('\r')) {
    value.chop(1);
  }
  return QString::fromUtf8(value);
}

bool SecretStore::remove(const QString& accountId, const QString& kind,
                         QString* errorMessage) const {
  const QString command = executable();
  if (command.isEmpty()) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("secret-tool is not installed");
    }
    return false;
  }
  QProcess process;
  QStringList arguments = {QStringLiteral("clear")};
  arguments.append(attributes(accountId, kind));
  process.start(command, arguments, QIODevice::ReadOnly);
  if (!process.waitForFinished(kSecretToolTimeoutMs)) {
    process.kill();
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("Secret Service clear timed out");
    }
    return false;
  }
  if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
    if (errorMessage != nullptr) {
      *errorMessage = safeProcessError(process, QStringLiteral("clear"));
    }
    return false;
  }
  return true;
}

}  // namespace omacalendar
