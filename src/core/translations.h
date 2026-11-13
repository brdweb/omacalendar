#pragma once

#include <QLocale>
#include <QString>

class QTranslator;

namespace omacalendar::translations {

// Where the build embeds the compiled .qm catalogues.
inline constexpr char kResourceDirectory[] = ":/i18n";

// Loads the OmaCalendar catalogue that best matches `locale` from `directory`
// into `translator`. English is the fallback for every other locale (and for
// the C locale a systemd user unit often runs under), because it carries the
// English plural forms for the "%n ...(s)" source strings. Returns false when
// no catalogue exists, for example in a build without Qt LinguistTools.
bool load(QTranslator* translator, const QLocale& locale,
          const QString& directory = QString::fromLatin1(kResourceDirectory));

// Loads the catalogue for the system locale and installs it on the running
// application. The translator is parented to the application.
bool installForSystemLocale();

}  // namespace omacalendar::translations
