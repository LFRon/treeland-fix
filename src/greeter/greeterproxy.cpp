// Copyright (C) 2025-2026 UnionTech Software Technology Co., Ltd.
// SPDX-License-Identifier: Apache-2.0 OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "greeterproxy.h"

// Treeland
#include "greeter/sessionmodel.h"
#include "greeter/usermodel.h"
#include "seat/helper.h"
#include "session/session.h"
#include "common/treelandlogging.h"
#include "core/lockscreen.h"

// DDM
#include <Login1Manager.h>
#include <Login1Session.h>
#include <rep_greeterddmremote_replica.h>

// Qt
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDBusInterface>
#include <QDBusPendingCall>
#include <QDBusReply>
#include <QGuiApplication>
#include <QLocalSocket>
#include <QRemoteObjectNode>
#include <QScopeGuard>
#include <QVariantMap>

// Waylib
#include <woutputrenderwindow.h>

// System
#include <security/pam_appl.h>
#include <systemd/sd-login.h>
#include <pwd.h>

using namespace DDM;

/////////////////////
// Local Functions //
/////////////////////

static bool localValidation(const QString &user, const QString &password)
{
    auto utf8Password = password.toUtf8();
    struct pam_conv conv = {
        []([[maybe_unused]] int num_msg,
           [[maybe_unused]] const struct pam_message **msg,
           struct pam_response **resp,
           void *appdata_ptr) {
            // pam uses free, we must malloc
            auto *reply = static_cast<pam_response *>(malloc(sizeof(pam_response)));
            reply->resp = strdup(static_cast<const char *>(appdata_ptr)); // Send password to PAM
            reply->resp_retcode = 0;
            *resp = reply;
            return PAM_SUCCESS;
        },
        static_cast<void *>(utf8Password.data()),
    };

    pam_handle_t *pamh = nullptr;

    int retval = pam_start("login", user.toUtf8().data(), &conv, &pamh);
    if (retval != PAM_SUCCESS) {
        return false;
    }

    retval = pam_authenticate(pamh, 0);
    pam_end(pamh, retval);

    return retval == PAM_SUCCESS;
}

static inline UserModel *userModel()
{
    return Helper::instance()->userModel();
}

static inline SessionModel *sessionModel()
{
    return Helper::instance()->sessionModel();
}

/////////////////////////////////
// Constructor & Deconstructor //
/////////////////////////////////

GreeterProxy::GreeterProxy(QObject *parent)
    : QObject(parent)
{
    m_socket = new QLocalSocket(this);

    // connect signals
    connect(m_socket, &QLocalSocket::connected, this, &GreeterProxy::connected);
    connect(m_socket, &QLocalSocket::disconnected, this, &GreeterProxy::disconnected);
    connect(m_socket, &QLocalSocket::errorOccurred, this, &GreeterProxy::error);

    auto conn = QDBusConnection::systemBus();
    conn.connect(Logind::serviceName(),
                 Logind::managerPath(),
                 Logind::managerIfaceName(),
                 "SessionNew",
                 this,
                 SLOT(onSessionNew(QString, QDBusObjectPath)));
    conn.connect(Logind::serviceName(),
                 Logind::managerPath(),
                 Logind::managerIfaceName(),
                 "SessionRemoved",
                 this,
                 SLOT(onSessionRemoved(QString, QDBusObjectPath)));
    conn.connect("org.deepin.DisplayManager",
                 "/org/deepin/DisplayManager",
                 "org.deepin.DisplayManager",
                 "AuthInfoChanged",
                 this,
                 SLOT(updateAuthSocket()));

    updateAuthSocket();
}

GreeterProxy::~GreeterProxy() { }

////////////////////////
// Properties setters //
////////////////////////

void GreeterProxy::setShowShutdownView(bool show) {
    if (m_showShutdownView != show) {
        m_showShutdownView = show;
        Q_EMIT showShutdownViewChanged(show);
    }
}

void GreeterProxy::setLock(bool isLocked)
{
    if (isLocked && !m_isLocked) {
        m_isLocked = true;
        if (m_lockScreen && !m_lockScreen->isVisible())
            m_lockScreen->lock();
        Q_EMIT lockChanged(true);
    } else if (!isLocked && m_isLocked) {
        m_failedAttempts = 0;
        Q_EMIT failedAttemptsChanged(0);
        m_isLocked = false;
        if (m_lockScreen && m_lockScreen->isVisible())
            Q_EMIT m_lockScreen->unlock();
        Q_EMIT lockChanged(false);
    }
    if (m_showShutdownView)
        setShowShutdownView(false);
}

///////////
// Slots //
///////////

void GreeterProxy::powerOff()
{
    if (isConnected())
        m_remoteReplica->powerOff();
}

void GreeterProxy::reboot()
{
    if (isConnected())
        m_remoteReplica->reboot();
}

void GreeterProxy::suspend()
{
    if (isConnected())
        m_remoteReplica->suspend();
}

void GreeterProxy::hibernate()
{
    if (isConnected())
        m_remoteReplica->hibernate();
}

void GreeterProxy::hybridSleep()
{
    if (isConnected())
        m_remoteReplica->hybridSleep();
}

void GreeterProxy::login(const QString &user, const QString &password, const int sessionIndex)
{
    if (!isConnected()) {
        qCDebug(treelandGreeter) << "Socket is not valid. Local password check.";
        if (localValidation(user, password)) {
            setLock(false);
        } else {
            Q_EMIT failedAttemptsChanged(++m_failedAttempts);
        }
        return;
    }

    // get model index
    QModelIndex index = sessionModel()->index(sessionIndex, 0);

    // send command to the daemon
    DDM::Session::Type type =
        static_cast<DDM::Session::Type>(sessionModel()->data(index, SessionModel::TypeRole).toInt());
    QString name = sessionModel()->data(index, SessionModel::FileRole).toString();
    qCInfo(treelandGreeter) << "Logging user" << user << "in with" << type << "session" << name;
    m_remoteReplica->login(user, password, type, name);
}

void GreeterProxy::unlock(const QString &user, const QString &password)
{
    if (!isConnected()) {
        qCDebug(treelandGreeter) << "Socket is not valid. Local password check.";
        if (localValidation(user, password)) {
            setLock(false);
        } else {
            Q_EMIT failedAttemptsChanged(++m_failedAttempts);
        }
        return;
    }

    auto userInfo = userModel()->get(user);
    if (userInfo.isValid()) {
        qCInfo(treelandGreeter) << "Unlocking user" << user;
        m_remoteReplica->unlock(user, password);
    }
}

void GreeterProxy::logout()
{
    auto session = Helper::instance()->sessionManager()->activeSession().lock();
    qCInfo(treelandGreeter) << "Logging user" << session->username() << "out with session id" << session->id();
    if (isConnected())
        m_remoteReplica->logout(session->id());
}

void GreeterProxy::lock()
{
    auto session = Helper::instance()->sessionManager()->activeSession().lock();
    if (!session || session->username() == "dde") {
        qCInfo(treelandGreeter) << "Trying to lock when no user session active, show lockscreen directly.";
        setLock(true);
        return;
    }
    qCInfo(treelandGreeter) << "Locking user" << session->username() << "with session id" << session->id();
    if (isConnected())
        m_remoteReplica->lock(session->id());
}

//////////////////////////////
// Logind session listeners //
//////////////////////////////

void GreeterProxy::onSessionNew(const QString &id, [[maybe_unused]] const QDBusObjectPath &path)
{
    QByteArray sessionBa = id.toLocal8Bit();
    const char *session = sessionBa.constData();
    char *username = nullptr;
    char *service = nullptr;
    auto guard = qScopeGuard([&]() {
        if (username)
            free(username);
        if (service)
            free(service);
    });
    if (sd_session_get_username(session, &username) < 0) {
        qCWarning(treelandGreeter) << "sd_session_get_username() failed for session id:" << id;
        return;
    }
    if (sd_session_get_service(session, &service) < 0) {
        qCWarning(treelandGreeter) << "sd_session_get_service() failed for session id:" << id;
        return;
    }

    if (strcmp(service, "ddm") == 0) {
        QString user = QString::fromLocal8Bit(username);
        qCInfo(treelandGreeter) << "New session added: id=" << id << ", user=" << user;
        userModel()->updateUserLoginState(user, true);
        // userLoggedIn signal is connected with Helper::updateActiveUserSession
        Q_EMIT userModel()->userLoggedIn(user, id.toInt());

        // Connect to Lock/Unlock signals
        auto conn = QDBusConnection::systemBus();
        conn.connect(Logind::serviceName(),
                     path.path(),
                     Logind::sessionIfaceName(),
                     "Lock",
                     this,
                     SLOT(onSessionLock()));
        conn.connect(Logind::serviceName(),
                     path.path(),
                     Logind::sessionIfaceName(),
                     "Unlock",
                     this,
                     SLOT(onSessionUnlock()));

        if (userModel()->currentUserName() == user)
            setLock(false);
        if (!m_hasActiveSession) {
            m_hasActiveSession = true;
            Q_EMIT hasActiveSessionChanged(true);
        }
    }
}

void GreeterProxy::onSessionRemoved(const QString &id, [[maybe_unused]] const QDBusObjectPath &path)
{
    // Disconnect from Lock/Unlock signals, if any
    auto conn = QDBusConnection::systemBus();
    conn.disconnect(Logind::serviceName(),
                    path.path(),
                    Logind::sessionIfaceName(),
                    "Lock",
                    this,
                    SLOT(onSessionLock()));
    conn.disconnect(Logind::serviceName(),
                    path.path(),
                    Logind::sessionIfaceName(),
                    "Unlock",
                    this,
                    SLOT(onSessionUnlock()));

    auto session = Helper::instance()->sessionManager()->sessionForId(id.toInt());
    if (session) {
        QString username = session->username();
        qCInfo(treelandGreeter) << "Session removed: id=" << id << ", user=" << username;
        if (Helper::instance()->sessionManager()->activeSession().lock() == session)
            setLock(true);
        userModel()->updateUserLoginState(username, false);
        Helper::instance()->sessionManager()->removeSession(session);
    }

    if (m_hasActiveSession && Helper::instance()->sessionManager()->sessions().isEmpty()) {
        m_hasActiveSession = false;
        Q_EMIT hasActiveSessionChanged(false);
    }
}

void GreeterProxy::onSessionLock()
{
    const QString path = message().path();
    QThreadPool::globalInstance()->start([this, path] {
        OrgFreedesktopLogin1SessionInterface session("org.freedesktop.login1",
                                                     path,
                                                     QDBusConnection::systemBus());
        int id = session.id().toInt();
        qCInfo(treelandGreeter) << "Lock signal received for session id:" << id;
        auto activeSession = Helper::instance()->sessionManager()->activeSession().lock();
        if (!activeSession)
            qCWarning(treelandGreeter)
                << "Lock signal received for non-exist session id:" << id << ", ignore.";
        else if (activeSession->id() != id)
            qCWarning(treelandGreeter)
                << "Lock signal received for non-active session id:" << id << ", ignore.";
        else
            QMetaObject::invokeMethod(this, [this] {
                setLock(true);
            });
    });
}

void GreeterProxy::onSessionUnlock()
{
    const QString path = message().path();
    QThreadPool::globalInstance()->start([this, path] {
        OrgFreedesktopLogin1SessionInterface session("org.freedesktop.login1",
                                                     path,
                                                     QDBusConnection::systemBus());
        int id = session.id().toInt();
        const QString username = session.name();
        qCInfo(treelandGreeter) << "Unlock signal received for session id:" << id;
        auto activeSession = Helper::instance()->sessionManager()->activeSession().lock();
        if (!activeSession) {
            qCWarning(treelandGreeter)
                << "Unlock signal received for non-exist session id:" << id << ", ignore.";
        } else if (activeSession->id() != id) {
            qCWarning(treelandGreeter)
                << "Unlock signal received for non-active session id:" << id << ", lock it back.";
            QMetaObject::invokeMethod(this, [this, id] {
                if (isConnected())
                    m_remoteReplica->lock(id);
            });
        } else {
            QMetaObject::invokeMethod(this, [this] {
                setLock(false);
            });
        }
    });
}

///////////////////////
// DDM Communication //
///////////////////////

bool GreeterProxy::isConnected() const
{
    return m_remoteReplica && m_remoteReplica->state() == QRemoteObjectReplica::Valid;
}

void GreeterProxy::connected()
{
    qCInfo(treelandGreeter) << "Connected to the ddm";
    Q_EMIT socketConnected();

    resetRemote();
    m_remoteNode.reset(new QRemoteObjectNode);
    m_remoteNode->addClientSideConnection(m_socket);
    m_remoteReplica.reset(m_remoteNode->acquire<GreeterDDMRemoteReplica>());

    connect(m_remoteReplica.get(), &GreeterDDMRemoteReplica::hostNameChanged, this, [this](const QString &hostName) {
        if (m_hostName == hostName)
            return;
        m_hostName = hostName;
        Q_EMIT hostNameChanged(m_hostName);
    });
    connect(m_remoteReplica.get(), &GreeterDDMRemoteReplica::canPowerOffChanged, this, [this](bool canPowerOff) {
        if (m_canPowerOff == canPowerOff)
            return;
        m_canPowerOff = canPowerOff;
        Q_EMIT canPowerOffChanged(m_canPowerOff);
    });
    connect(m_remoteReplica.get(), &GreeterDDMRemoteReplica::canRebootChanged, this, [this](bool canReboot) {
        if (m_canReboot == canReboot)
            return;
        m_canReboot = canReboot;
        Q_EMIT canRebootChanged(m_canReboot);
    });
    connect(m_remoteReplica.get(), &GreeterDDMRemoteReplica::canSuspendChanged, this, [this](bool canSuspend) {
        if (m_canSuspend == canSuspend)
            return;
        m_canSuspend = canSuspend;
        Q_EMIT canSuspendChanged(m_canSuspend);
    });
    connect(m_remoteReplica.get(), &GreeterDDMRemoteReplica::canHibernateChanged, this, [this](bool canHibernate) {
        if (m_canHibernate == canHibernate)
            return;
        m_canHibernate = canHibernate;
        Q_EMIT canHibernateChanged(m_canHibernate);
    });
    connect(m_remoteReplica.get(), &GreeterDDMRemoteReplica::canHybridSleepChanged, this, [this](bool canHybridSleep) {
        if (m_canHybridSleep == canHybridSleep)
            return;
        m_canHybridSleep = canHybridSleep;
        Q_EMIT canHybridSleepChanged(m_canHybridSleep);
    });
    connect(m_remoteReplica.get(), &GreeterDDMRemoteReplica::informationMessage, this, &GreeterProxy::informationMessage);
    connect(m_remoteReplica.get(), &GreeterDDMRemoteReplica::loginFailed, this, [this](const QString &user) {
        qCDebug(treelandGreeter) << "Message received from daemon: LoginFailed" << user;
        Q_EMIT failedAttemptsChanged(++m_failedAttempts);
    });
    connect(m_remoteReplica.get(), &GreeterDDMRemoteReplica::switchToGreeter, this, [this] {
        qCInfo(treelandGreeter) << "switch to greeter";
        lock();
    });
    connect(m_remoteReplica.get(), &GreeterDDMRemoteReplica::userActivated, this, [this](const QString &user, int sessionId) {
        if (!userModel()->getUser(user)) {
            qCInfo(treelandGreeter) << "activate user, but switch to greeter";
            lock();
            return;
        }

        userModel()->setCurrentUserName(user);
        qCInfo(treelandGreeter) << "activate successfully: " << user << ", XDG_SESSION_ID: " << sessionId;
    });
    connect(m_remoteReplica.get(), &GreeterDDMRemoteReplica::userLoggedIn, this, [this](const QString &user, int sessionId) {
        qCInfo(treelandGreeter) << "User " << user << " is already logged in";
        auto userPtr = userModel()->getUser(user);
        if (userPtr) {
            userModel()->updateUserLoginState(user, true);
            Q_EMIT userModel()->userLoggedIn(user, sessionId);
            QThreadPool::globalInstance()->start([this, sessionId] {
                auto conn = QDBusConnection::systemBus();
                OrgFreedesktopLogin1ManagerInterface manager("org.freedesktop.login1",
                                                             Logind::managerPath(),
                                                             conn);
                auto reply = manager.GetSession(QString::number(sessionId));
                reply.waitForFinished();
                if (!reply.isValid()) {
                    qCWarning(treelandGreeter) << "Failed to get session path for session id:" << sessionId << ", error:" << reply.error().message();
                    return;
                }
                auto path = reply.value();
                conn.connect(Logind::serviceName(),
                             path.path(),
                             Logind::sessionIfaceName(),
                             "Lock",
                             this,
                             SLOT(onSessionLock()));
                conn.connect(Logind::serviceName(),
                             path.path(),
                             Logind::sessionIfaceName(),
                             "Unlock",
                             this,
                             SLOT(onSessionUnlock()));
            });
        } else {
            qCWarning(treelandGreeter) << "User " << user << " logged in but not found";
        }
    });
    connect(m_remoteReplica.get(), &QRemoteObjectReplica::stateChanged, this,
            [this](QRemoteObjectReplica::State state, QRemoteObjectReplica::State oldState) {
                qCInfo(treelandGreeter) << "Greeter remote state changed from" << oldState << "to" << state;
                if (state != QRemoteObjectReplica::Valid)
                    return;

                syncRemoteState();
                m_remoteReplica->connectGreeter();
            });
}

void GreeterProxy::disconnected()
{
    qCWarning(treelandGreeter) << "Disconnected from the ddm";
    resetRemote();
    Q_EMIT socketDisconnected();
}

void GreeterProxy::error()
{
    qCCritical(treelandGreeter) << "Socket error: " << m_socket->errorString();
}

void GreeterProxy::updateAuthSocket()
{
    QThreadPool::globalInstance()->start([this]() {
        QDBusInterface manager("org.deepin.DisplayManager",
                               "/org/deepin/DisplayManager",
                               "org.deepin.DisplayManager",
                               QDBusConnection::systemBus());
        QDBusReply<QString> reply = manager.call("AuthInfo");
        if (!reply.isValid()) {
            qCWarning(treelandGreeter) << "Failed to get auth info from display manager:" << reply.error().message();
            return;
        }
        const QString &socket = reply.value();
        QMetaObject::invokeMethod(this, [this, socket] {
            if (m_socket->state() == QLocalSocket::ConnectedState)
                m_socket->disconnectFromServer();

            m_socket->connectToServer(socket);
        });
    });
}

void GreeterProxy::syncRemoteState()
{
    if (!m_remoteReplica)
        return;

    if (m_hostName != m_remoteReplica->hostName()) {
        m_hostName = m_remoteReplica->hostName();
        Q_EMIT hostNameChanged(m_hostName);
    }
    if (m_canPowerOff != m_remoteReplica->canPowerOff()) {
        m_canPowerOff = m_remoteReplica->canPowerOff();
        Q_EMIT canPowerOffChanged(m_canPowerOff);
    }
    if (m_canReboot != m_remoteReplica->canReboot()) {
        m_canReboot = m_remoteReplica->canReboot();
        Q_EMIT canRebootChanged(m_canReboot);
    }
    if (m_canSuspend != m_remoteReplica->canSuspend()) {
        m_canSuspend = m_remoteReplica->canSuspend();
        Q_EMIT canSuspendChanged(m_canSuspend);
    }
    if (m_canHibernate != m_remoteReplica->canHibernate()) {
        m_canHibernate = m_remoteReplica->canHibernate();
        Q_EMIT canHibernateChanged(m_canHibernate);
    }
    if (m_canHybridSleep != m_remoteReplica->canHybridSleep()) {
        m_canHybridSleep = m_remoteReplica->canHybridSleep();
        Q_EMIT canHybridSleepChanged(m_canHybridSleep);
    }
}

void GreeterProxy::resetRemote()
{
    m_remoteReplica.reset();
    m_remoteNode.reset();
}
