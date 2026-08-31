#pragma once

#include <QAbstractListModel>
#include <QVariantList>
#include <QVariantMap>

namespace omacalendar {

// A read-only QML presentation model for daemon DTOs.  Role numbers are
// deliberately backed by one append-only name table in the implementation so
// views can bind to stable roles while the full, forward-compatible DTO remains
// available through the modelData role.
class PresentationListModel final : public QAbstractListModel {
  Q_OBJECT
  Q_PROPERTY(int count READ rowCount NOTIFY countChanged)

 public:
  explicit PresentationListModel(QObject* parent = nullptr);

  [[nodiscard]] int rowCount(const QModelIndex& parent = QModelIndex()) const override;
  [[nodiscard]] QVariant data(const QModelIndex& index,
                              int role = Qt::DisplayRole) const override;
  [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

  Q_INVOKABLE [[nodiscard]] QVariantMap get(int row) const;
  Q_INVOKABLE [[nodiscard]] QVariantList toList() const;

  void replace(const QVariantList& rows);
  void clear();

 signals:
  void countChanged();

 private:
  QList<QVariantMap> m_rows;
};

}  // namespace omacalendar
