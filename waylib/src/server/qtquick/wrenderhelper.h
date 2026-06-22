// Copyright (C) 2023 JiDe Zhang <zhangjide@deepin.org>.
// SPDX-License-Identifier: Apache-2.0 OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#pragma once

#include <wglobal.h>
#include <qwglobal.h>

#include <QObject>
#include <QQuickRenderTarget>
#include <QSGRendererInterface>

QT_BEGIN_NAMESPACE
class QQuickRenderControl;
class QRhiTexture;
class QSGTexture;
class QSGPlainTexture;
class QRhi;
QT_END_NAMESPACE

QW_BEGIN_NAMESPACE
class qw_renderer;
class qw_allocator;
class qw_backend;
class qw_buffer;
class qw_texture;
QW_END_NAMESPACE

struct wlr_buffer;

WAYLIB_SERVER_BEGIN_NAMESPACE

class WRenderHelperPrivate;
class WAYLIB_SERVER_EXPORT WRenderHelper : public QObject, public WObject
{
    Q_OBJECT
    Q_PROPERTY(QSize size READ size WRITE setSize NOTIFY sizeChanged FINAL)
    W_DECLARE_PRIVATE(WRenderHelper)

public:
    explicit WRenderHelper(QW_NAMESPACE::qw_renderer *renderer, QObject *parent = nullptr);

    QSize size() const;
    void setSize(const QSize &size);

    static QSGRendererInterface::GraphicsApi getGraphicsApi(QQuickRenderControl *rc);
    static QSGRendererInterface::GraphicsApi getGraphicsApi();

    static QW_NAMESPACE::qw_buffer *toBuffer(QW_NAMESPACE::qw_renderer *renderer, QSGTexture *texture, QSGRendererInterface::GraphicsApi api);

    QQuickRenderTarget acquireRenderTarget(QQuickRenderControl *rc, QW_NAMESPACE::qw_buffer *buffer);
    std::pair<QW_NAMESPACE::qw_buffer*, QQuickRenderTarget> lastRenderTarget() const;
    static QW_NAMESPACE::qw_renderer *createRenderer(QW_NAMESPACE::qw_backend *backend);
    static QW_NAMESPACE::qw_renderer *createRenderer(QW_NAMESPACE::qw_backend *backend, QSGRendererInterface::GraphicsApi api);

    static void setupRendererBackend(QW_NAMESPACE::qw_backend *testBackend = nullptr);
    static QSGRendererInterface::GraphicsApi probe(QW_NAMESPACE::qw_backend *testBackend, const QList<QSGRendererInterface::GraphicsApi> &apiList);

    struct NativeTextureCleanup {
        enum class Type {
            None,
            OpenGLTexture,
        };

        Type type = Type::None;
        quint64 texture = 0;
        void *eglImage = nullptr;
        void *eglDisplay = nullptr;
    };

    static void releaseNativeTexture(NativeTextureCleanup *cleanup);

    static bool makeTexture(QRhi *rhi, QW_NAMESPACE::qw_texture *handle,
                            QSGPlainTexture *texture, QW_NAMESPACE::qw_buffer *buffer = nullptr,
                            NativeTextureCleanup *nativeCleanup = nullptr);

    // Submit a sync-only Vulkan command and import the exported sync_file into
    // the dmabuf backing \p buffer. This helper does not record an image layout
    // barrier: Qt RHI keeps its render target layout tracking internally, and
    // the current Vulkan+GL path renders through EGL dmabuf import instead of
    // exposing Qt-rendered VkImages directly to KMS. The helper is retained for
    // the old explicit-sync experiment and is not used by the current render
    // path.
    static void transitionVkImageToGeneral(QRhi *rhi, QRhiTexture *texture,
                                           QW_NAMESPACE::qw_buffer *buffer);

    struct TextureEntry {
        wlr_buffer *buffer;
        QW_NAMESPACE::qw_texture *texture;
        QRhiTexture *rhiTexture;
        NativeTextureCleanup nativeCleanup;
    };
    static TextureEntry newTexture(QW_NAMESPACE::qw_allocator *allocator,
                                   QW_NAMESPACE::qw_renderer *renderer,
                                   uint32_t drmFormat, uint64_t drmModifier,
                                   QRhi *rhi, const QSize &size,
                                   int rhiFormat, int rhiFlags);
    static TextureEntry newTextureLike(QW_NAMESPACE::qw_allocator *allocator,
                                       QW_NAMESPACE::qw_renderer *renderer,
                                       QRhiTexture *texture, QRhi *rhi, int rhiFlags);
    static QW_NAMESPACE::qw_buffer *lookupBuffer(const QRhiRenderTarget *rt);
    static QW_NAMESPACE::qw_buffer *lookupBuffer(const QRhiTexture *texture);

Q_SIGNALS:
    void sizeChanged();

private:
    W_PRIVATE_SLOT(void onBufferDestroy())
};

WAYLIB_SERVER_END_NAMESPACE
