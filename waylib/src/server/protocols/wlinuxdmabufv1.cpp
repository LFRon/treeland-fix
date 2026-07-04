// Copyright (C) 2026 UnionTech Software Technology Co., Ltd.
// SPDX-License-Identifier: Apache-2.0 OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "wlinuxdmabufv1.h"

#include "private/wglobal_p.h"
#include "wayliblogging.h"

#include <qwdisplay.h>
#include <qwlinuxdmabufv1.h>
#include <qwrenderer.h>

QW_USE_NAMESPACE
WAYLIB_SERVER_BEGIN_NAMESPACE

static constexpr uint32_t linuxDmabufV1Version = 4;

class Q_DECL_HIDDEN WLinuxDmabufV1Private : public WObjectPrivate
{
public:
    WLinuxDmabufV1Private(WLinuxDmabufV1 *qq, qw_renderer *renderer)
        : WObjectPrivate(qq)
        , renderer(renderer)
    {
    }

    inline qw_linux_dmabuf_v1 *handle() const
    {
        return q_func()->nativeInterface<qw_linux_dmabuf_v1>();
    }

    qw_renderer *renderer = nullptr;

    W_DECLARE_PUBLIC(WLinuxDmabufV1)
};

WLinuxDmabufV1::WLinuxDmabufV1(qw_renderer *renderer)
    : WObject(*new WLinuxDmabufV1Private(this, renderer))
{
}

qw_linux_dmabuf_v1 *WLinuxDmabufV1::handle() const
{
    return nativeInterface<qw_linux_dmabuf_v1>();
}

QByteArrayView WLinuxDmabufV1::interfaceName() const
{
    return "zwp_linux_dmabuf_v1";
}

void WLinuxDmabufV1::create(WServer *server)
{
    W_D(WLinuxDmabufV1);
    if (m_handle || !server || !server->handle())
        return;

    if (!d->renderer || !d->renderer->handle()) {
        qCWarning(lcWlLinuxDmabuf)
            << "linux-dmabuf-v1 not created: renderer is unavailable";
        return;
    }

    m_handle = qw_linux_dmabuf_v1::create_with_renderer(*server->handle(),
                                                        linuxDmabufV1Version,
                                                        *d->renderer);
    if (!m_handle) {
        qCWarning(lcWlLinuxDmabuf)
            << "Failed to create linux-dmabuf-v1 global from renderer";
        return;
    }

    qCInfo(lcWlLinuxDmabuf)
        << "Created linux-dmabuf-v1 global from renderer"
        << "version" << linuxDmabufV1Version;
}

void WLinuxDmabufV1::destroy([[maybe_unused]] WServer *server)
{
}

wl_global *WLinuxDmabufV1::global() const
{
    if (!handle() || !handle()->handle())
        return nullptr;

    return handle()->handle()->global;
}

WAYLIB_SERVER_END_NAMESPACE

#include "moc_wlinuxdmabufv1.cpp"
