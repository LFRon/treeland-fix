// Copyright (C) 2026 UnionTech Software Technology Co., Ltd.
// SPDX-License-Identifier: Apache-2.0 OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#pragma once

#include <wglobal.h>

#include <QObject>
#include <QInputDevice>

struct wlr_keyboard;
struct wlr_keyboard_group;
struct wlr_keyboard_key_event;
struct wl_array;

QW_BEGIN_NAMESPACE
class qw_keyboard_group;
class qw_keyboard;
QW_END_NAMESPACE

WAYLIB_SERVER_BEGIN_NAMESPACE

class WInputDevice;
class WKeyboardGroupPrivate;
class WAYLIB_SERVER_EXPORT WKeyboardGroup : public QObject
{
    Q_OBJECT
    QML_UNCREATABLE("Can't create in qml")

public:
    explicit WKeyboardGroup(QObject *parent = nullptr);
    ~WKeyboardGroup() override;

    struct wlr_keyboard_group *nativeHandle() const;
    struct wlr_keyboard *keyboard() const;
    inline QInputDevice *qtDevice() const;

    bool addKeyboard(WInputDevice *device);
    void removeKeyboard(WInputDevice *device);

    void ensureKeyboarQtDevice(WInputDevice *device, const QString &seatName);
    QPointer<QInputDevice> keyboarQtDevice();

Q_SIGNALS:
    void beforeDestroy();
    void key(wlr_keyboard_key_event *event);
    void modifiers();
    void keymap();
    void repeatInfo();
    void enter(wl_array *keys);
    void leave(wl_array *keys);
    void keyboardsChanged();

private:
    std::unique_ptr<WKeyboardGroupPrivate> d;
};

WAYLIB_SERVER_END_NAMESPACE

Q_DECLARE_OPAQUE_POINTER(wlr_keyboard_key_event*)
Q_DECLARE_OPAQUE_POINTER(wl_array*)
