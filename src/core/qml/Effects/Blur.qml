// Copyright (C) 2024-2026 UnionTech Software Technology Co., Ltd.
// SPDX-License-Identifier: Apache-2.0 OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

import QtQuick
import QtQuick.Effects
import QtQuick.Shapes
import Waylib.Server
import Treeland

Item {
    id: root
    smooth: true

    property real radius: 0
    property bool radiusEnabled: radius > 0
    property int blurMax: Helper.config.blurStrength
    property bool blurEnabled: blurMax > 0 && blurAmount > 0
    property real blurAmount: Helper.config.blurAmount
    property real multiplier: Helper.config.blurMultiplier
    property real brightness: 0.0
    property real contrast: 0.0
    property real saturation: Helper.config.glassSaturation
    property real glassSpecular: Helper.config.glassSpecular
    property real glassTint: 0.0
    property bool glassEnabled: Helper.config.glassEnabled

    z: parent.z ? parent.z - 1 : -1
    anchors.fill: parent

    // On Vulkan the RenderBufferBlitter backdrop-capture path is not supported
    // yet. Skip creating it (and dependent effect components) and fall back to
    // a plain translucent black rectangle.
    Loader {
        anchors.fill: parent
        sourceComponent: WaylibHelper.isVulkanBackend ? vulkanFallback : blitterContent
    }

    Component {
        id: blitterContent
        RenderBufferBlitter {
            id: blitter
            smooth: true
            anchors.fill: parent

            // Dispatch between Liquid Glass and traditional blur via a Loader so only
            // the active branch is instantiated.  Toggling the DConfig key unloads one
            // Component and loads the other.
            Loader {
                anchors.fill: parent
                sourceComponent: root.glassEnabled ? glassComponent : blurComponent
            }

            Component {
                id: glassComponent
                GlassEffect {
                    anchors.fill: parent
                    source: blitter.content
                    radius: root.radius
                    blurEnabled: root.blurEnabled
                    blurMax: root.blurMax
                    blurAmount: root.blurAmount
                    blurMultiplier: root.multiplier
                    bezelWidth: Helper.config.glassBezel
                    thickness: Helper.config.glassThickness
                    ior: 1.33
                    specular: root.glassSpecular
                    tint: root.glassTint
                    brightness: root.brightness
                    contrast: root.contrast
                    saturation: root.saturation
                    contentEdgePull: 0.0
                    contentRampEnd: 0.0
                    refractionMaxTan: 3.3
                    profilePower: Helper.config.glassProfilePower
                    innerShadow: 0.0
                }
            }

            Component {
                id: blurComponent
                Item {
                    anchors.fill: parent

                    MultiEffect {
                        id: blur
                        anchors.fill: parent
                        layer.enabled: root.radiusEnabled
                        smooth: root.radiusEnabled
                        opacity: root.radiusEnabled ? 0 : root.opacity
                        source: blitter.content
                        autoPaddingEnabled: false
                        blurEnabled: root.blurEnabled
                        blur: root.blurAmount
                        blurMax: root.blurMax
                        blurMultiplier: root.multiplier
                        saturation: 0.2
                    }

                    Loader {
                        x: blur.x
                        y: blur.y
                        active: root.radiusEnabled
                        sourceComponent: Shape {
                            anchors.fill: parent
                            preferredRendererType: Shape.CurveRenderer
                            ShapePath {
                                strokeWidth: 0
                                fillItem: blur
                                PathRectangle {
                                    width: blur.width
                                    height: blur.height
                                    radius: root.radius
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    Component {
        id: vulkanFallback
        Rectangle {
            anchors.fill: parent
            radius: root.radius
            color: Qt.rgba(0, 0, 0, 0.15)
        }
    }
}
