#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QSignalSpy>
#include <QtTest>
#include <memory>

#include "app/presentationlistmodel.h"

using namespace omacalendar;

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

    // Content replacement resets delegates without reporting a false count
    // change; forward-compatible fields remain accessible through modelData.
    QVariantMap changed = event;
    changed.insert(QStringLiteral("summary"), QStringLiteral("Design review"));
    model.replace({changed});
    QCOMPARE(countSpy.count(), 1);
    QCOMPARE(model.get(0).value(QStringLiteral("futureProviderField")).toInt(), 42);
    QCOMPARE(model.get(-1), QVariantMap());
    QCOMPARE(model.get(9), QVariantMap());
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
