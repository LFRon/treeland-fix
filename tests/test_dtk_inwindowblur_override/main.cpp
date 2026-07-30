// Copyright (C) 2026 UnionTech Software Technology Co., Ltd.
// SPDX-License-Identifier: Apache-2.0 OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include <QGuiApplication>
#include <QMetaProperty>
#include <QQmlAbstractUrlInterceptor>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QScopedPointer>
#include <QtTest>

namespace {

// Mirrors src/core/qmlengine.cpp: rewrite DTK's overridable InWindowBlur to
// Treeland's Vulkan-safe override. Kept local so the test does not construct
// the full compositor QmlEngine (which loads dozens of components and can
// corrupt heap on teardown in headless CI).
class DtkInWindowBlurInterceptor final : public QObject, public QQmlAbstractUrlInterceptor
{
public:
    using QObject::QObject;

    QUrl intercept(const QUrl &url, DataType type) override
    {
        if (type == DataType::QmlFile
            && url.path().endsWith(QStringLiteral("/overridable/InWindowBlur.qml"))) {
            return QUrl(QStringLiteral("qrc:/treeland/override/dtk/InWindowBlur.qml"));
        }
        return url;
    }
};

} // namespace

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

    QQmlEngine engine;
    engine.addImportPath(QStringLiteral(TREELAND_QML_IMPORT_PATH));
    engine.addImportPath(QStringLiteral(WAYLIB_QML_IMPORT_PATH));
    engine.addUrlInterceptor(new DtkInWindowBlurInterceptor(&engine));

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
                valid: true

                Rectangle {
                    objectName: "externalChild"
                }
            }

            D.FloatingPanel {
                objectName: "floatingPanel"
                width: 200
                height: 100
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

    QVERIFY(inWindowBlur->setProperty("offscreen", true));
    QCOMPARE(content->isVisible(), false);

    auto externalChild = root->findChild<QQuickItem *>(QStringLiteral("externalChild"));
    QVERIFY(externalChild);
    QCOMPARE(externalChild->parentItem(), content->parentItem());
    QVERIFY(externalChild->parentItem() != content);

    auto floatingPanel = root->findChild<QQuickItem *>(QStringLiteral("floatingPanel"));
    QVERIFY(floatingPanel);
    auto panelBackground = floatingPanel->property("background").value<QQuickItem *>();
    QVERIFY(panelBackground);
    auto panelContent = panelBackground->property("content").value<QQuickItem *>();
    QVERIFY(panelContent);
    QVERIFY2(panelContent->metaObject()->indexOfProperty("glassEnabled") >= 0,
             "D.FloatingPanel must consume the overridden D.InWindowBlur");

    // Tear down QML objects before the engine so DConfig/Helper singletons and
    // QQuickItem trees do not race during process exit (seen as heap corruption
    // / SIGABRT after a green Totals line on Deepin CI).
    root.reset();
    engine.clearComponentCache();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents();
}

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    TestDtkInWindowBlurOverride tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "main.moc"
