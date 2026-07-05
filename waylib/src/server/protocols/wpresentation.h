// Copyright (C) 2026 UnionTech Software Technology Co., Ltd.
// SPDX-License-Identifier: Apache-2.0 OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#pragma once

#include <WServer>

struct wlr_output;
struct wlr_surface;

QW_BEGIN_NAMESPACE
class qw_presentation;
QW_END_NAMESPACE

WAYLIB_SERVER_BEGIN_NAMESPACE

class WBackend;
class WPresentationPrivate;
class WAYLIB_SERVER_EXPORT WPresentation : public QObject, public WObject, public WServerInterface
{
    Q_OBJECT
    W_DECLARE_PRIVATE(WPresentation)

public:
    explicit WPresentation(WBackend *backend = nullptr);

    QW_NAMESPACE::qw_presentation *handle() const;
    void surfaceTexturedOnOutput(wlr_surface *surface, wlr_output *output) const;

    QByteArrayView interfaceName() const override;

protected:
    void create(WServer *server) override;
    void destroy(WServer *server) override;
    wl_global *global() const override;
};

WAYLIB_SERVER_END_NAMESPACE
