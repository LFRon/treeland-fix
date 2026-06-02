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
#include <rep_ddmremote_replica.h>

// Qt
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QGuiApplication>
#include <QLocalSocket>
#include <QRemoteObjectNode>
#include <QRemoteObjectPendingCallWatcher>
#include <QTimer>
#include <QVariantMap>

// Waylib
#include <woutputrenderwindow.h>

// System
#include <security/pam_appl.h>
#include <pwd.h>

using namespace DDM;

namespace {
constexpr auto ddmRemoteSocketName = "org.deepin.dde.ddm.qro";
}

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

    connectToDDM();
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
        watchRemoteCall("powerOff", m_remoteReplica->powerOff());
}

void GreeterProxy::reboot()
{
    if (isConnected())
        watchRemoteCall("reboot", m_remoteReplica->reboot());
}

void GreeterProxy::suspend()
{
    if (isConnected())
        watchRemoteCall("suspend", m_remoteReplica->suspend());
}

void GreeterProxy::hibernate()
{
    if (isConnected())
        watchRemoteCall("hibernate", m_remoteReplica->hibernate());
}

void GreeterProxy::hybridSleep()
{
    if (isConnected())
        watchRemoteCall("hybridSleep", m_remoteReplica->hybridSleep());
}

void GreeterProxy::login(const QString &user, const QString &password, const int sessionIndex)
{
    auto userInfo = userModel()->get(user);
    if (!isConnected() || !userInfo.isValid()) {
        qCDebug(treelandGreeter) << "Socket is not valid or user not found. Local password check.";
        if (localValidation(user, password)) {
            setLock(false);
        } else {
            Q_EMIT failedAttemptsChanged(++m_failedAttempts);
        }
        return;
    }

    if (userInfo.loggedIn) {
        qCInfo(treelandGreeter) << "Unlocking user" << user;
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
    watchRemoteCall("login", m_remoteReplica->login(user, password, type, name));
}

void GreeterProxy::logout()
{
    auto session = Helper::instance()->sessionManager()->activeSession().lock();
    if (!session) {
        qCWarning(treelandGreeter) << "Trying to logout when no user session active.";
        return;
    }

    qCInfo(treelandGreeter) << "Logging user" << session->username() << "out with session id" << session->id();
    if (isConnected())
        watchRemoteCall("logout", m_remoteReplica->logout(session->id()));
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
    setLock(true);
}

////////////////////////////
// DDM session listeners //
////////////////////////////

void GreeterProxy::addUserSession(const QString &user, int sessionId)
{
    qCInfo(treelandGreeter) << "User session added: id=" << sessionId << ", user=" << user;
    auto userPtr = userModel()->getUser(user);
    if (!userPtr) {
        qCWarning(treelandGreeter) << "User" << user << "logged in but not found";
        return;
    }

    userModel()->updateUserLoginState(user, true);
    Q_EMIT userModel()->userLoggedIn(user, sessionId);

    if (userModel()->currentUserName() == user)
        setLock(false);
    if (!m_hasActiveSession) {
        m_hasActiveSession = true;
        Q_EMIT hasActiveSessionChanged(true);
    }
}

void GreeterProxy::removeUserSession(const QString &user, int sessionId)
{
    qCInfo(treelandGreeter) << "User session removed: id=" << sessionId << ", user=" << user;
    auto session = Helper::instance()->sessionManager()->sessionForId(sessionId);
    if (session) {
        if (Helper::instance()->sessionManager()->activeSession().lock() == session)
            setLock(true);
        Helper::instance()->sessionManager()->removeSession(session);
    }
    userModel()->updateUserLoginState(user, false);

    if (m_hasActiveSession && Helper::instance()->sessionManager()->sessions().isEmpty()) {
        m_hasActiveSession = false;
        Q_EMIT hasActiveSessionChanged(false);
    }
}

///////////////////////
// DDM Communication //
///////////////////////

QString GreeterProxy::hostName() const
{
    return m_remoteReplica ? m_remoteReplica->hostName() : QString();
}

bool GreeterProxy::canPowerOff() const
{
    return m_remoteReplica && m_remoteReplica->canPowerOff();
}

bool GreeterProxy::canReboot() const
{
    return m_remoteReplica && m_remoteReplica->canReboot();
}

bool GreeterProxy::canSuspend() const
{
    return m_remoteReplica && m_remoteReplica->canSuspend();
}

bool GreeterProxy::canHibernate() const
{
    return m_remoteReplica && m_remoteReplica->canHibernate();
}

bool GreeterProxy::canHybridSleep() const
{
    return m_remoteReplica && m_remoteReplica->canHybridSleep();
}

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
    m_remoteReplica.reset(m_remoteNode->acquire<DDMRemoteReplica>());

    connect(m_remoteReplica.get(), &DDMRemoteReplica::hostNameChanged, this, &GreeterProxy::hostNameChanged);
    connect(m_remoteReplica.get(), &DDMRemoteReplica::canPowerOffChanged, this, &GreeterProxy::canPowerOffChanged);
    connect(m_remoteReplica.get(), &DDMRemoteReplica::canRebootChanged, this, &GreeterProxy::canRebootChanged);
    connect(m_remoteReplica.get(), &DDMRemoteReplica::canSuspendChanged, this, &GreeterProxy::canSuspendChanged);
    connect(m_remoteReplica.get(), &DDMRemoteReplica::canHibernateChanged, this, &GreeterProxy::canHibernateChanged);
    connect(m_remoteReplica.get(), &DDMRemoteReplica::canHybridSleepChanged, this, &GreeterProxy::canHybridSleepChanged);
    connect(m_remoteReplica.get(), &DDMRemoteReplica::informationMessage, this, &GreeterProxy::informationMessage);
    connect(m_remoteReplica.get(), &DDMRemoteReplica::loginFailed, this, &GreeterProxy::onLoginFailed);
    connect(m_remoteReplica.get(), &DDMRemoteReplica::userSessionAdded, this, &GreeterProxy::addUserSession);
    connect(m_remoteReplica.get(), &DDMRemoteReplica::userSessionRemoved, this, &GreeterProxy::removeUserSession);
    connect(m_remoteReplica.get(), &QRemoteObjectReplica::stateChanged, this, &GreeterProxy::remoteStateChanged);
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
    if (m_socket->state() == QLocalSocket::UnconnectedState)
        QTimer::singleShot(1000, this, &GreeterProxy::connectToDDM);
}

void GreeterProxy::connectToDDM()
{
    if (m_socket->state() != QLocalSocket::UnconnectedState)
        m_socket->abort();

    m_socket->connectToServer(QString::fromLatin1(ddmRemoteSocketName));
}

void GreeterProxy::onLoginFailed(const QString &user)
{
    qCDebug(treelandGreeter) << "Message received from daemon: LoginFailed" << user;
    Q_EMIT failedAttemptsChanged(++m_failedAttempts);
}

void GreeterProxy::remoteStateChanged(QRemoteObjectReplica::State state, QRemoteObjectReplica::State oldState)
{
    qCInfo(treelandGreeter) << "Greeter remote state changed from" << oldState << "to" << state;
    if (state != QRemoteObjectReplica::Valid)
        return;

    Q_EMIT hostNameChanged(hostName());
    Q_EMIT canPowerOffChanged(canPowerOff());
    Q_EMIT canRebootChanged(canReboot());
    Q_EMIT canSuspendChanged(canSuspend());
    Q_EMIT canHibernateChanged(canHibernate());
    Q_EMIT canHybridSleepChanged(canHybridSleep());
    watchRemoteCall("connectGreeter", m_remoteReplica->connectGreeter());
}

void GreeterProxy::watchRemoteCall(const char *operation, const QRemoteObjectPendingCall &call)
{
    auto *watcher = new QRemoteObjectPendingCallWatcher(call, this);
    connect(watcher, &QRemoteObjectPendingCallWatcher::finished, this, [operation](QRemoteObjectPendingCallWatcher *watcher) {
        if (watcher->error() != QRemoteObjectPendingCall::NoError)
            qCWarning(treelandGreeter) << "DDM remote call failed:" << operation << watcher->error();
        watcher->deleteLater();
    });
}

void GreeterProxy::resetRemote()
{
    m_remoteReplica.reset();
    m_remoteNode.reset();
}
