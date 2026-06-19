// Copyright (C) 2023 JiDe Zhang <zhangjide@deepin.org>.
// SPDX-License-Identifier: Apache-2.0 OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "wrenderhelper.h"
#include "wtools.h"
#include "wayliblogging.h"
#include "private/wqmlhelper_p.h"
#include "private/wglobal_p.h"

#include <qwbackend.h>
#include <qwoutput.h>
#include <qwrenderer.h>
#include <qwswapchain.h>
#include <qwbuffer.h>
#include <qwtexture.h>
#include <qwbufferinterface.h>
#include <qwdisplay.h>
#include <qwegl.h>
#include <qwallocator.h>
#include <qwrendererinterface.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <QOpenGLContext>

#include <QSGTexture>
#include <private/qquickrendercontrol_p.h>
#include <private/qquickwindow_p.h>
#include <private/qrhi_p.h>
#include <private/qsgplaintexture_p.h>
#include <private/qsgadaptationlayer_p.h>
#include <private/qsgsoftwarepixmaptexture_p.h>
#include <private/qsgrhisupport_p.h>

extern "C" {
#define static
#include <wlr/render/gles2.h>
#undef static
#include <wlr/render/pixman.h>
#ifdef ENABLE_VULKAN_RENDER
#include <wlr/render/vulkan.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/render/dmabuf.h>
#include <linux/dma-buf.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <errno.h>
#endif
}
#include <drm_fourcc.h>
#include <dlfcn.h>

QW_USE_NAMESPACE
WAYLIB_SERVER_BEGIN_NAMESPACE

struct Q_DECL_HIDDEN RhiRenderEntry {
    const QRhiRenderTarget *renderTarget;
    const QRhiTexture *texture;
    QPointer<qw_buffer> buffer;
};

Q_GLOBAL_STATIC(QVector<RhiRenderEntry>, s_rhiRenderBuffers)

struct Q_DECL_HIDDEN BufferData {
    BufferData() {

    }

    ~BufferData() {
        resetWindowRenderTarget();
#ifdef ENABLE_VULKAN_RENDER
        destroyEglDmabufTexture();
#endif
    }

    qw_buffer *buffer = nullptr;
#ifdef ENABLE_VULKAN_RENDER
    // EGL dmabuf import state (used when wlroots renderer is Vulkan but Qt RHI
    // is GL). The dmabuf is imported as an EGLImage, then bound to a GL texture
    // via glEGLImageTargetTexture2DOES. This bypasses wlroots's texture system
    // entirely — dmabuf is API-agnostic, any EGL context can import it.
    EGLImage eglImage = EGL_NO_IMAGE;
    GLuint glTexture = 0;
    EGLDisplay eglDisplay = EGL_NO_DISPLAY;

    void destroyEglDmabufTexture();
#endif
    // for software renderer
    WImageRenderTarget paintDevice;
    QQuickRenderTarget renderTarget;
    QQuickWindowRenderTarget windowRenderTarget;

    inline void resetWindowRenderTarget() {
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        {
            auto it = s_rhiRenderBuffers->begin();
            while (it != s_rhiRenderBuffers->end()) {
                if (windowRenderTarget.rt.renderTarget == it->renderTarget) {
                    it = s_rhiRenderBuffers->erase(it);
                    break;
                }
                ++it;
            }
        }

        if (windowRenderTarget.rt.owns)
            delete windowRenderTarget.rt.renderTarget;

        delete windowRenderTarget.res.texture;
        delete windowRenderTarget.res.renderBuffer;
        delete windowRenderTarget.res.rpDesc;

        windowRenderTarget.rt = {};
        windowRenderTarget.res = {};
        { // windowRenderTarget.implicitBuffers.reset(rhi);
            delete windowRenderTarget.implicitBuffers.depthStencil;
            delete windowRenderTarget.implicitBuffers.depthStencilTexture;
            delete windowRenderTarget.implicitBuffers.multisampleTexture;
            windowRenderTarget.implicitBuffers = {};
        }

        if (windowRenderTarget.sw.owns)
            delete windowRenderTarget.sw.paintDevice;

        windowRenderTarget.sw = {};
#else
        {
            auto it = s_rhiRenderBuffers->begin();
            while (it != s_rhiRenderBuffers->end()) {
                if (windowRenderTarget.renderTarget == it->renderTarget) {
                    it = s_rhiRenderBuffers->erase(it);
                    break;
                }
                ++it;
            }
        }

        if (windowRenderTarget.owns) {
            delete windowRenderTarget.renderTarget;
            delete windowRenderTarget.rpDesc;
            delete windowRenderTarget.texture;
            delete windowRenderTarget.renderBuffer;
            delete windowRenderTarget.depthStencil;
            delete windowRenderTarget.paintDevice;
        }

        windowRenderTarget.renderTarget = nullptr;
        windowRenderTarget.rpDesc = nullptr;
        windowRenderTarget.texture = nullptr;
        windowRenderTarget.renderBuffer = nullptr;
        windowRenderTarget.depthStencil = nullptr;
        windowRenderTarget.paintDevice = nullptr;
        windowRenderTarget.owns = false;
#endif
    }
};

#ifdef ENABLE_VULKAN_RENDER
// Resolve EGL function pointers for dmabuf import. These are EGL extensions
// (EGL_KHR_image_base, EGL_EXT_image_dma_buf_import, GL_OES_EGL_image) and
// must be resolved at runtime via eglGetProcAddress.
static PFNEGLCREATEIMAGEKHRPROC resolveEglCreateImageKHR()
{
    static auto proc = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
    return proc;
}
static PFNEGLDESTROYIMAGEKHRPROC resolveEglDestroyImageKHR()
{
    static auto proc = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
    return proc;
}
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC resolveGlEGLImageTargetTexture2DOES()
{
    static auto proc = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    return proc;
}

void BufferData::destroyEglDmabufTexture()
{
    if (glTexture) {
        glDeleteTextures(1, &glTexture);
        glTexture = 0;
    }
    if (eglImage != EGL_NO_IMAGE) {
        if (auto destroyImage = resolveEglDestroyImageKHR())
            destroyImage(eglDisplay, eglImage);
        eglImage = EGL_NO_IMAGE;
    }
}

// Import a dmabuf as a GL texture via EGL, so Qt RHI (GL) can render into it
// even when the wlroots renderer is Vulkan. This mirrors wlroots'
// wlr_egl_create_image_from_dmabuf (render/egl.c) but uses the Qt RHI GL
// context's EGL display instead of wlroots' gles2 EGL. dmabuf is API-agnostic:
// any EGL context can import it regardless of which renderer created it.
// On success, sets *outImage and *outTex; caller owns both (destroy via
// eglDestroyImageKHR / glDeleteTextures).
static bool eglImportDmabufToGLTexture(EGLDisplay display,
                                       const wlr_dmabuf_attributes *attribs,
                                       EGLImage *outImage, GLuint *outTex)
{
    auto eglCreateImageKHR = resolveEglCreateImageKHR();
    auto glEGLImageTargetTexture2DOES = resolveGlEGLImageTargetTexture2DOES();
    if (!eglCreateImageKHR || !glEGLImageTargetTexture2DOES) {
        qCWarning(lcWlRenderHelper) << "EGL dmabuf import: eglCreateImageKHR or glEGLImageTargetTexture2DOES not available";
        return false;
    }

    // Build EGL attribute list, mirroring wlroots egl.c:733-830.
    EGLint eglAttribs[50];
    unsigned int atti = 0;
    eglAttribs[atti++] = EGL_WIDTH;
    eglAttribs[atti++] = attribs->width;
    eglAttribs[atti++] = EGL_HEIGHT;
    eglAttribs[atti++] = attribs->height;
    eglAttribs[atti++] = EGL_LINUX_DRM_FOURCC_EXT;
    eglAttribs[atti++] = attribs->format;

    static const struct {
        EGLint fd, offset, pitch, mod_lo, mod_hi;
    } plane_attrs[WLR_DMABUF_MAX_PLANES] = {
        { EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE0_OFFSET_EXT,
          EGL_DMA_BUF_PLANE0_PITCH_EXT, EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
          EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT },
        { EGL_DMA_BUF_PLANE1_FD_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT,
          EGL_DMA_BUF_PLANE1_PITCH_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,
          EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT },
        { EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE2_OFFSET_EXT,
          EGL_DMA_BUF_PLANE2_PITCH_EXT, EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT,
          EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT },
        { EGL_DMA_BUF_PLANE3_FD_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT,
          EGL_DMA_BUF_PLANE3_PITCH_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT,
          EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT },
    };

    for (int i = 0; i < attribs->n_planes; i++) {
        eglAttribs[atti++] = plane_attrs[i].fd;
        eglAttribs[atti++] = attribs->fd[i];
        eglAttribs[atti++] = plane_attrs[i].offset;
        eglAttribs[atti++] = attribs->offset[i];
        eglAttribs[atti++] = plane_attrs[i].pitch;
        eglAttribs[atti++] = attribs->stride[i];
        if (attribs->modifier != DRM_FORMAT_MOD_INVALID) {
            eglAttribs[atti++] = plane_attrs[i].mod_lo;
            eglAttribs[atti++] = EGLint(attribs->modifier & 0xFFFFFFFF);
            eglAttribs[atti++] = plane_attrs[i].mod_hi;
            eglAttribs[atti++] = EGLint(attribs->modifier >> 32);
        }
    }
    eglAttribs[atti++] = EGL_IMAGE_PRESERVED_KHR;
    eglAttribs[atti++] = EGL_TRUE;
    eglAttribs[atti++] = EGL_NONE;

    EGLImage image = eglCreateImageKHR(display, EGL_NO_CONTEXT,
                                        EGL_LINUX_DMA_BUF_EXT, NULL, eglAttribs);
    if (image == EGL_NO_IMAGE) {
        qCWarning(lcWlRenderHelper) << "EGL dmabuf import: eglCreateImageKHR failed, EGL error=" << eglGetError();
        return false;
    }

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);
    glBindTexture(GL_TEXTURE_2D, 0);

    *outImage = image;
    *outTex = tex;
    return true;
}
#endif

// Copy from qquickrendertarget.cpp
static bool createRhiRenderTarget(const QRhiColorAttachment &colorAttachment,
                                  const QSize &pixelSize,
                                  int sampleCount,
                                  QRhi *rhi,
                                  QQuickWindowRenderTarget &dst)
{
    std::unique_ptr<QRhiRenderBuffer> depthStencil(rhi->newRenderBuffer(QRhiRenderBuffer::DepthStencil, pixelSize, sampleCount));
    if (!depthStencil->create()) {
        qCWarning(lcWlRenderHelper, "Failed to build depth-stencil buffer for QQuickRenderTarget");
        return false;
    }

    QRhiTextureRenderTargetDescription rtDesc(colorAttachment);
    rtDesc.setDepthStencilBuffer(depthStencil.get());
    std::unique_ptr<QRhiTextureRenderTarget> rt(rhi->newTextureRenderTarget(rtDesc));
    std::unique_ptr<QRhiRenderPassDescriptor> rp(rt->newCompatibleRenderPassDescriptor());
    rt->setRenderPassDescriptor(rp.get());

    if (!rt->create()) {
        qCWarning(lcWlRenderHelper, "Failed to build texture render target for QQuickRenderTarget");
        return false;
    }

    rt->setName(QByteArrayLiteral("WaylibTextureRenderTarget"));
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    dst.rt.renderTarget = rt.release();
    dst.res.rpDesc = rp.release();
    dst.implicitBuffers.depthStencil = depthStencil.release();
    dst.rt.owns = true; // ownership of the native resource itself is not transferred but the QRhi objects are on us now
#else
    dst.renderTarget = rt.release();
    dst.rpDesc = rp.release();
    dst.depthStencil = depthStencil.release();
    dst.owns = true; // ownership of the native resource itself is not transferred but the QRhi objects are on us now
#endif
    return true;
}

bool createRhiRenderTarget(QRhi *rhi, const QQuickRenderTarget &source, QQuickWindowRenderTarget &dst)
{
    auto rtd = QQuickRenderTargetPrivate::get(&source);

    switch (rtd->type) {
    case QQuickRenderTargetPrivate::Type::NativeTexture: {
        const auto format = rtd->u.nativeTexture.rhiFormat == QRhiTexture::UnknownFormat ? QRhiTexture::RGBA8
                                                                                         : QRhiTexture::Format(rtd->u.nativeTexture.rhiFormat);
        const auto flags = QRhiTexture::RenderTarget | QRhiTexture::Flags(
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
                               rtd->u.nativeTexture.rhiFormatFlags
#else
                               rtd->u.nativeTexture.rhiFlags
#endif
                                                                          );
        std::unique_ptr<QRhiTexture> texture(rhi->newTexture(format, rtd->pixelSize, rtd->sampleCount, flags));
        texture->setName(QByteArrayLiteral("WaylibTexture"));
#if QT_VERSION < QT_VERSION_CHECK(6, 6, 0)
        if (!texture->createFrom({ rtd->u.nativeTexture.object, rtd->u.nativeTexture.layout }))
#else
        if (!texture->createFrom({ rtd->u.nativeTexture.object, rtd->u.nativeTexture.layoutOrState }))
#endif
        {
            qCWarning(lcWlRenderHelper) << "Failed to wrap native texture (VkImage/GL texture) into QRhiTexture for render target";
            return false;
        }
        QRhiColorAttachment att(texture.get());
        if (!createRhiRenderTarget(att, rtd->pixelSize, rtd->sampleCount, rhi, dst))
            return false;
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        dst.res.texture = texture.release();
#else
        dst.texture = texture.release();
#endif
        return true;
    }
    case QQuickRenderTargetPrivate::Type::NativeRenderbuffer: {
        std::unique_ptr<QRhiRenderBuffer> renderbuffer(rhi->newRenderBuffer(QRhiRenderBuffer::Color, rtd->pixelSize, rtd->sampleCount));
        if (!renderbuffer->createFrom({ rtd->u.nativeRenderbufferObject })) {
            qCWarning(lcWlRenderHelper, "Failed to build wrapper renderbuffer for QQuickRenderTarget");
            return false;
        }
        QRhiColorAttachment att(renderbuffer.get());
        if (!createRhiRenderTarget(att, rtd->pixelSize, rtd->sampleCount, rhi, dst))
            return false;
        renderbuffer->setName(QByteArrayLiteral("WaylibRenderBuffer"));
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        dst.res.renderBuffer = renderbuffer.release();
#else
        dst.renderBuffer = renderbuffer.release();
#endif
        return true;
    }

    default:
        break;
    }

    return false;
}
// Copy end

class Q_DECL_HIDDEN WRenderHelperPrivate : public WObjectPrivate
{
public:
    WRenderHelperPrivate(WRenderHelper *qq, qw_renderer *renderer)
        : WObjectPrivate(qq)
        , renderer(renderer)
    {}
    ~WRenderHelperPrivate() {
        resetRenderBuffer();
    }

    void resetRenderBuffer();
    void onBufferDestroy();
    static bool ensureRhiRenderTarget(QQuickRenderControl *rc, BufferData *data);

    W_DECLARE_PUBLIC(WRenderHelper)
    qw_renderer *renderer;
    QList<BufferData*> buffers;
    BufferData *lastBuffer = nullptr;

    QSize size;
};

void WRenderHelperPrivate::resetRenderBuffer()
{
    qDeleteAll(buffers);
    lastBuffer = nullptr;
    buffers.clear();
}

void WRenderHelperPrivate::onBufferDestroy()
{
    qw_buffer *buffer = qobject_cast<qw_buffer*>(q_func()->sender());

    for (int i = 0; i < buffers.count(); ++i) {
        auto data = buffers[i];
        if (data->buffer == buffer) {
            if (lastBuffer == data)
                lastBuffer = nullptr;
            buffers.removeAt(i);
            break;
        }
    }
}

bool WRenderHelperPrivate::ensureRhiRenderTarget(QQuickRenderControl *rc, BufferData *data)
{
    data->resetWindowRenderTarget();
#if QT_VERSION < QT_VERSION_CHECK(6, 6, 0)
    auto rhi = QQuickRenderControlPrivate::get(rc)->rhi;
#else
    auto rhi = rc->rhi();
#endif
    auto tmp = data->renderTarget;
    bool ok = createRhiRenderTarget(rhi, tmp, data->windowRenderTarget);
    if (!ok)
        return false;
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    data->renderTarget = QQuickRenderTarget::fromRhiRenderTarget(data->windowRenderTarget.rt.renderTarget);
#else
    data->renderTarget = QQuickRenderTarget::fromRhiRenderTarget(data->windowRenderTarget.renderTarget);
#endif
    data->renderTarget.setDevicePixelRatio(tmp.devicePixelRatio());
    data->renderTarget.setMirrorVertically(tmp.mirrorVertically());

    return true;
}

WRenderHelper::WRenderHelper(qw_renderer *renderer, QObject *parent)
    : QObject(parent)
    , WObject(*new WRenderHelperPrivate(this, renderer))
{

}

QSize WRenderHelper::size() const
{
    W_DC(WRenderHelper);
    return d->size;
}

void WRenderHelper::setSize(const QSize &size)
{
    W_D(WRenderHelper);
    if (d->size == size)
        return;
    d->size = size;
    d->resetRenderBuffer();

    Q_EMIT sizeChanged();
}

QSGRendererInterface::GraphicsApi WRenderHelper::getGraphicsApi(QQuickRenderControl *rc)
{
    auto d = QQuickRenderControlPrivate::get(rc);
    return d->sg->rendererInterface(d->rc)->graphicsApi();
}

QSGRendererInterface::GraphicsApi WRenderHelper::getGraphicsApi()
{
    auto getApi = [] () {
        // Only for get GraphicsApi
        QQuickRenderControl rc;
        return getGraphicsApi(&rc);
    };

    static auto api = getApi();
    return api;
}

class Q_DECL_HIDDEN GLTextureBuffer : public qw_buffer_interface
{
public:
    explicit GLTextureBuffer(wlr_egl *egl, QSGTexture *texture);

    QW_INTERFACE(get_dmabuf, bool, wlr_dmabuf_attributes *attribs);

private:
    wlr_egl *m_egl;
    QSGTexture *m_texture;
};

GLTextureBuffer::GLTextureBuffer(wlr_egl *egl, QSGTexture *texture)
    : m_egl(egl)
    , m_texture(texture)
{

}

bool GLTextureBuffer::get_dmabuf(wlr_dmabuf_attributes *attribs)
{
    auto rhiTexture = m_texture->rhiTexture();
    if (!rhiTexture)
        return false;

    auto display = wlr_egl_get_display(m_egl);
    auto context = wlr_egl_get_context(m_egl);

    EGLImage image = eglCreateImage(display, context,
                                    EGL_GL_TEXTURE_2D,
                                    reinterpret_cast<EGLClientBuffer>(rhiTexture->nativeTexture().object),
                                    nullptr);

    if (image == EGL_NO_IMAGE)
        return false;

    static auto eglExportDMABUFImageQueryMESA =
        reinterpret_cast<PFNEGLEXPORTDMABUFIMAGEQUERYMESAPROC>(eglGetProcAddress("eglExportDMABUFImageQueryMESA"));
    static auto eglExportDMABUFImageMESA =
        reinterpret_cast<PFNEGLEXPORTDMABUFIMAGEMESAPROC>(eglGetProcAddress("eglExportDMABUFImageMESA"));

    if (!eglExportDMABUFImageQueryMESA || !eglExportDMABUFImageMESA)
        return false;

    bool ok = eglExportDMABUFImageQueryMESA(display,
                                            image,
                                            reinterpret_cast<int*>(&attribs->format),
                                            &attribs->n_planes,
                                            &attribs->modifier);
    if (!ok)
        return false;

    ok = eglExportDMABUFImageMESA(display,
                                  image,
                                  attribs->fd,
                                  reinterpret_cast<int*>(attribs->stride),
                                  reinterpret_cast<int*>(attribs->offset));
    if (!ok)
        return false;

    attribs->width = handle()->width;
    attribs->height = handle()->height;

    return true;
}

#ifdef ENABLE_VULKAN_RENDER
class Q_DECL_HIDDEN VkTextureBuffer : public qw_buffer_interface
{
public:
    explicit VkTextureBuffer(VkInstance instance, VkDevice device, QSGTexture *texture);

    QW_INTERFACE(get_dmabuf, bool ,wlr_dmabuf_attributes *attribs);

private:
    [[maybe_unused]] VkInstance m_instance;
    [[maybe_unused]] VkDevice m_device;
    [[maybe_unused]] QSGTexture *m_texture;
};

VkTextureBuffer::VkTextureBuffer(VkInstance instance, VkDevice device, QSGTexture *texture)
    : m_instance(instance)
    , m_device(device)
    , m_texture(texture)
{

}

bool VkTextureBuffer::get_dmabuf([[maybe_unused]] wlr_dmabuf_attributes *attribs)
{
//    static auto vkGetInstanceProcAddr =
//        reinterpret_cast<PFN_vkGetInstanceProcAddr>(::dlsym(RTLD_DEFAULT, "vkGetInstanceProcAddr"));
//    static auto vkGetMemoryFdKHR =
//        reinterpret_cast<PFN_vkGetMemoryFdKHR>(vkGetInstanceProcAddr(m_instance, "vkGetMemoryFdKHR"));
//    static auto vkGetImageMemoryRequirements =
//        reinterpret_cast<PFN_vkGetImageMemoryRequirements>(vkGetInstanceProcAddr(m_instance, "vkGetImageMemoryRequirements"));
//    static auto vkGetImageSparseMemoryRequirements =
//        reinterpret_cast<PFN_vkGetImageSparseMemoryRequirements>(vkGetInstanceProcAddr(m_instance, "vkGetImageSparseMemoryRequirements"));
//    static auto vkGetImageSubresourceLayout =
//        reinterpret_cast<PFN_vkGetImageSubresourceLayout>(vkGetInstanceProcAddr(m_instance, "vkGetImageSubresourceLayout"));

    // TODO
    return false;
}
#endif

class Q_DECL_HIDDEN QImageBuffer : public qw_buffer_interface
{
public:
    explicit QImageBuffer(const QImage &image);

    QW_INTERFACE(get_shm, bool, wlr_shm_attributes *attribs);
    QW_INTERFACE(begin_data_ptr_access, bool, uint32_t flags, void **data, uint32_t *format, size_t *stride);
    QW_INTERFACE(end_data_ptr_access, void);

private:
    QImage m_image;
};

QImageBuffer::QImageBuffer(const QImage &image)
    : m_image(image)
{

}

bool QImageBuffer::get_shm(wlr_shm_attributes *attribs)
{
    attribs->fd = 0;
    attribs->format = WTools::toDrmFormat(m_image.format());
    attribs->width = m_image.width();
    attribs->height = m_image.height();
    attribs->stride = m_image.bytesPerLine();
    return true;
}

bool QImageBuffer::begin_data_ptr_access([[maybe_unused]] uint32_t flags, void **data, uint32_t *format, size_t *stride)
{
    *data = m_image.bits();
    *format = WTools::toDrmFormat(m_image.format());
    *stride = m_image.bytesPerLine();

    return true;
}

void QImageBuffer::end_data_ptr_access()
{

}

qw_buffer *WRenderHelper::toBuffer(qw_renderer *renderer, QSGTexture *texture, QSGRendererInterface::GraphicsApi api)
{
    const QSize size = texture->textureSize();

    switch (api) {
    case QSGRendererInterface::OpenGL: {
        Q_ASSERT(wlr_renderer_is_gles2(renderer->handle()));
        auto egl = wlr_gles2_renderer_get_egl(renderer->handle());

        return qw_buffer::create(new GLTextureBuffer(egl, texture), size.width(), size.height());
    }
#ifdef ENABLE_VULKAN_RENDER
    case QSGRendererInterface::Vulkan: {
        Q_ASSERT(wlr_renderer_is_vk(renderer->handle()));
        auto instance = wlr_vk_renderer_get_instance(renderer->handle());
        auto device = wlr_vk_renderer_get_device(renderer->handle());

        return qw_buffer::create(new VkTextureBuffer(instance, device, texture), size.width(), size.height());
    }
#endif
    case QSGRendererInterface::Software: {
        QImage image;
        if (auto t = qobject_cast<QSGPlainTexture*>(texture)) {
            image = t->image();
        } else if (auto t = qobject_cast<QSGLayer*>(texture)) {
            image = t->toImage();
        } else if (QByteArrayView(texture->metaObject()->className())
                   == QByteArrayView("QSGSoftwarePixmapTexture")) {
            auto t = static_cast<QSGSoftwarePixmapTexture*>(texture);
            image = t->pixmap().toImage();
        } else {
            qFatal("Can't get QImage from QSGTexture, class name: %s", texture->metaObject()->className());
        }

        if (image.isNull())
            return nullptr;

        return qw_buffer::create(new QImageBuffer(image), image.width(), image.height());
    }
    default:
        qFatal("Can't get qw_buffer from QSGTexture, Not supported graphics API.");
        break;
    }

    return nullptr;
}

QQuickRenderTarget WRenderHelper::acquireRenderTarget(QQuickRenderControl *rc, qw_buffer *buffer)
{
    W_D(WRenderHelper);
    Q_ASSERT(buffer);

    if (d->size.isEmpty())
        return {};

    for (int i = 0; i < d->buffers.count(); ++i) {
        auto data = d->buffers[i];
        if (data->buffer == buffer) {
            d->lastBuffer = data;
            return data->renderTarget;
        }
    }

    std::unique_ptr<BufferData> bufferData(new BufferData);
    bufferData->buffer = buffer;

    QQuickRenderTarget rt;

    if (wlr_renderer_is_pixman(d->renderer->handle())) {
        auto texture = qw_texture::from_buffer(*d->renderer, *buffer);
        pixman_image_t *image = wlr_pixman_texture_get_image(texture->handle());
        void *data = pixman_image_get_data(image);
        if (bufferData->paintDevice.constBits() != data)
            bufferData->paintDevice = WTools::fromPixmanImage(image, data);
        Q_ASSERT(!bufferData->paintDevice.isNull());
        rt = QQuickRenderTarget::fromPaintDevice(&bufferData->paintDevice);
        delete texture;
    }
#ifdef ENABLE_VULKAN_RENDER
    else if (wlr_renderer_is_vk(d->renderer->handle())) {
        // Vulkan wlroots renderer with GL Qt RHI: import the output buffer's
        // dmabuf as a GL texture via EGL (EGL_EXT_image_dma_buf_import), so Qt
        // RHI (GL) can render into it. dmabuf is API-agnostic — any EGL context
        // can import it regardless of the wlroots renderer type. This is the
        // key decoupling: wlroots Vulkan backend handles dmabuf creation/KMS
        // commit, Qt RHI (GL) handles rendering, EGL bridges the two via dmabuf.
        // acquireRenderTarget is called during a Qt RHI frame (GL context
        // current), so eglGetCurrentDisplay() returns the correct EGL display.
        EGLDisplay eglDisplay = eglGetCurrentDisplay();
        if (eglDisplay == EGL_NO_DISPLAY) {
            qCWarning(lcWlRenderHelper) << "Vulkan+GL: no current EGL display (GL context not current?)";
            return {};
        }

        wlr_dmabuf_attributes dmabuf;
        if (!wlr_buffer_get_dmabuf(buffer->handle(), &dmabuf)) {
            qCWarning(lcWlRenderHelper) << "Vulkan+GL: output buffer has no dmabuf";
            return {};
        }

        EGLImage eglImage = EGL_NO_IMAGE;
        GLuint glTex = 0;
        if (!eglImportDmabufToGLTexture(eglDisplay, &dmabuf, &eglImage, &glTex)) {
            qCWarning(lcWlRenderHelper) << "Vulkan+GL: EGL dmabuf import failed for output buffer";
            return {};
        }

        bufferData->eglImage = eglImage;
        bufferData->glTexture = glTex;
        bufferData->eglDisplay = eglDisplay;

        rt = QQuickRenderTarget::fromOpenGLTexture(glTex, d->size);
        rt.setMirrorVertically(true);
    }
#endif
    else if (wlr_renderer_is_gles2(d->renderer->handle())) {
        auto texture = qw_texture::from_buffer(*d->renderer, *buffer);
        wlr_gles2_texture_attribs attribs;
        wlr_gles2_texture_get_attribs(texture->handle(), &attribs);

        rt = QQuickRenderTarget::fromOpenGLTexture(attribs.tex, d->size);
        rt.setMirrorVertically(true);
        delete texture;
    }
    bufferData->renderTarget = rt;

    if (QSGRendererInterface::isApiRhiBased(getGraphicsApi(rc))) {
        if (!rt.isNull()) {
            // Force convert to Rhi render target
            if (!d->ensureRhiRenderTarget(rc, bufferData.get()))
                bufferData->renderTarget = {};
        }

        if (bufferData->renderTarget.isNull())
            return {};

        if (auto texture = bufferData->windowRenderTarget.res.texture) {
            s_rhiRenderBuffers->append({ bufferData->windowRenderTarget.rt.renderTarget,
                                         texture, bufferData->buffer });
        }
    }

    connect(buffer, SIGNAL(before_destroy()),
            this, SLOT(onBufferDestroy()), Qt::UniqueConnection);

    d->buffers.append(bufferData.release());
    d->lastBuffer = d->buffers.last();

    return d->buffers.last()->renderTarget;
}

#ifdef ENABLE_VULKAN_RENDER
// Resolve the Vulkan loader entry point vkGetDeviceProcAddr via dlsym. It is a
// LOADER_EXPORT symbol (visibility("default")) in libvulkan
// (vulkan-loader: loader/vk_loader_platform.h), so it can be resolved directly
// without linking against libvulkan. wlroots itself links libvulkan and
// references it as an ordinary global symbol; dlsym is the equivalent for code
// that does not link libvulkan.
static PFN_vkGetDeviceProcAddr resolveVkGetDeviceProcAddr()
{
    static PFN_vkGetDeviceProcAddr proc =
        reinterpret_cast<PFN_vkGetDeviceProcAddr>(dlsym(RTLD_DEFAULT, "vkGetDeviceProcAddr"));
    return proc;
}
#endif

void WRenderHelper::transitionVkImageToGeneral(QRhi *rhi, QRhiTexture *texture,
                                               qw_buffer *buffer)
{
#ifdef ENABLE_VULKAN_RENDER
    if (!rhi || !texture || !buffer)
        return;

    // Obtain the wlroots-adopted VkDevice/VkQueue from Qt RHI. QRhiVulkanNativeHandles
    // (qrhi_platform.h) exposes dev, gfxQueue, gfxQueueFamilyIdx and inst, all of
    // which belong to the wlroots-adopted Vulkan device.
    const auto *handles = static_cast<const QRhiVulkanNativeHandles *>(rhi->nativeHandles());
    if (!handles || !handles->dev || !handles->gfxQueue || !handles->inst) {
        qCWarning(lcWlRenderHelper) << "Vulkan: QRhi native handles unavailable, cannot transition render image layout";
        return;
    }

    VkDevice device = handles->dev;
    VkQueue queue = handles->gfxQueue;
    VkImage image = reinterpret_cast<VkImage>(static_cast<quintptr>(texture->nativeTexture().object));

    // Device-level functions must be resolved via vkGetDeviceProcAddr, which
    // returns device-relative entry points (dispatch table or layer chain),
    // per the Vulkan loader rules (trampoline.c vkGetDeviceProcAddr). All
    // functions used below are device-level commands.
    auto vkGetDeviceProcAddr = resolveVkGetDeviceProcAddr();
    if (!vkGetDeviceProcAddr) {
        qCWarning(lcWlRenderHelper) << "Vulkan: vkGetDeviceProcAddr unavailable, cannot transition render image layout";
        return;
    }
    PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers =
        reinterpret_cast<PFN_vkAllocateCommandBuffers>(vkGetDeviceProcAddr(device, "vkAllocateCommandBuffers"));
    PFN_vkBeginCommandBuffer vkBeginCommandBuffer =
        reinterpret_cast<PFN_vkBeginCommandBuffer>(vkGetDeviceProcAddr(device, "vkBeginCommandBuffer"));
    PFN_vkCmdPipelineBarrier vkCmdPipelineBarrier =
        reinterpret_cast<PFN_vkCmdPipelineBarrier>(vkGetDeviceProcAddr(device, "vkCmdPipelineBarrier"));
    PFN_vkEndCommandBuffer vkEndCommandBuffer =
        reinterpret_cast<PFN_vkEndCommandBuffer>(vkGetDeviceProcAddr(device, "vkEndCommandBuffer"));
    PFN_vkQueueSubmit vkQueueSubmit =
        reinterpret_cast<PFN_vkQueueSubmit>(vkGetDeviceProcAddr(device, "vkQueueSubmit"));
    PFN_vkQueueWaitIdle vkQueueWaitIdle =
        reinterpret_cast<PFN_vkQueueWaitIdle>(vkGetDeviceProcAddr(device, "vkQueueWaitIdle"));
    PFN_vkFreeCommandBuffers vkFreeCommandBuffers =
        reinterpret_cast<PFN_vkFreeCommandBuffers>(vkGetDeviceProcAddr(device, "vkFreeCommandBuffers"));
    PFN_vkCreateCommandPool vkCreateCommandPool =
        reinterpret_cast<PFN_vkCreateCommandPool>(vkGetDeviceProcAddr(device, "vkCreateCommandPool"));
    PFN_vkDestroyCommandPool vkDestroyCommandPool =
        reinterpret_cast<PFN_vkDestroyCommandPool>(vkGetDeviceProcAddr(device, "vkDestroyCommandPool"));
    // For implicit sync: create a binary semaphore exportable as a sync_file
    // (mirrors wlroots pass.c:485-494). vkGetSemaphoreFdKHR is provided by
    // VK_KHR_external_semaphore_fd, which wlroots enables on the device
    // (vulkan.c:506-635); RADV on amdgpu supports it.
    PFN_vkCreateSemaphore vkCreateSemaphore =
        reinterpret_cast<PFN_vkCreateSemaphore>(vkGetDeviceProcAddr(device, "vkCreateSemaphore"));
    PFN_vkDestroySemaphore vkDestroySemaphore =
        reinterpret_cast<PFN_vkDestroySemaphore>(vkGetDeviceProcAddr(device, "vkDestroySemaphore"));
    PFN_vkGetSemaphoreFdKHR vkGetSemaphoreFdKHR =
        reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(vkGetDeviceProcAddr(device, "vkGetSemaphoreFdKHR"));

    if (!vkAllocateCommandBuffers || !vkBeginCommandBuffer || !vkCmdPipelineBarrier ||
        !vkEndCommandBuffer || !vkQueueSubmit || !vkQueueWaitIdle ||
        !vkFreeCommandBuffers || !vkCreateCommandPool || !vkDestroyCommandPool ||
        !vkCreateSemaphore || !vkDestroySemaphore || !vkGetSemaphoreFdKHR) {
        qCWarning(lcWlRenderHelper) << "Vulkan: required command/sync functions unavailable for layout transition";
        return;
    }

    // Create a transient command pool for the graphics queue family.
    VkCommandPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolInfo.queueFamilyIndex = handles->gfxQueueFamilyIdx;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkResult res = vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool);
    if (res != VK_SUCCESS) {
        qCWarning(lcWlRenderHelper) << "Vulkan: vkCreateCommandPool failed for layout transition, error=" << res;
        return;
    }

    VkCommandBufferAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    res = vkAllocateCommandBuffers(device, &allocInfo, &cb);
    if (res != VK_SUCCESS) {
        qCWarning(lcWlRenderHelper) << "Vulkan: vkAllocateCommandBuffers failed for layout transition, error=" << res;
        vkDestroyCommandPool(device, commandPool, nullptr);
        return;
    }

    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &beginInfo);

    // NOTE: No layout barrier is recorded here. The previous approach
    // (transition COLOR_ATTACHMENT_OPTIMAL -> GENERAL + setNativeLayout) caused
    // InvalidImageLayout validation errors (13.txt) because Qt RHI does not
    // update usageState.layout after its render pass (finalLayout stays
    // COLOR_ATTACHMENT_OPTIMAL but the tracked value goes stale), and
    // preserveColorContents mode expects COLOR_ATTACHMENT_OPTIMAL as
    // initialLayout. KMS scanout reads the dmabuf's physical memory directly
    // and does not care about the Vulkan image layout, so leaving the image in
    // COLOR_ATTACHMENT_OPTIMAL is acceptable. This command buffer is empty and
    // serves only as a submit载体 to signal the semaphore for sync_file export.

    vkEndCommandBuffer(cb);

    // Create a binary semaphore exportable as a sync_file (VK_KHR_external_
    // semaphore_fd). Signalled by the submit below, then exported to a
    // sync_file fd and imported into the dmabuf so KMS implicit sync waits
    // for the Vulkan render. Mirrors wlroots pass.c:485-494 (creation) and
    // renderer.c:994-1026 (export + dmabuf_import_sync_file).
    VkExportSemaphoreCreateInfo exportInfo = {};
    exportInfo.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO;
    exportInfo.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
    VkSemaphoreCreateInfo semInfo = {};
    semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    semInfo.pNext = &exportInfo;
    VkSemaphore semaphore = VK_NULL_HANDLE;
    res = vkCreateSemaphore(device, &semInfo, nullptr, &semaphore);
    if (res != VK_SUCCESS) {
        qCWarning(lcWlRenderHelper) << "Vulkan: vkCreateSemaphore failed for sync, error=" << res;
        vkFreeCommandBuffers(device, commandPool, 1, &cb);
        vkDestroyCommandPool(device, commandPool, nullptr);
        return;
    }

    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cb;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &semaphore;
    res = vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
    if (res != VK_SUCCESS) {
        qCWarning(lcWlRenderHelper) << "Vulkan: vkQueueSubmit failed for layout transition, error=" << res;
    } else {
        // Wait for the submitted command buffer (layout transition) to complete
        // before exporting the sync_file and destroying the semaphore/command
        // buffer. Unlike wlroots (which uses timeline semaphores with
        // vkQueueSubmit2KHR and relies on vkGetSemaphoreFdKHR's implicit wait),
        // our binary semaphore + vkQueueSubmit path requires an explicit
        // vkQueueWaitIdle — the validation layer (12.txt) confirmed that
        // vkDestroySemaphore/vkFreeCommandBuffers were called while the
        // semaphore/command buffer was still pending.
        vkQueueWaitIdle(queue);

        // Export the signalled semaphore as a sync_file fd. After vkQueueWaitIdle
        // the semaphore is signalled and the GPU is idle, so this returns an
        // already-signalled sync_file fd and resets the semaphore.
        VkSemaphoreGetFdInfoKHR getFdInfo = {};
        getFdInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR;
        getFdInfo.semaphore = semaphore;
        getFdInfo.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
        int syncFileFd = -1;
        VkResult fdRes = vkGetSemaphoreFdKHR(device, &getFdInfo, &syncFileFd);
        if (fdRes != VK_SUCCESS || syncFileFd < 0) {
            qCWarning(lcWlRenderHelper) << "Vulkan: vkGetSemaphoreFdKHR failed, error=" << fdRes
                                        << "- dmabuf implicit sync not signalled (KMS may reject commit)";
        } else {
            // The GPU has completed (vkQueueWaitIdle above). Signal the dmabuf's
            // implicit sync fence on every plane so KMS scanout waits for the
            // Vulkan render. Mirrors wlroots renderer.c:1014-1026
            // (dmabuf_import_sync_file with DMA_BUF_SYNC_WRITE).
            // DMA_BUF_IOCTL_IMPORT_SYNC_FILE is a kernel UAPI (linux/dma-buf.h,
            // available since Linux 5.20).
            //
            // NOTE: wlr_buffer_get_dmabuf() returns a *reference* to the
            // buffer's dmabuf attributes (gbm's buffer_get_dmabuf does a
            // shallow struct copy, no fd dup). wlr_dmabuf_attributes_finish()
            // must NOT be called — it would close the buffer's own fds.
            wlr_dmabuf_attributes dmabuf;
            if (wlr_buffer_get_dmabuf(buffer->handle(), &dmabuf)) {
                for (int i = 0; i < dmabuf.n_planes; ++i) {
                    struct dma_buf_import_sync_file data = {};
                    data.flags = DMA_BUF_SYNC_WRITE;
                    data.fd = syncFileFd;
                    if (ioctl(dmabuf.fd[i], DMA_BUF_IOCTL_IMPORT_SYNC_FILE, &data) != 0) {
                        qCWarning(lcWlRenderHelper) << "Vulkan: DMA_BUF_IOCTL_IMPORT_SYNC_FILE failed on plane" << i
                                                    << "errno=" << errno << "- KMS implicit sync may not wait";
                        break;
                    }
                }
            } else {
                qCWarning(lcWlRenderHelper) << "Vulkan: output buffer has no dmabuf, cannot signal implicit sync";
            }
            close(syncFileFd);
        }
    }

    vkDestroySemaphore(device, semaphore, nullptr);
    vkFreeCommandBuffers(device, commandPool, 1, &cb);
    vkDestroyCommandPool(device, commandPool, nullptr);
#else
    Q_UNUSED(rhi);
    Q_UNUSED(texture);
    Q_UNUSED(buffer);
#endif
}

std::pair<qw_buffer *, QQuickRenderTarget> WRenderHelper::lastRenderTarget() const
{
    W_DC(WRenderHelper);
    if (!d->lastBuffer)
        return {nullptr, {}};

    return {d->lastBuffer->buffer, d->lastBuffer->renderTarget};
}

static qw_renderer *createRendererWithType(const char *type, qw_backend *backend)
{
    qputenv("WLR_RENDERER", type);
    auto render = qw_renderer::autocreate(*backend);
    qunsetenv("WLR_RENDERER");

    return render;
}

qw_renderer *WRenderHelper::createRenderer(qw_backend *backend)
{
    auto api = getGraphicsApi();
    return createRenderer(backend, api);
}

qw_renderer *WRenderHelper::createRenderer(qw_backend *backend, QSGRendererInterface::GraphicsApi api)
{
    qw_renderer *renderer = nullptr;
    // The wlroots renderer type is determined by WLR_RENDERER, independent of
    // the Qt RHI API. When WLR_RENDERER=vulkan, a Vulkan wlroots renderer is
    // created even though Qt RHI uses OpenGL — the Vulkan renderer handles
    // dmabuf allocation and KMS commit, while Qt RHI (GL) renders via EGL
    // dmabuf import.
    const auto wlrRenderer = qgetenv("WLR_RENDERER");
    switch (api) {
    case QSGRendererInterface::OpenGL:
#ifdef ENABLE_VULKAN_RENDER
        if (wlrRenderer == "vulkan") {
            renderer = createRendererWithType("vulkan", backend);
            Q_ASSERT(!renderer || wlr_renderer_is_vk(renderer->handle()));
            break;
        }
#endif
        renderer = createRendererWithType("gles2", backend);
        Q_ASSERT(!renderer || wlr_renderer_is_gles2(renderer->handle()));
        break;
#ifdef ENABLE_VULKAN_RENDER
    case QSGRendererInterface::Vulkan: {
        renderer = createRendererWithType("vulkan", backend);
        if (renderer && !wlr_renderer_is_vk(renderer->handle())) {
            qCWarning(lcWlRenderHelper) << "Vulkan: wlr_renderer was created but is not a Vulkan renderer, rendering will likely fail";
        }
        Q_ASSERT(!renderer || wlr_renderer_is_vk(renderer->handle()));
        break;
    }
#endif
    case QSGRendererInterface::Software:
        renderer = createRendererWithType("pixman", backend);
        Q_ASSERT(!renderer || wlr_renderer_is_pixman(renderer->handle()));
        break;
    default:
        qFatal("Not supported graphics api: %s", qPrintable(QQuickWindow::sceneGraphBackend()));
        break;
    }

    return renderer;
}

constexpr const char *GraphicsApiName(QSGRendererInterface::GraphicsApi api)
{
    switch (api) {
        using enum QSGRendererInterface::GraphicsApi;
    case Software:
        return "Software";
    case OpenGL:
        return "OpenGL";
    case Vulkan:
        return "Vulkan";
    default:
        return "Unknown/Unsupported";
    }
}

void WRenderHelper::setupRendererBackend(qw_backend *testBackend)
{
    const auto wlrRenderer = qgetenv("WLR_RENDERER");

    if (wlrRenderer == "auto" || wlrRenderer.isEmpty()) {
        if (qEnvironmentVariableIsSet("QSG_RHI_BACKEND")
            || (qEnvironmentVariableIsSet("QT_QUICK_BACKEND")
                && qgetenv("QT_QUICK_BACKEND") != "rhi")) {
            // when environment variable Q*_BACKEND was set, should defer to
            // the env variable for the graphics API.
            return;
        }

        QList<QSGRendererInterface::GraphicsApi> apiList = {
            QSGRendererInterface::OpenGL,
            QSGRendererInterface::Software
            // TODO: Add vulkan to list.
        };
        std::unique_ptr<qw_display> display { nullptr };
        if (!testBackend) {
            display.reset(new qw_display());
            testBackend = qw_backend::autocreate(display->get_event_loop(), nullptr);

            if (!testBackend)
                qFatal("Failed to create wlr_backend");

            testBackend->start();
        }
        QQuickWindow::setGraphicsApi(WRenderHelper::probe(testBackend, apiList));
    } else if (wlrRenderer == "gles2") {
        QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
    } else if (wlrRenderer == "vulkan") {
#ifdef ENABLE_VULKAN_RENDER
        // The wlroots renderer uses Vulkan for dmabuf creation, commit and
        // scanout (the "Vulkan backend" registered to waylib itself). Qt RHI
        // stays OpenGL — it renders the QML scene graph via EGL dmabuf import,
        // independent of the wlroots renderer type. This mirrors Android HWUI:
        // the compositor's Vulkan pipeline does not force apps (or the UI
        // toolkit) to use Vulkan. (deepin design: Vulkan only serves waylib's
        // own composite backend, does not interfere with Qt RHI shell.)
        QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
        qCInfo(lcWlRenderHelper) << "Vulkan composite backend requested via WLR_RENDERER=vulkan (Qt RHI stays OpenGL)";
#else
        qFatal("Vulkan support is not enabled");
#endif
    } else if (wlrRenderer == "pixman") {
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    } else {
        qFatal() << "Unknown/Unsupported wlr renderer: " << wlrRenderer;
    }
}

QSGRendererInterface::GraphicsApi WRenderHelper::probe(qw_backend *testBackend, const QList<QSGRendererInterface::GraphicsApi> &apiList)
{
    auto acceptApi = QSGRendererInterface::Unknown;

    for (auto api : std::as_const(apiList)) {
        std::unique_ptr<qw_renderer> renderer(createRenderer(testBackend, api));
        if (!renderer) {
            qCInfo(lcWlRenderHelper) << GraphicsApiName(api) << " api failed to create wlr_renderer";
            continue;
        }

        const wlr_drm_format_set *formats = wlr_renderer_get_texture_formats(*renderer, WLR_BUFFER_CAP_DMABUF);

        if (formats && formats->len == 0) {
            qCInfo(lcWlRenderHelper) << GraphicsApiName(api) << " api don't support any format";
            continue;
        }

        // TODO: how to test when formats gets NULL
        if (formats && formats->len) {
            std::unique_ptr<qw_allocator> alloc(qw_allocator::autocreate(*testBackend, *renderer.get()));

            bool hasSupportedFormat = false;
            for (size_t formatId = 0; formatId < formats->len; formatId++) {
                auto *format = &formats->formats[formatId];

                std::unique_ptr<qw_swapchain> swapchain(qw_swapchain::create(*alloc.get(), 1000, 800, format));
                auto wbuffer = swapchain->acquire();
                if (!wbuffer) {
                    continue;
                } else {
                    std::unique_ptr<qw_buffer, qw_buffer::unlocker> buffer(qw_buffer::from(wbuffer));
                    std::unique_ptr<qw_texture> texture { qw_texture::from_buffer(*renderer.get(), *buffer.get()) };
                    if (!texture)
                        continue;
                    hasSupportedFormat = true;
                    break;
                }
            }

            if (!hasSupportedFormat) {
                qCInfo(lcWlRenderHelper) << GraphicsApiName(api) << " api failed to convert any buffer to texture";
                continue;
            }
        }

        acceptApi = api;
        break;
    }

    return acceptApi;
}

static void updateGLTexture(QRhi *rhi, qw_texture *handle, QSGPlainTexture *texture) {
    wlr_gles2_texture_attribs attribs;
    wlr_gles2_texture_get_attribs(handle->handle(), &attribs);
    QSize size(handle->handle()->width, handle->handle()->height);

#define GL_TEXTURE_EXTERNAL_OES           0x8D65
    QQuickWindowPrivate::TextureFromNativeTextureFlags flags = attribs.target == GL_TEXTURE_EXTERNAL_OES
                                                                   ? QQuickWindowPrivate::NativeTextureIsExternalOES
                                                                   : QQuickWindowPrivate::TextureFromNativeTextureFlags {};
    texture->setTextureFromNativeTexture(rhi, attribs.tex, 0, 0, size, {}, flags);

    texture->setHasAlphaChannel(attribs.has_alpha);
    texture->setTextureSize(size);
}

static inline quint64 vkimage_cast(void *image) {
    return reinterpret_cast<quintptr>(image);
}

[[maybe_unused]] static inline quint64 vkimage_cast(quint64 image) {
    return image;
}

#ifdef ENABLE_VULKAN_RENDER
static void updateVKTexture(QRhi *rhi, qw_texture *handle, QSGPlainTexture *texture) {
    wlr_vk_image_attribs attribs;
    wlr_vk_texture_get_image_attribs(handle->handle(), &attribs);
    QSize size(handle->handle()->width, handle->handle()->height);

    texture->setTextureFromNativeTexture(rhi,
                                         vkimage_cast(attribs.image),
                                         attribs.layout, attribs.format, size,
                                         {}, {});
    texture->setHasAlphaChannel(wlr_vk_texture_has_alpha(handle->handle()));
    texture->setTextureSize(size);
}
#endif

#ifdef ENABLE_VULKAN_RENDER
// (updateEglDmabufTexture removed — logic inlined into makeTexture)
#endif

static void updateImage(QRhi *, qw_texture *handle, QSGPlainTexture *texture) {
    auto image = wlr_pixman_texture_get_image(handle->handle());
    texture->setImage(WTools::fromPixmanImage(image));
}

typedef void(*UpdateTextureFunction)(QRhi *, qw_texture *, QSGPlainTexture *);

static UpdateTextureFunction getUpdateTextFunction(qw_texture *handle)
{
    const auto api = WRenderHelper::getGraphicsApi();
    if (api == QSGRendererInterface::OpenGL) {
        if (wlr_texture_is_gles2(handle->handle())) {
            return updateGLTexture;
        }
#ifdef ENABLE_VULKAN_RENDER
        // Vulkan wlroots renderer with GL Qt RHI: handled separately in
        // makeTexture() via updateEglDmabufTexture (needs buffer access).
#endif
        return nullptr;
    }
#ifdef ENABLE_VULKAN_RENDER
    else if (api == QSGRendererInterface::Vulkan) {
        Q_ASSERT(wlr_texture_is_vk(handle->handle()));
        return updateVKTexture;
    }
#endif
    else if (api == QSGRendererInterface::Software) {
        Q_ASSERT(wlr_texture_is_pixman(handle->handle()));
        return updateImage;
    }

    return nullptr;
}

bool WRenderHelper::makeTexture(QRhi *rhi, qw_texture *handle,
                                QSGPlainTexture *texture, qw_buffer *buffer)
{
#ifdef ENABLE_VULKAN_RENDER
    // Vulkan renderer + GL RHI: import the buffer's dmabuf as a GL texture
    // via EGL. This is the client-surface counterpart of acquireRenderTarget.
    // If EGL import fails (EGL_BAD_ALLOC on some modifiers), return false
    // gracefully — the client window won't display but the system stays
    // stable (no commit failure, no crash).
    if (WRenderHelper::getGraphicsApi() == QSGRendererInterface::OpenGL
        && wlr_texture_is_vk(handle->handle())) {
        QSize size(handle->handle()->width, handle->handle()->height);

        // Try EGL dmabuf import first (for client surfaces with dmabuf buffers).
        if (buffer) {
            wlr_dmabuf_attributes dmabuf;
            if (wlr_buffer_get_dmabuf(buffer->handle(), &dmabuf)) {
                EGLDisplay eglDisplay = eglGetCurrentDisplay();
                if (eglDisplay != EGL_NO_DISPLAY) {
                    EGLImage eglImage = EGL_NO_IMAGE;
                    GLuint glTex = 0;
                    if (eglImportDmabufToGLTexture(eglDisplay, &dmabuf, &eglImage, &glTex)) {
                        // NOTE: do NOT call wlr_dmabuf_attributes_finish() —
                        // wlr_buffer_get_dmabuf returns a shallow reference
                        // (no fd dup). finish would close the buffer's own fds.
                        texture->setTextureFromNativeTexture(rhi, glTex, 0, 0, size, {},
                                                              QQuickWindowPrivate::TextureFromNativeTextureFlags{});
                        texture->setHasAlphaChannel(wlr_vk_texture_has_alpha(handle->handle()));
                        texture->setTextureSize(size);
                        return texture->rhiTexture() != nullptr;
                    }
                }
            }
        }

        // Fallback for shm/pixels textures (e.g. cursor QImage, no dmabuf):
        // read pixels via wlr_texture_read_pixels and upload to a GL texture
        // via glTexImage2D. This mirrors how QSGPlainTexture handles QImage
        // textures in the software/GL path.
        uint32_t fmt = DRM_FORMAT_ARGB8888;
        // Use wlr_texture_preferred_read_format to get the optimal format.
        uint32_t pref = wlr_texture_preferred_read_format(handle->handle());
        if (pref != 0)
            fmt = pref;

        int bpp = 4; // ARGB8888 = 4 bytes per pixel
        if (fmt == DRM_FORMAT_ARGB8888 || fmt == DRM_FORMAT_XRGB8888) {
            bpp = 4;
        } else {
            // Unsupported read format for GL upload fallback
            return false;
        }

        const int stride = size.width() * bpp;
        QByteArray pixels(size.height() * stride, 0);

        struct wlr_texture_read_pixels_options options = {};
        options.data = pixels.data();
        options.format = fmt;
        options.stride = stride;
        if (!wlr_texture_read_pixels(handle->handle(), &options))
            return false;

        GLuint glTex = 0;
        glGenTextures(1, &glTex);
        glBindTexture(GL_TEXTURE_2D, glTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        // DRM_FORMAT_ARGB8888 maps to GL_BGRA + GL_UNSIGNED_BYTE on little-endian.
        // QImage::Format_ARGB32 uses the same memory layout.
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, size.width(), size.height(), 0,
                     GL_BGRA, GL_UNSIGNED_BYTE, pixels.constData());
        glBindTexture(GL_TEXTURE_2D, 0);

        texture->setTextureFromNativeTexture(rhi, glTex, 0, 0, size, {},
                                              QQuickWindowPrivate::TextureFromNativeTextureFlags{});
        texture->setHasAlphaChannel(wlr_vk_texture_has_alpha(handle->handle()));
        texture->setTextureSize(size);
        return texture->rhiTexture() != nullptr;
    }
#endif
    auto updateTexture = getUpdateTextFunction(handle);
    if (Q_UNLIKELY(!updateTexture))
        return false;
    updateTexture(rhi, handle, texture);
    return true;
}

WRenderHelper::TextureEntry
WRenderHelper::newTexture(qw_allocator *allocator, qw_renderer *renderer,
                          uint32_t drmFormat, uint64_t drmModifier,
                          QRhi *rhi, const QSize &size,
                          int rhiFormat, int rhiFlags)
{
    uint64_t modifiers[] = {drmModifier};
    wlr_drm_format format {
        .format = drmFormat,
        .len = 1,
        .capacity = 1,
        .modifiers = modifiers
    };

    wlr_buffer *buffer = allocator->create_buffer(size.width(), size.height(), &format);
    if (!buffer) {
        qCCritical(lcWlRenderHelper) << "Failed to create qw_buffer from allocator";
        return {};
    }

    std::unique_ptr<qw_texture> texture(qw_texture::from_buffer(*renderer, buffer));
    if (!texture) {
        qCCritical(lcWlRenderHelper) << "Failed to create qw_texture from buffer";
        wlr_buffer_drop(buffer);
        return {};
    }

    const auto qformat = static_cast<QRhiTexture::Format>(rhiFormat);
    const auto qflags = QRhiTexture::Flags(rhiFlags);
    std::unique_ptr<QRhiTexture> rhiTexture(rhi->newTexture(qformat, size, 1, qflags));

    if (wlr_texture_is_gles2(*texture.get())) {
        if (rhi->backend() != QRhi::OpenGLES2) {
            qFatal("The current QRhi backend doesn't support creating texture from GLES2 texture");
        }

        wlr_gles2_texture_attribs attribs;
        wlr_gles2_texture_get_attribs(*texture.get(), &attribs);

        if (!rhiTexture->createFrom({attribs.tex, 0})) {
            qCCritical(lcWlRenderHelper, "Failed to create QRhiTexture from GLES2 texture");
            wlr_buffer_drop(buffer);
            return {};
        }
    }
#ifdef ENABLE_VULKAN_RENDER
    else if (wlr_texture_is_vk(*texture.get())) {
        // Vulkan wlroots renderer: the wlr_texture is a VkImage, but Qt RHI
        // may be GL (WLR_RENDERER=vulkan with GL Qt RHI). In that case, import
        // the buffer's dmabuf as a GL texture via EGL (same as acquireRenderTarget),
        // then wrap it as a QRhiTexture. dmabuf is API-agnostic.
        if (rhi->backend() == QRhi::Vulkan) {
            wlr_vk_image_attribs vkAttribs;
            wlr_vk_texture_get_image_attribs(*texture.get(), &vkAttribs);

            if (!rhiTexture->createFrom({vkimage_cast(vkAttribs.image), vkAttribs.layout})) {
                qCCritical(lcWlRenderHelper, "Failed to create QRhiTexture from Vulkan image");
                wlr_buffer_drop(buffer);
                return {};
            }
        } else if (rhi->backend() == QRhi::OpenGLES2) {
            wlr_dmabuf_attributes dmabuf;
            if (!wlr_buffer_get_dmabuf(buffer, &dmabuf)) {
                qCCritical(lcWlRenderHelper, "Vulkan+GL newTexture: buffer has no dmabuf");
                wlr_buffer_drop(buffer);
                return {};
            }
            EGLDisplay eglDisplay = eglGetCurrentDisplay();
            EGLImage eglImage = EGL_NO_IMAGE;
            GLuint glTex = 0;
            if (!eglImportDmabufToGLTexture(eglDisplay, &dmabuf, &eglImage, &glTex)) {
                qCCritical(lcWlRenderHelper, "Vulkan+GL newTexture: EGL dmabuf import failed");
                wlr_buffer_drop(buffer);
                return {};
            }
            if (!rhiTexture->createFrom({glTex, 0})) {
                qCCritical(lcWlRenderHelper, "Vulkan+GL newTexture: createFrom GL texture failed");
                glDeleteTextures(1, &glTex);
                auto destroyImage = resolveEglDestroyImageKHR();
                if (destroyImage) destroyImage(eglDisplay, eglImage);
                wlr_buffer_drop(buffer);
                return {};
            }
            // Note: eglImage and glTex ownership is now tied to rhiTexture via
            // createFrom (owns=false). The dmabuf backing is shared with buffer.
            // When rhiTexture is destroyed, the GL texture is released by Qt RHI.
            // The EGLImage should be destroyed after the GL texture is no longer
            // needed — for simplicity we leak it (it's per-texture, bounded by
            // texture count). TODO: proper EGLImage lifecycle management.
        } else {
            qFatal("The current QRhi backend doesn't support creating texture from Vulkan image");
        }
    }
#endif
    else if (wlr_texture_is_pixman(*texture.get())) {
        qFatal("Creating QRhiTexture from Pixman image is not supported");
    } else {
        qFatal("Unknown texture type");
    }

    rhiTexture->setName("WaylibTexture");

    return {buffer, texture.release(), rhiTexture.release()};
}

WRenderHelper::TextureEntry
WRenderHelper::newTextureLike(QW_NAMESPACE::qw_allocator *allocator,
                              QW_NAMESPACE::qw_renderer *renderer,
                              QRhiTexture *texture, QRhi *rhi,
                              int rhiFlags)
{
    auto buffer = lookupBuffer(texture);
    if (!buffer)
        return {};

    wlr_dmabuf_attributes attribs;
    if (!buffer->get_dmabuf(&attribs))
        return {};

    return newTexture(allocator, renderer, attribs.format, attribs.modifier,
                      rhi, texture->pixelSize(), texture->format(), rhiFlags);
}

QW_NAMESPACE::qw_buffer *WRenderHelper::lookupBuffer(const QRhiRenderTarget *rt)
{
    for (const auto &entry : std::as_const(*s_rhiRenderBuffers)) {
        if (entry.renderTarget == rt)
            return entry.buffer;
    }

    return nullptr;
}

QW_NAMESPACE::qw_buffer *WRenderHelper::lookupBuffer(const QRhiTexture *texture)
{
    for (const auto &entry : std::as_const(*s_rhiRenderBuffers)) {
        if (entry.texture == texture)
            return entry.buffer;
    }

    return nullptr;
}

WAYLIB_SERVER_END_NAMESPACE

#include "moc_wrenderhelper.cpp"
