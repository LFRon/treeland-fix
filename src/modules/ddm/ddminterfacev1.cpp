// Copyright (C) 2025-2026 UnionTech Software Technology Co., Ltd.
// SPDX-License-Identifier: Apache-2.0 OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "ddminterfacev1.h"
#include "common/treelandlogging.h"
#include "greeterproxy.h"
#include "helper.h"
#include "usermodel.h"

#include <qwdisplay.h>

#include <QDebug>
#include <QFile>
#include <QLocalServer>
#include <QLocalSocket>
#include <QRemoteObjectHost>

#include <errno.h>
#include <string.h>
#include <sys/socket.h>

namespace {
constexpr auto remoteObjectName = "TreelandDDMRemote";
constexpr auto remoteObjectSocketName = "org.deepin.TreelandDDMRemote";
constexpr auto ddmServiceCgroup = "/ddm.service";

bool readPeerCredentials(qintptr socketDescriptor, struct ucred *credentials)
{
    const auto fd = static_cast<int>(socketDescriptor);
    if (fd < 0)
        return false;

    socklen_t length = sizeof(*credentials);
    return getsockopt(fd, SOL_SOCKET, SO_PEERCRED, credentials, &length) == 0
        && length == sizeof(*credentials);
}

bool peerCgroupContains(pid_t pid, const QString &marker)
{
    QFile file(QStringLiteral("/proc/%1/cgroup").arg(pid));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;

    while (!file.atEnd()) {
        if (QString::fromUtf8(file.readLine()).contains(marker))
            return true;
    }

    return false;
}

bool isExpectedDdmPeer(const QLocalSocket &socket)
{
    struct ucred credentials {};
    if (!readPeerCredentials(socket.socketDescriptor(), &credentials)) {
        qCCritical(treelandCore) << "Failed to read DDM peer credentials:" << strerror(errno);
        return false;
    }

    if (credentials.uid != 0) {
        qCWarning(treelandCore) << "Rejecting DDM remote peer with unexpected uid"
                                << credentials.uid;
        return false;
    }

    if (!peerCgroupContains(credentials.pid, QString::fromLatin1(ddmServiceCgroup))) {
        qCWarning(treelandCore) << "Rejecting DDM remote peer outside ddm.service cgroup:"
                                << credentials.pid;
        return false;
    }

    return true;
}
}

DDMInterfaceV1Private::DDMInterfaceV1Private(DDMInterfaceV1 *_q)
    : TreelandDDMRemoteSource()
    , q(_q)
{
}

void DDMInterfaceV1Private::startHost()
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
            if (!isExpectedDdmPeer(*socket)) {
                socket->disconnectFromServer();
                continue;
            }

            host->addHostSideConnection(socket);
        }
    });

    qCInfo(treelandCore) << "DDM remote object is listening on"
                         << server->fullServerName();
}

void DDMInterfaceV1Private::stopHost()
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

void DDMInterfaceV1Private::switchToGreeter()
{
    qCWarning(treelandCore) << "DDM remote: switchToGreeter";
    Helper::instance()->showLockScreen(false);
}

void DDMInterfaceV1Private::switchToUser(QString username)
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

void DDMInterfaceV1Private::activateSession()
{
    qCWarning(treelandCore) << "DDM remote: activateSession";
    Helper::instance()->activateSession();
}

void DDMInterfaceV1Private::deactivateSession()
{
    qCWarning(treelandCore) << "DDM remote: deactivateSession";
    Helper::instance()->deactivateSession();
}

void DDMInterfaceV1Private::enableRender()
{
    qCWarning(treelandCore) << "DDM remote: enableRender";
    Helper::instance()->enableRender();
}

void DDMInterfaceV1Private::disableRender()
{
    qCWarning(treelandCore) << "DDM remote: disableRender";
    Helper::instance()->disableRender();
}

DDMInterfaceV1::DDMInterfaceV1(QObject *parent)
    : QObject(parent)
    , WServerInterface()
    , d(new DDMInterfaceV1Private(this))
{
}

DDMInterfaceV1::~DDMInterfaceV1() = default;

QByteArrayView DDMInterfaceV1::interfaceName() const
{
    return QByteArrayView(remoteObjectName);
}

bool DDMInterfaceV1::isConnected() const
{
    return d->host != nullptr;
}

void DDMInterfaceV1::create(WServer *server)
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

void DDMInterfaceV1::destroy([[maybe_unused]] WServer *server)
{
    d->stopHost();
}

wl_global *DDMInterfaceV1::global() const
{
    return nullptr;
}
