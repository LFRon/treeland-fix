// Copyright (C) 2022 JiDe Zhang <zccrs@live.com>.
// SPDX-License-Identifier: Apache-2.0 OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#pragma once

#include <qwglobal.h>

extern "C" {
#include <wlr/render/wlr_texture.h>
#if WLR_HAVE_VULKAN_RENDERER
#include <wlr/render/vulkan.h>
#endif
}

QW_BEGIN_NAMESPACE

class QW_CLASS_REINTERPRET_CAST(texture)
{
public:
    QW_FUNC_STATIC(texture, from_pixels, qw_texture *, wlr_renderer *renderer, uint32_t fmt, uint32_t stride, uint32_t width, uint32_t height, const void *data)
    QW_FUNC_STATIC(texture, from_dmabuf, qw_texture *, wlr_renderer *renderer, wlr_dmabuf_attributes *attribs)
    QW_FUNC_STATIC(texture, from_buffer, qw_texture *, wlr_renderer *renderer, wlr_buffer *buffer)
#if WLR_HAVE_VULKAN_RENDERER
    QW_FUNC_MEMBER(vk_texture, get_image_attribs, void, wlr_vk_image_attribs *attribs)
    QW_FUNC_MEMBER(vk_texture, has_alpha, bool)
#endif

    QW_FUNC_MEMBER(texture, update_from_buffer, bool, wlr_buffer *buffer, const pixman_region32_t *damage)

private:
    friend class qw_reinterpret_cast;
    QW_FUNC_MEMBER(texture, destroy, void)
};

QW_END_NAMESPACE
