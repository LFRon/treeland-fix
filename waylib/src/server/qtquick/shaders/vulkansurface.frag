// Copyright (C) 2026 UnionTech Software Technology Co., Ltd.
// SPDX-License-Identifier: Apache-2.0 OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#version 440

layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    vec4 qt_OpacityAndFlags;
};

layout(binding = 1) uniform sampler2D sourceTexture;

void main()
{
    vec4 color = texture(sourceTexture, vTexCoord);
    if (qt_OpacityAndFlags.y > 0.5)
        color.a = 1.0;
    fragColor = color * qt_OpacityAndFlags.x;
}
