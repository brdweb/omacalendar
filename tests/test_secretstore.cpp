#include <QFile>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

#include "core/secretstore.h"

using namespace omacalendar;

class SecretStoreTest final : public QObject {
  Q_OBJECT

 private slots:
  void initTestCase();
  void cleanupTestCase();
  void asynchronousSuccess();
  void failuresAreSanitized();
  void absentRemoveHasStableClassification();
  void timeoutKeepsEventLoopResponsive();
  void cancellationAndDestructionAreSafe();
  void invalidRequestsCompleteAsynchronously();

 private:
  void setMode(const QByteArray& mode, const QByteArray& value = {});

  QTemporaryDir m_directory;
  QByteArray m_originalPath;
  QByteArray m_originalMode;
  QByteArray m_originalValue;
  bool m_hadMode = false;
  bool m_hadValue = false;
};

void SecretStoreTest::initTestCase() {
  QVERIFY(m_directory.isValid());
  const QString helperPath = m_directory.filePath(QStringLiteral("secret-tool"));
  QFile helper(helperPath);
  QVERIFY(helper.open(QIODevice::WriteOnly));
  const QByteArray script = QByteArrayLiteral(R"SH(#!/bin/sh
mode="${FAKE_SECRET_MODE:-success}"
case "$mode" in
  success)
    case "$1" in
      lookup) printf '%s\n' "$FAKE_SECRET_VALUE" ;;
      store) IFS= read -r ignored; exit 0 ;;
      clear) exit 0 ;;
      *) exit 2 ;;
    esac
    ;;
  failure)
    printf '%s\n' "$FAKE_SECRET_VALUE" >&2
    exit 9
    ;;
  missing)
    [ "$1" = clear ] && exit 1
    exit 4
    ;;
  timeout)
    exec /bin/sleep 5
    ;;
  *) exit 3 ;;
esac
)SH");
  QCOMPARE(helper.write(script), script.size());
  helper.close();
  QVERIFY(QFile::setPermissions(helperPath, QFileDevice::ReadOwner |
                                                QFileDevice::WriteOwner |
                                                QFileDevice::ExeOwner));

  m_originalPath = qgetenv("PATH");
  m_hadMode = qEnvironmentVariableIsSet("FAKE_SECRET_MODE");
  m_hadValue = qEnvironmentVariableIsSet("FAKE_SECRET_VALUE");
  m_originalMode = qgetenv("FAKE_SECRET_MODE");
  m_originalValue = qgetenv("FAKE_SECRET_VALUE");
  qputenv("PATH", m_directory.path().toUtf8() + ':' + m_originalPath);
  setMode(QByteArrayLiteral("success"), QByteArrayLiteral("initial"));
}

void SecretStoreTest::cleanupTestCase() {
  qputenv("PATH", m_originalPath);
  if (m_hadMode) {
    qputenv("FAKE_SECRET_MODE", m_originalMode);
  } else {
    qunsetenv("FAKE_SECRET_MODE");
  }
  if (m_hadValue) {
    qputenv("FAKE_SECRET_VALUE", m_originalValue);
  } else {
    qunsetenv("FAKE_SECRET_VALUE");
  }
}

void SecretStoreTest::setMode(const QByteArray& mode, const QByteArray& value) {
  qputenv("FAKE_SECRET_MODE", mode);
  qputenv("FAKE_SECRET_VALUE", value);
}

void SecretStoreTest::asynchronousSuccess() {
  setMode(QByteArrayLiteral("success"),
          QByteArrayLiteral("  preserved secret value  "));
  AsyncSecretStore store;
  store.setTimeoutMs(1000);
  QVERIFY(store.isAvailable());

  bool completed = false;
  SecretStoreResult result;
  const SecretStoreOperationId storeId = store.storeAsync(
      QStringLiteral("account-1"), QStringLiteral("password"),
      QStringLiteral("write-only-secret"), [&](SecretStoreResult value) {
        result = std::move(value);
        completed = true;
      });
  QVERIFY(storeId != kInvalidSecretStoreOperationId);
  QTRY_VERIFY_WITH_TIMEOUT(completed, 1000);
  QVERIFY(result.success);
  QVERIFY(result.value.isEmpty());

  completed = false;
  result = {};
  const SecretStoreOperationId lookupId =
      store.lookupAsync(QStringLiteral("account-1"), QStringLiteral("password"),
                        [&](SecretStoreResult value) {
                          result = std::move(value);
                          completed = true;
                        });
  QVERIFY(lookupId != kInvalidSecretStoreOperationId);
  QTRY_VERIFY_WITH_TIMEOUT(completed, 1000);
  QVERIFY(result.success);
  QCOMPARE(result.value, QStringLiteral("  preserved secret value  "));

  completed = false;
  result = {};
  const SecretStoreOperationId removeId =
      store.removeAsync(QStringLiteral("account-1"), QStringLiteral("password"),
                        [&](SecretStoreResult value) {
                          result = std::move(value);
                          completed = true;
                        });
  QVERIFY(removeId != kInvalidSecretStoreOperationId);
  QTRY_VERIFY_WITH_TIMEOUT(completed, 1000);
  QVERIFY(result.success);
}

void SecretStoreTest::failuresAreSanitized() {
  const QByteArray secret = QByteArrayLiteral("NEVER-ECHO-THIS-CREDENTIAL");
  setMode(QByteArrayLiteral("failure"), secret);
  AsyncSecretStore store;
  store.setTimeoutMs(1000);
  bool completed = false;
  SecretStoreResult result;
  const SecretStoreOperationId failureId =
      store.storeAsync(QStringLiteral("account-2"), QStringLiteral("password"),
                       QString::fromLatin1(secret), [&](SecretStoreResult value) {
                         result = std::move(value);
                         completed = true;
                       });
  QVERIFY(failureId != kInvalidSecretStoreOperationId);
  QTRY_VERIFY_WITH_TIMEOUT(completed, 1000);
  QVERIFY(!result.success);
  QCOMPARE(result.errorCode, QStringLiteral("process_failed"));
  QVERIFY(result.value.isEmpty());
  QVERIFY(!result.errorMessage.contains(QString::fromLatin1(secret)));

  // The compatibility API is sanitized as well, even though existing callers
  // still block until they migrate to AsyncSecretStore.
  SecretStore compatibilityStore;
  QString compatibilityError;
  QVERIFY(compatibilityStore
              .lookup(QStringLiteral("account-2"), QStringLiteral("password"),
                      &compatibilityError)
              .isEmpty());
  QVERIFY(!compatibilityError.contains(QString::fromLatin1(secret)));
}

void SecretStoreTest::absentRemoveHasStableClassification() {
  setMode(QByteArrayLiteral("missing"));
  AsyncSecretStore store;
  store.setTimeoutMs(1000);
  bool completed = false;
  SecretStoreResult result;
  const SecretStoreOperationId operationId =
      store.removeAsync(QStringLiteral("missing-account"),
                        QStringLiteral("refresh-token"), [&](SecretStoreResult value) {
                          result = std::move(value);
                          completed = true;
                        });
  QVERIFY(operationId != kInvalidSecretStoreOperationId);
  QTRY_VERIFY_WITH_TIMEOUT(completed, 1000);
  QVERIFY(!result.success);
  QCOMPARE(result.errorCode, QStringLiteral("not_found"));
  QVERIFY(!result.errorMessage.contains(QStringLiteral("exit"), Qt::CaseInsensitive));
}

void SecretStoreTest::timeoutKeepsEventLoopResponsive() {
  setMode(QByteArrayLiteral("timeout"));
  AsyncSecretStore store;
  store.setTimeoutMs(120);
  bool heartbeat = false;
  bool completed = false;
  SecretStoreResult result;
  QTimer::singleShot(10, this, [&]() { heartbeat = true; });
  const SecretStoreOperationId timeoutId =
      store.lookupAsync(QStringLiteral("account-3"), QStringLiteral("password"),
                        [&](SecretStoreResult value) {
                          result = std::move(value);
                          completed = true;
                        });
  QVERIFY(timeoutId != kInvalidSecretStoreOperationId);

  QTRY_VERIFY_WITH_TIMEOUT(heartbeat, 80);
  QVERIFY(!completed);
  QTRY_VERIFY_WITH_TIMEOUT(completed, 1000);
  QVERIFY(!result.success);
  QVERIFY(result.timedOut);
  QCOMPARE(result.errorCode, QStringLiteral("timeout"));
}

void SecretStoreTest::cancellationAndDestructionAreSafe() {
  setMode(QByteArrayLiteral("timeout"));
  AsyncSecretStore store;
  store.setTimeoutMs(2000);
  bool completed = false;
  SecretStoreResult result;
  const SecretStoreOperationId operationId =
      store.lookupAsync(QStringLiteral("account-4"), QStringLiteral("password"),
                        [&](SecretStoreResult value) {
                          result = std::move(value);
                          completed = true;
                        });
  QTimer::singleShot(20, this, [&store, operationId]() { store.cancel(operationId); });
  QTRY_VERIFY_WITH_TIMEOUT(completed, 500);
  QVERIFY(result.cancelled);
  QCOMPARE(result.errorCode, QStringLiteral("cancelled"));

  bool lateCallback = false;
  auto* temporaryStore = new AsyncSecretStore;
  temporaryStore->setTimeoutMs(1000);
  const SecretStoreOperationId temporaryId = temporaryStore->lookupAsync(
      QStringLiteral("account-5"), QStringLiteral("password"),
      [&](SecretStoreResult) { lateCallback = true; });
  QVERIFY(temporaryId != kInvalidSecretStoreOperationId);
  QTest::qWait(30);
  delete temporaryStore;
  QTest::qWait(150);
  QVERIFY(!lateCallback);
}

void SecretStoreTest::invalidRequestsCompleteAsynchronously() {
  setMode(QByteArrayLiteral("success"));
  AsyncSecretStore store;
  bool methodReturned = false;
  bool completed = false;
  SecretStoreResult result;
  const SecretStoreOperationId invalidId =
      store.lookupAsync({}, QStringLiteral("password"), [&](SecretStoreResult value) {
        QVERIFY(methodReturned);
        result = std::move(value);
        completed = true;
      });
  QVERIFY(invalidId != kInvalidSecretStoreOperationId);
  methodReturned = true;
  QVERIFY(!completed);
  QTRY_VERIFY_WITH_TIMEOUT(completed, 500);
  QCOMPARE(result.errorCode, QStringLiteral("invalid_request"));
}

QTEST_MAIN(SecretStoreTest)

#include "test_secretstore.moc"
