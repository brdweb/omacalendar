#include "core/secretstore.h"

#include <QHash>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>
#include <utility>

namespace omacalendar {
namespace {

constexpr int kSecretToolTimeoutMs = 30000;

QString safeProcessError(QProcess& process, const QString& action) {
  // Never expose helper stderr. Some implementations and test doubles echo
  // the supplied secret when reporting an error.
  process.readAllStandardError();
  return QStringLiteral("Secret Service %1 failed with exit code %2")
      .arg(action)
      .arg(process.exitCode());
}

QString secretToolExecutable() {
  return QStandardPaths::findExecutable(QStringLiteral("secret-tool"));
}

bool validAttribute(const QString& value) {
  if (value.isEmpty() || value.size() > 256 || value.startsWith(QLatin1Char('-'))) {
    return false;
  }
  for (const QChar character : value) {
    if (character.isNull() || character.isHighSurrogate() ||
        character.isLowSurrogate() || character.category() == QChar::Other_Control ||
        character == QLatin1Char('\n') || character == QLatin1Char('\r')) {
      return false;
    }
  }
  return true;
}

SecretStoreResult failureResult(const QString& code, const QString& message) {
  SecretStoreResult result;
  result.errorCode = code;
  result.errorMessage = message;
  return result;
}

void disposeProcess(QProcess* process, QObject* owner) {
  if (process == nullptr) {
    return;
  }
  process->disconnect(owner);
  if (process->state() == QProcess::NotRunning) {
    process->deleteLater();
    return;
  }
  process->setParent(nullptr);
  QObject::connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
                   process, [process]() { process->deleteLater(); });
  process->kill();
}

QStringList attributes(const QString& accountId, const QString& kind) {
  return {QStringLiteral("application"), QStringLiteral("omacalendar"),
          QStringLiteral("account"),     accountId,
          QStringLiteral("kind"),        kind};
}

}  // namespace

QString SecretStore::executable() { return secretToolExecutable(); }

bool SecretStore::isAvailable() const { return !executable().isEmpty(); }

bool SecretStore::store(const QString& accountId, const QString& kind,
                        const QString& secret, QString* errorMessage) const {
  if (!validAttribute(accountId) || !validAttribute(kind) ||
      secret.toUtf8().size() > 1024 * 1024) {
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
  if (!validAttribute(accountId) || !validAttribute(kind)) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("Secret account and kind are required");
    }
    return {};
  }
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
  if (!validAttribute(accountId) || !validAttribute(kind)) {
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

struct AsyncSecretStore::Operation {
  SecretStoreOperationId id = kInvalidSecretStoreOperationId;
  Action action = Action::Lookup;
  QString accountId;
  QString kind;
  QByteArray pendingSecret;
  QByteArray standardOutput;
  Callback callback;
  QProcess* process = nullptr;
  QTimer* timeout = nullptr;
  bool outputTooLarge = false;
};

class AsyncSecretStore::Private final {
 public:
  QHash<SecretStoreOperationId, Operation*> operations;
};

AsyncSecretStore::AsyncSecretStore(QObject* parent)
    : QObject(parent), m_private(new Private) {}

AsyncSecretStore::~AsyncSecretStore() {
  const QList<Operation*> operations = m_private->operations.values();
  m_private->operations.clear();
  for (Operation* operation : operations) {
    if (operation->timeout != nullptr) {
      operation->timeout->stop();
    }
    if (operation->process != nullptr) {
      disposeProcess(operation->process, this);
    }
    operation->pendingSecret.fill('\0');
    operation->standardOutput.fill('\0');
    delete operation;
  }
  delete m_private;
}

bool AsyncSecretStore::isAvailable() const { return !secretToolExecutable().isEmpty(); }

int AsyncSecretStore::timeoutMs() const { return m_timeoutMs; }

void AsyncSecretStore::setTimeoutMs(const int timeoutMs) {
  m_timeoutMs = qBound(1, timeoutMs, 5 * 60 * 1000);
}

SecretStoreOperationId AsyncSecretStore::storeAsync(const QString& accountId,
                                                    const QString& kind,
                                                    const QString& secret,
                                                    Callback callback) {
  return enqueue(Action::Store, accountId, kind, secret, std::move(callback));
}

SecretStoreOperationId AsyncSecretStore::lookupAsync(const QString& accountId,
                                                     const QString& kind,
                                                     Callback callback) {
  return enqueue(Action::Lookup, accountId, kind, {}, std::move(callback));
}

SecretStoreOperationId AsyncSecretStore::removeAsync(const QString& accountId,
                                                     const QString& kind,
                                                     Callback callback) {
  return enqueue(Action::Remove, accountId, kind, {}, std::move(callback));
}

SecretStoreOperationId AsyncSecretStore::enqueue(const Action action,
                                                 const QString& accountId,
                                                 const QString& kind,
                                                 const QString& secret,
                                                 Callback callback) {
  SecretStoreOperationId operationId = kInvalidSecretStoreOperationId;
  do {
    operationId = m_nextOperationId++;
  } while (operationId == kInvalidSecretStoreOperationId ||
           m_private->operations.contains(operationId));
  auto* operation = new Operation;
  operation->id = operationId;
  operation->action = action;
  operation->accountId = accountId;
  operation->kind = kind;
  operation->pendingSecret = secret.toUtf8();
  operation->callback = std::move(callback);
  m_private->operations.insert(operationId, operation);

  QTimer::singleShot(0, this, [this, operationId]() { launch(operationId); });
  return operationId;
}

void AsyncSecretStore::launch(const SecretStoreOperationId operationId) {
  Operation* operation = m_private->operations.value(operationId, nullptr);
  if (operation == nullptr) {
    return;
  }
  if (!validAttribute(operation->accountId) || !validAttribute(operation->kind) ||
      operation->pendingSecret.size() > 1024 * 1024) {
    complete(
        operationId,
        failureResult(QStringLiteral("invalid_request"),
                      QStringLiteral("Secret account, kind, or value is invalid")));
    return;
  }
  const QString executable = secretToolExecutable();
  if (executable.isEmpty()) {
    complete(operationId,
             failureResult(QStringLiteral("helper_unavailable"),
                           QStringLiteral("Secret Service helper is unavailable")));
    return;
  }

  QStringList arguments;
  if (operation->action == Action::Store) {
    arguments = {QStringLiteral("store"),
                 QStringLiteral("--label=OmaCalendar %1").arg(operation->kind)};
  } else if (operation->action == Action::Lookup) {
    arguments = {QStringLiteral("lookup")};
  } else {
    arguments = {QStringLiteral("clear")};
  }
  arguments.append(attributes(operation->accountId, operation->kind));

  auto* process = new QProcess(this);
  auto* timeout = new QTimer(this);
  timeout->setSingleShot(true);
  operation->process = process;
  operation->timeout = timeout;

  connect(timeout, &QTimer::timeout, this, [this, operationId]() {
    Operation* current = m_private->operations.value(operationId, nullptr);
    if (current == nullptr) {
      return;
    }
    if (current->process != nullptr) {
      current->process->kill();
    }
    SecretStoreResult result = failureResult(
        QStringLiteral("timeout"), QStringLiteral("Secret Service request timed out"));
    result.timedOut = true;
    complete(operationId, std::move(result));
  });
  connect(process, &QProcess::started, this, [this, operationId]() {
    Operation* current = m_private->operations.value(operationId, nullptr);
    if (current == nullptr || current->action != Action::Store) {
      return;
    }
    current->process->write(current->pendingSecret);
    current->process->write("\n");
    current->process->closeWriteChannel();
    current->pendingSecret.fill('\0');
    current->pendingSecret.clear();
  });
  connect(process, &QProcess::readyReadStandardOutput, this, [this, operationId]() {
    Operation* current = m_private->operations.value(operationId, nullptr);
    if (current == nullptr) {
      return;
    }
    const QByteArray output = current->process->readAllStandardOutput();
    if (current->action == Action::Lookup) {
      current->standardOutput.append(output);
      if (current->standardOutput.size() > 1024 * 1024) {
        current->outputTooLarge = true;
        current->process->kill();
      }
    }
  });
  connect(process, &QProcess::readyReadStandardError, this, [this, operationId]() {
    Operation* current = m_private->operations.value(operationId, nullptr);
    if (current != nullptr) {
      current->process->readAllStandardError();
    }
  });
  connect(process, &QProcess::errorOccurred, this,
          [this, operationId](const QProcess::ProcessError processError) {
            if (processError != QProcess::FailedToStart ||
                !m_private->operations.contains(operationId)) {
              return;
            }
            complete(
                operationId,
                failureResult(QStringLiteral("start_failed"),
                              QStringLiteral("Secret Service helper could not start")));
          });
  connect(
      process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
      [this, operationId](const int exitCode, const QProcess::ExitStatus exitStatus) {
        Operation* current = m_private->operations.value(operationId, nullptr);
        if (current == nullptr) {
          return;
        }
        if (current->action == Action::Lookup) {
          current->standardOutput.append(current->process->readAllStandardOutput());
        } else {
          current->process->readAllStandardOutput();
        }
        current->process->readAllStandardError();
        if (current->outputTooLarge) {
          complete(
              operationId,
              failureResult(QStringLiteral("result_too_large"),
                            QStringLiteral("Secret Service result was too large")));
          return;
        }
        if (exitStatus == QProcess::NormalExit && exitCode == 1 &&
            current->action == Action::Remove) {
          complete(operationId,
                   failureResult(QStringLiteral("not_found"),
                                 QStringLiteral("Secret Service item was not found")));
          return;
        }
        if (exitStatus != QProcess::NormalExit || exitCode != 0) {
          complete(operationId,
                   failureResult(QStringLiteral("process_failed"),
                                 QStringLiteral("Secret Service request failed")));
          return;
        }
        SecretStoreResult result;
        result.success = true;
        if (current->action == Action::Lookup) {
          while (current->standardOutput.endsWith('\n') ||
                 current->standardOutput.endsWith('\r')) {
            current->standardOutput.chop(1);
          }
          result.value = QString::fromUtf8(current->standardOutput);
        }
        complete(operationId, std::move(result));
      });

  const QIODevice::OpenMode mode =
      operation->action == Action::Store ? QIODevice::ReadWrite : QIODevice::ReadOnly;
  timeout->start(m_timeoutMs);
  process->start(executable, arguments, mode);
}

void AsyncSecretStore::complete(const SecretStoreOperationId operationId,
                                SecretStoreResult result) {
  Operation* operation = m_private->operations.take(operationId);
  if (operation == nullptr) {
    return;
  }
  if (operation->timeout != nullptr) {
    operation->timeout->stop();
    operation->timeout->deleteLater();
  }
  if (operation->process != nullptr) {
    disposeProcess(operation->process, this);
  }
  operation->pendingSecret.fill('\0');
  operation->standardOutput.fill('\0');
  Callback callback = std::move(operation->callback);
  delete operation;
  if (callback) {
    QTimer::singleShot(
        0, this,
        [callback = std::move(callback), result = std::move(result)]() mutable {
          callback(std::move(result));
        });
  }
}

void AsyncSecretStore::cancel(const SecretStoreOperationId operationId) {
  Operation* operation = m_private->operations.value(operationId, nullptr);
  if (operation == nullptr) {
    return;
  }
  if (operation->process != nullptr) {
    operation->process->kill();
  }
  SecretStoreResult result =
      failureResult(QStringLiteral("cancelled"),
                    QStringLiteral("Secret Service request was cancelled"));
  result.cancelled = true;
  complete(operationId, std::move(result));
}

void AsyncSecretStore::cancelAll() {
  const QList<SecretStoreOperationId> operationIds = m_private->operations.keys();
  for (const SecretStoreOperationId operationId : operationIds) {
    cancel(operationId);
  }
}

}  // namespace omacalendar
