// Copyright (C) 2026 UnionTech Software Technology Co., Ltd.
// SPDX-License-Identifier: Apache-2.0 OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#pragma once

#include "wrenderhelper.h"
#include "wglobal.h"

#include <QPointer>
#include <QSGRenderNode>
#include <rhi/qrhi.h>

#include <cstdint>
#include <memory>

QT_BEGIN_NAMESPACE
class QQuickWindow;
QT_END_NAMESPACE

struct wlr_surface;

QW_BEGIN_NAMESPACE
class qw_buffer;
QW_END_NAMESPACE

WAYLIB_SERVER_BEGIN_NAMESPACE

class Q_DECL_HIDDEN WVulkanSurfaceRenderNode : public QSGRenderNode
{
public:
    struct NativeResources;

    explicit WVulkanSurfaceRenderNode(QQuickWindow *window);
    ~WVulkanSurfaceRenderNode() override;

    bool setBuffer(QW_NAMESPACE::qw_buffer *buffer, wlr_surface *surface, uint32_t surfaceCommitSeq);
    void setGeometry(const QRectF &targetRect, const QRectF &sourceRect, qreal devicePixelRatio);
    void setSmooth(bool smooth);
    bool isReady() const;

    void prepare() override;
    void render(const RenderState *state) override;
    void releaseResources() override;
    RenderingFlags flags() const override;
    StateFlags changedStates() const override;
    QRectF rect() const override;

private:
    bool ensureImported(wlr_surface *surface);
    bool ensureNativeResources();
    void releaseImport();
    void resetNativeImageResources();
    void resetNativeResources();
    void resetAll();

    QPointer<QQuickWindow> m_window;
    QRhi *m_rhi = nullptr;
    QW_NAMESPACE::qw_buffer *m_buffer = nullptr;
    wlr_surface *m_surface = nullptr;
    uint32_t m_surfaceCommitSeq = 0;
    WRenderHelper::ImportedVulkanTexture m_importedTexture;

    QRectF m_targetRect;
    QRectF m_sourceRect;
    qreal m_devicePixelRatio = 1.0;
    bool m_smooth = true;
    bool m_importDirty = true;
    bool m_verticesDirty = true;
    bool m_samplerDirty = true;
    bool m_importFailed = false;
    bool m_loggedPrepareState = false;
    bool m_loggedRenderState = false;

    std::unique_ptr<NativeResources> m_native;
};

WAYLIB_SERVER_END_NAMESPACE
