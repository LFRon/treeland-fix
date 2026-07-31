// Copyright (C) 2026 UnionTech Software Technology Co., Ltd.
// SPDX-License-Identifier: Apache-2.0 OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

import QtQuick
import Treeland

Item {
    id: control

    property bool offscreen: false
    property alias radius: blur.blurMax
    property alias multiplier: blur.multiplier
    property alias content: blur
    default property alias data: blur.data
    property alias valid: blur.valid

    Blur {
        id: blur
        anchors.fill: parent
        visible: control.valid && !control.offscreen
    }
}
