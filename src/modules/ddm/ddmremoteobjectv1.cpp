// Copyright (C) 2026 UnionTech Software Technology Co., Ltd.
// SPDX-License-Identifier: Apache-2.0 OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "ddmremoteobjectv1.h"

#include "rep_treelandddmremote_source.h"

#include "common/treelandlogging.h"
#include "greeterproxy.h"
#include "helper.h"
#include "usermodel.h"

#include <QDebug>
#include <QLocalServer>
#include <QLocalSocket>
#include <QRemoteObjectHost>

namespace {
constexpr auto remoteObjectName = "TreelandDDMRemote";
constexpr auto remoteObjectSocketName = "org.deepin.TreelandDDMRemote";
}

class DDMRemoteObjectV1Private : public TreelandDDMRemoteSource
{
public:
    explicit DDMRemoteObjectV1Private(DDMRemoteObjectV1 *qObject);
    void startHost();
    void stopHost();
    void switchToGreeter() override;
    void switchToUser(QString username) override;

    DDMRemoteObjectV1 *q = nullptr;
    QRemoteObjectHost *host = nullptr;
    QLocalServer *server = nullptr;
};

DDMRemoteObjectV1Private::DDMRemoteObjectV1Private(DDMRemoteObjectV1 *qObject)
    : TreelandDDMRemoteSource()
    , q(qObject)
{
}

void DDMRemoteObjectV1Private::startHost()
{
    if (host)
        return;

    QLocalServer::removeServer(QString::fromLatin1(remoteObjectSocketName));

    server = new QLocalServer(q);
    server->setSocketOptions(QLocalServer::UserAccessOption);
    if (!server->listen(QString::fromLatin1(remoteObjectSocketName))) {
        qCCritical(treelandCore) << "Failed to listen DDM remote socket:"
                                 << server->errorString();
        delete server;
        server = nullptr;
        return;
    }

    host = new QRemoteObjectHost(q);
    if (!host->enableRemoting(this, QString::fromLatin1(remoteObjectName))) {
        qCCritical(treelandCore) << "Failed to enable DDM remote object";
        delete host;
        host = nullptr;
        delete server;
        server = nullptr;
        QLocalServer::removeServer(QString::fromLatin1(remoteObjectSocketName));
        return;
    }

    QObject::connect(server, &QLocalServer::newConnection, q, [this] {
        while (server && server->hasPendingConnections()) {
            auto *socket = server->nextPendingConnection();
            if (!socket)
                continue;

            QObject::connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
            host->addHostSideConnection(socket);
        }
    });

    qCInfo(treelandCore) << "DDM remote object is listening on"
                         << server->fullServerName();
}

void DDMRemoteObjectV1Private::stopHost()
{
    if (server) {
        server->close();
        delete server;
        server = nullptr;
        QLocalServer::removeServer(QString::fromLatin1(remoteObjectSocketName));
    }

    if (!host)
        return;

    delete host;
    host = nullptr;
    qCInfo(treelandCore) << "DDM remote object stopped";
}

void DDMRemoteObjectV1Private::switchToGreeter()
{
    qCWarning(treelandCore) << "DDM remote: switchToGreeter";
    Helper::instance()->showLockScreen(false);
}

void DDMRemoteObjectV1Private::switchToUser(QString username)
{
    qCWarning(treelandCore) << "DDM remote: switchToUser" << username;
    auto helper = Helper::instance();
    if (username == "dde") {
        helper->showLockScreen(false);
    } else if (username != helper->userModel()->currentUserName()) {
        helper->userModel()->setCurrentUserName(username);
        helper->showLockScreen(false);
    }
}

DDMRemoteObjectV1::DDMRemoteObjectV1(QObject *parent)
    : QObject(parent)
    , WServerInterface()
    , d(new DDMRemoteObjectV1Private(this))
{
}

DDMRemoteObjectV1::~DDMRemoteObjectV1() = default;

QByteArrayView DDMRemoteObjectV1::interfaceName() const
{
    return QByteArrayView(remoteObjectName);
}

bool DDMRemoteObjectV1::isConnected() const
{
    return d->host != nullptr;
}

void DDMRemoteObjectV1::create(WServer *server)
{
    Q_UNUSED(server)

    auto *greeterProxy = Helper::instance()->greeterProxy();
    Q_ASSERT(greeterProxy);

    QObject::connect(greeterProxy, &GreeterProxy::socketConnected, this, [this] {
        d->startHost();
    }, Qt::UniqueConnection);
    QObject::connect(greeterProxy, &GreeterProxy::socketDisconnected, this, [this] {
        d->stopHost();
    }, Qt::UniqueConnection);

    if (greeterProxy->isConnected())
        d->startHost();
}

void DDMRemoteObjectV1::destroy([[maybe_unused]] WServer *server)
{
    d->stopHost();
}

wl_global *DDMRemoteObjectV1::global() const
{
    return nullptr;
}
