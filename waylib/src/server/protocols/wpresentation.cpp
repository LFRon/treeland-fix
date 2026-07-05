// Copyright (C) 2026 UnionTech Software Technology Co., Ltd.
// SPDX-License-Identifier: Apache-2.0 OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "wpresentation.h"

#include "private/wglobal_p.h"
#include "wayliblogging.h"
#include "wbackend.h"

#include <qwbackend.h>
#include <qwdisplay.h>
#include <qwpresentation.h>

QW_USE_NAMESPACE
WAYLIB_SERVER_BEGIN_NAMESPACE

static constexpr uint32_t presentationVersion = 2;

class Q_DECL_HIDDEN WPresentationPrivate : public WObjectPrivate
{
public:
    WPresentationPrivate(WPresentation *qq, WBackend *backend)
        : WObjectPrivate(qq)
        , backend(backend)
    {
    }

    inline qw_presentation *handle() const
    {
        return q_func()->nativeInterface<qw_presentation>();
    }

    WBackend *backend = nullptr;

    W_DECLARE_PUBLIC(WPresentation)
};

WPresentation::WPresentation(WBackend *backend)
    : WObject(*new WPresentationPrivate(this, backend))
{
}

qw_presentation *WPresentation::handle() const
{
    return nativeInterface<qw_presentation>();
}

void WPresentation::surfaceTexturedOnOutput(wlr_surface *surface, wlr_output *output) const
{
    if (!handle() || !surface || !output)
        return;

    wlr_presentation_surface_textured_on_output(surface, output);
}

QByteArrayView WPresentation::interfaceName() const
{
    return "wp_presentation";
}

void WPresentation::create(WServer *server)
{
    W_D(WPresentation);
    if (m_handle || !server || !server->handle())
        return;

    if (!d->backend || !d->backend->handle()) {
        qCWarning(lcWlPresentation)
            << "presentation-time not created: backend is unavailable";
        return;
    }

    auto *presentation = wlr_presentation_create(server->handle()->handle(),
                                                 d->backend->handle()->handle(),
                                                 presentationVersion);
    m_handle = qw_presentation::from(presentation);
    if (!m_handle) {
        qCWarning(lcWlPresentation)
            << "Failed to create presentation-time global";
        return;
    }

    qCInfo(lcWlPresentation)
        << "Created presentation-time global"
        << "version" << presentationVersion;
}

void WPresentation::destroy([[maybe_unused]] WServer *server)
{
}

wl_global *WPresentation::global() const
{
    if (!handle() || !handle()->handle())
        return nullptr;

    return handle()->handle()->global;
}

WAYLIB_SERVER_END_NAMESPACE

#include "moc_wpresentation.cpp"
