#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QRandomGenerator>
#include <QSignalSpy>
#include <QtTest>
#include <memory>

#include "app/presentationlistmodel.h"

using namespace omacalendar;

namespace {

QVariantMap row(const QString& id, const QString& summary,
                const QString& recurrenceId = {}) {
  QVariantMap value{{QStringLiteral("id"), id}, {QStringLiteral("summary"), summary}};
  if (!recurrenceId.isEmpty()) {
    value.insert(QStringLiteral("recurrenceId"), recurrenceId);
  }
  return value;
}

QStringList summaries(const PresentationListModel& model) {
  QStringList result;
  for (int index = 0; index < model.rowCount(); ++index) {
    result.append(model.get(index).value(QStringLiteral("summary")).toString());
  }
  return result;
}

// Rebuilds the model's rows purely from its change signals. If replace()
// reports a wrong or missing signal, the mirror drifts from the model.
class SignalMirror final {
 public:
  explicit SignalMirror(PresentationListModel* model) : m_model(model) {
    m_rows = model->toList();
    QObject::connect(model, &QAbstractItemModel::rowsInserted, model,
                     [this](const QModelIndex&, int first, int last) {
                       for (int index = first; index <= last; ++index) {
                         m_rows.insert(index, m_model->get(index));
                       }
                     });
    QObject::connect(model, &QAbstractItemModel::rowsRemoved, model,
                     [this](const QModelIndex&, int first, int last) {
                       m_rows.remove(first, last - first + 1);
                     });
    QObject::connect(model, &QAbstractItemModel::rowsMoved, model,
                     [this](const QModelIndex&, int first, int last, const QModelIndex&,
                            int destination) {
                       QCOMPARE(first, last);
                       m_rows.move(first,
                                   destination > first ? destination - 1 : destination);
                     });
    QObject::connect(
        model, &QAbstractItemModel::dataChanged, model,
        [this](const QModelIndex& topLeft, const QModelIndex& bottomRight) {
          for (int index = topLeft.row(); index <= bottomRight.row(); ++index) {
            m_rows[index] = m_model->get(index);
          }
        });
    QObject::connect(model, &QAbstractItemModel::modelReset, model,
                     [this]() { m_rows = m_model->toList(); });
  }

  [[nodiscard]] bool matches() const { return m_rows == m_model->toList(); }

 private:
  PresentationListModel* m_model;
  QVariantList m_rows;
};

}  // namespace

class PresentationListModelTest final : public QObject {
  Q_OBJECT

 private slots:
  void exposesStableRolesAndWholeDto() {
    PresentationListModel model;
    QSignalSpy countSpy(&model, &PresentationListModel::countChanged);
    const QVariantMap event{
        {QStringLiteral("id"), QStringLiteral("event-1")},
        {QStringLiteral("summary"), QStringLiteral("Architecture review")},
        {QStringLiteral("futureProviderField"), 42}};

    model.replace({event});

    QCOMPARE(model.rowCount(), 1);
    QCOMPARE(countSpy.count(), 1);
    QCOMPARE(model.get(0), event);
    QCOMPARE(model.toList(), QVariantList{event});

    const QHash<int, QByteArray> roles = model.roleNames();
    const int modelDataRole = roles.key(QByteArrayLiteral("modelData"), -1);
    const int idRole = roles.key(QByteArrayLiteral("id"), -1);
    const int summaryRole = roles.key(QByteArrayLiteral("summary"), -1);
    QVERIFY(modelDataRole >= Qt::UserRole);
    QVERIFY(idRole >= Qt::UserRole);
    QVERIFY(summaryRole >= Qt::UserRole);
    QCOMPARE(model.data(model.index(0), modelDataRole).toMap(), event);
    QCOMPARE(model.data(model.index(0), idRole).toString(), QStringLiteral("event-1"));
    QCOMPARE(model.data(model.index(0), summaryRole).toString(),
             QStringLiteral("Architecture review"));

    // Content replacement updates the row without reporting a false count
    // change; forward-compatible fields remain accessible through modelData.
    QVariantMap changed = event;
    changed.insert(QStringLiteral("summary"), QStringLiteral("Design review"));
    model.replace({changed});
    QCOMPARE(countSpy.count(), 1);
    QCOMPARE(model.get(0).value(QStringLiteral("futureProviderField")).toInt(), 42);
    QCOMPARE(model.get(-1), QVariantMap());
    QCOMPARE(model.get(9), QVariantMap());
  }

  void editsAreReportedAsRowChanges() {
    PresentationListModel model;
    SignalMirror mirror(&model);
    model.replace({row("a", "A"), row("b", "B"), row("c", "C")});
    QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
    QSignalSpy changes(&model, &QAbstractItemModel::dataChanged);
    QSignalSpy inserts(&model, &QAbstractItemModel::rowsInserted);
    QSignalSpy removals(&model, &QAbstractItemModel::rowsRemoved);

    model.replace({row("a", "A"), row("b", "B edited"), row("c", "C")});

    QCOMPARE(resets.count(), 0);
    QCOMPARE(inserts.count(), 0);
    QCOMPARE(removals.count(), 0);
    QCOMPARE(changes.count(), 1);
    QCOMPARE(changes.first().at(0).toModelIndex().row(), 1);
    QVERIFY(mirror.matches());
    QCOMPARE(summaries(model), (QStringList{"A", "B edited", "C"}));
  }

  void insertionsRemovalsAndMovesAvoidResets() {
    PresentationListModel model;
    SignalMirror mirror(&model);
    model.replace(
        {row("a", "A"), row("b", "B"), row("c", "C"), row("d", "D"), row("e", "E")});
    QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
    QSignalSpy inserts(&model, &QAbstractItemModel::rowsInserted);
    QSignalSpy removals(&model, &QAbstractItemModel::rowsRemoved);
    QSignalSpy moves(&model, &QAbstractItemModel::rowsMoved);
    QSignalSpy countSpy(&model, &PresentationListModel::countChanged);

    // b is removed, x is inserted, e moves to the front, d changes.
    model.replace({row("e", "E"), row("a", "A"), row("x", "X"), row("c", "C"),
                   row("d", "D edited")});

    QCOMPARE(resets.count(), 0);
    QCOMPARE(removals.count(), 1);
    QCOMPARE(inserts.count(), 1);
    QCOMPARE(moves.count(), 1);
    QCOMPARE(countSpy.count(), 0);
    QVERIFY(mirror.matches());
    QCOMPARE(summaries(model), (QStringList{"E", "A", "X", "C", "D edited"}));

    model.replace({row("a", "A")});
    QCOMPARE(resets.count(), 0);
    QCOMPARE(countSpy.count(), 1);
    QVERIFY(mirror.matches());
    QCOMPARE(summaries(model), QStringList{"A"});
  }

  void occurrencesAreKeyedByRecurrenceId() {
    PresentationListModel model;
    SignalMirror mirror(&model);
    model.replace({row("series", "Monday", "2026-09-21"),
                   row("series", "Tuesday", "2026-09-22")});
    QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
    QSignalSpy changes(&model, &QAbstractItemModel::dataChanged);

    model.replace({row("series", "Monday", "2026-09-21"),
                   row("series", "Tuesday moved", "2026-09-22")});

    QCOMPARE(resets.count(), 0);
    QCOMPARE(changes.count(), 1);
    QCOMPARE(changes.first().at(0).toModelIndex().row(), 1);
    QVERIFY(mirror.matches());
  }

  void ambiguousOrReorderedRowsFallBackToReset() {
    PresentationListModel model;
    SignalMirror mirror(&model);
    model.replace({row("a", "A"), row("b", "B"), row("c", "C"), row("d", "D")});
    QSignalSpy resets(&model, &QAbstractItemModel::modelReset);

    // Duplicate identities cannot be matched row for row.
    model.replace({row("a", "A"), row("a", "A again")});
    QCOMPARE(resets.count(), 1);
    QVERIFY(mirror.matches());
    QCOMPARE(summaries(model), (QStringList{"A", "A again"}));

    // Rows without an id have no identity either.
    model.replace({QVariantMap{{QStringLiteral("summary"), QStringLiteral("anon")}}});
    QCOMPARE(resets.count(), 2);

    // A full reversal moves most rows; one reset is cheaper.
    model.replace({row("a", "A"), row("b", "B"), row("c", "C"), row("d", "D")});
    QCOMPARE(resets.count(), 3);
    model.replace({row("d", "D"), row("c", "C"), row("b", "B"), row("a", "A")});
    QCOMPARE(resets.count(), 4);
    QVERIFY(mirror.matches());
    QCOMPARE(summaries(model), (QStringList{"D", "C", "B", "A"}));
  }

  void randomRefreshesKeepSignalsConsistent() {
    PresentationListModel model;
    SignalMirror mirror(&model);
    QRandomGenerator random(20260926);
    for (int round = 0; round < 400; ++round) {
      // Draw a subset of 40 ids, in a mostly stable order with a few swaps,
      // and edit some summaries.
      QVariantList rows;
      for (int id = 0; id < 40; ++id) {
        if (random.bounded(4) != 0) {
          const QString summary = random.bounded(6) == 0
                                      ? QStringLiteral("edited %1").arg(round)
                                      : QStringLiteral("row %1").arg(id);
          rows.append(row(QStringLiteral("id-%1").arg(id), summary));
        }
      }
      for (int swap = random.bounded(4); swap > 0 && rows.size() > 1; --swap) {
        rows.swapItemsAt(random.bounded(rows.size()), random.bounded(rows.size()));
      }
      model.replace(rows);
      QVERIFY2(mirror.matches(), qPrintable(QStringLiteral("round %1").arg(round)));
      QCOMPARE(model.toList(), rows);
    }
  }

  void suppliesModelDataToQmlDelegates() {
    PresentationListModel model;
    model.replace(
        {QVariantMap{{QStringLiteral("id"), QStringLiteral("event-2")},
                     {QStringLiteral("summary"), QStringLiteral("Typed QML row")}}});

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("PresentationModel"),
                                             &model);
    QQmlComponent component(&engine);
    component.setData(R"QML(
      import QtQml
      import QtQml.Models
      QtObject {
        id: root
        property string observed: ""
        property Instantiator rows: Instantiator {
          model: PresentationModel
          delegate: QtObject {
            required property var modelData
            Component.onCompleted: root.observed = modelData.summary
          }
        }
      }
    )QML",
                      QUrl(QStringLiteral("inline:PresentationModel.qml")));
    QTRY_VERIFY_WITH_TIMEOUT(component.status() != QQmlComponent::Loading, 2000);
    QVERIFY2(component.status() == QQmlComponent::Ready,
             qPrintable(component.errorString()));
    std::unique_ptr<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));
    QTRY_COMPARE(object->property("observed").toString(),
                 QStringLiteral("Typed QML row"));
  }
};

QTEST_GUILESS_MAIN(PresentationListModelTest)
#include "test_presentationlistmodel.moc"
