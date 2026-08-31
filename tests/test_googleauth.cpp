#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

#include "core/database.h"
#include "providers/google/googleauth.h"
#include "providers/google/googlesync.h"

using namespace omacalendar;
using namespace omacalendar::google;

class GoogleAuthAsyncTest final : public QObject {
  Q_OBJECT

 private slots:
  void initTestCase();
  void cleanupTestCase();
  void delayedClientLookupQueuesAuthorizationWithoutBlocking();
  void delayedClientSecretStoreKeepsEventLoopResponsive();
  void clientSecretStoreFailureIsSanitized();
  void cancellationAndDestructionSuppressLateAuthorization();
  void absentRefreshTokenRemovalIsIdempotent();
  void delayedDisconnectCommitsOnlyAfterCredentialRemoval();

 private:
  void setHelper(const QByteArray& mode, const QByteArray& value = {},
                 const QByteArray& delay = QByteArrayLiteral("0"));

  QTemporaryDir m_directory;
  QByteArray m_originalPath;
  QByteArray m_originalMode;
  QByteArray m_originalValue;
  QByteArray m_originalDelay;
  bool m_hadMode = false;
  bool m_hadValue = false;
  bool m_hadDelay = false;
};

void GoogleAuthAsyncTest::initTestCase() {
  QVERIFY(m_directory.isValid());
  QFile helper(m_directory.filePath(QStringLiteral("secret-tool")));
  QVERIFY(helper.open(QIODevice::WriteOnly));
  const QByteArray script = QByteArrayLiteral(R"SH(#!/bin/sh
mode="${FAKE_GOOGLE_SECRET_MODE:-success}"
delay="${FAKE_GOOGLE_SECRET_DELAY:-0}"
case "$1:$mode" in
  lookup:success)
    /bin/sleep "$delay"
    printf '%s\n' "$FAKE_GOOGLE_SECRET_VALUE"
    ;;
  store:success)
    IFS= read -r ignored
    /bin/sleep "$delay"
    exit 0
    ;;
  clear:success)
    /bin/sleep "$delay"
    exit 0
    ;;
  clear:missing)
    /bin/sleep "$delay"
    exit 1
    ;;
  store:failure)
    IFS= read -r ignored
    /bin/sleep "$delay"
    printf '%s\n' "$FAKE_GOOGLE_SECRET_VALUE" >&2
    exit 9
    ;;
  *) exit 7 ;;
esac
)SH");
  QCOMPARE(helper.write(script), script.size());
  helper.close();
  QVERIFY(QFile::setPermissions(helper.fileName(), QFileDevice::ReadOwner |
                                                       QFileDevice::WriteOwner |
                                                       QFileDevice::ExeOwner));

  m_originalPath = qgetenv("PATH");
  m_hadMode = qEnvironmentVariableIsSet("FAKE_GOOGLE_SECRET_MODE");
  m_hadValue = qEnvironmentVariableIsSet("FAKE_GOOGLE_SECRET_VALUE");
  m_hadDelay = qEnvironmentVariableIsSet("FAKE_GOOGLE_SECRET_DELAY");
  m_originalMode = qgetenv("FAKE_GOOGLE_SECRET_MODE");
  m_originalValue = qgetenv("FAKE_GOOGLE_SECRET_VALUE");
  m_originalDelay = qgetenv("FAKE_GOOGLE_SECRET_DELAY");
  qputenv("PATH", m_directory.path().toUtf8() + ':' + m_originalPath);
}

void GoogleAuthAsyncTest::cleanupTestCase() {
  qputenv("PATH", m_originalPath);
  const auto restore = [](const char* name, const bool existed,
                          const QByteArray& value) {
    if (existed) {
      qputenv(name, value);
    } else {
      qunsetenv(name);
    }
  };
  restore("FAKE_GOOGLE_SECRET_MODE", m_hadMode, m_originalMode);
  restore("FAKE_GOOGLE_SECRET_VALUE", m_hadValue, m_originalValue);
  restore("FAKE_GOOGLE_SECRET_DELAY", m_hadDelay, m_originalDelay);
}

void GoogleAuthAsyncTest::setHelper(const QByteArray& mode, const QByteArray& value,
                                    const QByteArray& delay) {
  qputenv("FAKE_GOOGLE_SECRET_MODE", mode);
  qputenv("FAKE_GOOGLE_SECRET_VALUE", value);
  qputenv("FAKE_GOOGLE_SECRET_DELAY", delay);
}

void GoogleAuthAsyncTest::delayedClientLookupQueuesAuthorizationWithoutBlocking() {
  setHelper(QByteArrayLiteral("success"), QByteArrayLiteral("stored-client-secret"),
            QByteArrayLiteral("0.25"));
  GoogleAuthManager auth;
  QSignalSpy authorizationUrl(&auth, &GoogleAuthManager::authorizationUrlReady);
  bool heartbeat = false;
  QTimer::singleShot(25, this, [&heartbeat]() { heartbeat = true; });

  QString error;
  QVERIFY2(auth.restoreClient(QStringLiteral("client-id.example.invalid"), &error),
           qPrintable(error));
  QVERIFY2(auth.startAuthorization(QStringLiteral("queued-account"), &error),
           qPrintable(error));
  QCOMPARE(authorizationUrl.count(), 0);
  QTRY_VERIFY_WITH_TIMEOUT(heartbeat, 150);
  QCOMPARE(authorizationUrl.count(), 0);
  QTRY_COMPARE_WITH_TIMEOUT(authorizationUrl.count(), 1, 1500);
  QCOMPARE(authorizationUrl.first().at(0).toString(), QStringLiteral("queued-account"));
  auth.cancel(QStringLiteral("queued-account"));
}

void GoogleAuthAsyncTest::delayedClientSecretStoreKeepsEventLoopResponsive() {
  setHelper(QByteArrayLiteral("success"), {}, QByteArrayLiteral("0.25"));
  GoogleAuthManager auth;
  QSignalSpy completion(&auth, &GoogleAuthManager::clientConfigurationFinished);
  bool heartbeat = false;
  QTimer::singleShot(25, this, [&heartbeat]() { heartbeat = true; });

  QString error;
  QVERIFY2(auth.configureClient(QStringLiteral("client-id.example.invalid"),
                                QStringLiteral("write-only-secret"), &error),
           qPrintable(error));
  QTRY_VERIFY_WITH_TIMEOUT(heartbeat, 150);
  QCOMPARE(completion.count(), 0);
  QTRY_COMPARE_WITH_TIMEOUT(completion.count(), 1, 1500);
  QVERIFY(completion.first().at(0).toBool());
}

void GoogleAuthAsyncTest::clientSecretStoreFailureIsSanitized() {
  const QByteArray secret = QByteArrayLiteral("NEVER-ECHO-GOOGLE-CREDENTIAL");
  setHelper(QByteArrayLiteral("failure"), secret, QByteArrayLiteral("0.05"));
  GoogleAuthManager auth;
  QSignalSpy completion(&auth, &GoogleAuthManager::clientConfigurationFinished);
  QString error;
  QVERIFY2(auth.configureClient(QStringLiteral("client-id.example.invalid"),
                                QString::fromLatin1(secret), &error),
           qPrintable(error));
  QTRY_COMPARE_WITH_TIMEOUT(completion.count(), 1, 1000);
  QVERIFY(!completion.first().at(0).toBool());
  QCOMPARE(completion.first().at(1).toString(), QStringLiteral("process_failed"));
  QVERIFY(!completion.first().at(2).toString().contains(QString::fromLatin1(secret)));
}

void GoogleAuthAsyncTest::cancellationAndDestructionSuppressLateAuthorization() {
  setHelper(QByteArrayLiteral("success"), QByteArrayLiteral("refresh-token"),
            QByteArrayLiteral("0.25"));
  GoogleAuthManager auth;
  QString error;
  QVERIFY(
      auth.configureClient(QStringLiteral("client-id.example.invalid"), {}, &error));
  QSignalSpy failed(&auth, &GoogleAuthManager::authorizationFailed);
  QVERIFY(auth.restoreAuthorization(QStringLiteral("cancelled-account"), &error));
  QTimer::singleShot(25, &auth,
                     [&auth]() { auth.cancel(QStringLiteral("cancelled-account")); });
  QTest::qWait(400);
  QCOMPARE(failed.count(), 0);
  QVERIFY(!auth.hasAccessToken(QStringLiteral("cancelled-account")));

  bool lateSignal = false;
  auto* temporary = new GoogleAuthManager;
  QVERIFY(temporary->restoreClient(QStringLiteral("other-client.example.invalid")));
  connect(temporary, &GoogleAuthManager::authorizationUrlReady, this,
          [&lateSignal]() { lateSignal = true; });
  QVERIFY(temporary->startAuthorization(QStringLiteral("destroyed-account")));
  QTimer::singleShot(25, temporary, [temporary]() { delete temporary; });
  QTest::qWait(400);
  QVERIFY(!lateSignal);
}

void GoogleAuthAsyncTest::absentRefreshTokenRemovalIsIdempotent() {
  setHelper(QByteArrayLiteral("missing"), {}, QByteArrayLiteral("0.05"));
  GoogleAuthManager auth;
  QSignalSpy completion(&auth, &GoogleAuthManager::forgetFinished);
  QString error;
  QVERIFY2(auth.forget(QStringLiteral("already-removed-account"), &error),
           qPrintable(error));
  QTRY_COMPARE_WITH_TIMEOUT(completion.count(), 1, 1000);
  QCOMPARE(completion.first().at(0).toString(),
           QStringLiteral("already-removed-account"));
  QVERIFY(completion.first().at(1).toBool());
  QVERIFY(completion.first().at(2).toString().isEmpty());
}

void GoogleAuthAsyncTest::delayedDisconnectCommitsOnlyAfterCredentialRemoval() {
  setHelper(QByteArrayLiteral("success"), {}, QByteArrayLiteral("0.25"));
  QTemporaryDir databaseDirectory;
  QVERIFY(databaseDirectory.isValid());
  Database database;
  QString error;
  QVERIFY2(
      database.open(databaseDirectory.filePath(QStringLiteral("calendar.db")), &error),
      qPrintable(error));
  Account account;
  account.id = QStringLiteral("disconnect-account");
  account.provider = ProviderKind::Google;
  account.displayName = QStringLiteral("Delayed disconnect");
  account.enabled = true;
  account.authStatus = QStringLiteral("connected");
  QVERIFY2(database.upsertAccount(account, &error), qPrintable(error));

  GoogleSync sync(&database);
  QSignalSpy changed(&sync, &GoogleSync::accountChanged);
  bool heartbeat = false;
  QTimer::singleShot(25, this, [&heartbeat]() { heartbeat = true; });
  QVERIFY2(sync.disconnectAccount(account.id, false, &error), qPrintable(error));
  QCOMPARE(database.account(account.id).authStatus, QStringLiteral("disconnecting"));
  QTRY_VERIFY_WITH_TIMEOUT(heartbeat, 150);
  QCOMPARE(database.account(account.id).authStatus, QStringLiteral("disconnecting"));
  QTRY_COMPARE_WITH_TIMEOUT(database.account(account.id).authStatus,
                            QStringLiteral("disconnected"), 1500);
  QVERIFY(changed.count() >= 2);
}

QTEST_MAIN(GoogleAuthAsyncTest)

#include "test_googleauth.moc"
