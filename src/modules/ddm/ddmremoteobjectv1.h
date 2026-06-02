// Copyright (C) 2026 UnionTech Software Technology Co., Ltd.
// SPDX-License-Identifier: Apache-2.0 OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#pragma once

#include "wserver.h"

#include <memory>

WAYLIB_SERVER_USE_NAMESPACE
QW_USE_NAMESPACE

class QLocalServer;
class QRemoteObjectHost;
class DDMRemoteObjectV1;

class DDMRemoteObjectV1Private;

class DDMRemoteObjectV1 : public QObject, public WServerInterface
{
    Q_OBJECT
public:
    explicit DDMRemoteObjectV1(QObject *parent = nullptr);
    ~DDMRemoteObjectV1() override;

    bool isConnected() const;

    QByteArrayView interfaceName() const override;
    static constexpr int InterfaceVersion = 1;

protected:
    void create(WServer *server) override;
    void destroy(WServer *server) override;
    wl_global *global() const override;

private:
    std::unique_ptr<DDMRemoteObjectV1Private> d;
};
