// Copyright (C) 2024-2026 UnionTech Software Technology Co., Ltd.
// SPDX-License-Identifier: Apache-2.0 OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "wsgtextureprovider.h"
#include "woutputrenderwindow.h"
#include "wrenderhelper.h"
#include "private/wglobal_p.h"
#include "wayliblogging.h"

#include <qwtexture.h>
#include <qwbuffer.h>
#include <qwrenderer.h>

#include <QByteArray>
#include <rhi/qrhi.h>
#include <private/qsgplaintexture_p.h>

#ifdef ENABLE_VULKAN_RENDER
extern "C" {
#include <wlr/render/dmabuf.h>
}
#endif

WAYLIB_SERVER_BEGIN_NAMESPACE

static quintptr pointerAddress(const void *ptr)
{
    return reinterpret_cast<quintptr>(ptr);
}

static QSize bufferSize(qw_buffer *buffer)
{
    return buffer && buffer->handle()
        ? QSize(buffer->handle()->width, buffer->handle()->height)
        : QSize();
}

static int bufferLocks(qw_buffer *buffer)
{
    return buffer && buffer->handle() ? buffer->handle()->n_locks : -1;
}

#ifdef ENABLE_VULKAN_RENDER
static bool envFlagExplicitlyDisabled(const char *name)
{
    const QByteArray value = qgetenv(name).trimmed().toLower();
    return value == "0" || value == "false" || value == "no" || value == "off";
}

static bool bufferHasDmabuf(qw_buffer *buffer)
{
    if (!buffer || !buffer->handle())
        return false;

    wlr_dmabuf_attributes dmabuf = {};
    return wlr_buffer_get_dmabuf(buffer->handle(), &dmabuf);
}

static bool directDmabufImportEnabled()
{
    static const bool enabled = !envFlagExplicitlyDisabled("WAYLIB_VK_DIRECT_DMABUF")
        && !envFlagExplicitlyDisabled("TREELAND_VK_DIRECT_DMABUF")
        && !envFlagExplicitlyDisabled("WAYLIB_VK_DIRECT_CLIENT_DMABUF")
        && !envFlagExplicitlyDisabled("TREELAND_VK_DIRECT_CLIENT_DMABUF");
    return enabled;
}
#endif

class Q_DECL_HIDDEN WSGTextureProviderPrivate : public WObjectPrivate
{
public:
    WSGTextureProviderPrivate(WSGTextureProvider *qq, WOutputRenderWindow *window)
        : WObjectPrivate(qq)
        , window(window)
    {
        qtTexture.setOwnsTexture(false);
        qtTexture.setFiltering(smooth ? QSGTexture::Linear
                                      : QSGTexture::Nearest);
        qtTexture.setMipmapFiltering(smooth ? QSGTexture::Linear
                                            : QSGTexture::Nearest);
    }

    ~WSGTextureProviderPrivate() {
        cleanTexture();
    }

    bool isVulkanRenderer() const {
#ifdef ENABLE_VULKAN_RENDER
        return window && window->renderer()
            && wlr_renderer_is_vk(window->renderer()->handle());
#else
        return false;
#endif
    }

    static void releaseDetachedTexture(qw_texture *texture, bool ownsTexture,
                                       QRhiTexture *rhiTexture,
                                       WRenderHelper::NativeTextureCleanup nativeCleanup)
    {
        if (nativeCleanup.type != WRenderHelper::NativeTextureCleanup::Type::None)
            delete rhiTexture;
        WRenderHelper::releaseNativeTexture(&nativeCleanup);

        if (ownsTexture && texture)
            delete texture;
    }

    void cleanTexture() {
        const bool releaseImportedWrapper =
            nativeCleanup.type != WRenderHelper::NativeTextureCleanup::Type::None;
        QRhiTexture *importedWrapper = releaseImportedWrapper ? rhiTexture : nullptr;

        // Native imports are non-owned by QSGPlainTexture and are released here
        // together with their import objects. QImage fallbacks switch the
        // QSGPlainTexture back to owned mode, so setTexture(nullptr) deletes
        // those Qt-created textures normally.
        if (rhiTexture) {
            qtTexture.setTexture(nullptr);
            rhiTexture = nullptr;
        }
        delete importedWrapper;
        WRenderHelper::releaseNativeTexture(&nativeCleanup);

        if (ownsTexture && texture)
            delete texture;
        texture = nullptr;
        ownsTexture = false;
        buffer = nullptr;
    }

    bool replaceTexture(qw_texture *newTexture, bool newOwnsTexture, qw_buffer *newBuffer,
                        bool allowBufferDirectImport = true, wlr_surface *surface = nullptr) {
        Q_ASSERT(newTexture);

        WRenderHelper::NativeTextureCleanup newCleanup;
        if (!WRenderHelper::makeTexture(window->rhi(), newTexture, &qtTexture,
                                        newBuffer, &newCleanup, allowBufferDirectImport,
                                        surface)) {
            WRenderHelper::releaseNativeTexture(&newCleanup);
#ifdef ENABLE_VULKAN_RENDER
            if (newBuffer && !allowBufferDirectImport && window && window->rhi()
                && window->rhi()->backend() == QRhi::Vulkan) {
                const bool hasExistingTexture = hasTexture();
                qCDebug(lcWlQtQuickTexture)
                    << "Vulkan renderer kept existing texture after unsafe native image wrapper was refused"
                    << "texture" << pointerAddress(newTexture)
                    << "buffer" << pointerAddress(newBuffer)
                    << "size" << newTexture->handle()->width << "x" << newTexture->handle()->height
                    << "keptExistingTexture" << hasExistingTexture;
                if (newOwnsTexture && newTexture != texture)
                    delete newTexture;
                return hasExistingTexture;
            }
#endif
            qCWarning(lcWlQtQuickTexture)
                << "Failed to make texture"
                << "texture" << pointerAddress(newTexture)
                << "size" << newTexture->handle()->width << "x" << newTexture->handle()->height;
            if (newOwnsTexture)
                delete newTexture;
            return false;
        }

        auto oldTexture = texture;
        const bool oldOwnsTexture = ownsTexture;
        auto oldRhiTexture = rhiTexture;
        auto oldNativeCleanup = nativeCleanup;

        texture = newTexture;
        ownsTexture = newOwnsTexture;
        buffer = newBuffer;
        rhiTexture = qtTexture.rhiTexture();
        nativeCleanup = newCleanup;

        releaseDetachedTexture(oldTexture == newTexture ? nullptr : oldTexture,
                               oldTexture == newTexture ? false : oldOwnsTexture,
                               oldRhiTexture,
                               oldNativeCleanup);
        return true;
    }

    bool replaceBufferWithDirectDmabufTexture(qw_buffer *newBuffer, wlr_surface *surface)
    {
#ifdef ENABLE_VULKAN_RENDER
        if (!newBuffer || !window || !window->rhi())
            return false;

        WRenderHelper::NativeTextureCleanup newCleanup;
        const auto backend = window->rhi()->backend();
        bool imported = false;
        const char *backendName = "unknown RHI";
        if (backend == QRhi::OpenGLES2) {
            backendName = "OpenGL RHI";
            imported = WRenderHelper::makeOpenGLTextureFromBuffer(window->rhi(), newBuffer,
                                                                  &qtTexture, &newCleanup);
        } else if (backend == QRhi::Vulkan) {
            backendName = "Vulkan RHI";
            imported = WRenderHelper::makeVulkanTextureFromBuffer(window->rhi(), newBuffer,
                                                                  &qtTexture, &newCleanup,
                                                                  surface);
        }

        if (!imported) {
            return false;
        }

        auto oldTexture = texture;
        const bool oldOwnsTexture = ownsTexture;
        auto oldRhiTexture = rhiTexture;
        auto oldNativeCleanup = nativeCleanup;

        texture = nullptr;
        ownsTexture = false;
        buffer = newBuffer;
        rhiTexture = qtTexture.rhiTexture();
        nativeCleanup = newCleanup;

        qCDebug(lcWlQtQuickTexture)
            << "Vulkan renderer dmabuf texture import path"
            << "qtBackend" << backendName
            << "buffer" << pointerAddress(newBuffer)
            << "size" << bufferSize(newBuffer)
            << "locks" << bufferLocks(newBuffer)
            << "alpha" << qtTexture.hasAlphaChannel();

        releaseDetachedTexture(oldTexture, oldOwnsTexture, oldRhiTexture, oldNativeCleanup);
        return true;
#else
        Q_UNUSED(newBuffer);
        return false;
#endif
    }

    bool hasTexture() const
    {
        return texture || rhiTexture;
    }

    void updateRhiTexture() {
        Q_ASSERT(texture);
        // NOTE: We cannot cache by wlr_texture* alone: wlroots may reuse the
        // same texture object (via wlr_client_buffer_apply_damage) while updating
        // its contents. Callers should re-run makeTexture for real buffer updates,
        // but reuse the provider texture for pure scene graph animation frames.
        WRenderHelper::NativeTextureCleanup newCleanup;
        bool ok = WRenderHelper::makeTexture(window->rhi(), texture, &qtTexture,
                                             buffer, &newCleanup);
        if (Q_UNLIKELY(!ok)) {
            WRenderHelper::releaseNativeTexture(&newCleanup);
            qCWarning(lcWlQtQuickTexture)
                << "Failed to make texture"
                << "texture" << pointerAddress(texture)
                << "size" << texture->handle()->width << "x" << texture->handle()->height;
            return;
        }

        rhiTexture = qtTexture.rhiTexture();
        nativeCleanup = newCleanup;
    }

    W_DECLARE_PUBLIC(WSGTextureProvider)

    QPointer<WOutputRenderWindow> window;

    // wlroots resources
    qw_texture *texture = nullptr;
    bool ownsTexture = false;
    qw_buffer *buffer = nullptr;

    // qt resources
    QSGPlainTexture qtTexture;
    QRhiTexture *rhiTexture = nullptr;
    WRenderHelper::NativeTextureCleanup nativeCleanup;
    bool smooth = true;
    bool directBufferImportAllowed = false;
};

WSGTextureProvider::WSGTextureProvider(WOutputRenderWindow *window)
    : WObject(*new WSGTextureProviderPrivate(this, window))
{

}

WOutputRenderWindow *WSGTextureProvider::window() const
{
    W_D(const WSGTextureProvider);
    return d->window;
}

bool WSGTextureProvider::prefersDirectBufferImport(WOutputRenderWindow *window)
{
#ifdef ENABLE_VULKAN_RENDER
    if (!directDmabufImportEnabled()
        || !window
        || !window->renderer()
        || !wlr_renderer_is_vk(window->renderer()->handle())) {
        return false;
    }

    const auto api = WRenderHelper::getGraphicsApi();
    if (api == QSGRendererInterface::OpenGL)
        return true;

    // Keep client surfaces inside the Qt Quick scene graph. The Vulkan path
    // first tries a conservative dmabuf -> QRhiTexture import and falls back
    // to synchronized upload/readback when import is not safe.
    if (api == QSGRendererInterface::Vulkan)
        return true;

    return false;
#else
    Q_UNUSED(window);
#endif
    return false;
}

bool WSGTextureProvider::directBufferImportAllowed() const
{
    W_DC(WSGTextureProvider);
    return d->directBufferImportAllowed;
}

void WSGTextureProvider::setDirectBufferImportAllowed(bool allowed)
{
    W_D(WSGTextureProvider);
    d->directBufferImportAllowed = allowed;
}

bool WSGTextureProvider::setBuffer(qw_buffer *buffer)
{
    return setBuffer(buffer, nullptr);
}

bool WSGTextureProvider::setBuffer(qw_buffer *buffer, wlr_surface *surface)
{
    W_D(WSGTextureProvider);

    const bool sameBuffer = buffer == d->buffer;
#ifdef ENABLE_VULKAN_RENDER
    const bool allowDirectBufferImport =
        d->directBufferImportAllowed && prefersDirectBufferImport(d->window);
    const bool needsSameBufferUpload =
        buffer
        && sameBuffer
        && !allowDirectBufferImport
        && d->isVulkanRenderer()
        && WRenderHelper::getGraphicsApi() == QSGRendererInterface::Vulkan;
#else
    const bool allowDirectBufferImport = false;
    const bool needsSameBufferUpload = false;
#endif

    if (sameBuffer && !allowDirectBufferImport && !needsSameBufferUpload) {
        // The buffer object is not changed, but maybe the buffer's content is changed.
        // So should emit textureChanged() signal too.
        if (buffer)
            Q_EMIT textureChanged();
        return true;
    }

    if (d->isVulkanRenderer()) {
        qCDebug(lcWlQtQuickTexture)
            << "Vulkan texture provider setBuffer"
            << "buffer" << pointerAddress(buffer)
            << "sameBuffer" << sameBuffer
            << "allowDirectBufferImport" << allowDirectBufferImport
            << "directBufferImportAllowed" << d->directBufferImportAllowed
            << "hasExistingTexture" << d->hasTexture()
            << "currentBuffer" << pointerAddress(d->buffer)
            << "size" << bufferSize(buffer);

        if (!buffer) {
            d->cleanTexture();
            Q_EMIT textureChanged();
            return true;
        }

        const bool directImportEligible = allowDirectBufferImport && bufferHasDmabuf(buffer);
        bool triedDirectBufferImport = false;
        if (directImportEligible) {
            triedDirectBufferImport = true;
            if (d->replaceBufferWithDirectDmabufTexture(buffer, surface)) {
                Q_EMIT textureChanged();
                return true;
            }

            qCDebug(lcWlQtQuickTexture)
                << "Vulkan renderer dmabuf texture import path unavailable,"
                   " falling back to synchronized wlroots texture import"
                << "buffer" << pointerAddress(buffer)
                << "sameBuffer" << sameBuffer
                << "size" << bufferSize(buffer)
                << "locks" << bufferLocks(buffer);
        }

        bool ownsTexture = false;
        qw_texture *texture = nullptr;
        if (auto clientBuffer = qw_client_buffer::get(*buffer)) {
            texture = qw_texture::from(clientBuffer->handle()->texture);
        } else {
            texture = qw_texture::from_buffer(*d->window->renderer(), *buffer);
            ownsTexture = true;
        }

        if (Q_UNLIKELY(!texture)) {
            qCWarning(lcWlQtQuickTexture)
                << "Failed to update texture from buffer"
                << "buffer" << pointerAddress(buffer)
                << "size" << bufferSize(buffer)
                << "locks" << bufferLocks(buffer);
            return false;
        }

        if (d->replaceTexture(texture, ownsTexture, buffer,
                              directImportEligible && !triedDirectBufferImport,
                              surface)) {
            Q_EMIT textureChanged();
            return true;
        }
        return false;
    }

    d->cleanTexture();
    d->buffer = buffer;

    if (buffer) {
        Q_ASSERT(d->window);
        if (auto clientBuffer = qw_client_buffer::get(*buffer)) {
            // Acquire texture from client buffer. wlroots already generate texture for us if this is a client buffer.
            // By the way, there is something wrong with getting texture from a client buffer using wlr_texture_from_buffer,
            // See: https://gitlab.freedesktop.org/wlroots/wlroots/-/issues/3897
            // Possible patch:  https://gitlab.freedesktop.org/wlroots/wlroots/-/merge_requests/4889
            d->texture = qw_texture::from(clientBuffer->handle()->texture);
            d->ownsTexture = false;
        } else {
            d->texture = qw_texture::from_buffer(*d->window->renderer(), *buffer);
            d->ownsTexture = true;
        }
        if (Q_UNLIKELY(!d->texture)) {
            qCWarning(lcWlQtQuickTexture)
                << "Failed to update texture from buffer"
                << "buffer" << pointerAddress(buffer)
                << "size" << bufferSize(buffer)
                << "locks" << bufferLocks(buffer);
        } else {
            d->updateRhiTexture();
        }
    }

    Q_EMIT textureChanged();
    return true;
}

bool WSGTextureProvider::setTexture(qw_texture *texture, qw_buffer *srcBuffer)
{
    return setTexture(texture, srcBuffer, nullptr);
}

bool WSGTextureProvider::setTexture(qw_texture *texture, qw_buffer *srcBuffer,
                                    wlr_surface *surface)
{
    W_D(WSGTextureProvider);
    if (d->isVulkanRenderer()) {
        qCDebug(lcWlQtQuickTexture)
            << "Vulkan texture provider setTexture"
            << "texture" << pointerAddress(texture)
            << "sourceBuffer" << pointerAddress(srcBuffer)
            << "directBufferImportAllowed" << d->directBufferImportAllowed
            << "hasExistingTexture" << d->hasTexture()
            << "currentBuffer" << pointerAddress(d->buffer)
            << "sourceSize" << bufferSize(srcBuffer);

        if (!texture) {
            d->cleanTexture();
            Q_EMIT textureChanged();
            return true;
        }

        const bool allowDirectBufferImport =
            d->directBufferImportAllowed && prefersDirectBufferImport(d->window);
        const bool directImportEligible =
            allowDirectBufferImport && bufferHasDmabuf(srcBuffer);
        bool triedDirectBufferImport = false;
        if (srcBuffer && directImportEligible) {
            triedDirectBufferImport = true;
            if (d->replaceBufferWithDirectDmabufTexture(srcBuffer, surface)) {
                Q_EMIT textureChanged();
                return true;
            }

            qCDebug(lcWlQtQuickTexture)
                << "Vulkan renderer dmabuf texture import path unavailable for provided wlroots texture,"
                   " falling back to wlroots texture wrapper"
                << "buffer" << pointerAddress(srcBuffer)
                << "texture" << pointerAddress(texture);
        }

        if (d->replaceTexture(texture, false, srcBuffer,
                              directImportEligible && !triedDirectBufferImport,
                              surface)) {
            Q_EMIT textureChanged();
            return true;
        }
        return false;
    }

    d->cleanTexture();
    d->texture = texture;
    d->buffer = srcBuffer;
    d->ownsTexture = false;
    if (texture)
        d->updateRhiTexture();

    Q_EMIT textureChanged();
    return true;
}

void WSGTextureProvider::invalidate()
{
    W_D(WSGTextureProvider);
    d->cleanTexture();
    d->window = nullptr;

    Q_EMIT textureChanged();
}

QSGTexture *WSGTextureProvider::texture() const
{
    W_DC(WSGTextureProvider);
    return d->hasTexture() ? const_cast<QSGPlainTexture*>(&d->qtTexture) : nullptr;
}

qw_texture *WSGTextureProvider::qwTexture() const
{
    W_DC(WSGTextureProvider);
    return d->texture;
}

qw_buffer *WSGTextureProvider::qwBuffer() const
{
    W_DC(WSGTextureProvider);
    return d->buffer;
}

bool WSGTextureProvider::smooth() const
{
    W_DC(WSGTextureProvider);
    return d->smooth;
}

void WSGTextureProvider::setSmooth(bool newSmooth)
{
    W_D(WSGTextureProvider);
    if (d->smooth == newSmooth)
        return;
    d->smooth = newSmooth;
    d->qtTexture.setFiltering(newSmooth ? QSGTexture::Linear
                                        : QSGTexture::Nearest);
    d->qtTexture.setMipmapFiltering(newSmooth ? QSGTexture::Linear
                                              : QSGTexture::Nearest);

    Q_EMIT smoothChanged();
}

WAYLIB_SERVER_END_NAMESPACE
