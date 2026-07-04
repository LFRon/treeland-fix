// Copyright (C) 2026 UnionTech Software Technology Co., Ltd.
// SPDX-License-Identifier: Apache-2.0 OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "processenvironment.h"

namespace Treeland::ProcessEnvironment {

void sanitizeClientEnvironment(QProcessEnvironment &environment)
{
    environment.remove(QStringLiteral("QT_RHI_VK_ASYNC_OFFSCREEN"));
    environment.remove(QStringLiteral("WAYLIB_QT_RHI_VK_ASYNC_OFFSCREEN_DEFAULTED"));
}

QProcessEnvironment clientSystemEnvironment()
{
    auto environment = QProcessEnvironment::systemEnvironment();
    sanitizeClientEnvironment(environment);
    return environment;
}

}
