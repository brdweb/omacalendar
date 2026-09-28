#pragma once

#include <QDateTime>
#include <QObject>
#include <QTimer>
#include <functional>

namespace omacalendar {

// Decides how often remote calendars are polled and when to sync right away.
// Polling is quicker while a desktop client says it is in use, slower on
// battery power, and a resume from sleep or regained connectivity asks for an
// immediate sync, rate-limited so a flapping network cannot hammer providers.
// Error backoff stays with each provider's RetryPolicy.
class SyncActivity final : public QObject {
  Q_OBJECT

 public:
  using NowProvider = std::function<QDateTime()>;

  static constexpr int kInteractiveIntervalMs = 2 * 60 * 1000;
  static constexpr int kDefaultIntervalMs = 5 * 60 * 1000;
  static constexpr int kBatteryIntervalMs = 10 * 60 * 1000;
  // A client must renew "in use" within this lease, so a client that exits
  // without saying so cannot keep polling fast.
  static constexpr int kInteractiveLeaseSeconds = 10 * 60;
  static constexpr int kMinimumTriggerGapSeconds = 60;
  // NetworkManager's NM_STATE_CONNECTED_GLOBAL.
  static constexpr uint kNetworkConnectedGlobal = 70;

  explicit SyncActivity(NowProvider nowProvider = {}, QObject* parent = nullptr);

  // Listens to logind, NetworkManager and UPower on the system bus.
  void connectSystemSignals();

  void setInteractive(bool interactive);
  void setOnBattery(bool onBattery);
  void networkStateChanged(uint state);
  void resumed();

  [[nodiscard]] bool interactive() const;
  [[nodiscard]] int pollIntervalMs() const;

 signals:
  void pollIntervalChanged(int intervalMs);
  void syncRequested();

 private slots:
  void onPrepareForSleep(bool sleeping);
  void onNetworkStateChanged(uint state);
  void onPowerPropertiesChanged(const QString& interfaceName,
                                const QVariantMap& changedProperties,
                                const QStringList& invalidatedProperties);

 private:
  [[nodiscard]] QDateTime now() const;
  void requestSync();
  void updateInterval();

  NowProvider m_nowProvider;
  QDateTime m_interactiveUntil;
  QDateTime m_lastTrigger;
  bool m_onBattery = false;
  uint m_networkState = 0;
  int m_intervalMs = kDefaultIntervalMs;
  QTimer m_leaseTimer;
};

}  // namespace omacalendar
