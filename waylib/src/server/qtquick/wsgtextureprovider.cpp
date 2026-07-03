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
#include <QMutex>
#include <QMutexLocker>
#include <QVector>
#include <rhi/qrhi.h>
#include <private/qsgplaintexture_p.h>

#include <limits>

#ifdef ENABLE_VULKAN_RENDER
extern "C" {
#include <wlr/render/dmabuf.h>
}
#include <drm_fourcc.h>
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
static constexpr int s_deferredVulkanTextureReleaseFrames = 3;

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
    return buffer->get_dmabuf(&dmabuf);
}

static qsizetype envMiBValue(const char *waylibName, const char *treelandName,
                             qsizetype defaultMiB)
{
    const char *names[] = { waylibName, treelandName };
    for (const char *name : names) {
        const QByteArray value = qgetenv(name).trimmed();
        if (value.isEmpty())
            continue;

        bool ok = false;
        const qlonglong mib = value.toLongLong(&ok);
        if (ok && mib > 0)
            return qsizetype(mib) * 1024 * 1024;
    }

    return defaultMiB * 1024 * 1024;
}

static int envIntValue(const char *waylibName, const char *treelandName,
                       int defaultValue)
{
    const char *names[] = { waylibName, treelandName };
    for (const char *name : names) {
        const QByteArray value = qgetenv(name).trimmed();
        if (value.isEmpty())
            continue;

        bool ok = false;
        const int parsed = value.toInt(&ok);
        if (ok && parsed > 0)
            return parsed;
    }

    return defaultValue;
}

static qsizetype drmFormatEstimatedBytesPerPixel(uint32_t format)
{
    switch (format) {
    case DRM_FORMAT_R8:
        return 1;
    case DRM_FORMAT_GR88:
    case DRM_FORMAT_RG88:
    case DRM_FORMAT_RGB565:
    case DRM_FORMAT_BGR565:
        return 2;
    case DRM_FORMAT_XRGB8888:
    case DRM_FORMAT_ARGB8888:
    case DRM_FORMAT_XBGR8888:
    case DRM_FORMAT_ABGR8888:
    case DRM_FORMAT_RGBX8888:
    case DRM_FORMAT_RGBA8888:
    case DRM_FORMAT_BGRX8888:
    case DRM_FORMAT_BGRA8888:
    case DRM_FORMAT_XRGB2101010:
    case DRM_FORMAT_ARGB2101010:
    case DRM_FORMAT_XBGR2101010:
    case DRM_FORMAT_ABGR2101010:
        return 4;
    default:
        // Most desktop client buffers are 32-bit ARGB/XRGB. Prefer a
        // conservative estimate so the cache trims earlier for uncommon formats.
        return 4;
    }
}

static qsizetype estimatedDmabufCost(const wlr_dmabuf_attributes &dmabuf)
{
    if (dmabuf.width <= 0 || dmabuf.height <= 0)
        return 0;

    return qsizetype(dmabuf.width)
        * qsizetype(dmabuf.height)
        * drmFormatEstimatedBytesPerPixel(dmabuf.format);
}

static bool directDmabufImportEnabled()
{
    static const bool enabled = !envFlagExplicitlyDisabled("WAYLIB_VK_DIRECT_DMABUF")
        && !envFlagExplicitlyDisabled("TREELAND_VK_DIRECT_DMABUF")
        && !envFlagExplicitlyDisabled("WAYLIB_VK_DIRECT_CLIENT_DMABUF")
        && !envFlagExplicitlyDisabled("TREELAND_VK_DIRECT_CLIENT_DMABUF");
    return enabled;
}

struct VulkanDmabufTextureCacheStats {
    qsizetype cachedBytes = 0;
    qsizetype budgetBytes = 0;
    int entries = 0;
    int maxEntries = 0;
    int activeRefs = 0;
    int deferredReleases = 0;
};

struct VulkanSharedDmabufTexture {
    QRhi *rhi = nullptr;
    QPointer<WOutputRenderWindow> window;
    QPointer<qw_buffer> buffer;
    QRhiTexture *rhiTexture = nullptr;
    WRenderHelper::NativeTextureCleanup nativeCleanup;
    QSize size;
    uint32_t drmFormat = 0;
    uint64_t drmModifier = 0;
    bool hasAlpha = false;
    quint32 lastContentSerial = 0;
    bool lastAcquireCacheHit = false;
    bool lastAcquireReacquired = false;
    qsizetype costBytes = 0;
    int refCount = 0;
    quint64 lastUsed = 0;
};

class Q_DECL_HIDDEN VulkanDmabufTextureCache
{
public:
    ~VulkanDmabufTextureCache()
    {
        QVector<WRenderHelper::NativeTextureCleanup> cleanups;
        {
            QMutexLocker locker(&m_mutex);
            for (auto *entry : std::as_const(m_entries)) {
                if (entry->rhiTexture)
                    entry->rhiTexture->deleteLater();
                if (entry->nativeCleanup.type != WRenderHelper::NativeTextureCleanup::Type::None)
                    cleanups.append(entry->nativeCleanup);
                delete entry;
            }
            m_entries.clear();
            for (const auto &release : std::as_const(m_deferredReleases)) {
                if (release.nativeCleanup.type != WRenderHelper::NativeTextureCleanup::Type::None)
                    cleanups.append(release.nativeCleanup);
            }
            m_deferredReleases.clear();
            m_cachedBytes = 0;
        }

        for (auto cleanup : cleanups)
            WRenderHelper::releaseNativeTexture(&cleanup);
    }

    VulkanSharedDmabufTexture *acquire(QRhi *rhi, WOutputRenderWindow *window,
                                       qw_buffer *buffer, wlr_surface *surface,
                                       quint32 contentSerial,
                                       bool allowOverBudgetImport)
    {
        if (!rhi || !window || !buffer)
            return nullptr;

        wlr_dmabuf_attributes dmabuf = {};
        if (!buffer->get_dmabuf(&dmabuf))
            return nullptr;

        ensureRenderEndConnection(window);

        if (auto *entry = acquireExisting(rhi, buffer, dmabuf, surface, contentSerial))
            return entry;

        const qsizetype costBytes = estimatedDmabufCost(dmabuf);
        if (!prepareForImport(costBytes, allowOverBudgetImport, buffer, dmabuf))
            return nullptr;

        WRenderHelper::ImportedVulkanTexture importedTexture;
        if (!WRenderHelper::importVulkanTextureFromBuffer(rhi, buffer, surface,
                                                          &importedTexture)) {
            return nullptr;
        }

        auto *entry = new VulkanSharedDmabufTexture;
        entry->rhi = rhi;
        entry->window = window;
        entry->buffer = buffer;
        entry->rhiTexture = importedTexture.texture;
        entry->nativeCleanup = importedTexture.nativeCleanup;
        entry->size = importedTexture.size;
        entry->drmFormat = importedTexture.drmFormat;
        entry->drmModifier = importedTexture.drmModifier;
        entry->hasAlpha = importedTexture.hasAlpha;
        entry->lastContentSerial = contentSerial;
        entry->lastAcquireCacheHit = false;
        entry->lastAcquireReacquired = true;
        entry->costBytes = costBytes;
        entry->refCount = 1;
        importedTexture.texture = nullptr;
        importedTexture.nativeCleanup = {};

        VulkanDmabufTextureCacheStats stats;
        {
            QMutexLocker locker(&m_mutex);
            pruneInvalidLocked();

            if (auto *existing = findEntryLocked(rhi, buffer, dmabuf)) {
                ++existing->refCount;
                existing->lastUsed = ++m_useSerial;
                stats = statsLocked();
                locker.unlock();
                WRenderHelper::releaseImportedVulkanTexture(&importedTexture);
                deleteImportedEntry(entry);
                if (acquireImportedEntry(existing, rhi, buffer, surface, contentSerial))
                    return existing;
                release(existing);
                return nullptr;
            }

            entry->lastUsed = ++m_useSerial;
            m_entries.append(entry);
            m_cachedBytes += entry->costBytes;
            pruneOverBudgetLocked(0);
            stats = statsLocked();
        }

        qCDebug(lcWlQtQuickTexture)
            << "Vulkan client dmabuf texture cached"
            << "buffer" << pointerAddress(buffer)
            << "rhiTexture" << entry->rhiTexture
            << "size" << entry->size
            << "format" << Qt::hex << entry->drmFormat << Qt::dec
            << "modifier" << Qt::hex << entry->drmModifier << Qt::dec
            << "entryBytes" << entry->costBytes
            << "cacheBytes" << stats.cachedBytes
            << "budgetBytes" << stats.budgetBytes
            << "entries" << stats.entries
            << "activeRefs" << stats.activeRefs;

        return entry;
    }

    void release(VulkanSharedDmabufTexture *entry)
    {
        if (!entry)
            return;

        QMutexLocker locker(&m_mutex);
        if (entry->refCount > 0)
            --entry->refCount;
        entry->lastUsed = ++m_useSerial;
        pruneInvalidLocked();
        pruneOverBudgetLocked(0);
    }

    void processRenderEnd(WOutputRenderWindow *window)
    {
        processDeferredReleases(window, false);

        QMutexLocker locker(&m_mutex);
        pruneInvalidLocked();
        pruneOverBudgetLocked(0);
    }

    void forceProcessDeferredReleases(WOutputRenderWindow *window)
    {
        processDeferredReleases(window, true);
    }

    void releaseWindow(WOutputRenderWindow *window)
    {
        QVector<WRenderHelper::NativeTextureCleanup> cleanups;
        {
            QMutexLocker locker(&m_mutex);
            for (int i = m_entries.size() - 1; i >= 0; --i) {
                auto *entry = m_entries[i];
                if (entry->window && entry->window != window)
                    continue;

                if (entry->refCount > 0) {
                    qCWarning(lcWlQtQuickTexture)
                        << "Vulkan client dmabuf cache kept active entry for destroyed window"
                        << "window" << pointerAddress(window)
                        << "buffer" << pointerAddress(entry->buffer.data())
                        << "rhiTexture" << entry->rhiTexture
                        << "refs" << entry->refCount;
                    continue;
                }

                queueDestroyEntryLocked(i, "window-destroyed", 0);
            }

            for (int i = m_deferredReleases.size() - 1; i >= 0; --i) {
                const auto &release = m_deferredReleases[i];
                if (release.window && release.window != window)
                    continue;
                cleanups.append(release.nativeCleanup);
                m_deferredReleases.removeAt(i);
            }

            for (int i = m_connectedWindows.size() - 1; i >= 0; --i) {
                if (!m_connectedWindows[i] || m_connectedWindows[i] == window)
                    m_connectedWindows.removeAt(i);
            }
        }

        for (auto cleanup : cleanups)
            WRenderHelper::releaseNativeTexture(&cleanup);
    }

    VulkanDmabufTextureCacheStats stats() const
    {
        QMutexLocker locker(&m_mutex);
        return statsLocked();
    }

    void ensureRenderEndConnection(WOutputRenderWindow *window);

private:
    struct DeferredRelease {
        QPointer<WOutputRenderWindow> window;
        WRenderHelper::NativeTextureCleanup nativeCleanup;
        int framesLeft = 0;
        qsizetype costBytes = 0;
    };

    VulkanSharedDmabufTexture *acquireExisting(QRhi *rhi, qw_buffer *buffer,
                                               const wlr_dmabuf_attributes &dmabuf,
                                               wlr_surface *surface,
                                               quint32 contentSerial)
    {
        VulkanSharedDmabufTexture *entry = nullptr;
        {
            QMutexLocker locker(&m_mutex);
            pruneInvalidLocked();
            entry = findEntryLocked(rhi, buffer, dmabuf);
            if (!entry)
                return nullptr;

            ++entry->refCount;
            entry->lastUsed = ++m_useSerial;
        }

        if (!acquireImportedEntry(entry, rhi, buffer, surface, contentSerial)) {
            release(entry);
            return nullptr;
        }

        return entry;
    }

    bool acquireImportedEntry(VulkanSharedDmabufTexture *entry, QRhi *rhi,
                              qw_buffer *buffer, wlr_surface *surface,
                              quint32 contentSerial)
    {
        if (!entry)
            return false;

        entry->lastAcquireCacheHit = true;
        entry->lastAcquireReacquired = false;

        if (contentSerial != 0 && entry->lastContentSerial == contentSerial)
            return true;

        if (!WRenderHelper::acquireImportedVulkanTextureFromBuffer(rhi, buffer,
                                                                   surface,
                                                                   &entry->nativeCleanup)) {
            return false;
        }

        entry->lastContentSerial = contentSerial;
        entry->lastAcquireReacquired = true;
        return true;
    }

    VulkanSharedDmabufTexture *findEntryLocked(QRhi *rhi, qw_buffer *buffer,
                                               const wlr_dmabuf_attributes &dmabuf) const
    {
        const QSize size(dmabuf.width, dmabuf.height);
        for (auto *entry : m_entries) {
            if (entry->rhi == rhi
                && entry->buffer == buffer
                && entry->size == size
                && entry->drmFormat == dmabuf.format
                && entry->drmModifier == dmabuf.modifier) {
                return entry;
            }
        }

        return nullptr;
    }

    bool prepareForImport(qsizetype costBytes, bool allowOverBudgetImport,
                          qw_buffer *buffer, const wlr_dmabuf_attributes &dmabuf)
    {
        QMutexLocker locker(&m_mutex);
        pruneInvalidLocked();
        pruneOverBudgetLocked(costBytes);

        const bool entryLimitReached = m_maxEntries > 0
            && m_entries.size() + 1 > m_maxEntries;
        const bool budgetReached = m_budgetBytes > 0
            && m_cachedBytes + costBytes > m_budgetBytes;

        if (entryLimitReached || (budgetReached && !allowOverBudgetImport)) {
            qCDebug(lcWlQtQuickTexture)
                << "Vulkan client dmabuf texture import throttled by cache budget"
                << "buffer" << pointerAddress(buffer)
                << "size" << QSize(dmabuf.width, dmabuf.height)
                << "format" << Qt::hex << dmabuf.format << Qt::dec
                << "modifier" << Qt::hex << dmabuf.modifier << Qt::dec
                << "entryBytes" << costBytes
                << "cacheBytes" << m_cachedBytes
                << "budgetBytes" << m_budgetBytes
                << "entries" << m_entries.size()
                << "maxEntries" << m_maxEntries
                << "allowOverBudgetImport" << allowOverBudgetImport;
            return false;
        }

        return true;
    }

    void pruneInvalidLocked()
    {
        for (int i = m_entries.size() - 1; i >= 0; --i) {
            const auto *entry = m_entries[i];
            if (entry->refCount == 0 && !entry->buffer)
                queueDestroyEntryLocked(i, "buffer-destroyed");
        }
    }

    void pruneOverBudgetLocked(qsizetype incomingCost)
    {
        while (true) {
            const bool overEntryLimit = m_maxEntries > 0
                && m_entries.size() + (incomingCost > 0 ? 1 : 0) > m_maxEntries;
            const bool overBudget = m_budgetBytes > 0
                && m_cachedBytes + incomingCost > m_budgetBytes;
            if (!overEntryLimit && !overBudget)
                return;

            int evictIndex = -1;
            quint64 oldestUse = std::numeric_limits<quint64>::max();
            for (int i = 0; i < m_entries.size(); ++i) {
                const auto *entry = m_entries[i];
                if (entry->refCount > 0)
                    continue;

                if (entry->lastUsed < oldestUse) {
                    oldestUse = entry->lastUsed;
                    evictIndex = i;
                }
            }

            if (evictIndex < 0)
                return;

            queueDestroyEntryLocked(evictIndex, "over-budget");
        }
    }

    void queueDestroyEntryLocked(int index, const char *reason,
                                 int framesLeft = s_deferredVulkanTextureReleaseFrames)
    {
        if (index < 0 || index >= m_entries.size())
            return;

        auto *entry = m_entries.takeAt(index);
        m_cachedBytes -= entry->costBytes;

        qCDebug(lcWlQtQuickTexture)
            << "Vulkan client dmabuf texture cache evicted"
            << "reason" << reason
            << "buffer" << pointerAddress(entry->buffer.data())
            << "rhiTexture" << entry->rhiTexture
            << "size" << entry->size
            << "format" << Qt::hex << entry->drmFormat << Qt::dec
            << "modifier" << Qt::hex << entry->drmModifier << Qt::dec
            << "entryBytes" << entry->costBytes
            << "remainingBytes" << m_cachedBytes
            << "remainingEntries" << m_entries.size();

        if (entry->rhiTexture)
            entry->rhiTexture->deleteLater();

        if (entry->nativeCleanup.type != WRenderHelper::NativeTextureCleanup::Type::None) {
            m_deferredReleases.append({
                entry->window,
                entry->nativeCleanup,
                framesLeft,
                entry->costBytes,
            });
            entry->nativeCleanup = {};
        }

        delete entry;
    }

    void deleteImportedEntry(VulkanSharedDmabufTexture *entry)
    {
        if (!entry)
            return;

        WRenderHelper::ImportedVulkanTexture imported;
        imported.texture = entry->rhiTexture;
        imported.nativeCleanup = entry->nativeCleanup;
        WRenderHelper::releaseImportedVulkanTexture(&imported);
        delete entry;
    }

    void processDeferredReleases(WOutputRenderWindow *window, bool force)
    {
        QVector<WRenderHelper::NativeTextureCleanup> cleanups;
        {
            QMutexLocker locker(&m_mutex);
            for (int i = m_deferredReleases.size() - 1; i >= 0; --i) {
                auto &release = m_deferredReleases[i];
                if (window && release.window != window)
                    continue;

                if (!force && --release.framesLeft > 0)
                    continue;

                cleanups.append(release.nativeCleanup);
                m_deferredReleases.removeAt(i);
            }
        }

        for (auto cleanup : cleanups)
            WRenderHelper::releaseNativeTexture(&cleanup);
    }

    VulkanDmabufTextureCacheStats statsLocked() const
    {
        VulkanDmabufTextureCacheStats stats;
        stats.cachedBytes = m_cachedBytes;
        stats.budgetBytes = m_budgetBytes;
        stats.entries = m_entries.size();
        stats.maxEntries = m_maxEntries;
        stats.deferredReleases = m_deferredReleases.size();
        for (const auto *entry : m_entries)
            stats.activeRefs += entry->refCount;
        return stats;
    }

    mutable QMutex m_mutex;
    QVector<VulkanSharedDmabufTexture *> m_entries;
    QVector<DeferredRelease> m_deferredReleases;
    QVector<QPointer<WOutputRenderWindow>> m_connectedWindows;
    qsizetype m_cachedBytes = 0;
    quint64 m_useSerial = 0;
    const qsizetype m_budgetBytes =
        envMiBValue("WAYLIB_VK_CLIENT_DMABUF_CACHE_MB",
                    "TREELAND_VK_CLIENT_DMABUF_CACHE_MB",
                    512);
    const int m_maxEntries =
        envIntValue("WAYLIB_VK_CLIENT_DMABUF_CACHE_ENTRIES",
                    "TREELAND_VK_CLIENT_DMABUF_CACHE_ENTRIES",
                    128);
};

Q_GLOBAL_STATIC(VulkanDmabufTextureCache, s_vulkanDmabufTextureCache)

void VulkanDmabufTextureCache::ensureRenderEndConnection(WOutputRenderWindow *window)
{
    if (!window)
        return;

    bool needsConnection = false;
    {
        QMutexLocker locker(&m_mutex);
        for (int i = m_connectedWindows.size() - 1; i >= 0; --i) {
            if (!m_connectedWindows[i]) {
                m_connectedWindows.removeAt(i);
                continue;
            }

            if (m_connectedWindows[i] == window)
                return;
        }

        m_connectedWindows.append(window);
        needsConnection = true;
    }

    if (!needsConnection)
        return;

    QObject::connect(window, &WOutputRenderWindow::renderEnd,
                     window, [window] {
                         s_vulkanDmabufTextureCache->processRenderEnd(window);
                     });
    QObject::connect(window, &QObject::destroyed,
                     window, [window] {
                         s_vulkanDmabufTextureCache->releaseWindow(window);
                     });
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
#ifdef ENABLE_VULKAN_RENDER
        processDeferredVulkanTextureReleases(true);
#endif
    }

    bool isVulkanRenderer() const {
#ifdef ENABLE_VULKAN_RENDER
        return window && window->renderer()
            && window->renderer()->is_vk();
#else
        return false;
#endif
    }

#ifdef ENABLE_VULKAN_RENDER
    struct DeferredVulkanTextureRelease {
        WRenderHelper::NativeTextureCleanup nativeCleanup;
        int framesLeft = 0;
    };

    void ensureDeferredVulkanTextureReleaseConnection()
    {
        if (deferredVulkanTextureReleaseConnection || !window)
            return;

        W_Q(WSGTextureProvider);
        deferredVulkanTextureReleaseConnection =
            QObject::connect(window, &WOutputRenderWindow::renderEnd,
                             q, [this] {
                                 processDeferredVulkanTextureReleases(false);
                             });
    }

    void processDeferredVulkanTextureReleases(bool force)
    {
        if (force && window && window->rhi())
            window->rhi()->finish();

        for (int i = deferredVulkanTextureReleases.size() - 1; i >= 0; --i) {
            auto &release = deferredVulkanTextureReleases[i];
            if (!force && --release.framesLeft > 0)
                continue;

            auto cleanup = release.nativeCleanup;
            deferredVulkanTextureReleases.removeAt(i);
            WRenderHelper::releaseNativeTexture(&cleanup);
        }

        if (force)
            s_vulkanDmabufTextureCache->forceProcessDeferredReleases(window);
    }

    void deferVulkanNativeTextureRelease(WRenderHelper::NativeTextureCleanup *cleanup,
                                         QRhiTexture *rhiTexture)
    {
        if (!cleanup || cleanup->type != WRenderHelper::NativeTextureCleanup::Type::VulkanTexture)
            return;

        if (rhiTexture)
            rhiTexture->deleteLater();

        deferredVulkanTextureReleases.append({
            *cleanup,
            s_deferredVulkanTextureReleaseFrames,
        });
        qCDebug(lcWlQtQuickTexture)
            << "Vulkan renderer deferred old client texture native release"
            << "rhiTexture" << rhiTexture
            << "frames" << s_deferredVulkanTextureReleaseFrames
            << "pending" << deferredVulkanTextureReleases.size();

        *cleanup = {};
        ensureDeferredVulkanTextureReleaseConnection();
    }

    void releaseActiveVulkanDmabufTexture()
    {
        if (!activeVulkanDmabufTexture)
            return;

        s_vulkanDmabufTextureCache->release(activeVulkanDmabufTexture);
        activeVulkanDmabufTexture = nullptr;
    }

    void activateSharedVulkanDmabufTexture(VulkanSharedDmabufTexture *entry,
                                           qw_buffer *newBuffer,
                                           quint32 newBufferContentSerial,
                                           const char *backendName,
                                           bool cacheHit,
                                           bool reacquired)
    {
        auto oldTexture = texture;
        const bool oldOwnsTexture = ownsTexture;
        auto oldRhiTexture = activeVulkanDmabufTexture ? nullptr : rhiTexture;
        auto oldNativeCleanup = activeVulkanDmabufTexture
            ? WRenderHelper::NativeTextureCleanup{}
            : nativeCleanup;
        auto *oldActiveVulkanDmabufTexture = activeVulkanDmabufTexture;

        texture = nullptr;
        ownsTexture = false;
        buffer = newBuffer;
        bufferContentSerial = newBufferContentSerial;
        qtTexture.setOwnsTexture(false);
        qtTexture.setTexture(entry->rhiTexture);
        qtTexture.setHasAlphaChannel(entry->hasAlpha);
        qtTexture.setTextureSize(entry->size);
        rhiTexture = entry->rhiTexture;
        hasPendingImageTexture = false;
        nativeCleanup = {};
        if (entry == oldActiveVulkanDmabufTexture) {
            s_vulkanDmabufTextureCache->release(entry);
        } else {
            activeVulkanDmabufTexture = entry;
        }

        const auto stats = s_vulkanDmabufTextureCache->stats();

        qCDebug(lcWlQtQuickTexture)
            << "Vulkan renderer dmabuf texture import path"
            << "qtBackend" << backendName
            << "cacheHit" << cacheHit
            << "reacquired" << reacquired
            << "buffer" << pointerAddress(newBuffer)
            << "contentSerial" << newBufferContentSerial
            << "rhiTexture" << rhiTexture
            << "size" << entry->size
            << "format" << Qt::hex << entry->drmFormat << Qt::dec
            << "modifier" << Qt::hex << entry->drmModifier << Qt::dec
            << "locks" << bufferLocks(newBuffer)
            << "alpha" << qtTexture.hasAlphaChannel()
            << "entryBytes" << entry->costBytes
            << "cacheBytes" << stats.cachedBytes
            << "budgetBytes" << stats.budgetBytes
            << "cacheEntries" << stats.entries
            << "activeRefs" << stats.activeRefs
            << "deferredReleases" << stats.deferredReleases;

        releaseDetachedTexture(oldTexture, oldOwnsTexture, oldRhiTexture, oldNativeCleanup);
        if (oldActiveVulkanDmabufTexture && oldActiveVulkanDmabufTexture != entry)
            s_vulkanDmabufTextureCache->release(oldActiveVulkanDmabufTexture);
        ensureDeferredVulkanTextureReleaseConnection();
    }
#endif

    void releaseDetachedTexture(qw_texture *texture, bool ownsTexture,
                                QRhiTexture *rhiTexture,
                                WRenderHelper::NativeTextureCleanup nativeCleanup)
    {
#ifdef ENABLE_VULKAN_RENDER
        if (nativeCleanup.type == WRenderHelper::NativeTextureCleanup::Type::VulkanTexture) {
            deferVulkanNativeTextureRelease(&nativeCleanup, rhiTexture);
        } else
#endif
        {
            if (nativeCleanup.type != WRenderHelper::NativeTextureCleanup::Type::None)
                delete rhiTexture;
            WRenderHelper::releaseNativeTexture(&nativeCleanup);
        }

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
        if (rhiTexture || hasPendingImageTexture) {
            qtTexture.setTexture(nullptr);
            rhiTexture = nullptr;
        }
        hasPendingImageTexture = false;
        releaseDetachedTexture(nullptr, false, importedWrapper, nativeCleanup);
        nativeCleanup = {};

        if (ownsTexture && texture)
            delete texture;
        texture = nullptr;
        ownsTexture = false;
        buffer = nullptr;
        bufferContentSerial = 0;
#ifdef ENABLE_VULKAN_RENDER
        releaseActiveVulkanDmabufTexture();
#endif
    }

    bool replaceTexture(qw_texture *newTexture, bool newOwnsTexture, qw_buffer *newBuffer,
                        bool allowBufferDirectImport = true, wlr_surface *surface = nullptr,
                        QRhiCommandBuffer *commandBuffer = nullptr,
                        quint32 newBufferContentSerial = 0) {
        Q_ASSERT(newTexture);

        WRenderHelper::NativeTextureCleanup newCleanup;
        if (!WRenderHelper::makeTexture(window->rhi(), newTexture, &qtTexture,
                                        newBuffer, &newCleanup, allowBufferDirectImport,
                                        surface, commandBuffer)) {
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
#ifdef ENABLE_VULKAN_RENDER
        auto oldRhiTexture = activeVulkanDmabufTexture ? nullptr : rhiTexture;
        auto oldNativeCleanup = activeVulkanDmabufTexture
            ? WRenderHelper::NativeTextureCleanup{}
            : nativeCleanup;
        auto *oldActiveVulkanDmabufTexture = activeVulkanDmabufTexture;
#else
        auto oldRhiTexture = rhiTexture;
        auto oldNativeCleanup = nativeCleanup;
#endif

        texture = newTexture;
        ownsTexture = newOwnsTexture;
        buffer = newBuffer;
        bufferContentSerial = newBufferContentSerial;
        rhiTexture = qtTexture.rhiTexture();
        hasPendingImageTexture = !rhiTexture && !qtTexture.textureSize().isEmpty();
        nativeCleanup = newCleanup;
#ifdef ENABLE_VULKAN_RENDER
        activeVulkanDmabufTexture = nullptr;
#endif

        releaseDetachedTexture(oldTexture == newTexture ? nullptr : oldTexture,
                               oldTexture == newTexture ? false : oldOwnsTexture,
                               oldRhiTexture,
                               oldNativeCleanup);
#ifdef ENABLE_VULKAN_RENDER
        if (oldActiveVulkanDmabufTexture)
            s_vulkanDmabufTextureCache->release(oldActiveVulkanDmabufTexture);
#endif
        return true;
    }

    bool replaceBufferWithDirectDmabufTexture(qw_buffer *newBuffer, wlr_surface *surface,
                                             quint32 newBufferContentSerial)
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
            const bool allowOverBudgetImport = !hasTexture();
            auto *entry = s_vulkanDmabufTextureCache->acquire(window->rhi(), window,
                                                              newBuffer, surface,
                                                              newBufferContentSerial,
                                                              allowOverBudgetImport);
            if (entry) {
                activateSharedVulkanDmabufTexture(entry, newBuffer,
                                                  newBufferContentSerial,
                                                  backendName,
                                                  entry->lastAcquireCacheHit,
                                                  entry->lastAcquireReacquired);
                return true;
            }
        }

        if (!imported) {
            return false;
        }

        auto oldTexture = texture;
        const bool oldOwnsTexture = ownsTexture;
        auto oldRhiTexture = activeVulkanDmabufTexture ? nullptr : rhiTexture;
        auto oldNativeCleanup = activeVulkanDmabufTexture
            ? WRenderHelper::NativeTextureCleanup{}
            : nativeCleanup;
        auto *oldActiveVulkanDmabufTexture = activeVulkanDmabufTexture;

        texture = nullptr;
        ownsTexture = false;
        buffer = newBuffer;
        bufferContentSerial = newBufferContentSerial;
        rhiTexture = qtTexture.rhiTexture();
        hasPendingImageTexture = false;
        nativeCleanup = newCleanup;
        activeVulkanDmabufTexture = nullptr;

        qCDebug(lcWlQtQuickTexture)
            << "Vulkan renderer dmabuf texture import path"
            << "qtBackend" << backendName
            << "buffer" << pointerAddress(newBuffer)
            << "size" << bufferSize(newBuffer)
            << "locks" << bufferLocks(newBuffer)
            << "alpha" << qtTexture.hasAlphaChannel();

        releaseDetachedTexture(oldTexture, oldOwnsTexture, oldRhiTexture, oldNativeCleanup);
        if (oldActiveVulkanDmabufTexture)
            s_vulkanDmabufTextureCache->release(oldActiveVulkanDmabufTexture);
        return true;
#else
        Q_UNUSED(newBuffer);
        Q_UNUSED(surface);
        Q_UNUSED(newBufferContentSerial);
        return false;
#endif
    }

    bool replaceBufferWithNonDmabufUpload(qw_buffer *newBuffer,
                                          wlr_surface *surface,
                                          QRhiCommandBuffer *commandBuffer,
                                          quint32 newBufferContentSerial)
    {
#ifdef ENABLE_VULKAN_RENDER
        if (!newBuffer || !window || !window->rhi()
            || window->rhi()->backend() != QRhi::Vulkan
            || bufferHasDmabuf(newBuffer)) {
            return false;
        }

        if (!WRenderHelper::makeVulkanTextureFromNonDmabufBuffer(window->rhi(),
                                                                 newBuffer,
                                                                 &qtTexture,
                                                                 surface,
                                                                 commandBuffer)) {
            return false;
        }

        auto oldTexture = texture;
        const bool oldOwnsTexture = ownsTexture;
        auto oldRhiTexture = activeVulkanDmabufTexture ? nullptr : rhiTexture;
        auto oldNativeCleanup = activeVulkanDmabufTexture
            ? WRenderHelper::NativeTextureCleanup{}
            : nativeCleanup;
        auto *oldActiveVulkanDmabufTexture = activeVulkanDmabufTexture;

        texture = nullptr;
        ownsTexture = false;
        buffer = newBuffer;
        bufferContentSerial = newBufferContentSerial;
        rhiTexture = qtTexture.rhiTexture();
        hasPendingImageTexture = !rhiTexture && !qtTexture.textureSize().isEmpty();
        nativeCleanup = {};
        activeVulkanDmabufTexture = nullptr;

        qCDebug(lcWlQtQuickTexture)
            << "Vulkan renderer non-dmabuf buffer uploaded without wlroots texture wrapper"
            << "buffer" << pointerAddress(newBuffer)
            << "contentSerial" << newBufferContentSerial
            << "size" << bufferSize(newBuffer)
            << "locks" << bufferLocks(newBuffer)
            << "rhiTexture" << rhiTexture
            << "alpha" << qtTexture.hasAlphaChannel()
            << "commandBuffer" << pointerAddress(commandBuffer);

        releaseDetachedTexture(oldTexture, oldOwnsTexture, oldRhiTexture, oldNativeCleanup);
        if (oldActiveVulkanDmabufTexture)
            s_vulkanDmabufTextureCache->release(oldActiveVulkanDmabufTexture);
        return true;
#else
        Q_UNUSED(newBuffer);
        Q_UNUSED(surface);
        Q_UNUSED(commandBuffer);
        Q_UNUSED(newBufferContentSerial);
        return false;
#endif
    }

    bool hasTexture() const
    {
        return texture || rhiTexture || hasPendingImageTexture;
    }

    void updateRhiTexture(QRhiCommandBuffer *commandBuffer = nullptr) {
        Q_ASSERT(texture);
        // NOTE: We cannot cache by wlr_texture* alone: wlroots may reuse the
        // same texture object (via wlr_client_buffer_apply_damage) while updating
        // its contents. Callers should re-run makeTexture for real buffer updates,
        // but reuse the provider texture for pure scene graph animation frames.
        WRenderHelper::NativeTextureCleanup newCleanup;
        bool ok = WRenderHelper::makeTexture(window->rhi(), texture, &qtTexture,
                                             buffer, &newCleanup, true, nullptr,
                                             commandBuffer);
        if (Q_UNLIKELY(!ok)) {
            WRenderHelper::releaseNativeTexture(&newCleanup);
            qCWarning(lcWlQtQuickTexture)
                << "Failed to make texture"
                << "texture" << pointerAddress(texture)
                << "size" << texture->handle()->width << "x" << texture->handle()->height;
            return;
        }

        rhiTexture = qtTexture.rhiTexture();
        hasPendingImageTexture = !rhiTexture && !qtTexture.textureSize().isEmpty();
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
    bool hasPendingImageTexture = false;
    bool smooth = true;
    bool directBufferImportAllowed = false;
    quint32 bufferContentSerial = 0;
#ifdef ENABLE_VULKAN_RENDER
    QVector<DeferredVulkanTextureRelease> deferredVulkanTextureReleases;
    VulkanSharedDmabufTexture *activeVulkanDmabufTexture = nullptr;
    QMetaObject::Connection deferredVulkanTextureReleaseConnection;
#endif
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
        || !window->renderer()->is_vk()) {
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
    return setBuffer(buffer, nullptr, nullptr, 0);
}

bool WSGTextureProvider::setBuffer(qw_buffer *buffer, wlr_surface *surface)
{
    return setBuffer(buffer, surface, nullptr, 0);
}

bool WSGTextureProvider::setBuffer(qw_buffer *buffer, wlr_surface *surface,
                                   QRhiCommandBuffer *commandBuffer)
{
    return setBuffer(buffer, surface, commandBuffer, 0);
}

bool WSGTextureProvider::setBuffer(qw_buffer *buffer, wlr_surface *surface,
                                   QRhiCommandBuffer *commandBuffer,
                                   quint32 bufferContentSerial)
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
        d->bufferContentSerial = bufferContentSerial;
        if (buffer)
            Q_EMIT textureChanged();
        return true;
    }

    if (d->isVulkanRenderer()) {
#ifdef ENABLE_VULKAN_RENDER
        const bool bufferHasNativeDmabuf = bufferHasDmabuf(buffer);
#else
        const bool bufferHasNativeDmabuf = false;
#endif
        qCDebug(lcWlQtQuickTexture)
            << "Vulkan texture provider setBuffer"
            << "buffer" << pointerAddress(buffer)
            << "sameBuffer" << sameBuffer
            << "allowDirectBufferImport" << allowDirectBufferImport
            << "directBufferImportAllowed" << d->directBufferImportAllowed
            << "bufferHasDmabuf" << bufferHasNativeDmabuf
            << "hasExistingTexture" << d->hasTexture()
            << "currentBuffer" << pointerAddress(d->buffer)
            << "bufferContentSerial" << bufferContentSerial
            << "currentBufferContentSerial" << d->bufferContentSerial
            << "commandBuffer" << pointerAddress(commandBuffer)
            << "size" << bufferSize(buffer);

        if (!buffer) {
            d->cleanTexture();
            Q_EMIT textureChanged();
            return true;
        }

        const bool directImportEligible = allowDirectBufferImport && bufferHasNativeDmabuf;
        bool triedDirectBufferImport = false;
        if (directImportEligible) {
            triedDirectBufferImport = true;
            if (d->replaceBufferWithDirectDmabufTexture(buffer, surface,
                                                        bufferContentSerial)) {
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

        if (!bufferHasNativeDmabuf
            && WRenderHelper::getGraphicsApi() == QSGRendererInterface::Vulkan) {
            if (d->replaceBufferWithNonDmabufUpload(buffer, surface, commandBuffer,
                                                    bufferContentSerial)) {
                Q_EMIT textureChanged();
                return true;
            }

            qCDebug(lcWlQtQuickTexture)
                << "Vulkan renderer direct non-dmabuf upload unavailable,"
                   " falling back to wlroots texture path"
                << "buffer" << pointerAddress(buffer)
                << "sameBuffer" << sameBuffer
                << "size" << bufferSize(buffer)
                << "locks" << bufferLocks(buffer)
                << "commandBuffer" << pointerAddress(commandBuffer);
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
                              surface, commandBuffer, bufferContentSerial)) {
            Q_EMIT textureChanged();
            return true;
        }
        return false;
    }

    d->cleanTexture();
    d->buffer = buffer;
    d->bufferContentSerial = bufferContentSerial;

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
    return setTexture(texture, srcBuffer, nullptr, nullptr, 0);
}

bool WSGTextureProvider::setTexture(qw_texture *texture, qw_buffer *srcBuffer,
                                    wlr_surface *surface)
{
    return setTexture(texture, srcBuffer, surface, nullptr, 0);
}

bool WSGTextureProvider::setTexture(qw_texture *texture, qw_buffer *srcBuffer,
                                    wlr_surface *surface,
                                    QRhiCommandBuffer *commandBuffer)
{
    return setTexture(texture, srcBuffer, surface, commandBuffer, 0);
}

bool WSGTextureProvider::setTexture(qw_texture *texture, qw_buffer *srcBuffer,
                                    wlr_surface *surface,
                                    QRhiCommandBuffer *commandBuffer,
                                    quint32 bufferContentSerial)
{
    W_D(WSGTextureProvider);
    if (d->isVulkanRenderer()) {
#ifdef ENABLE_VULKAN_RENDER
        const bool sourceBufferHasDmabuf = bufferHasDmabuf(srcBuffer);
#else
        const bool sourceBufferHasDmabuf = false;
#endif
        qCDebug(lcWlQtQuickTexture)
            << "Vulkan texture provider setTexture"
            << "texture" << pointerAddress(texture)
            << "sourceBuffer" << pointerAddress(srcBuffer)
            << "directBufferImportAllowed" << d->directBufferImportAllowed
            << "sourceBufferHasDmabuf" << sourceBufferHasDmabuf
            << "hasExistingTexture" << d->hasTexture()
            << "currentBuffer" << pointerAddress(d->buffer)
            << "bufferContentSerial" << bufferContentSerial
            << "currentBufferContentSerial" << d->bufferContentSerial
            << "commandBuffer" << pointerAddress(commandBuffer)
            << "sourceSize" << bufferSize(srcBuffer);

        if (!texture) {
            d->cleanTexture();
            Q_EMIT textureChanged();
            return true;
        }

        const bool allowDirectBufferImport =
            d->directBufferImportAllowed && prefersDirectBufferImport(d->window);
        const bool directImportEligible =
            allowDirectBufferImport && sourceBufferHasDmabuf;
        bool triedDirectBufferImport = false;
        if (srcBuffer && directImportEligible) {
            triedDirectBufferImport = true;
            if (d->replaceBufferWithDirectDmabufTexture(srcBuffer, surface,
                                                        bufferContentSerial)) {
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
                              surface, commandBuffer, bufferContentSerial)) {
            Q_EMIT textureChanged();
            return true;
        }
        return false;
    }

    d->cleanTexture();
    d->texture = texture;
    d->buffer = srcBuffer;
    d->bufferContentSerial = bufferContentSerial;
    d->ownsTexture = false;
    if (texture)
        d->updateRhiTexture(commandBuffer);

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
