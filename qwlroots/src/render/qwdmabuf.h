// Copyright (C) 2025-2026 UnionTech Software Technology Co., Ltd.
// SPDX-License-Identifier: Apache-2.0 OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#pragma once

#include <qwglobal.h>

extern "C" {
#include <wlr/render/dmabuf.h>
}

#include <linux/dma-buf.h>
#if __has_include(<linux/sync_file.h>)
#include <linux/sync_file.h>
#endif
#include <linux/version.h>
#include <sys/ioctl.h>
#include <sys/utsname.h>
#include <xf86drm.h>

#include <cstdlib>
#include <cstring>

#if !defined(DMA_BUF_IOCTL_IMPORT_SYNC_FILE)
struct dma_buf_import_sync_file {
    __u32 flags;
    __s32 fd;
};

#define DMA_BUF_IOCTL_IMPORT_SYNC_FILE _IOW(DMA_BUF_BASE, 3, struct dma_buf_import_sync_file)
#endif

#if !defined(DMA_BUF_IOCTL_EXPORT_SYNC_FILE)
struct dma_buf_export_sync_file {
    __u32 flags;
    __s32 fd;
};

#define DMA_BUF_IOCTL_EXPORT_SYNC_FILE _IOWR(DMA_BUF_BASE, 2, struct dma_buf_export_sync_file)
#endif

#if !defined(SYNC_IOC_MERGE)
#define SYNC_IOC_MAGIC '>'
struct sync_merge_data {
    char name[32];
    __s32 fd2;
    __s32 fence;
    __u32 flags;
    __u32 pad;
};

#define SYNC_IOC_MERGE _IOWR(SYNC_IOC_MAGIC, 3, struct sync_merge_data)
#endif

QW_BEGIN_NAMESPACE

class QW_CLASS_REINTERPRET_CAST(dmabuf_attributes)
{
public:
    QW_FUNC_MEMBER(dmabuf_attributes, finish, void, void)

    QW_FUNC_STATIC(dmabuf_attributes, copy, bool, wlr_dmabuf_attributes *dst, const wlr_dmabuf_attributes *src)

    QW_ALWAYS_INLINE static bool sync_file_import_export_supported()
    {
        static const bool supported = [] {
            utsname name = {};
            if (uname(&name) != 0)
                return false;
            if (std::strcmp(name.sysname, "Linux") != 0)
                return false;

            for (size_t i = 0; name.release[i] != '\0'; ++i) {
                const char ch = name.release[i];
                if ((ch < '0' || ch > '9') && ch != '.') {
                    name.release[i] = '\0';
                    break;
                }
            }

            char *rel = std::strtok(name.release, ".");
            const int major = rel ? std::atoi(rel) : 0;
            rel = std::strtok(nullptr, ".");
            const int minor = rel ? std::atoi(rel) : 0;
            rel = std::strtok(nullptr, ".");
            const int patch = rel ? std::atoi(rel) : 0;
            return KERNEL_VERSION(major, minor, patch) >= KERNEL_VERSION(5, 20, 0);
        }();
        return supported;
    }

    QW_ALWAYS_INLINE static bool import_sync_file(int dmabufFd, uint32_t flags, int syncFileFd)
    {
        dma_buf_import_sync_file data = {};
        data.flags = flags;
        data.fd = syncFileFd;
        return drmIoctl(dmabufFd, DMA_BUF_IOCTL_IMPORT_SYNC_FILE, &data) == 0;
    }

    QW_ALWAYS_INLINE static int export_sync_file(int dmabufFd, uint32_t flags)
    {
        dma_buf_export_sync_file data = {};
        data.flags = flags;
        data.fd = -1;
        if (drmIoctl(dmabufFd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &data) != 0)
            return -1;
        return data.fd;
    }

    QW_ALWAYS_INLINE static int merge_sync_file(int syncFileFd, int syncFileFd2, const char *name)
    {
        sync_merge_data data = {};
        if (name && name[0] != '\0') {
            std::strncpy(data.name, name, sizeof(data.name) - 1);
        } else {
            std::strncpy(data.name, "waylib-vulkan-acquire", sizeof(data.name) - 1);
        }
        data.fd2 = syncFileFd2;
        data.fence = -1;
        if (::ioctl(syncFileFd, SYNC_IOC_MERGE, &data) != 0)
            return -1;
        return data.fence;
    }
};

QW_END_NAMESPACE
