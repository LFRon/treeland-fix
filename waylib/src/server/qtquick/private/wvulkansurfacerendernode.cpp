// Copyright (C) 2026 UnionTech Software Technology Co., Ltd.
// SPDX-License-Identifier: Apache-2.0 OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "wvulkansurfacerendernode_p.h"

#include "wayliblogging.h"

#include <qwbuffer.h>

#include <QFile>
#include <QQuickWindow>
#include <QVulkanInstance>
#include <QVector>
#include <QVector4D>
#include <rhi/qshader.h>

#include <vulkan/vulkan.h>

#include <cstring>
#include <dlfcn.h>

extern "C" {
#include <wlr/types/wlr_compositor.h>
}

WAYLIB_SERVER_BEGIN_NAMESPACE

namespace {

struct SurfaceVertex
{
    float x;
    float y;
    float u;
    float v;
};

struct SurfaceUniform
{
    QMatrix4x4 matrix;
    QVector4D opacityAndFlags;
};

struct ShaderSource
{
    QByteArray code;
    QByteArray entryPoint;
};

struct NativeBuffer
{
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
};

struct VulkanFunctions
{
    PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties = nullptr;
    PFN_vkCreateImageView vkCreateImageView = nullptr;
    PFN_vkDestroyImageView vkDestroyImageView = nullptr;
    PFN_vkCreateSampler vkCreateSampler = nullptr;
    PFN_vkDestroySampler vkDestroySampler = nullptr;
    PFN_vkCreateDescriptorSetLayout vkCreateDescriptorSetLayout = nullptr;
    PFN_vkDestroyDescriptorSetLayout vkDestroyDescriptorSetLayout = nullptr;
    PFN_vkCreateDescriptorPool vkCreateDescriptorPool = nullptr;
    PFN_vkDestroyDescriptorPool vkDestroyDescriptorPool = nullptr;
    PFN_vkAllocateDescriptorSets vkAllocateDescriptorSets = nullptr;
    PFN_vkUpdateDescriptorSets vkUpdateDescriptorSets = nullptr;
    PFN_vkCreatePipelineLayout vkCreatePipelineLayout = nullptr;
    PFN_vkDestroyPipelineLayout vkDestroyPipelineLayout = nullptr;
    PFN_vkCreateShaderModule vkCreateShaderModule = nullptr;
    PFN_vkDestroyShaderModule vkDestroyShaderModule = nullptr;
    PFN_vkCreateGraphicsPipelines vkCreateGraphicsPipelines = nullptr;
    PFN_vkDestroyPipeline vkDestroyPipeline = nullptr;
    PFN_vkCreateBuffer vkCreateBuffer = nullptr;
    PFN_vkDestroyBuffer vkDestroyBuffer = nullptr;
    PFN_vkGetBufferMemoryRequirements vkGetBufferMemoryRequirements = nullptr;
    PFN_vkAllocateMemory vkAllocateMemory = nullptr;
    PFN_vkFreeMemory vkFreeMemory = nullptr;
    PFN_vkBindBufferMemory vkBindBufferMemory = nullptr;
    PFN_vkMapMemory vkMapMemory = nullptr;
    PFN_vkUnmapMemory vkUnmapMemory = nullptr;
    PFN_vkCmdBindPipeline vkCmdBindPipeline = nullptr;
    PFN_vkCmdSetViewport vkCmdSetViewport = nullptr;
    PFN_vkCmdSetScissor vkCmdSetScissor = nullptr;
    PFN_vkCmdBindDescriptorSets vkCmdBindDescriptorSets = nullptr;
    PFN_vkCmdBindVertexBuffers vkCmdBindVertexBuffers = nullptr;
    PFN_vkCmdDraw vkCmdDraw = nullptr;
};

static PFN_vkGetDeviceProcAddr resolveVkGetDeviceProcAddr()
{
    static PFN_vkGetDeviceProcAddr proc =
        reinterpret_cast<PFN_vkGetDeviceProcAddr>(dlsym(RTLD_DEFAULT, "vkGetDeviceProcAddr"));
    return proc;
}

static PFN_vkGetInstanceProcAddr resolveVkGetInstanceProcAddr()
{
    static PFN_vkGetInstanceProcAddr proc =
        reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(RTLD_DEFAULT, "vkGetInstanceProcAddr"));
    return proc;
}

static const char *vkResultName(VkResult result)
{
    switch (result) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_EVENT_SET: return "VK_EVENT_SET";
    case VK_EVENT_RESET: return "VK_EVENT_RESET";
    case VK_INCOMPLETE: return "VK_INCOMPLETE";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_FRAGMENTED_POOL: return "VK_ERROR_FRAGMENTED_POOL";
#ifdef VK_ERROR_UNKNOWN
    case VK_ERROR_UNKNOWN: return "VK_ERROR_UNKNOWN";
#endif
    default: return "VK_RESULT_UNKNOWN";
    }
}

static quintptr pointerAddress(const void *ptr)
{
    return reinterpret_cast<quintptr>(ptr);
}

static QRect opaqueRegionExtents(wlr_surface *surface)
{
    if (!surface || !pixman_region32_not_empty(&surface->opaque_region))
        return {};

    const pixman_box32_t *box = pixman_region32_extents(&surface->opaque_region);
    if (!box)
        return {};

    return QRect(QPoint(box->x1, box->y1), QPoint(box->x2 - 1, box->y2 - 1));
}

static ShaderSource loadSpirvShader(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        qCWarning(lcWlVulkanCompositor)
            << "Vulkan surface render node failed to open shader" << path;
        return {};
    }

    const QShader shader = QShader::fromSerialized(file.readAll());
    if (!shader.isValid()) {
        qCWarning(lcWlVulkanCompositor)
            << "Vulkan surface render node loaded invalid shader" << path;
        return {};
    }

    for (const QShaderKey &key : shader.availableShaders()) {
        if (key.source() != QShader::SpirvShader)
            continue;

        const QShaderCode code = shader.shader(key);
        if (code.shader().isEmpty())
            continue;

        ShaderSource source;
        source.code = code.shader();
        source.entryPoint = code.entryPoint().isEmpty() ? QByteArrayLiteral("main")
                                                        : code.entryPoint();
        return source;
    }

    qCWarning(lcWlVulkanCompositor)
        << "Vulkan surface render node shader has no SPIR-V payload" << path;
    return {};
}

static VkSampleCountFlagBits toVkSampleCount(int sampleCount)
{
    switch (sampleCount) {
    case 2: return VK_SAMPLE_COUNT_2_BIT;
    case 4: return VK_SAMPLE_COUNT_4_BIT;
    case 8: return VK_SAMPLE_COUNT_8_BIT;
    case 16: return VK_SAMPLE_COUNT_16_BIT;
    case 32: return VK_SAMPLE_COUNT_32_BIT;
    case 64: return VK_SAMPLE_COUNT_64_BIT;
    default: return VK_SAMPLE_COUNT_1_BIT;
    }
}

static int findMemoryType(const VulkanFunctions &f,
                          VkPhysicalDevice physicalDevice,
                          uint32_t typeBits,
                          VkMemoryPropertyFlags properties)
{
    if (!f.vkGetPhysicalDeviceMemoryProperties)
        return -1;

    VkPhysicalDeviceMemoryProperties memoryProperties = {};
    f.vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memoryProperties);
    for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
        const bool typeMatches = typeBits & (1u << i);
        const bool flagsMatch =
            (memoryProperties.memoryTypes[i].propertyFlags & properties) == properties;
        if (typeMatches && flagsMatch)
            return int(i);
    }

    return -1;
}

} // namespace

struct WVulkanSurfaceRenderNode::NativeResources
{
    VkInstance instance = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkSampleCountFlagBits sampleCount = VK_SAMPLE_COUNT_1_BIT;

    VkImage image = VK_NULL_HANDLE;
    VkFormat imageFormat = VK_FORMAT_UNDEFINED;
    VkImageLayout imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageView imageView = VK_NULL_HANDLE;

    VkSampler sampler = VK_NULL_HANDLE;
    bool samplerSmooth = true;

    VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    bool descriptorDirty = true;

    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkShaderModule vertexShader = VK_NULL_HANDLE;
    VkShaderModule fragmentShader = VK_NULL_HANDLE;
    QByteArray vertexEntryPoint = QByteArrayLiteral("main");
    QByteArray fragmentEntryPoint = QByteArrayLiteral("main");

    NativeBuffer vertexBuffer;
    NativeBuffer uniformBuffer;
    VulkanFunctions f;
};

WVulkanSurfaceRenderNode::WVulkanSurfaceRenderNode(QQuickWindow *window)
    : m_window(window)
{
}

WVulkanSurfaceRenderNode::~WVulkanSurfaceRenderNode()
{
    resetAll();
}

bool WVulkanSurfaceRenderNode::setBuffer(qw_buffer *buffer,
                                         wlr_surface *surface,
                                         uint32_t surfaceCommitSeq)
{
    if (m_buffer == buffer
        && m_surface == surface
        && m_surfaceCommitSeq == surfaceCommitSeq
        && m_importedTexture.hasNativeImage()
        && !m_importDirty) {
        return true;
    }

    if (m_buffer != buffer || m_surface != surface || m_surfaceCommitSeq != surfaceCommitSeq) {
        m_buffer = buffer;
        m_surface = surface;
        m_surfaceCommitSeq = surfaceCommitSeq;
        m_importDirty = true;
        m_importFailed = false;
        m_loggedPrepareState = false;
        m_loggedRenderState = false;
    }

    const bool ready = ensureImported(surface);
    if (!ready)
        markDirty(QSGNode::DirtyMaterial);
    return ready;
}

void WVulkanSurfaceRenderNode::setGeometry(const QRectF &targetRect,
                                           const QRectF &sourceRect,
                                           qreal devicePixelRatio)
{
    if (m_targetRect == targetRect
        && m_sourceRect == sourceRect
        && qFuzzyCompare(m_devicePixelRatio, devicePixelRatio)) {
        return;
    }

    m_targetRect = targetRect;
    m_sourceRect = sourceRect;
    m_devicePixelRatio = devicePixelRatio;
    m_verticesDirty = true;
    markDirty(QSGNode::DirtyGeometry);
}

void WVulkanSurfaceRenderNode::setSmooth(bool smooth)
{
    if (m_smooth == smooth)
        return;

    m_smooth = smooth;
    m_samplerDirty = true;
    markDirty(QSGNode::DirtyMaterial);
}

bool WVulkanSurfaceRenderNode::isReady() const
{
    return !m_importFailed && m_importedTexture.hasNativeImage();
}

static bool resolveNativeFunctions(WVulkanSurfaceRenderNode::NativeResources *native)
{
    if (!native || native->device == VK_NULL_HANDLE)
        return false;

    auto vkGetDeviceProcAddr = resolveVkGetDeviceProcAddr();
    auto vkGetInstanceProcAddr = resolveVkGetInstanceProcAddr();
    if (!vkGetDeviceProcAddr || !vkGetInstanceProcAddr) {
        qCWarning(lcWlVulkanCompositor)
            << "Vulkan surface render node cannot resolve Vulkan loader entry points";
        return false;
    }

#define LOAD_DEVICE_PROC(name) \
    native->f.name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(native->device, #name))
#define LOAD_INSTANCE_PROC(name) \
    native->f.name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(native->instance, #name))

    LOAD_INSTANCE_PROC(vkGetPhysicalDeviceMemoryProperties);
    LOAD_DEVICE_PROC(vkCreateImageView);
    LOAD_DEVICE_PROC(vkDestroyImageView);
    LOAD_DEVICE_PROC(vkCreateSampler);
    LOAD_DEVICE_PROC(vkDestroySampler);
    LOAD_DEVICE_PROC(vkCreateDescriptorSetLayout);
    LOAD_DEVICE_PROC(vkDestroyDescriptorSetLayout);
    LOAD_DEVICE_PROC(vkCreateDescriptorPool);
    LOAD_DEVICE_PROC(vkDestroyDescriptorPool);
    LOAD_DEVICE_PROC(vkAllocateDescriptorSets);
    LOAD_DEVICE_PROC(vkUpdateDescriptorSets);
    LOAD_DEVICE_PROC(vkCreatePipelineLayout);
    LOAD_DEVICE_PROC(vkDestroyPipelineLayout);
    LOAD_DEVICE_PROC(vkCreateShaderModule);
    LOAD_DEVICE_PROC(vkDestroyShaderModule);
    LOAD_DEVICE_PROC(vkCreateGraphicsPipelines);
    LOAD_DEVICE_PROC(vkDestroyPipeline);
    LOAD_DEVICE_PROC(vkCreateBuffer);
    LOAD_DEVICE_PROC(vkDestroyBuffer);
    LOAD_DEVICE_PROC(vkGetBufferMemoryRequirements);
    LOAD_DEVICE_PROC(vkAllocateMemory);
    LOAD_DEVICE_PROC(vkFreeMemory);
    LOAD_DEVICE_PROC(vkBindBufferMemory);
    LOAD_DEVICE_PROC(vkMapMemory);
    LOAD_DEVICE_PROC(vkUnmapMemory);
    LOAD_DEVICE_PROC(vkCmdBindPipeline);
    LOAD_DEVICE_PROC(vkCmdSetViewport);
    LOAD_DEVICE_PROC(vkCmdSetScissor);
    LOAD_DEVICE_PROC(vkCmdBindDescriptorSets);
    LOAD_DEVICE_PROC(vkCmdBindVertexBuffers);
    LOAD_DEVICE_PROC(vkCmdDraw);

#undef LOAD_DEVICE_PROC
#undef LOAD_INSTANCE_PROC

    const bool ok = native->f.vkGetPhysicalDeviceMemoryProperties
        && native->f.vkCreateImageView
        && native->f.vkDestroyImageView
        && native->f.vkCreateSampler
        && native->f.vkDestroySampler
        && native->f.vkCreateDescriptorSetLayout
        && native->f.vkDestroyDescriptorSetLayout
        && native->f.vkCreateDescriptorPool
        && native->f.vkDestroyDescriptorPool
        && native->f.vkAllocateDescriptorSets
        && native->f.vkUpdateDescriptorSets
        && native->f.vkCreatePipelineLayout
        && native->f.vkDestroyPipelineLayout
        && native->f.vkCreateShaderModule
        && native->f.vkDestroyShaderModule
        && native->f.vkCreateGraphicsPipelines
        && native->f.vkDestroyPipeline
        && native->f.vkCreateBuffer
        && native->f.vkDestroyBuffer
        && native->f.vkGetBufferMemoryRequirements
        && native->f.vkAllocateMemory
        && native->f.vkFreeMemory
        && native->f.vkBindBufferMemory
        && native->f.vkMapMemory
        && native->f.vkUnmapMemory
        && native->f.vkCmdBindPipeline
        && native->f.vkCmdSetViewport
        && native->f.vkCmdSetScissor
        && native->f.vkCmdBindDescriptorSets
        && native->f.vkCmdBindVertexBuffers
        && native->f.vkCmdDraw;
    if (!ok) {
        qCWarning(lcWlVulkanCompositor)
            << "Vulkan surface render node cannot resolve required Vulkan functions";
    }
    return ok;
}

static void destroyNativeBuffer(WVulkanSurfaceRenderNode::NativeResources *native,
                                NativeBuffer *buffer)
{
    if (!native || !buffer || native->device == VK_NULL_HANDLE)
        return;

    if (buffer->buffer != VK_NULL_HANDLE && native->f.vkDestroyBuffer)
        native->f.vkDestroyBuffer(native->device, buffer->buffer, nullptr);
    if (buffer->memory != VK_NULL_HANDLE && native->f.vkFreeMemory)
        native->f.vkFreeMemory(native->device, buffer->memory, nullptr);

    *buffer = {};
}

static bool createNativeBuffer(WVulkanSurfaceRenderNode::NativeResources *native,
                               VkDeviceSize size,
                               VkBufferUsageFlags usage,
                               NativeBuffer *buffer)
{
    if (!native || !buffer)
        return false;
    if (buffer->buffer != VK_NULL_HANDLE && buffer->size >= size)
        return true;

    destroyNativeBuffer(native, buffer);

    VkBufferCreateInfo bufferInfo = {};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkResult res = native->f.vkCreateBuffer(native->device, &bufferInfo, nullptr, &buffer->buffer);
    if (res != VK_SUCCESS) {
        qCWarning(lcWlVulkanCompositor)
            << "Vulkan surface render node failed to create host buffer"
            << vkResultName(res) << int(res)
            << "size" << qulonglong(size)
            << "usage" << Qt::hex << usage << Qt::dec;
        *buffer = {};
        return false;
    }

    VkMemoryRequirements requirements = {};
    native->f.vkGetBufferMemoryRequirements(native->device, buffer->buffer, &requirements);
    const int memoryType = findMemoryType(native->f,
                                          native->physicalDevice,
                                          requirements.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                                              | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (memoryType < 0) {
        qCWarning(lcWlVulkanCompositor)
            << "Vulkan surface render node failed to find host-visible memory"
            << "requirements" << Qt::hex << requirements.memoryTypeBits << Qt::dec
            << "size" << qulonglong(size);
        destroyNativeBuffer(native, buffer);
        return false;
    }

    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = requirements.size;
    allocInfo.memoryTypeIndex = uint32_t(memoryType);

    res = native->f.vkAllocateMemory(native->device, &allocInfo, nullptr, &buffer->memory);
    if (res != VK_SUCCESS) {
        qCWarning(lcWlVulkanCompositor)
            << "Vulkan surface render node failed to allocate host buffer memory"
            << vkResultName(res) << int(res)
            << "allocationSize" << qulonglong(allocInfo.allocationSize)
            << "memoryType" << memoryType;
        destroyNativeBuffer(native, buffer);
        return false;
    }

    res = native->f.vkBindBufferMemory(native->device, buffer->buffer, buffer->memory, 0);
    if (res != VK_SUCCESS) {
        qCWarning(lcWlVulkanCompositor)
            << "Vulkan surface render node failed to bind host buffer memory"
            << vkResultName(res) << int(res);
        destroyNativeBuffer(native, buffer);
        return false;
    }

    buffer->size = size;
    return true;
}

static bool writeNativeBuffer(WVulkanSurfaceRenderNode::NativeResources *native,
                              const NativeBuffer &buffer,
                              const void *data,
                              size_t size)
{
    if (!native || buffer.memory == VK_NULL_HANDLE || !data || size > buffer.size)
        return false;

    void *mapped = nullptr;
    VkResult res = native->f.vkMapMemory(native->device, buffer.memory, 0,
                                         VkDeviceSize(size), 0, &mapped);
    if (res != VK_SUCCESS || !mapped) {
        qCWarning(lcWlVulkanCompositor)
            << "Vulkan surface render node failed to map host buffer"
            << vkResultName(res) << int(res)
            << "size" << qulonglong(size);
        return false;
    }

    std::memcpy(mapped, data, size);
    native->f.vkUnmapMemory(native->device, buffer.memory);
    return true;
}

static bool createShaderModule(WVulkanSurfaceRenderNode::NativeResources *native,
                               const ShaderSource &source,
                               VkShaderModule *shaderModule)
{
    if (!native || !shaderModule || source.code.isEmpty() || source.code.size() % 4 != 0)
        return false;

    QVector<uint32_t> words(source.code.size() / 4);
    std::memcpy(words.data(), source.code.constData(), size_t(source.code.size()));

    VkShaderModuleCreateInfo shaderInfo = {};
    shaderInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shaderInfo.codeSize = size_t(source.code.size());
    shaderInfo.pCode = words.constData();

    VkResult res = native->f.vkCreateShaderModule(native->device,
                                                  &shaderInfo,
                                                  nullptr,
                                                  shaderModule);
    if (res != VK_SUCCESS) {
        qCWarning(lcWlVulkanCompositor)
            << "Vulkan surface render node failed to create shader module"
            << vkResultName(res) << int(res)
            << "codeSize" << source.code.size();
        *shaderModule = VK_NULL_HANDLE;
        return false;
    }

    return true;
}

void WVulkanSurfaceRenderNode::prepare()
{
    if (!ensureImported(m_surface) || !ensureNativeResources())
        return;

    if (m_verticesDirty) {
        const QSize textureSize = m_importedTexture.size;
        const qreal textureWidth = qMax(1, textureSize.width());
        const qreal textureHeight = qMax(1, textureSize.height());
        QRectF source = m_sourceRect;
        if (!source.isValid() || source.isEmpty())
            source = QRectF(QPointF(0, 0), textureSize);

        const float u0 = float(source.left() / textureWidth);
        const float v0 = float(source.top() / textureHeight);
        const float u1 = float(source.right() / textureWidth);
        const float v1 = float(source.bottom() / textureHeight);
        const SurfaceVertex vertices[] = {
            { float(m_targetRect.left()),  float(m_targetRect.top()),    u0, v0 },
            { float(m_targetRect.left()),  float(m_targetRect.bottom()), u0, v1 },
            { float(m_targetRect.right()), float(m_targetRect.top()),    u1, v0 },
            { float(m_targetRect.right()), float(m_targetRect.bottom()), u1, v1 },
        };
        if (!writeNativeBuffer(m_native.get(), m_native->vertexBuffer,
                               vertices, sizeof(vertices))) {
            return;
        }
        m_verticesDirty = false;
    }

    SurfaceUniform uniform = {};
    uniform.matrix = *projectionMatrix() * *matrix();
    uniform.opacityAndFlags = QVector4D(float(inheritedOpacity()),
                                        m_importedTexture.hasAlpha ? 0.0f : 1.0f,
                                        0.0f,
                                        0.0f);
    if (!writeNativeBuffer(m_native.get(), m_native->uniformBuffer,
                           &uniform, sizeof(uniform))) {
        return;
    }

    if (!m_loggedPrepareState) {
        qCDebug(lcWlVulkanCompositor)
            << "Vulkan surface render node prepared native draw"
            << "buffer" << pointerAddress(m_buffer)
            << "surface" << m_surface
            << "surfaceCommitSeq" << m_surfaceCommitSeq
            << "targetRect" << m_targetRect
            << "sourceRect" << m_sourceRect
            << "devicePixelRatio" << m_devicePixelRatio
            << "textureSize" << m_importedTexture.size
            << "hasAlpha" << m_importedTexture.hasAlpha
            << "forceOpaqueInShader" << !m_importedTexture.hasAlpha
            << "inheritedOpacity" << inheritedOpacity()
            << "surfaceSize" << (m_surface ? QSize(m_surface->current.width,
                                                   m_surface->current.height)
                                            : QSize())
            << "surfaceOpaqueRegionEmpty"
            << (!m_surface || !pixman_region32_not_empty(&m_surface->opaque_region))
            << "surfaceOpaqueExtents" << opaqueRegionExtents(m_surface)
            << "imageFormat" << m_importedTexture.nativeViewFormat
            << "imageLayout" << m_importedTexture.nativeLayout;
        m_loggedPrepareState = true;
    }
}

void WVulkanSurfaceRenderNode::render(const RenderState *state)
{
    if (!isReady() || !m_native || m_native->pipeline == VK_NULL_HANDLE
        || m_native->descriptorSet == VK_NULL_HANDLE
        || m_native->vertexBuffer.buffer == VK_NULL_HANDLE) {
        return;
    }

    QRhiCommandBuffer *cb = commandBuffer();
    const auto *cbHandles =
        static_cast<const QRhiVulkanCommandBufferNativeHandles *>(cb->nativeHandles());
    if (!cbHandles || cbHandles->commandBuffer == VK_NULL_HANDLE) {
        qCWarning(lcWlVulkanCompositor)
            << "Vulkan surface render node cannot render: QRhi command buffer handle unavailable";
        return;
    }

    VkCommandBuffer vkCommandBuffer = cbHandles->commandBuffer;
    const QSize renderTargetSize = renderTarget()->pixelSize();

    m_native->f.vkCmdBindPipeline(vkCommandBuffer,
                                  VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  m_native->pipeline);

    VkViewport viewport = {};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = float(renderTargetSize.width());
    viewport.height = float(renderTargetSize.height());
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    m_native->f.vkCmdSetViewport(vkCommandBuffer, 0, 1, &viewport);

    const bool scissorEnabled = state && state->scissorEnabled();
    const QRect scissorRect = scissorEnabled
        ? state->scissorRect()
        : QRect(QPoint(), renderTargetSize);
    VkRect2D scissor = {};
    scissor.offset = { scissorRect.x(), scissorRect.y() };
    scissor.extent = { uint32_t(qMax(0, scissorRect.width())),
                       uint32_t(qMax(0, scissorRect.height())) };
    m_native->f.vkCmdSetScissor(vkCommandBuffer, 0, 1, &scissor);

    m_native->f.vkCmdBindDescriptorSets(vkCommandBuffer,
                                        VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        m_native->pipelineLayout,
                                        0,
                                        1,
                                        &m_native->descriptorSet,
                                        0,
                                        nullptr);

    VkDeviceSize vertexOffset = 0;
    m_native->f.vkCmdBindVertexBuffers(vkCommandBuffer,
                                       0,
                                       1,
                                       &m_native->vertexBuffer.buffer,
                                       &vertexOffset);
    m_native->f.vkCmdDraw(vkCommandBuffer, 4, 1, 0, 0);

    if (!m_loggedRenderState) {
        qCDebug(lcWlVulkanCompositor)
            << "Vulkan surface render node native draw submitted"
            << "buffer" << pointerAddress(m_buffer)
            << "surface" << m_surface
            << "surfaceCommitSeq" << m_surfaceCommitSeq
            << "renderTargetSize" << renderTargetSize
            << "scissorEnabled" << scissorEnabled
            << "scissorRect" << scissorRect
            << "targetRect" << m_targetRect
            << "textureSize" << m_importedTexture.size
            << "hasAlpha" << m_importedTexture.hasAlpha;
        m_loggedRenderState = true;
    }
}

void WVulkanSurfaceRenderNode::releaseResources()
{
    resetAll();
}

QSGRenderNode::RenderingFlags WVulkanSurfaceRenderNode::flags() const
{
    return QSGRenderNode::BoundedRectRendering
        | QSGRenderNode::DepthAwareRendering;
}

QSGRenderNode::StateFlags WVulkanSurfaceRenderNode::changedStates() const
{
    return QSGRenderNode::ViewportState | QSGRenderNode::ScissorState;
}

QRectF WVulkanSurfaceRenderNode::rect() const
{
    return m_targetRect;
}

bool WVulkanSurfaceRenderNode::ensureImported(wlr_surface *surface)
{
    if (!m_window || !m_window->rhi() || !m_buffer) {
        releaseImport();
        m_importFailed = true;
        return false;
    }

    QRhi *rhi = m_window->rhi();
    if (m_rhi && m_rhi != rhi)
        resetAll();
    m_rhi = rhi;

    if (!m_importDirty && m_importedTexture.hasNativeImage())
        return true;

    releaseImport();
    m_importDirty = false;

    if (!WRenderHelper::importVulkanNativeTextureFromBuffer(m_rhi,
                                                            m_buffer,
                                                            surface,
                                                            &m_importedTexture)
        || !m_importedTexture.hasNativeImage()) {
        m_importFailed = true;
        qCDebug(lcWlVulkanCompositor)
            << "Vulkan surface render node could not import client dmabuf"
            << "buffer" << pointerAddress(m_buffer)
            << "surfaceCommitSeq" << m_surfaceCommitSeq;
        return false;
    }

    m_importFailed = false;
    m_verticesDirty = true;
    m_samplerDirty = true;
    resetNativeImageResources();

    qCDebug(lcWlVulkanCompositor)
        << "Vulkan surface render node imported client dmabuf for native draw"
        << "buffer" << pointerAddress(m_buffer)
        << "surfaceCommitSeq" << m_surfaceCommitSeq
        << "size" << m_importedTexture.size
        << "format" << Qt::hex << m_importedTexture.drmFormat << Qt::dec
        << "modifier" << Qt::hex << m_importedTexture.drmModifier << Qt::dec
        << "alpha" << m_importedTexture.hasAlpha
        << "nativeImage" << Qt::hex << m_importedTexture.nativeImage << Qt::dec
        << "nativeViewFormat" << m_importedTexture.nativeViewFormat
        << "nativeLayout" << m_importedTexture.nativeLayout
        << "surface" << m_surface
        << "surfaceSize" << (m_surface ? QSize(m_surface->current.width,
                                               m_surface->current.height)
                                        : QSize())
        << "surfaceOpaqueRegionEmpty"
        << (!m_surface || !pixman_region32_not_empty(&m_surface->opaque_region))
        << "surfaceOpaqueExtents" << opaqueRegionExtents(m_surface);
    return true;
}

bool WVulkanSurfaceRenderNode::ensureNativeResources()
{
    if (!m_rhi || m_rhi->backend() != QRhi::Vulkan || !m_importedTexture.hasNativeImage())
        return false;

    const auto *handles = static_cast<const QRhiVulkanNativeHandles *>(m_rhi->nativeHandles());
    if (!handles || !handles->inst || handles->inst->vkInstance() == VK_NULL_HANDLE
        || handles->dev == VK_NULL_HANDLE || handles->physDev == VK_NULL_HANDLE) {
        qCWarning(lcWlVulkanCompositor)
            << "Vulkan surface render node cannot prepare: QRhi Vulkan handles unavailable";
        return false;
    }

    if (!m_native
        || m_native->instance != handles->inst->vkInstance()
        || m_native->device != handles->dev
        || m_native->physicalDevice != handles->physDev) {
        resetNativeResources();
        m_native = std::make_unique<NativeResources>();
        m_native->instance = handles->inst->vkInstance();
        m_native->device = handles->dev;
        m_native->physicalDevice = handles->physDev;
        if (!resolveNativeFunctions(m_native.get())) {
            resetNativeResources();
            return false;
        }
    }

    if (!createNativeBuffer(m_native.get(),
                            sizeof(SurfaceVertex) * 4,
                            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                            &m_native->vertexBuffer)
        || !createNativeBuffer(m_native.get(),
                               sizeof(SurfaceUniform),
                               VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                               &m_native->uniformBuffer)) {
        return false;
    }

    if (m_samplerDirty || m_native->sampler == VK_NULL_HANDLE
        || m_native->samplerSmooth != m_smooth) {
        if (m_native->sampler != VK_NULL_HANDLE && m_native->f.vkDestroySampler)
            m_native->f.vkDestroySampler(m_native->device, m_native->sampler, nullptr);
        m_native->sampler = VK_NULL_HANDLE;

        const VkFilter filter = m_smooth ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        VkSamplerCreateInfo samplerInfo = {};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = filter;
        samplerInfo.minFilter = filter;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.maxLod = 0.0f;
        VkResult res = m_native->f.vkCreateSampler(m_native->device,
                                                   &samplerInfo,
                                                   nullptr,
                                                   &m_native->sampler);
        if (res != VK_SUCCESS) {
            qCWarning(lcWlVulkanCompositor)
                << "Vulkan surface render node failed to create sampler"
                << vkResultName(res) << int(res)
                << "smooth" << m_smooth;
            return false;
        }
        m_native->samplerSmooth = m_smooth;
        m_native->descriptorDirty = true;
        m_samplerDirty = false;
    }

    const VkImage image = VkImage(m_importedTexture.nativeImage);
    const VkFormat imageFormat = VkFormat(m_importedTexture.nativeViewFormat);
    const VkImageLayout imageLayout = VkImageLayout(m_importedTexture.nativeLayout);
    if (m_native->imageView == VK_NULL_HANDLE
        || m_native->image != image
        || m_native->imageFormat != imageFormat
        || m_native->imageLayout != imageLayout) {
        resetNativeImageResources();

        m_native->image = image;
        m_native->imageFormat = imageFormat;
        m_native->imageLayout = imageLayout;

        VkImageViewCreateInfo viewInfo = {};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = imageFormat;
        viewInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;

        VkResult res = m_native->f.vkCreateImageView(m_native->device,
                                                     &viewInfo,
                                                     nullptr,
                                                     &m_native->imageView);
        if (res != VK_SUCCESS) {
            qCWarning(lcWlVulkanCompositor)
                << "Vulkan surface render node failed to create native image view"
                << vkResultName(res) << int(res)
                << "image" << Qt::hex << m_importedTexture.nativeImage << Qt::dec
                << "format" << imageFormat
                << "layout" << imageLayout
                << "size" << m_importedTexture.size;
            resetNativeImageResources();
            return false;
        }
        m_native->descriptorDirty = true;
    }

    if (m_native->descriptorSetLayout == VK_NULL_HANDLE) {
        VkDescriptorSetLayoutBinding bindings[2] = {};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[1].binding = 1;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo layoutInfo = {};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = 2;
        layoutInfo.pBindings = bindings;

        VkResult res = m_native->f.vkCreateDescriptorSetLayout(m_native->device,
                                                               &layoutInfo,
                                                               nullptr,
                                                               &m_native->descriptorSetLayout);
        if (res != VK_SUCCESS) {
            qCWarning(lcWlVulkanCompositor)
                << "Vulkan surface render node failed to create descriptor set layout"
                << vkResultName(res) << int(res);
            return false;
        }
    }

    if (m_native->descriptorPool == VK_NULL_HANDLE) {
        VkDescriptorPoolSize poolSizes[2] = {};
        poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        poolSizes[0].descriptorCount = 1;
        poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSizes[1].descriptorCount = 1;

        VkDescriptorPoolCreateInfo poolInfo = {};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = 2;
        poolInfo.pPoolSizes = poolSizes;

        VkResult res = m_native->f.vkCreateDescriptorPool(m_native->device,
                                                          &poolInfo,
                                                          nullptr,
                                                          &m_native->descriptorPool);
        if (res != VK_SUCCESS) {
            qCWarning(lcWlVulkanCompositor)
                << "Vulkan surface render node failed to create descriptor pool"
                << vkResultName(res) << int(res);
            return false;
        }

        VkDescriptorSetAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = m_native->descriptorPool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &m_native->descriptorSetLayout;
        res = m_native->f.vkAllocateDescriptorSets(m_native->device,
                                                   &allocInfo,
                                                   &m_native->descriptorSet);
        if (res != VK_SUCCESS) {
            qCWarning(lcWlVulkanCompositor)
                << "Vulkan surface render node failed to allocate descriptor set"
                << vkResultName(res) << int(res);
            return false;
        }
        m_native->descriptorDirty = true;
    }

    if (m_native->descriptorDirty) {
        VkDescriptorBufferInfo uniformInfo = {};
        uniformInfo.buffer = m_native->uniformBuffer.buffer;
        uniformInfo.range = sizeof(SurfaceUniform);

        VkDescriptorImageInfo imageInfo = {};
        imageInfo.sampler = m_native->sampler;
        imageInfo.imageView = m_native->imageView;
        imageInfo.imageLayout = m_native->imageLayout;

        VkWriteDescriptorSet writes[2] = {};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = m_native->descriptorSet;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].pBufferInfo = &uniformInfo;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = m_native->descriptorSet;
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].pImageInfo = &imageInfo;

        m_native->f.vkUpdateDescriptorSets(m_native->device, 2, writes, 0, nullptr);
        m_native->descriptorDirty = false;
    }

    if (m_native->vertexShader == VK_NULL_HANDLE) {
        const ShaderSource source =
            loadSpirvShader(QStringLiteral(":/waylib/shaders/vulkansurface.vert.qsb"));
        if (!createShaderModule(m_native.get(), source, &m_native->vertexShader))
            return false;
        m_native->vertexEntryPoint = source.entryPoint;
    }

    if (m_native->fragmentShader == VK_NULL_HANDLE) {
        const ShaderSource source =
            loadSpirvShader(QStringLiteral(":/waylib/shaders/vulkansurface.frag.qsb"));
        if (!createShaderModule(m_native.get(), source, &m_native->fragmentShader))
            return false;
        m_native->fragmentEntryPoint = source.entryPoint;
    }

    if (m_native->pipelineLayout == VK_NULL_HANDLE) {
        VkPipelineLayoutCreateInfo layoutInfo = {};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &m_native->descriptorSetLayout;

        VkResult res = m_native->f.vkCreatePipelineLayout(m_native->device,
                                                          &layoutInfo,
                                                          nullptr,
                                                          &m_native->pipelineLayout);
        if (res != VK_SUCCESS) {
            qCWarning(lcWlVulkanCompositor)
                << "Vulkan surface render node failed to create pipeline layout"
                << vkResultName(res) << int(res);
            return false;
        }
    }

    auto *rpDesc = renderTarget() ? renderTarget()->renderPassDescriptor() : nullptr;
    const auto *rpHandles = rpDesc
        ? static_cast<const QRhiVulkanRenderPassNativeHandles *>(rpDesc->nativeHandles())
        : nullptr;
    const VkRenderPass renderPass = rpHandles ? rpHandles->renderPass : VK_NULL_HANDLE;
    const VkSampleCountFlagBits sampleCount =
        toVkSampleCount(renderTarget() ? renderTarget()->sampleCount() : 1);
    if (renderPass == VK_NULL_HANDLE) {
        qCWarning(lcWlVulkanCompositor)
            << "Vulkan surface render node cannot create pipeline: render pass unavailable";
        return false;
    }

    if (m_native->pipeline != VK_NULL_HANDLE
        && m_native->renderPass == renderPass
        && m_native->sampleCount == sampleCount) {
        return true;
    }

    if (m_native->pipeline != VK_NULL_HANDLE) {
        m_native->f.vkDestroyPipeline(m_native->device, m_native->pipeline, nullptr);
        m_native->pipeline = VK_NULL_HANDLE;
    }

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = m_native->vertexShader;
    stages[0].pName = m_native->vertexEntryPoint.constData();
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = m_native->fragmentShader;
    stages[1].pName = m_native->fragmentEntryPoint.constData();

    VkVertexInputBindingDescription binding = {};
    binding.binding = 0;
    binding.stride = sizeof(SurfaceVertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attributes[2] = {};
    attributes[0].location = 0;
    attributes[0].binding = 0;
    attributes[0].format = VK_FORMAT_R32G32_SFLOAT;
    attributes[0].offset = offsetof(SurfaceVertex, x);
    attributes[1].location = 1;
    attributes[1].binding = 0;
    attributes[1].format = VK_FORMAT_R32G32_SFLOAT;
    attributes[1].offset = offsetof(SurfaceVertex, u);

    VkPipelineVertexInputStateCreateInfo vertexInput = {};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = 2;
    vertexInput.pVertexAttributeDescriptions = attributes;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly = {};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;

    VkPipelineViewportStateCreateInfo viewportState = {};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterization = {};
    rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode = VK_CULL_MODE_NONE;
    rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample = {};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = sampleCount;

    VkPipelineColorBlendAttachmentState blendAttachment = {};
    blendAttachment.blendEnable = VK_TRUE;
    blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT
        | VK_COLOR_COMPONENT_G_BIT
        | VK_COLOR_COMPONENT_B_BIT
        | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo colorBlend = {};
    colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &blendAttachment;

    const VkDynamicState dynamicStates[2] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
    };
    VkPipelineDynamicStateCreateInfo dynamicState = {};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamicStates;

    VkGraphicsPipelineCreateInfo pipelineInfo = {};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterization;
    pipelineInfo.pMultisampleState = &multisample;
    pipelineInfo.pColorBlendState = &colorBlend;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = m_native->pipelineLayout;
    pipelineInfo.renderPass = renderPass;
    pipelineInfo.subpass = 0;

    VkResult res = m_native->f.vkCreateGraphicsPipelines(m_native->device,
                                                         VK_NULL_HANDLE,
                                                         1,
                                                         &pipelineInfo,
                                                         nullptr,
                                                         &m_native->pipeline);
    if (res != VK_SUCCESS) {
        qCWarning(lcWlVulkanCompositor)
            << "Vulkan surface render node failed to create native graphics pipeline"
            << vkResultName(res) << int(res)
            << "sampleCount" << sampleCount
            << "renderTargetSize" << (renderTarget() ? renderTarget()->pixelSize() : QSize());
        m_native->pipeline = VK_NULL_HANDLE;
        return false;
    }

    m_native->renderPass = renderPass;
    m_native->sampleCount = sampleCount;
    qCDebug(lcWlVulkanCompositor)
        << "Vulkan surface render node native graphics pipeline created"
        << "buffer" << pointerAddress(m_buffer)
        << "surfaceCommitSeq" << m_surfaceCommitSeq
        << "renderTargetSize" << (renderTarget() ? renderTarget()->pixelSize() : QSize())
        << "sampleCount" << sampleCount
        << "hasAlpha" << m_importedTexture.hasAlpha
        << "forceOpaqueInShader" << !m_importedTexture.hasAlpha;
    return true;
}

void WVulkanSurfaceRenderNode::releaseImport()
{
    resetNativeImageResources();
    WRenderHelper::releaseImportedVulkanTexture(&m_importedTexture);
}

void WVulkanSurfaceRenderNode::resetNativeImageResources()
{
    if (!m_native)
        return;

    if (m_native->imageView != VK_NULL_HANDLE && m_native->f.vkDestroyImageView)
        m_native->f.vkDestroyImageView(m_native->device, m_native->imageView, nullptr);

    m_native->image = VK_NULL_HANDLE;
    m_native->imageFormat = VK_FORMAT_UNDEFINED;
    m_native->imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    m_native->imageView = VK_NULL_HANDLE;
    m_native->descriptorDirty = true;
}

void WVulkanSurfaceRenderNode::resetNativeResources()
{
    if (!m_native)
        return;

    resetNativeImageResources();

    if (m_native->pipeline != VK_NULL_HANDLE && m_native->f.vkDestroyPipeline)
        m_native->f.vkDestroyPipeline(m_native->device, m_native->pipeline, nullptr);
    if (m_native->pipelineLayout != VK_NULL_HANDLE && m_native->f.vkDestroyPipelineLayout)
        m_native->f.vkDestroyPipelineLayout(m_native->device, m_native->pipelineLayout, nullptr);
    if (m_native->descriptorPool != VK_NULL_HANDLE && m_native->f.vkDestroyDescriptorPool)
        m_native->f.vkDestroyDescriptorPool(m_native->device, m_native->descriptorPool, nullptr);
    if (m_native->descriptorSetLayout != VK_NULL_HANDLE && m_native->f.vkDestroyDescriptorSetLayout)
        m_native->f.vkDestroyDescriptorSetLayout(m_native->device, m_native->descriptorSetLayout, nullptr);
    if (m_native->sampler != VK_NULL_HANDLE && m_native->f.vkDestroySampler)
        m_native->f.vkDestroySampler(m_native->device, m_native->sampler, nullptr);
    if (m_native->vertexShader != VK_NULL_HANDLE && m_native->f.vkDestroyShaderModule)
        m_native->f.vkDestroyShaderModule(m_native->device, m_native->vertexShader, nullptr);
    if (m_native->fragmentShader != VK_NULL_HANDLE && m_native->f.vkDestroyShaderModule)
        m_native->f.vkDestroyShaderModule(m_native->device, m_native->fragmentShader, nullptr);

    destroyNativeBuffer(m_native.get(), &m_native->vertexBuffer);
    destroyNativeBuffer(m_native.get(), &m_native->uniformBuffer);

    m_native.reset();
}

void WVulkanSurfaceRenderNode::resetAll()
{
    resetNativeResources();
    WRenderHelper::releaseImportedVulkanTexture(&m_importedTexture);
    m_importDirty = true;
    m_verticesDirty = true;
    m_samplerDirty = true;
    m_importFailed = false;
    m_loggedPrepareState = false;
    m_loggedRenderState = false;
}

WAYLIB_SERVER_END_NAMESPACE
