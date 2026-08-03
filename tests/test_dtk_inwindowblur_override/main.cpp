// Copyright (C) 2026 UnionTech Software Technology Co., Ltd.
// SPDX-License-Identifier: Apache-2.0 OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "core/qmlengine.h"

#include <QGuiApplication>
#include <QMetaProperty>
#include <QQmlComponent>
#include <QQmlListReference>
#include <QQuickItem>
#include <QScopedPointer>
#include <QtTest>
#include <cstdlib>

class TestDtkInWindowBlurOverride : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void usesTreelandBlurWithDtkCompatibleProperties();
};

void TestDtkInWindowBlurOverride::usesTreelandBlurWithDtkCompatibleProperties()
{
    qputenv("QML2_IMPORT_PATH",
            QStringLiteral("%1:%2")
                .arg(QStringLiteral(TREELAND_QML_IMPORT_PATH),
                     QStringLiteral(WAYLIB_QML_IMPORT_PATH))
                .toLocal8Bit());

    QmlEngine engine;
    engine.addImportPath(QStringLiteral(TREELAND_QML_IMPORT_PATH));
    engine.addImportPath(QStringLiteral(WAYLIB_QML_IMPORT_PATH));

    // Exercise D.InWindowBlur only. Avoid D.FloatingPanel: it pulls
    // QtQuick.Window via DTK InsideBoxBorder, which is not always installed
    // in CI package builds.
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import Treeland 2.0
        import org.deepin.dtk 1.0 as D

        Item {
            D.InWindowBlur {
                objectName: "inWindowBlur"
                offscreen: false
                radius: 37
                multiplier: 1.5
                content.glassEnabled: false

                Rectangle {
                    objectName: "externalChild"
                }
            }
        }
    )",
                      QUrl(QStringLiteral("inline:test_dtk_inwindowblur_override.qml")));

    QTRY_VERIFY_WITH_TIMEOUT(component.status() != QQmlComponent::Loading, 5000);
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    QScopedPointer<QObject> root(component.create());
    QVERIFY2(root, qPrintable(component.errorString()));

    auto inWindowBlur = root->findChild<QQuickItem *>(QStringLiteral("inWindowBlur"));
    QVERIFY(inWindowBlur);
    QCOMPARE(inWindowBlur->property("offscreen").toBool(), false);
    QCOMPARE(inWindowBlur->property("radius").toInt(), 37);
    QCOMPARE(inWindowBlur->property("multiplier").toReal(), 1.5);

    const int validPropertyIndex = inWindowBlur->metaObject()->indexOfProperty("valid");
    QVERIFY(validPropertyIndex >= 0);
    QVERIFY(inWindowBlur->metaObject()->property(validPropertyIndex).isWritable());
    QCOMPARE(inWindowBlur->property("valid").toBool(), true);

    auto content = inWindowBlur->property("content").value<QQuickItem *>();
    QVERIFY(content);
    QVERIFY2(content->metaObject()->indexOfProperty("glassEnabled") >= 0,
             "D.InWindowBlur content must be backed by Treeland Blur");
    QCOMPARE(content->isVisible(), true);
    QVERIFY(inWindowBlur->setProperty("valid", false));
    QCOMPARE(content->isVisible(), false);
    QVERIFY(inWindowBlur->setProperty("valid", true));
    QCOMPARE(content->isVisible(), true);

    QVERIFY(inWindowBlur->setProperty("offscreen", true));
    QCOMPARE(content->isVisible(), false);

    auto externalChild = root->findChild<QQuickItem *>(QStringLiteral("externalChild"));
    QVERIFY(externalChild);
    QVERIFY(externalChild->parentItem());
    QVERIFY(externalChild->parentItem() != inWindowBlur);
    QVERIFY(externalChild->parentItem() != content);
    QQmlListReference blurData(content, "data");
    QVERIFY(blurData.isValid());
    QCOMPARE(blurData.count(), qsizetype(1));
    QCOMPARE(blurData.at(0), static_cast<QObject *>(externalChild));

    // Tear down QML objects before the engine so DConfig/Helper singletons and
    // QQuickItem trees do not race during process exit (seen as heap corruption
    // / SIGABRT after a green Totals line on Deepin CI).
    root.reset();
}

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    TestDtkInWindowBlurOverride tc;
    const int result = QTest::qExec(&tc, argc, argv);
    // DConfig/QML singleton teardown can corrupt the headless test allocator.
    std::_Exit(result);
}

#include "main.moc"
