// Copyright (C) 2026 UnionTech Software Technology Co., Ltd.
// SPDX-License-Identifier: Apache-2.0 OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#pragma once

#include <WServer>

QW_BEGIN_NAMESPACE
class qw_linux_drm_syncobj_manager_v1;
class qw_renderer;
QW_END_NAMESPACE

WAYLIB_SERVER_BEGIN_NAMESPACE

class WLinuxDrmSyncobjManagerV1Private;
class WAYLIB_SERVER_EXPORT WLinuxDrmSyncobjManagerV1 : public QObject, public WObject, public WServerInterface
{
    Q_OBJECT
    W_DECLARE_PRIVATE(WLinuxDrmSyncobjManagerV1)

public:
    explicit WLinuxDrmSyncobjManagerV1(QW_NAMESPACE::qw_renderer *renderer = nullptr);

    QW_NAMESPACE::qw_linux_drm_syncobj_manager_v1 *handle() const;

    QByteArrayView interfaceName() const override;

protected:
    void create(WServer *server) override;
    void destroy(WServer *server) override;
    wl_global *global() const override;
};

WAYLIB_SERVER_END_NAMESPACE
