// Copyright (C) 2026 UnionTech Software Technology Co., Ltd.
// SPDX-License-Identifier: Apache-2.0 OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "wlinuxdrmsyncobjv1.h"

#include "private/wglobal_p.h"
#include "wayliblogging.h"

#include <qwdisplay.h>
#include <qwlinuxdrmsyncobjv1.h>
#include <qwrenderer.h>

QW_USE_NAMESPACE
WAYLIB_SERVER_BEGIN_NAMESPACE

static constexpr uint32_t linuxDrmSyncobjV1Version = 1;

class Q_DECL_HIDDEN WLinuxDrmSyncobjManagerV1Private : public WObjectPrivate
{
public:
    WLinuxDrmSyncobjManagerV1Private(WLinuxDrmSyncobjManagerV1 *qq, qw_renderer *renderer)
        : WObjectPrivate(qq)
        , renderer(renderer)
    {
    }

    inline qw_linux_drm_syncobj_manager_v1 *handle() const
    {
        return q_func()->nativeInterface<qw_linux_drm_syncobj_manager_v1>();
    }

    qw_renderer *renderer = nullptr;

    W_DECLARE_PUBLIC(WLinuxDrmSyncobjManagerV1)
};

WLinuxDrmSyncobjManagerV1::WLinuxDrmSyncobjManagerV1(qw_renderer *renderer)
    : WObject(*new WLinuxDrmSyncobjManagerV1Private(this, renderer))
{
}

qw_linux_drm_syncobj_manager_v1 *WLinuxDrmSyncobjManagerV1::handle() const
{
    return nativeInterface<qw_linux_drm_syncobj_manager_v1>();
}

QByteArrayView WLinuxDrmSyncobjManagerV1::interfaceName() const
{
    return "wp_linux_drm_syncobj_manager_v1";
}

void WLinuxDrmSyncobjManagerV1::create(WServer *server)
{
    W_D(WLinuxDrmSyncobjManagerV1);
    if (m_handle || !server || !server->handle())
        return;

    if (!d->renderer || !d->renderer->handle()) {
        qCWarning(lcWlLinuxDrmSyncobj)
            << "linux-drm-syncobj-v1 not created: renderer is unavailable";
        return;
    }

    const int drmFd = d->renderer->get_drm_fd();
    if (drmFd < 0) {
        qCWarning(lcWlLinuxDrmSyncobj)
            << "linux-drm-syncobj-v1 not created: renderer has no DRM fd";
        return;
    }

    m_handle = qw_linux_drm_syncobj_manager_v1::create(*server->handle(),
                                                       linuxDrmSyncobjV1Version,
                                                       drmFd);
    if (!m_handle) {
        qCWarning(lcWlLinuxDrmSyncobj)
            << "Failed to create linux-drm-syncobj-v1 global"
            << "drmFd" << drmFd;
        return;
    }

    qCInfo(lcWlLinuxDrmSyncobj)
        << "Created linux-drm-syncobj-v1 global"
        << "version" << linuxDrmSyncobjV1Version
        << "drmFd" << drmFd;
}

void WLinuxDrmSyncobjManagerV1::destroy([[maybe_unused]] WServer *server)
{
}

wl_global *WLinuxDrmSyncobjManagerV1::global() const
{
    if (!handle() || !handle()->handle())
        return nullptr;

    return handle()->handle()->global;
}

WAYLIB_SERVER_END_NAMESPACE

#include "moc_wlinuxdrmsyncobjv1.cpp"
