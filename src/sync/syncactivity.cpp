#include "sync/syncactivity.h"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusVariant>
#include <utility>

namespace omacalendar {

SyncActivity::SyncActivity(NowProvider nowProvider, QObject* parent)
    : QObject(parent), m_nowProvider(std::move(nowProvider)) {
  m_leaseTimer.setSingleShot(true);
  connect(&m_leaseTimer, &QTimer::timeout, this, &SyncActivity::updateInterval);
}

void SyncActivity::connectSystemSignals() {
  QDBusConnection bus = QDBusConnection::systemBus();
  if (!bus.isConnected()) {
    return;
  }
  bus.connect(QStringLiteral("org.freedesktop.login1"),
              QStringLiteral("/org/freedesktop/login1"),
              QStringLiteral("org.freedesktop.login1.Manager"),
              QStringLiteral("PrepareForSleep"), this, SLOT(onPrepareForSleep(bool)));
  bus.connect(QStringLiteral("org.freedesktop.NetworkManager"),
              QStringLiteral("/org/freedesktop/NetworkManager"),
              QStringLiteral("org.freedesktop.NetworkManager"),
              QStringLiteral("StateChanged"), this, SLOT(onNetworkStateChanged(uint)));
  bus.connect(QStringLiteral("org.freedesktop.UPower"),
              QStringLiteral("/org/freedesktop/UPower"),
              QStringLiteral("org.freedesktop.DBus.Properties"),
              QStringLiteral("PropertiesChanged"), this,
              SLOT(onPowerPropertiesChanged(QString, QVariantMap, QStringList)));

  // Seed the network and power state so the first change is recognised as one.
  QDBusInterface network(QStringLiteral("org.freedesktop.NetworkManager"),
                         QStringLiteral("/org/freedesktop/NetworkManager"),
                         QStringLiteral("org.freedesktop.DBus.Properties"), bus);
  if (network.isValid()) {
    auto* watcher = new QDBusPendingCallWatcher(
        network.asyncCall(QStringLiteral("Get"),
                          QStringLiteral("org.freedesktop.NetworkManager"),
                          QStringLiteral("State")),
        this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher]() {
      const QDBusPendingReply<QDBusVariant> reply = *watcher;
      if (!reply.isError() && m_networkState == 0) {
        m_networkState = reply.value().variant().toUInt();
      }
      watcher->deleteLater();
    });
  }
  QDBusInterface power(QStringLiteral("org.freedesktop.UPower"),
                       QStringLiteral("/org/freedesktop/UPower"),
                       QStringLiteral("org.freedesktop.DBus.Properties"), bus);
  if (power.isValid()) {
    auto* watcher = new QDBusPendingCallWatcher(
        power.asyncCall(QStringLiteral("Get"), QStringLiteral("org.freedesktop.UPower"),
                        QStringLiteral("OnBattery")),
        this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher]() {
      const QDBusPendingReply<QDBusVariant> reply = *watcher;
      if (!reply.isError()) {
        setOnBattery(reply.value().variant().toBool());
      }
      watcher->deleteLater();
    });
  }
}

void SyncActivity::setInteractive(const bool interactive) {
  if (interactive) {
    m_interactiveUntil = now().addSecs(kInteractiveLeaseSeconds);
    m_leaseTimer.start(kInteractiveLeaseSeconds * 1000);
  } else {
    m_interactiveUntil = {};
    m_leaseTimer.stop();
  }
  updateInterval();
}

void SyncActivity::setOnBattery(const bool onBattery) {
  m_onBattery = onBattery;
  updateInterval();
}

void SyncActivity::networkStateChanged(const uint state) {
  const bool regained =
      state == kNetworkConnectedGlobal && m_networkState != kNetworkConnectedGlobal;
  m_networkState = state;
  if (regained) {
    requestSync();
  }
}

void SyncActivity::resumed() { requestSync(); }

bool SyncActivity::interactive() const {
  return m_interactiveUntil.isValid() && now() < m_interactiveUntil;
}

int SyncActivity::pollIntervalMs() const { return m_intervalMs; }

void SyncActivity::onPrepareForSleep(const bool sleeping) {
  if (!sleeping) {
    // Give the network a moment to come back; a later "connected" state
    // change is rate-limited against this sync.
    QTimer::singleShot(5000, this, &SyncActivity::resumed);
  }
}

void SyncActivity::onNetworkStateChanged(const uint state) {
  networkStateChanged(state);
}

void SyncActivity::onPowerPropertiesChanged(const QString& interfaceName,
                                            const QVariantMap& changedProperties,
                                            const QStringList& invalidatedProperties) {
  Q_UNUSED(invalidatedProperties)
  if (interfaceName == QStringLiteral("org.freedesktop.UPower") &&
      changedProperties.contains(QStringLiteral("OnBattery"))) {
    setOnBattery(changedProperties.value(QStringLiteral("OnBattery")).toBool());
  }
}

QDateTime SyncActivity::now() const {
  return m_nowProvider ? m_nowProvider() : QDateTime::currentDateTimeUtc();
}

void SyncActivity::requestSync() {
  const QDateTime current = now();
  if (m_lastTrigger.isValid() &&
      m_lastTrigger.secsTo(current) < kMinimumTriggerGapSeconds) {
    return;
  }
  m_lastTrigger = current;
  emit syncRequested();
}

void SyncActivity::updateInterval() {
  const int interval = interactive() ? kInteractiveIntervalMs
                       : m_onBattery ? kBatteryIntervalMs
                                     : kDefaultIntervalMs;
  if (interval == m_intervalMs) {
    return;
  }
  m_intervalMs = interval;
  emit pollIntervalChanged(interval);
}

}  // namespace omacalendar
