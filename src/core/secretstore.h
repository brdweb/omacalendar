#pragma once

#include <QString>

namespace omacalendar {

class SecretStore final {
 public:
  [[nodiscard]] bool isAvailable() const;

  bool store(const QString& accountId, const QString& kind, const QString& secret,
             QString* errorMessage = nullptr) const;
  [[nodiscard]] QString lookup(const QString& accountId, const QString& kind,
                               QString* errorMessage = nullptr) const;
  bool remove(const QString& accountId, const QString& kind,
              QString* errorMessage = nullptr) const;

 private:
  static QString executable();
};

}  // namespace omacalendar
