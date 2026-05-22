// Copyright (C) 2024 Lu YaNing <luyaning@uniontech.com>.
// SPDX-License-Identifier: Apache-2.0 OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "qwayland-treeland-virtual-output-manager-v1.h"

#include <private/qwaylandscreen_p.h>
#include <private/qwaylandwindow_p.h>

#include <qwindow.h>

#include <QApplication>
#include <QPushButton>
#include <QtWaylandClient/QWaylandClientExtension>
#include <qlogging.h>

class VirtualOutputManager
    : public QWaylandClientExtensionTemplate<VirtualOutputManager>
    , public QtWayland::treeland_virtual_output_manager_v1
{
    Q_OBJECT
public:
    explicit VirtualOutputManager();

    void treeland_virtual_output_manager_v1_virtual_output_list(wl_array *names) override
    {
        if (!names || names->size == 0)
            return;

        char *data = static_cast<char *>(names->data);
        char *end = data + names->size;
        QStringList nameList;

        while (data < end && *data != '\0') {
            QString name = QString::fromUtf8(data);
            nameList << name;
            data += name.size() + 1;
        }

        qInfo() << "Virtual output list:" << nameList;
        Q_EMIT virtualOutputListReceived(nameList);
    }

signals:
    void virtualOutputListReceived(const QStringList &names);
};

VirtualOutputManager::VirtualOutputManager()
    : QWaylandClientExtensionTemplate<VirtualOutputManager>(1)
{
}

class VirtualOutput
    : public QWaylandClientExtensionTemplate<VirtualOutput>
    , public QtWayland::treeland_virtual_output_v1
{
    Q_OBJECT
public:
    explicit VirtualOutput(struct ::treeland_virtual_output_v1 *object);

    void treeland_virtual_output_v1_outputs(const QString &name, wl_array *outputs) override
    {
        qInfo() << "Screen group name:" << name;

        if (!outputs || outputs->size == 0) {
            qInfo() << "  No outputs (not found or empty)";
            return;
        }

        char *data = static_cast<char *>(outputs->data);
        char *end = data + outputs->size;
        QStringList outputList;

        while (data < end && *data != '\0') {
            QString output = QString::fromUtf8(data);
            outputList << output;
            data += output.size() + 1;
        }

        qInfo() << "  Outputs:" << outputList;
    }

    void treeland_virtual_output_v1_error(uint32_t code, const QString &message) override
    {

        qInfo() << "error code:" << code << " error message:" << message;
    }
};

VirtualOutput::VirtualOutput(struct ::treeland_virtual_output_v1 *object)
    : QWaylandClientExtensionTemplate<VirtualOutput>(1)
    , QtWayland::treeland_virtual_output_v1(object)
{
}

// ./test-virtual-output HDMI-A-1 VGA-1

// 点击 Create virtual output 按钮创建虚拟屏，点击 Restore settings 按钮销毁虚拟屏

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "wayland");
    QApplication app(argc, argv);
    VirtualOutputManager manager;

    QObject::connect(
        &manager,
        &VirtualOutputManager::activeChanged,
        &manager,
        [&manager, argc, argv] {
            if (manager.isActive()) {
                QWidget *widget = new QWidget;
                widget->setAttribute(Qt::WA_TranslucentBackground);
                widget->resize(640, 480);

                QPushButton *createButton = new QPushButton("Create virtual output", widget);
                createButton->setGeometry(0, 0, 150, 50);

                QPushButton *restoreButton = new QPushButton("Restore settings", widget);
                restoreButton->setGeometry(0, 60, 150, 50);

                QPushButton *listButton = new QPushButton("Get virtual output list", widget);
                listButton->setGeometry(0, 120, 150, 50);

                QObject::connect(listButton, &QPushButton::clicked, [&manager]() {
                    manager.get_virtual_output_list();
                });

                QObject::connect(&manager, &VirtualOutputManager::virtualOutputListReceived, [&manager, restoreButton](const QStringList &names) {
                    qInfo() << names;
                    for (const auto &name : names) {
                        auto *obj = manager.get_virtual_output(name);
                        auto *vo = new VirtualOutput(obj);

                        // Connect restore button to destroy this virtual output
                        QObject::disconnect(restoreButton, &QPushButton::clicked, nullptr, nullptr);
                        QObject::connect(restoreButton, &QPushButton::clicked, [vo]() {
                            vo->destroy();
                            qInfo() << "Virtual output destroyed via restore";
                        });
                    }
                });

                widget->show();

                // Collect available screens for the create button
                QWindow *window = widget->windowHandle();
                QList<QtWaylandClient::QWaylandScreen *> screens;
                if (window && window->handle()) {
                    QtWaylandClient::QWaylandWindow *waylandWindow =
                        static_cast<QtWaylandClient::QWaylandWindow *>(window->handle());
                    screens = waylandWindow->display()->screens();

                    qInfo() << "Available screens:";
                    for (auto *screen : screens) {
                        qInfo() << "  Screen name:" << screen->name();
                    }
                }

                // Track the current virtual output for create/destroy pairing

                QObject::connect(createButton, &QPushButton::clicked, [&manager, screens, argv, argc, restoreButton]() mutable {
                    // Destroy previous virtual output if exists
                    // if (currentOutput) {
                    //     currentOutput->destroy();
                    //     currentOutput.reset();
                    // }
                    QSharedPointer<VirtualOutput> currentOutput;

                    // Determine screen names: use argv if provided, otherwise all available screens
                    QList<QString> screenNames;
                    if (argc >= 3) {
                        for (int i = 1; i < argc; ++i) {
                            screenNames.append(QString::fromUtf8(argv[i]));
                        }
                    } else {
                        for (auto *screen : screens) {
                            screenNames.append(screen->name());
                        }
                    }

                    if (screenNames.isEmpty()) {
                        qInfo() << "No screens available to create virtual output";
                        return;
                    }

                    // 实际使用需要判断主屏（被镜像的屏幕），将主屏放在wl_array的第一个,复制屏幕依次填充
                    QByteArray screenNameArray;
                    for (auto screen : screenNames) {
                        screenNameArray.append(screen.toUtf8());
                        screenNameArray.append('\0');
                    }
                    screenNameArray.append('\0');

                    currentOutput.reset(new VirtualOutput(
                        manager.create_virtual_output("copyscreen1",
                                                      screenNameArray))); // "copyscreen1": 客户端自定义分组名称
                    qInfo() << "Virtual output created with screens:" << screenNames;

                    // Disconnect old restore connection and reconnect to destroy current output
                    QObject::disconnect(restoreButton, &QPushButton::clicked, nullptr, nullptr);
                    QObject::connect(restoreButton, &QPushButton::clicked, [currentOutput]() mutable {
                        if (currentOutput) {
                            currentOutput->destroy();
                            currentOutput.reset();
                            qInfo() << "Virtual output destroyed";
                        }
                    });
                });

                // Print help tip if no screen names provided via argv
                if (argc < 3) {
                    qInfo() << "Tip: Specify screen names to clone:";
                    qInfo() << "  ./test-virtual-output HDMI-A-1 VGA-1";
                }
            }
        });

    return app.exec();
}

#include "main.moc"
