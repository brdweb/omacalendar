#pragma once

#include <QLocalServer>
#include <QObject>
#include <QString>
#include <memory>

class QLockFile;

namespace omacalendar {

class ApplicationInstance final : public QObject {
  Q_OBJECT

 public:
  explicit ApplicationInstance(QObject* parent = nullptr);
  ~ApplicationInstance() override;

  [[nodiscard]] bool claimPrimary();
  [[nodiscard]] bool forwardToPrimary(const QString& startupArgument) const;
  bool activate(const QString& startupArgument);

 signals:
  void activationRequested(const QString& startupArgument);

 private:
  void acceptConnections();

  bool m_primary = false;
  QString m_serverPath;
  QLocalServer m_server;
  std::unique_ptr<QLockFile> m_lock;
};

}  // namespace omacalendar
