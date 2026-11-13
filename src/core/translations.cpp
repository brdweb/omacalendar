#include "core/translations.h"

#include <QCoreApplication>
#include <QTranslator>

namespace omacalendar::translations {

bool load(QTranslator* translator, const QLocale& locale, const QString& directory) {
  if (translator == nullptr) {
    return false;
  }
  const QString catalogue = QStringLiteral("omacalendar");
  if (locale.language() != QLocale::C &&
      translator->load(locale, catalogue, QStringLiteral("_"), directory)) {
    return true;
  }
  return translator->load(catalogue + QStringLiteral("_en"), directory);
}

bool installForSystemLocale() {
  auto* application = QCoreApplication::instance();
  if (application == nullptr) {
    return false;
  }
  auto* translator = new QTranslator(application);
  if (!load(translator, QLocale())) {
    delete translator;
    return false;
  }
  return QCoreApplication::installTranslator(translator);
}

}  // namespace omacalendar::translations
