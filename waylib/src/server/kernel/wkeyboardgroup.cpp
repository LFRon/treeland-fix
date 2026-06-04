// Copyright (C) 2026 UnionTech Software Technology Co., Ltd.
// SPDX-License-Identifier: Apache-2.0 OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "wkeyboardgroup.h"
#include "winputdevice.h"
#include "platformplugin/qwlrootsintegration.h"

#include <qwkeyboardgroup.h>
#include <qwkeyboard.h>

#include <QPointer>

QW_USE_NAMESPACE
WAYLIB_SERVER_BEGIN_NAMESPACE

class Q_DECL_HIDDEN WKeyboardGroupPrivate
{
public:
    WKeyboardGroupPrivate(WKeyboardGroup *qq)
        : q(qq)
    {
        nativeHandle = wlr_keyboard_group_create();
    }

    WKeyboardGroup *q = nullptr;
    QPointer<QInputDevice> keyboardQtDevice = nullptr;
    struct wlr_keyboard_group *nativeHandle = nullptr;

    wl_listener destoryListener;

    wl_listener enterListener;
    wl_listener leaveListener;

    wl_listener keyListener;
    wl_listener modifiersListener;
    wl_listener keymapListener;
    wl_listener repeatInfoListener;

    void setupKeyboardListeners();
    void removeKeyboardListeners();

    static void onDestroyCallback(struct wl_listener *listener, void *data);
    static void onEnterCallback(struct wl_listener *listener, void *data);
    static void onLeaveCallback(struct wl_listener *listener, void *data);
    static void onKeyCallback(struct wl_listener *listener, void *data);
    static void onModifiersCallback(struct wl_listener *listener, void *data);
    static void onKeymapCallback(struct wl_listener *listener, void *data);
    static void onRepeatInfoCallback(struct wl_listener *listener, void *data);
};

void WKeyboardGroupPrivate::setupKeyboardListeners()
{
    destoryListener.notify = onDestroyCallback;
    wl_signal_add(&nativeHandle->keyboard.base.events.destroy, &destoryListener);

    enterListener.notify = onEnterCallback;
    wl_signal_add(&nativeHandle->events.enter, &enterListener);

    leaveListener.notify = onLeaveCallback;
    wl_signal_add(&nativeHandle->events.leave, &leaveListener);

    keyListener.notify = onKeyCallback;
    wl_signal_add(&nativeHandle->keyboard.events.key, &keyListener);

    modifiersListener.notify = onModifiersCallback;
    wl_signal_add(&nativeHandle->keyboard.events.modifiers, &modifiersListener);

    keymapListener.notify = onKeymapCallback;
    wl_signal_add(&nativeHandle->keyboard.events.keymap, &keymapListener);

    repeatInfoListener.notify = onRepeatInfoCallback;
    wl_signal_add(&nativeHandle->keyboard.events.repeat_info, &repeatInfoListener);
}

void WKeyboardGroupPrivate::removeKeyboardListeners()
{
    wl_list_remove(&enterListener.link);
    wl_list_remove(&leaveListener.link);
    wl_list_remove(&keyListener.link);
    wl_list_remove(&modifiersListener.link);
    wl_list_remove(&keymapListener.link);
    wl_list_remove(&repeatInfoListener.link);

    nativeHandle = nullptr;
}

void WKeyboardGroupPrivate::onDestroyCallback(struct wl_listener *listener,
                                              [[maybe_unused]] void *data)
{
    WKeyboardGroupPrivate *d = static_cast<WKeyboardGroupPrivate *>(
        wl_container_of(listener, d, destoryListener));
    d->removeKeyboardListeners();

    Q_EMIT d->q->beforeDestroy();
}

void WKeyboardGroupPrivate::onEnterCallback(struct wl_listener *listener,
                                            [[maybe_unused]] void *data)
{
    WKeyboardGroupPrivate *d = static_cast<WKeyboardGroupPrivate *>(
        wl_container_of(listener, d, enterListener));

    Q_EMIT d->q->enter(static_cast<wl_array*>(data));
}

void WKeyboardGroupPrivate::onLeaveCallback(struct wl_listener *listener,
                                            [[maybe_unused]] void *data)
{
    WKeyboardGroupPrivate *d = static_cast<WKeyboardGroupPrivate *>(
        wl_container_of(listener, d, leaveListener));

    Q_EMIT d->q->leave(static_cast<wl_array*>(data));
}

void WKeyboardGroupPrivate::onKeyCallback(struct wl_listener *listener,
                                          [[maybe_unused]] void *data)
{
    WKeyboardGroupPrivate *d = static_cast<WKeyboardGroupPrivate *>(
        wl_container_of(listener, d, keyListener));

    Q_EMIT d->q->key(static_cast<wlr_keyboard_key_event*>(data));
}

void WKeyboardGroupPrivate::onModifiersCallback(struct wl_listener *listener,
                                                [[maybe_unused]] void *data)
{
    WKeyboardGroupPrivate *d = static_cast<WKeyboardGroupPrivate *>(
        wl_container_of(listener, d, modifiersListener));

    Q_EMIT d->q->modifiers();
}

void WKeyboardGroupPrivate::onKeymapCallback(struct wl_listener *listener,
                                             [[maybe_unused]] void *data)
{
    WKeyboardGroupPrivate *d = static_cast<WKeyboardGroupPrivate *>(
        wl_container_of(listener, d, keymapListener));

    Q_EMIT d->q->keymap();
}

void WKeyboardGroupPrivate::onRepeatInfoCallback(struct wl_listener *listener,
                                                 [[maybe_unused]] void *data)
{
    WKeyboardGroupPrivate *d = static_cast<WKeyboardGroupPrivate *>(
        wl_container_of(listener, d, repeatInfoListener));

    Q_EMIT d->q->repeatInfo();
}

WKeyboardGroup::WKeyboardGroup(QObject *parent)
    : QObject(parent)
    , d(new WKeyboardGroupPrivate(this))
{
    d->setupKeyboardListeners();
}

WKeyboardGroup::~WKeyboardGroup()
{
    if (d->nativeHandle) {
        wlr_keyboard_group_destroy(d->nativeHandle);
    }

    if (d->keyboardQtDevice) {
        delete d->keyboardQtDevice;
    }
}

wlr_keyboard_group *WKeyboardGroup::nativeHandle() const
{
    return d->nativeHandle;
}

wlr_keyboard *WKeyboardGroup::keyboard() const
{
    if (!d->nativeHandle) {
        return nullptr;
    }

    return &d->nativeHandle->keyboard;
}

QInputDevice *WKeyboardGroup::qtDevice() const
{
    return d->keyboardQtDevice;
}

bool WKeyboardGroup::addKeyboard(WInputDevice *device)
{
    if (!d->nativeHandle) {
        return false;
    }

    if (!device || device->type() != WInputDevice::Type::Keyboard) {
        return false;
    }

    auto *qw_kb = qobject_cast<qw_keyboard*>(device->handle());
    Q_ASSERT(qw_kb);

    if (!wlr_keyboard_group_add_keyboard(d->nativeHandle, qw_kb->handle())) {
        return false;
    }

    Q_EMIT keyboardsChanged();

    return true;
}

void WKeyboardGroup::removeKeyboard(WInputDevice *device)
{
    if (!d->nativeHandle) {
        return;
    }

    auto *qw_kb = qobject_cast<qw_keyboard*>(device->handle());
    Q_ASSERT(qw_kb);

    wlr_keyboard_group_remove_keyboard(d->nativeHandle, qw_kb->handle());

    Q_EMIT keyboardsChanged();
}

void WKeyboardGroup::ensureKeyboarQtDevice(WInputDevice *device, const QString &seatName)
{
    if (!d->nativeHandle) {
        return;
    }

    if (d->keyboardQtDevice) {
        return;
    }

    d->keyboardQtDevice = QWlrootsIntegration::instance()->addKeyboardInputDevice(this, device, seatName);
}

QPointer<QInputDevice> WKeyboardGroup::keyboarQtDevice()
{
    return d->keyboardQtDevice;
}

WAYLIB_SERVER_END_NAMESPACE
