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

#include <rhi/qrhi.h>
#include <private/qsgplaintexture_p.h>

WAYLIB_SERVER_BEGIN_NAMESPACE

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

    void cleanTexture() {
        const bool releaseImportedWrapper =
            nativeCleanup.type != WRenderHelper::NativeTextureCleanup::Type::None;
        QRhiTexture *importedWrapper = releaseImportedWrapper ? rhiTexture : nullptr;

        // QSGPlainTexture does not own the QRhiTexture wrapper in this provider.
        // For Vulkan+GL imports, QRhiTexture::createFrom() also does not own the
        // native GL object, so drop Qt's reference, delete the wrapper, then
        // release the native import objects created by WRenderHelper.
        if (rhiTexture) {
            qtTexture.setTexture(nullptr);
            rhiTexture = nullptr;
        }
        delete importedWrapper;
        WRenderHelper::releaseNativeTexture(&nativeCleanup);

        if (ownsTexture && texture)
            delete texture;
        texture = nullptr;
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
            qCWarning(lcWlQtQuickTexture) << "Failed to make texture:" << texture
                                        << ", width height:" << texture->handle()->width
                                        << texture->handle()->height;
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

void WSGTextureProvider::setBuffer(qw_buffer *buffer)
{
    if (buffer == qwBuffer()) {
        // The buffer object is not changed, but maybe the buffer's content is changed.
        // So should emit textureChanged() signal too.
        if (buffer)
            Q_EMIT textureChanged();
        return;
    }

    W_D(WSGTextureProvider);
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
            qCWarning(lcWlQtQuickTexture) << "Failed to update texture from buffer:" << buffer
                                        << ", width height:" << buffer->handle()->width
                                        << buffer->handle()->height
                                        << ", n_locks:" << buffer->handle()->n_locks;
        } else {
            d->updateRhiTexture();
        }
    }

    Q_EMIT textureChanged();
}

void WSGTextureProvider::setTexture(qw_texture *texture, qw_buffer *srcBuffer)
{
    W_D(WSGTextureProvider);
    d->cleanTexture();
    d->texture = texture;
    d->buffer = srcBuffer;
    d->ownsTexture = false;
    if (texture)
        d->updateRhiTexture();

    Q_EMIT textureChanged();
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
    return d->texture ? const_cast<QSGPlainTexture*>(&d->qtTexture) : nullptr;
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
