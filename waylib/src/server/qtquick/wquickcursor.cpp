// Copyright (C) 2023-2026 UnionTech Software Technology Co., Ltd.
// SPDX-License-Identifier: Apache-2.0 OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "wquickcursor.h"
#include "wayliblogging.h"
#include "woutputrenderwindow.h"
#include "woutputitem.h"
#include "woutput.h"
#include "wcursorimage.h"
#include "wsgtextureprovider.h"
#include "wimagebuffer.h"
#include "wseat.h"
#include "wsurfaceitem.h"
#include "wrenderhelper.h"

#include <qwxcursormanager.h>
#include <qwbuffer.h>
#include <qwcursor.h>
#include <qwoutput.h>
#include <qwrenderer.h>
#include <qwtexture.h>
#include <qwcompositor.h>

#include <QElapsedTimer>
#include <QHash>
#include <QSGImageNode>
#include <QSGRendererInterface>
#include <private/qquickitem_p.h>
#include <private/qsgplaintexture_p.h>

QW_USE_NAMESPACE
WAYLIB_SERVER_BEGIN_NAMESPACE

static bool envFlagEnabled(const char *name, bool defaultValue)
{
    if (!qEnvironmentVariableIsSet(name))
        return defaultValue;

    const QByteArray value = qgetenv(name).trimmed().toLower();
    return !(value == "0" || value == "false" || value == "no" || value == "off");
}

static bool detachedCursorDiagnosticsEnabled()
{
    static const bool enabled = envFlagEnabled("WAYLIB_VK_DETACHED_CURSOR_DIAGNOSTICS", false);
    return enabled;
}

class Q_DECL_HIDDEN CursorTextureProvider : public WSGTextureProvider
{
public:
    CursorTextureProvider(WOutputRenderWindow *window)
        : WSGTextureProvider(window)
    {

    }

    void setImage(const QImage &image) {
        if (image.isNull()) {
            resetBuffer();
            return;
        }

#ifdef ENABLE_VULKAN_RENDER
        if (isVulkanRenderer()) {
            if (isCachedImage(image) && buffer && directImageTextureValid)
                return;

            QImage ownedImage = image.copy();
            ownedImage.setDevicePixelRatio(image.devicePixelRatio());

            // WImageBufferImpl destroy following qw_buffer
            auto buffer = qw_buffer::create(new WImageBufferImpl(ownedImage),
                                           ownedImage.width(), ownedImage.height());
            this->buffer.reset(buffer);

            directImageTexture.setImage(ownedImage);
            directImageTexture.setHasAlphaChannel(ownedImage.hasAlphaChannel());
            directImageTextureValid = true;
            updateCachedImage(image);

            if (WSGTextureProvider::texture() || WSGTextureProvider::qwBuffer())
                WSGTextureProvider::setBuffer(nullptr);
            else
                Q_EMIT textureChanged();
            return;
        }
#endif

        // WImageBufferImpl destroy following qw_buffer
        auto buffer = qw_buffer::create(new WImageBufferImpl(image),
                                       image.width(), image.height());
        this->buffer.reset(buffer);
        setBuffer(this->buffer.get());
    }

    void setProxy(WSGTextureProvider *proxy) {
        if (this->proxy == proxy)
            return;

        if (this->proxy) {
            bool ok = this->proxy->disconnect(this);
            Q_ASSERT(ok);
        }

        this->proxy = proxy;

        if (this->proxy) {
            connect(proxy, &WSGTextureProvider::textureChanged,
                    this, &CursorTextureProvider::textureChanged);
        }

        Q_EMIT textureChanged();
    }

    void resetBuffer() {
#ifdef ENABLE_VULKAN_RENDER
        if (directImageTextureValid)
            clearDirectImageTexture();
#endif
        setBuffer(nullptr);
        buffer.reset();
#ifdef ENABLE_VULKAN_RENDER
        clearCachedImage();
#endif
    }
    void reset() {
        resetBuffer();
        setProxy(nullptr);
    }

    QSGTexture *texture() const override {
        if (proxy)
            return proxy->texture();
#ifdef ENABLE_VULKAN_RENDER
        if (directImageTextureValid)
            return const_cast<QSGPlainTexture *>(&directImageTexture);
#endif
        return WSGTextureProvider::texture();
    }
    qw_texture *qwTexture() const override {
        if (proxy)
            return proxy->qwTexture();
        return WSGTextureProvider::qwTexture();
    }
    qw_buffer *qwBuffer() const override {
        if (proxy)
            return proxy->qwBuffer();
#ifdef ENABLE_VULKAN_RENDER
        if (directImageTextureValid)
            return buffer.get();
#endif
        return WSGTextureProvider::qwBuffer();
    }

    QSGTexture *paintTexture() const {
#ifdef ENABLE_VULKAN_RENDER
        if (directImageTextureValid)
            return const_cast<QSGPlainTexture *>(&directImageTexture);
#endif
        return WSGTextureProvider::texture();
    }

#ifdef ENABLE_VULKAN_RENDER
    bool hasDirectImageTexture() const {
        return directImageTextureValid;
    }

    bool hasVulkanRenderer() const {
        return isVulkanRenderer();
    }
#endif

    std::unique_ptr<qw_buffer, qw_buffer::droper> buffer;
    QPointer<WSGTextureProvider> proxy;

#ifdef ENABLE_VULKAN_RENDER
private:
    bool isVulkanRenderer() const {
        return window() && window()->renderer()
            && window()->renderer()->is_vk();
    }

    bool isCachedImage(const QImage &image) const {
        return cachedImageKey == image.cacheKey()
            && cachedImageSize == image.size()
            && cachedImageDevicePixelRatio == image.devicePixelRatio()
            && cachedImageFormat == image.format();
    }

    void updateCachedImage(const QImage &image) {
        cachedImageKey = image.cacheKey();
        cachedImageSize = image.size();
        cachedImageDevicePixelRatio = image.devicePixelRatio();
        cachedImageFormat = image.format();
    }

    void clearDirectImageTexture() {
        directImageTexture.setTexture(nullptr);
        directImageTexture.setTextureSize({});
        directImageTexture.setHasAlphaChannel(false);
        directImageTextureValid = false;
    }

    void clearCachedImage() {
        cachedImageKey = 0;
        cachedImageSize = {};
        cachedImageDevicePixelRatio = 0;
        cachedImageFormat = QImage::Format_Invalid;
    }

    qint64 cachedImageKey = 0;
    QSize cachedImageSize;
    qreal cachedImageDevicePixelRatio = 0;
    QImage::Format cachedImageFormat = QImage::Format_Invalid;
    QSGPlainTexture directImageTexture;
    bool directImageTextureValid = false;
#endif
};

WQuickCursorAttached::WQuickCursorAttached(QQuickItem *parent)
    : QObject(parent)
{

}

QQuickItem *WQuickCursorAttached::parent() const
{
    return qobject_cast<QQuickItem*>(QObject::parent());
}

WGlobal::CursorShape WQuickCursorAttached::shape() const
{
    return static_cast<WGlobal::CursorShape>(parent()->cursor().shape());
}

void WQuickCursorAttached::setShape(WGlobal::CursorShape shape)
{
    if (this->shape() == shape)
        return;
    parent()->setCursor(WCursor::toQCursor(shape));
    Q_EMIT shapeChanged();
}

class Q_DECL_HIDDEN WQuickCursorPrivate : public QQuickItemPrivate
{
public:
    WQuickCursorPrivate(WQuickCursor *qq);
    ~WQuickCursorPrivate() {
    }

    inline static WQuickCursorPrivate *get(WQuickCursor *qq) {
        return qq->d_func();
    }

    void cleanTextureProvider();

    void invalidate() {
        cleanTextureProvider();
    }

    void setSurface(WSurface *surface);
    void enterOutput(WOutput *output);
    void leaveOutput(WOutput *output);
    void updateXCursorManager();
    void onImageChanged();
    void updateCursor();
    void updateImplicitSize();
    void setHotSpot(const QPoint &newHotSpot);
    void setDetachedPresentationActive(bool active);
    void resetDetachedFallback();
    void updateDetachedCursor();
    void releaseDetachedCursor();
    bool detachedCursorAllowed() const;
    bool cursorOnOutput() const;
    bool takeDetachedCursorOwnership();
    bool updateDetachedCursorImage();
    bool updateDetachedCursorSurface();
    void maybeLogDetachedCursorDiagnostics();

    inline quint32 getCursorSize() const {
        return qMax(cursorSize.width(), cursorSize.height());
    }

    Q_DECLARE_PUBLIC(WQuickCursor)

    mutable CursorTextureProvider *textureProvider = nullptr;

    QPointer<WCursor> cursor;
    QPointer<WOutput> output;
    WCursorImage *cursorImage = nullptr;

    QPointer<WSurfaceItemContent> cursorSurfaceItem;

    QString xcursorThemeName;
    QSize cursorSize = QSize(24, 24);
    QPoint hotSpot;
    bool detachedPresentation = false;
    bool detachedPresentationActive = false;
    bool detachedCursorUnavailable = false;
    QByteArray detachedCursorFallbackReason = QByteArrayLiteral("none");
    std::unique_ptr<qw_buffer, qw_buffer::droper> detachedCursorBuffer;
    qint64 detachedCursorImageKey = 0;
    QSize detachedCursorImageSize;
    qreal detachedCursorImageDevicePixelRatio = 0;
    QImage::Format detachedCursorImageFormat = QImage::Format_Invalid;
    QPointer<WSurface> detachedCursorSurface;
    QPoint detachedCursorSurfaceHotSpot;
    QElapsedTimer detachedCursorDiagnosticsTimer;
    quint64 detachedCursorMoveOnlyCount = 0;
    quint64 detachedCursorBufferUpdateCount = 0;
    quint64 detachedCursorSurfaceUpdateCount = 0;
    quint64 detachedCursorFallbackCount = 0;
    QMetaObject::Connection outputForceSoftwareCursorConnection;
    QMetaObject::Connection outputScaleConnection;
};

static QHash<WCursor *, QPointer<WQuickCursor>> &detachedCursorOwners()
{
    static QHash<WCursor *, QPointer<WQuickCursor>> owners;
    return owners;
}

void WQuickCursorPrivate::setHotSpot(const QPoint &newHotSpot)
{
    if (hotSpot == newHotSpot)
        return;
    hotSpot = newHotSpot;

    W_Q(WQuickCursor);
    Q_EMIT q->hotSpotChanged();
}

void WQuickCursorPrivate::setDetachedPresentationActive(bool active)
{
    if (detachedPresentationActive == active)
        return;

    detachedPresentationActive = active;

    W_Q(WQuickCursor);
    Q_EMIT q->detachedPresentationActiveChanged();
}

void WQuickCursorPrivate::resetDetachedFallback()
{
    detachedCursorUnavailable = false;
    detachedCursorFallbackReason = QByteArrayLiteral("none");
}

bool WQuickCursorPrivate::detachedCursorAllowed() const
{
#ifdef ENABLE_VULKAN_RENDER
    if (!detachedPresentation)
        return false;

    if (!envFlagEnabled("WAYLIB_VK_DETACHED_CURSOR", true))
        return false;

    if (!cursor || !output || !cursor->layout())
        return false;

    if (!cursor->isVisible())
        return false;

    if (output->forceSoftwareCursor())
        return false;

    if (WRenderHelper::getGraphicsApi() != QSGRendererInterface::Vulkan)
        return false;

    const auto renderer = output->renderer();
    if (!renderer || !renderer->is_vk())
        return false;

    const auto nativeOutput = output->nativeHandle();
    return nativeOutput
        && nativeOutput->software_cursor_locks == 0;
#else
    return false;
#endif
}

bool WQuickCursorPrivate::cursorOnOutput() const
{
    if (!cursor || !output || !output->isEnabled())
        return false;

    const QRectF outputGeometry(QPointF(output->position()),
                                QSizeF(output->effectiveSize()));
    return outputGeometry.contains(cursor->position());
}

bool WQuickCursorPrivate::takeDetachedCursorOwnership()
{
    if (!cursor)
        return false;

    W_Q(WQuickCursor);
    auto &owners = detachedCursorOwners();
    if (auto owner = owners.value(cursor)) {
        if (owner == q)
            return true;

        auto ownerPrivate = WQuickCursorPrivate::get(owner);
        if (ownerPrivate->cursorOnOutput()
            && ownerPrivate->detachedPresentationActive) {
            return false;
        }

        ownerPrivate->releaseDetachedCursor();
    }

    owners.insert(cursor, q);
    return true;
}

void WQuickCursorPrivate::releaseDetachedCursor()
{
    W_Q(WQuickCursor);
    if (cursor && detachedCursorOwners().value(cursor) == q) {
        cursor->handle()->unset_image();
        detachedCursorOwners().remove(cursor);
    }

    detachedCursorBuffer.reset();
    detachedCursorImageKey = 0;
    detachedCursorImageSize = {};
    detachedCursorImageDevicePixelRatio = 0;
    detachedCursorImageFormat = QImage::Format_Invalid;
    detachedCursorSurface.clear();
    detachedCursorSurfaceHotSpot = {};
    setDetachedPresentationActive(false);
}

bool WQuickCursorPrivate::updateDetachedCursorImage()
{
    if (!cursor || !cursorImage)
        return false;

    const QImage image = cursorImage->image();
    if (image.isNull())
        return false;

    const qreal imageScale = image.devicePixelRatio() > 0
        ? image.devicePixelRatio()
        : 1.0;
    const bool imageChanged = detachedCursorImageKey != image.cacheKey()
        || detachedCursorImageSize != image.size()
        || !qFuzzyCompare(detachedCursorImageDevicePixelRatio, image.devicePixelRatio())
        || detachedCursorImageFormat != image.format();

    if (imageChanged || !detachedPresentationActive || !detachedCursorBuffer) {
        QImage ownedImage = image.copy();
        ownedImage.setDevicePixelRatio(image.devicePixelRatio());
        detachedCursorBuffer.reset(qw_buffer::create(new WImageBufferImpl(ownedImage),
                                                     ownedImage.width(),
                                                     ownedImage.height()));
        if (!detachedCursorBuffer)
            return false;

        detachedCursorImageKey = image.cacheKey();
        detachedCursorImageSize = image.size();
        detachedCursorImageDevicePixelRatio = image.devicePixelRatio();
        detachedCursorImageFormat = image.format();
        detachedCursorSurface.clear();
        detachedCursorSurfaceHotSpot = {};

        const QPoint logicalHotSpot(qRound(cursorImage->hotSpot().x() / imageScale),
                                    qRound(cursorImage->hotSpot().y() / imageScale));
        cursor->handle()->set_buffer(detachedCursorBuffer->handle(),
                                     logicalHotSpot.x(),
                                     logicalHotSpot.y(),
                                     imageScale);
        ++detachedCursorBufferUpdateCount;
    }

    return true;
}

bool WQuickCursorPrivate::updateDetachedCursorSurface()
{
    if (!cursor)
        return false;

    const auto requestedSurface = cursor->requestedCursorSurface();
    WSurface *surface = requestedSurface.first;
    if (!surface || !surface->handle())
        return false;

    if (detachedCursorSurface != surface
        || detachedCursorSurfaceHotSpot != requestedSurface.second
        || !detachedPresentationActive) {
        cursor->handle()->set_surface(surface->handle()->handle(),
                                      requestedSurface.second.x(),
                                      requestedSurface.second.y());
        detachedCursorSurface = surface;
        detachedCursorSurfaceHotSpot = requestedSurface.second;
        detachedCursorBuffer.reset();
        detachedCursorImageKey = 0;
        detachedCursorImageSize = {};
        detachedCursorImageDevicePixelRatio = 0;
        detachedCursorImageFormat = QImage::Format_Invalid;
        ++detachedCursorSurfaceUpdateCount;
    }

    return true;
}

void WQuickCursorPrivate::updateDetachedCursor()
{
    if (!detachedCursorAllowed() || !cursorOnOutput()) {
        releaseDetachedCursor();
        maybeLogDetachedCursorDiagnostics();
        return;
    }

    if (detachedCursorUnavailable) {
        setDetachedPresentationActive(false);
        maybeLogDetachedCursorDiagnostics();
        return;
    }

    if (!takeDetachedCursorOwnership()) {
        setDetachedPresentationActive(false);
        maybeLogDetachedCursorDiagnostics();
        return;
    }

    bool imageUpdated = false;
    if (WGlobal::isClientResourceCursor(cursor->cursor())
        && cursor->requestedCursorShape() == WGlobal::CursorShape::Invalid) {
        imageUpdated = updateDetachedCursorSurface();
    } else {
        imageUpdated = updateDetachedCursorImage();
    }

    if (!imageUpdated) {
        releaseDetachedCursor();
        maybeLogDetachedCursorDiagnostics();
        return;
    }

    const auto nativeOutput = output->nativeHandle();
    const bool hardwareCursorActive = nativeOutput
        && nativeOutput->hardware_cursor
        && nativeOutput->hardware_cursor->enabled
        && nativeOutput->software_cursor_locks == 0;
    if (!hardwareCursorActive) {
        detachedCursorUnavailable = true;
        detachedCursorFallbackReason = QByteArrayLiteral("hardware-cursor-unavailable");
        ++detachedCursorFallbackCount;
        releaseDetachedCursor();
        maybeLogDetachedCursorDiagnostics();
        return;
    }

    setDetachedPresentationActive(true);
    maybeLogDetachedCursorDiagnostics();
}

void WQuickCursorPrivate::maybeLogDetachedCursorDiagnostics()
{
    if (!detachedCursorDiagnosticsEnabled())
        return;

    if (!detachedCursorDiagnosticsTimer.isValid()) {
        detachedCursorDiagnosticsTimer.start();
        return;
    }

    if (detachedCursorDiagnosticsTimer.elapsed() < 1000)
        return;

    qCInfo(lcWlDetachedCursor)
        << "Detached cursor summary"
        << "cursor" << cursor.data()
        << "output" << (output ? output->name() : QString())
        << "requested" << detachedPresentation
        << "active" << detachedPresentationActive
        << "unavailable" << detachedCursorUnavailable
        << "fallbackReason" << detachedCursorFallbackReason.constData()
        << "moveOnly" << detachedCursorMoveOnlyCount
        << "bufferUpdates" << detachedCursorBufferUpdateCount
        << "surfaceUpdates" << detachedCursorSurfaceUpdateCount
        << "fallbacks" << detachedCursorFallbackCount;

    detachedCursorMoveOnlyCount = 0;
    detachedCursorBufferUpdateCount = 0;
    detachedCursorSurfaceUpdateCount = 0;
    detachedCursorFallbackCount = 0;
    detachedCursorDiagnosticsTimer.restart();
}

WQuickCursorPrivate::WQuickCursorPrivate(WQuickCursor *)
    : QQuickItemPrivate()
{

}

void WQuickCursorPrivate::cleanTextureProvider()
{
    if (textureProvider) {
        class WQuickCursorCleanupJob : public QRunnable
        {
        public:
            WQuickCursorCleanupJob(QObject *object) : m_object(object) { }
            void run() override {
                delete m_object;
            }
            QObject *m_object;
        };

        // Delay clean the textures on the next render after.
        window->scheduleRenderJob(new WQuickCursorCleanupJob(textureProvider),
                                  QQuickWindow::AfterRenderingStage);
        textureProvider = nullptr;
    }
}

void WQuickCursorPrivate::setSurface(WSurface *surface)
{
    W_Q(WQuickCursor);

    if (surface) {
        if (!cursorSurfaceItem) {
            cursorSurfaceItem = new WSurfaceItemContent(q);
            cursorSurfaceItem->setIgnoreBufferOffset(true);
            QQuickItemPrivate::get(cursorSurfaceItem)->anchors()->setFill(q);
            bool ok = QObject::connect(cursorSurfaceItem, SIGNAL(implicitWidthChanged()),
                                       q, SLOT(updateImplicitSize()));
            Q_ASSERT(ok);
            ok = QObject::connect(cursorSurfaceItem, SIGNAL(implicitHeightChanged()),
                                  q, SLOT(updateImplicitSize()));
            Q_ASSERT(ok);
            QObject::connect(cursorSurfaceItem, &WSurfaceItemContent::bufferOffsetChanged,
                             q, [this] {
                auto rs = this->cursor->requestedCursorSurface();
                setHotSpot(rs.second - cursorSurfaceItem->bufferOffset());
            });
        }

        cursorSurfaceItem->setSurface(surface);
        if (textureProvider)
            textureProvider->setProxy(cursorSurfaceItem->wTextureProvider());
        Q_ASSERT(!q->flags().testFlag(QQuickItem::ItemHasContents));

        if (q->isVisible())
            enterOutput(output);
    } else {
        if (cursorSurfaceItem) {
            leaveOutput(output);
            cursorSurfaceItem->deleteLater();
            cursorSurfaceItem = nullptr;
        }
    }
    updateImplicitSize();
    if (q->flags().testFlag(QQuickItem::ItemHasContents))
        q->update();

    resetDetachedFallback();
    updateDetachedCursor();
}

void WQuickCursorPrivate::enterOutput(WOutput *output)
{
    if (!output)
        return;
    if (!cursorSurfaceItem)
        return;
    auto surface = cursorSurfaceItem->surface();
    if (!surface)
        return;
    surface->enterOutput(output);
}

void WQuickCursorPrivate::leaveOutput(WOutput *output)
{
    if (!output)
        return;
    if (!cursorSurfaceItem)
        return;
    auto surface = cursorSurfaceItem->surface();
    if (!surface)
        return;
    surface->leaveOutput(output);
}

void WQuickCursorPrivate::updateXCursorManager()
{
    cursorImage->setCursorTheme(xcursorThemeName.toLatin1(), getCursorSize());
    cursorImage->setScale(window ? window->effectiveDevicePixelRatio() : 1.0);
}

void WQuickCursorPrivate::onImageChanged()
{
    updateImplicitSize();

    if (!cursorSurfaceItem) {
        setHotSpot(cursorImage->hotSpot());
        resetDetachedFallback();
        updateDetachedCursor();
        if (!detachedPresentationActive)
            q_func()->update();
    }
}

void WQuickCursorPrivate::updateCursor()
{
    W_Q(WQuickCursor);

    auto cursor = this->cursor->cursor();
    if (WGlobal::isClientResourceCursor(cursor)) {
        // First try use cursor shape
        auto shape = this->cursor->requestedCursorShape();
        if (shape != WGlobal::CursorShape::Invalid) {
            q->setFlag(QQuickItem::ItemHasContents, true);
            setSurface(nullptr);
            cursorImage->setCursor(WCursor::toQCursor(shape));
            setHotSpot(cursorImage->hotSpot());
        } else { // Second use cursor surface
            auto rs = this->cursor->requestedCursorSurface();
            q->setFlag(QQuickItem::ItemHasContents, false);
            dirty(QQuickItemPrivate::Content);

            setSurface(rs.first);
            setHotSpot(rs.second);
        }
    } else {
        q->setFlag(QQuickItem::ItemHasContents, true);
        setSurface(nullptr);
        cursorImage->setCursor(cursor);
        setHotSpot(cursorImage->hotSpot());
    }

    Q_EMIT q_func()->validChanged();
    resetDetachedFallback();
    updateDetachedCursor();
}

void WQuickCursorPrivate::updateImplicitSize()
{
    W_Q(WQuickCursor);

    if (cursorSurfaceItem) {
        q->setImplicitSize(cursorSurfaceItem->implicitWidth(),
                           cursorSurfaceItem->implicitHeight());
    } else if (const QImage &i = cursorImage->image(); !i.isNull()) {
        const auto size = i.deviceIndependentSize();
        q->setImplicitSize(size.width(), size.height());
    } else {
        q->setImplicitSize(cursorSize.width(), cursorSize.height());
    }

    Q_EMIT q->hotSpotChanged();
}

WQuickCursor::WQuickCursor(QQuickItem *parent)
    : QQuickItem(*new WQuickCursorPrivate(this), parent)
{
    W_D(WQuickCursor);
    d->cursorImage = new WCursorImage(this);
    setFlag(QQuickItem::ItemHasContents);
    setImplicitSize(d->cursorSize.width(), d->cursorSize.height());

    connect(d->cursorImage, SIGNAL(imageChanged()), this, SLOT(onImageChanged()));
}

WQuickCursor::~WQuickCursor()
{
    d_func()->releaseDetachedCursor();
    // `d->window` will become nullptr in ~QQuickItem
    // cleanTextureProvider in ~WQuickCursorPrivate is too late
    d_func()->cleanTextureProvider();
}

WQuickCursorAttached *WQuickCursor::qmlAttachedProperties(QObject *target)
{
    if (!target->isQuickItemType())
        return nullptr;
    return new WQuickCursorAttached(qobject_cast<QQuickItem*>(target));
}

QSGTextureProvider *WQuickCursor::textureProvider() const
{
    if (QQuickItem::isTextureProvider())
        return QQuickItem::textureProvider();

    return wTextureProvider();
}

WSGTextureProvider *WQuickCursor::wTextureProvider() const
{
    W_DC(WQuickCursor);

    auto w = qobject_cast<WOutputRenderWindow*>(d->window);
    if (!w || !d->sceneGraphRenderContext() || QThread::currentThread() != d->sceneGraphRenderContext()->thread()) {
        qCWarning(lcWlQuickCursor, "WQuickCursor::textureProvider: can only be queried on the rendering thread of an WOutputRenderWindow");
        return nullptr;
    }

    if (!d->textureProvider) {
        Q_ASSERT(d->cursorImage);
        d->textureProvider = new CursorTextureProvider(w);
        if (d->cursorSurfaceItem && d->cursorSurfaceItem->surface())
            d->textureProvider->setProxy(d->cursorSurfaceItem->wTextureProvider());
        else
            d->textureProvider->setImage(d->cursorImage->image());
    }
    return d->textureProvider;
}

bool WQuickCursor::isTextureProvider() const
{
    return true;
}

bool WQuickCursor::valid() const
{
    W_DC(WQuickCursor);
    if (!d->cursor || !d->cursorImage)
        return false;

    return d->cursorSurfaceItem
           ||  !WGlobal::isInvalidCursor(d->cursorImage->cursor());
}

WCursor *WQuickCursor::cursor() const
{
    W_DC(WQuickCursor);
    return d->cursor;
}

void WQuickCursor::setCursor(WCursor *cursor)
{
    W_D(WQuickCursor);

    if (d->cursor == cursor)
        return;

    if (d->cursor) {
        d->releaseDetachedCursor();
        Q_ASSERT(d->cursor->eventWindow() == window());
        d->cursor->setEventWindow(nullptr);
        bool ok = QObject::disconnect(d->cursor, nullptr, this, nullptr);
        Q_ASSERT(ok);
    }
    d->cursor = cursor;

    if (d->cursor) {
        bool ok = connect(d->cursor, SIGNAL(cursorChanged()), this, SLOT(updateCursor()));
        Q_ASSERT(ok);

        ok = QObject::connect(d->cursor, SIGNAL(requestedCursorShapeChanged()),
                              this, SLOT(updateCursor()));
        Q_ASSERT(ok);
        ok = QObject::connect(d->cursor, SIGNAL(requestedCursorSurfaceChanged()),
                              this, SLOT(updateCursor()));
        Q_ASSERT(ok);
        ok = QObject::connect(d->cursor, &WCursor::visibleChanged,
                              this, [d] {
            if (!d->detachedPresentation && !d->detachedPresentationActive)
                return;
            d->resetDetachedFallback();
            d->updateDetachedCursor();
        });
        Q_ASSERT(ok);
        ok = QObject::connect(d->cursor, &WCursor::positionChanged,
                              this, [d] {
            if (!d->detachedPresentation && !d->detachedPresentationActive)
                return;
            if (d->detachedPresentationActive)
                ++d->detachedCursorMoveOnlyCount;
            d->updateDetachedCursor();
        });
        Q_ASSERT(ok);

        if (isComponentComplete()) {
            Q_ASSERT(!d->cursor->eventWindow()
                     || d->cursor->eventWindow() == window());
            d->cursor->setEventWindow(window());
            d->updateXCursorManager();
            d->updateCursor();
        }
    }

    Q_EMIT cursorChanged();
}

QString WQuickCursor::themeName() const
{
    W_DC(WQuickCursor);
    return d->xcursorThemeName;
}

void WQuickCursor::setThemeName(const QString &name)
{
    W_D(WQuickCursor);

    if (d->xcursorThemeName == name)
        return;
    d->xcursorThemeName = name;
    Q_EMIT themeNameChanged();
    if (isComponentComplete())
        QMetaObject::invokeMethod(this, "updateXCursorManager", Qt::QueuedConnection);
}

QSize WQuickCursor::sourceSize() const
{
    W_DC(WQuickCursor);
    return d->cursorSize;
}

void WQuickCursor::setSourceSize(const QSize &size)
{
    W_D(WQuickCursor);

    if (d->cursorSize == size)
        return;
    d->cursorSize = size;
    Q_EMIT sourceSizeChanged();
    if (isComponentComplete())
        QMetaObject::invokeMethod(this, "updateXCursorManager", Qt::QueuedConnection);
}

QPointF WQuickCursor::hotSpot() const
{
    W_DC(WQuickCursor);

    if (d->cursorSurfaceItem) {
        return QPointF(d->hotSpot) * (width() / d->cursorSurfaceItem->implicitWidth());
    } else if (d->cursorImage && !d->cursorImage->image().isNull()) {
        return QPointF(d->hotSpot) * (width() / d->cursorImage->image().width());
    }

    return {};
}

WOutput *WQuickCursor::output() const
{
    W_DC(WQuickCursor);
    return d->output;
}

void WQuickCursor::setOutput(WOutput *newOutput)
{
    W_D(WQuickCursor);
    if (d->output == newOutput)
        return;

    d->releaseDetachedCursor();
    if (d->outputForceSoftwareCursorConnection)
        QObject::disconnect(d->outputForceSoftwareCursorConnection);
    if (d->outputScaleConnection)
        QObject::disconnect(d->outputScaleConnection);

    if (isVisible()) {
        d->enterOutput(newOutput);
        d->leaveOutput(d->output);
    }

    d->output = newOutput;
    if (d->output) {
        d->outputForceSoftwareCursorConnection =
            QObject::connect(d->output, &WOutput::forceSoftwareCursorChanged,
                             this, [d] {
            d->resetDetachedFallback();
            d->updateDetachedCursor();
        });
        d->outputScaleConnection =
            QObject::connect(d->output, &WOutput::scaleChanged,
                             this, [d] {
            d->resetDetachedFallback();
            d->updateXCursorManager();
            d->updateDetachedCursor();
        });
    }

    Q_EMIT outputChanged();
    d->resetDetachedFallback();
    d->updateDetachedCursor();
}

bool WQuickCursor::detachedPresentation() const
{
    W_DC(WQuickCursor);
    return d->detachedPresentation;
}

void WQuickCursor::setDetachedPresentation(bool enabled)
{
    W_D(WQuickCursor);
    if (d->detachedPresentation == enabled)
        return;

    d->detachedPresentation = enabled;
    if (!enabled)
        d->releaseDetachedCursor();
    else {
        d->resetDetachedFallback();
        d->updateDetachedCursor();
    }

    Q_EMIT detachedPresentationChanged();
}

bool WQuickCursor::detachedPresentationActive() const
{
    W_DC(WQuickCursor);
    return d->detachedPresentationActive;
}

void WQuickCursor::invalidateSceneGraph()
{
    W_D(WQuickCursor);
    delete d->textureProvider;
    d->textureProvider = nullptr;
}

void WQuickCursor::componentComplete()
{
    W_D(WQuickCursor);

    if (d->cursor) {
        Q_ASSERT(!d->cursor->eventWindow()
                 || d->cursor->eventWindow() == window());
        d->cursor->setEventWindow(window());
        d->updateXCursorManager();
        d->updateCursor();
    }

    QQuickItem::componentComplete();
}

void WQuickCursor::itemChange(ItemChange change, const ItemChangeData &data)
{
    W_D(WQuickCursor);
    if (change == ItemSceneChange) {
        if (d->cursor)
            d->cursor->setEventWindow(data.window);
    } else if (change == ItemDevicePixelRatioHasChanged) {
        d->updateXCursorManager();
    } else if (change == ItemVisibleHasChanged) {
        // The visible state is set by compositor(following WOutputCursor::visible property on default)
        if (!d->detachedPresentationActive) {
            if (data.boolValue) {
                d->enterOutput(d->output);
            } else {
                d->leaveOutput(d->output);
            }
        }
    }

    QQuickItem::itemChange(change, data);
}

void WQuickCursor::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    W_DC(WQuickCursor);
    if (d->hotSpot.isNull() && newGeometry.size() != oldGeometry.size())
        Q_EMIT hotSpotChanged();
    QQuickItem::geometryChange(newGeometry, oldGeometry);
}

QSGNode *WQuickCursor::updatePaintNode(QSGNode *node, UpdatePaintNodeData *)
{
    W_DC(WQuickCursor);

    Q_ASSERT(!d->cursorSurfaceItem);
    auto tp = static_cast<CursorTextureProvider*>(wTextureProvider());
    Q_ASSERT(tp);
    Q_ASSERT(QThread::currentThread() == thread());

    if (d->cursorSurfaceItem && d->cursorSurfaceItem->surface()) {
        tp->setProxy(d->cursorSurfaceItem->wTextureProvider());
    } else {
        tp->setImage(d->cursorImage->image());
    }

    QSGTexture *texture = nullptr;
#ifdef ENABLE_VULKAN_RENDER
    if (tp->hasDirectImageTexture()) {
        texture = tp->paintTexture();
        if (!texture) {
            delete node;
            return nullptr;
        }
    } else
#endif
    if (!tp->buffer) {
        delete node;
        return nullptr;
    }
    else {
        texture = tp->WSGTextureProvider::texture();
    }
#ifdef ENABLE_VULKAN_RENDER
    if (!texture && tp->hasVulkanRenderer()) {
        delete node;
        return nullptr;
    }
#endif

    auto imageNode = static_cast<QSGImageNode*>(node);
    if (!imageNode)
        imageNode = window()->createImageNode();

    imageNode->setTexture(texture);
    imageNode->setOwnsTexture(false);
    imageNode->setSourceRect(QRectF(QPointF(0, 0), texture->textureSize()));
    imageNode->setRect(QRectF(QPointF(0, 0), QSizeF(width(), height())));
    imageNode->setFiltering(smooth() ? QSGTexture::Linear : QSGTexture::Nearest);
    imageNode->setMipmapFiltering(QSGTexture::None);
    imageNode->setAnisotropyLevel(antialiasing() ? QSGTexture::Anisotropy4x : QSGTexture::AnisotropyNone);

    return imageNode;
}

void WQuickCursor::releaseResources()
{
    W_D(WQuickCursor);

    d->invalidate();

    // Force to update the contents, avoid to render the invalid textures
    // Only mark dirty if we have a valid window to avoid crashes during window destruction
    if (window()) {
        QQuickItemPrivate::get(this)->dirty(QQuickItemPrivate::Content);
    }
}

WAYLIB_SERVER_END_NAMESPACE

#include "moc_wquickcursor.cpp"
