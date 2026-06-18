// Copyright (C) 2024 JiDe Zhang <zhangjide@deepin.org>.
// SPDX-License-Identifier: Apache-2.0 OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "core/treeland.h"
#include "seat/helper.h"
#include "utils/cmdline.h"

#include <wrenderhelper.h>

#include <qwbuffer.h>
#include <qwlogging.h>

#include <DGuiApplicationHelper>
#include <DLog>

#include <QFont>
#include <QGuiApplication>
#include <QMetaType>
#include <QPalette>

#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
#  include <private/qgenericunixtheme_p.h>
#else
#  include <private/qgenericunixthemes_p.h>
#endif

#include <wserver.h>

#include <qpa/qplatformtheme.h>

#include "treelanduserconfig.hpp"

WAYLIB_SERVER_USE_NAMESPACE
DCORE_USE_NAMESPACE;

class QDeepinTheme : public QGenericUnixTheme
{
public:
    QDeepinTheme() { s_instance = this; }
    ~QDeepinTheme() override { s_instance = nullptr; }

    const QPalette *palette(QPlatformTheme::Palette type) const override
    {
        if (type != QPlatformTheme::SystemPalette) {
            return QGenericUnixTheme::palette(type);
        }
        static QPalette palette;
        palette = Dtk::Gui::DGuiApplicationHelper::instance()->applicationPalette();
        return &palette;
    }

    QVariant themeHint(ThemeHint hint) const override
    {
        auto *config = Helper::instance() ? Helper::instance()->config() : nullptr;
        if (config) {
            switch (hint) {
            case CursorFlashTime:
                return config->cursorBlink() ? config->cursorBlinkTime() : 0;
            case MouseDoubleClickInterval:
                return config->doubleClickTime();
            default:
                break;
            }
        }
        return QGenericUnixTheme::themeHint(hint);
    }

    const QFont *font(Font type) const override
    {
        auto *config = Helper::instance() ? Helper::instance()->config() : nullptr;
        if (config) {
            switch (type) {
            case SystemFont: {
                static QFont f;
                f.setFamily(config->font());
                f.setPointSize(config->fontSize() / 10.0);
                return &f;
            }
            case FixedFont: {
                static QFont f;
                f.setFamily(config->monoFont());
                f.setPointSize(config->fontSize() / 10.0);
                return &f;
            }
            default:
                break;
            }
        }
        return QGenericUnixTheme::font(type);
    }

    static void notifyThemeChanged()
    {
        if (!s_instance)
            return;

        // Notify all top-level windows about the theme change
        for (auto *window : QGuiApplication::topLevelWindows()) {
            QEvent event(QEvent::ThemeChange);
            QCoreApplication::sendEvent(window, &event);
        }

        // Force Qt to re-query style hints and fonts
        auto *app = QGuiApplication::instance();
        QEvent fontEvent(QEvent::ApplicationFontChange);
        QCoreApplication::sendEvent(app, &fontEvent);
    }

private:
    static QDeepinTheme *s_instance;
};

QDeepinTheme *QDeepinTheme::s_instance = nullptr;

static void connectConfigSignals()
{
    auto *helper = Helper::instance();
    if (!helper)
        return;
    auto *config = helper->config();
    if (!config)
        return;

    config->disconnect();

    QObject::connect(config, &TreelandUserConfig::cursorBlinkChanged, &QDeepinTheme::notifyThemeChanged);
    QObject::connect(config, &TreelandUserConfig::cursorBlinkTimeChanged, &QDeepinTheme::notifyThemeChanged);
    QObject::connect(config, &TreelandUserConfig::doubleClickTimeChanged, &QDeepinTheme::notifyThemeChanged);
    QObject::connect(config, &TreelandUserConfig::fontChanged, &QDeepinTheme::notifyThemeChanged);
    QObject::connect(config, &TreelandUserConfig::monoFontChanged, &QDeepinTheme::notifyThemeChanged);
    QObject::connect(config, &TreelandUserConfig::fontSizeChanged, &QDeepinTheme::notifyThemeChanged);

    QDeepinTheme::notifyThemeChanged();
}

int main(int argc, char *argv[])
{
    qw_log::init();
    DTK_GUI_NAMESPACE::DGuiApplicationHelper::setAttribute(
        DTK_GUI_NAMESPACE::DGuiApplicationHelper::DontSaveApplicationTheme,
        true);
    WServer::initializeQPA({}, [](const QString &) {
        return static_cast<QPlatformTheme *>(new QDeepinTheme());
    });
    //    QQuickStyle::setStyle("Material");

    QGuiApplication::setAttribute(Qt::AA_UseOpenGLES);
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
    QGuiApplication::setQuitOnLastWindowClosed(false);

    QGuiApplication app(argc, argv);

    app.setOrganizationName("deepin");
    app.setApplicationName("treeland");

#ifdef QT_DEBUG
    DLogManager::registerConsoleAppender();
#endif

    // Enable console logging in non-debug builds via --console-log flag
    CmdLine::ref();
#ifndef QT_DEBUG
    if (CmdLine::ref().consoleLog()) {
        DLogManager::registerConsoleAppender();
    }
#endif
    DLogManager::registerJournalAppender();

    WRenderHelper::setupRendererBackend();
    if (CmdLine::ref().tryExec())
        return 0;
    Q_ASSERT(qw_buffer::get_objects().isEmpty());

    int quitCode = 0;
    {
        Treeland::Treeland treeland;

        connectConfigSignals();
        QObject::connect(Helper::instance(), &Helper::configChanged, &connectConfigSignals);

        quitCode = app.exec();
    }

    Q_ASSERT(qw_buffer::get_objects().isEmpty());

    return quitCode;
}
