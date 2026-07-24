# test_glass 3D Preview Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an independent Qt Quick 3D preview mode to `examples/test_glass` for inspecting Liquid Glass geometry and material parameters.

**Architecture:** Keep the current 2D `RenderBufferBlitter + GlassEffect` path as the runtime shader preview. Add a separate `View3D` mode backed by a C++ `GlassGeometry : QQuick3DGeometry` mesh generator exposed through the existing `GlassExample` QML module. Reuse the current parameter panel for both modes and add local yaw/pitch controls in the 3D preview.

**Tech Stack:** Qt 6.11, Qt Quick, Qt Quick Controls, Qt Quick 3D, C++ `QQuick3DGeometry`, QML `View3D`, `Model`, `PrincipledMaterial`, existing CMake `qt_add_qml_module`.

## Global Constraints

- Keep the existing 2D `GlassEffect` path unchanged and still available.
- The first 3D version is a geometry/material preview, not pixel-perfect `liquidglass.frag` parity.
- `refractionMaxTan` maximum is 20 in HTML and `test_glass`.
- 3D preview consumes `effectWidth`, `effectHeight`, `effectRadius`, `glassBezelWidth`, `glassThickness`, and `glassProfilePower`.
- 3D material consumes `glassSpecular`, `glassTint`, and `glassInnerShadow`.
- 3D drag changes yaw/pitch; double-click front reset returns yaw/pitch to zero.
- Build failure is acceptable if `Qt6::Quick3D` is unavailable; do not add silent runtime fallback.

---

## File Structure

- Modify `examples/test_glass/CMakeLists.txt`: add `Quick3D`, compile/link `glassgeometry.h/.cpp` into `test_glass` and expose the type in the `GlassExample` QML module.
- Create `examples/test_glass/glassgeometry.h`: declare `GlassGeometry`, QML properties, setters, and `rebuild()`.
- Create `examples/test_glass/glassgeometry.cpp`: generate rounded-rectangle 3D mesh data and upload attributes/indexes through `QQuick3DGeometry`.
- Modify `examples/test_glass/Main.qml`: import `QtQuick3D`, add preview mode state, add 3D scene, add drag/double-click/HUD, bind geometry/material to existing controls.
- Modify `examples/test_glass/Main.qml` and `misc/dconfig/org.deepin.dde.treeland.user.json`: preserve the already-required `refractionMaxTan` max 20 change.

---

### Task 1: Lock refractionMaxTan maximum at 20

**Files:**
- Modify: `examples/test_glass/Main.qml`
- Modify: `misc/dconfig/org.deepin.dde.treeland.user.json`
- Modify in sibling repo: `/home/zccrs/projects/liquid-glass-3d-visualizer/index.html`

**Interfaces:**
- Consumes: existing `glassRefractionMaxTan` property in `Main.qml`; existing `PARAMS.refractionMaxTan` in HTML.
- Produces: slider/config metadata with max 20.

- [ ] **Step 1: Write the failing static check**

Run:
```bash
python3 - <<'PY'
from pathlib import Path
checks = {
    '/home/zccrs/projects/liquid-glass-3d-visualizer/index.html': ['refractionMaxTan: { v: 2.75,min: 0.1, max: 20'],
    '/home/zccrs/projects/treeland-liquid-glass/examples/test_glass/Main.qml': ['Slider { from: 0.5; to: 20; stepSize: 0.05; value: root.glassRefractionMaxTan'],
    '/home/zccrs/projects/treeland-liquid-glass/misc/dconfig/org.deepin.dde.treeland.user.json': ['Range: 0.1-20.0.', '范围：0.1-20.0。'],
}
for path, needles in checks.items():
    text = Path(path).read_text()
    missing = [n for n in needles if n not in text]
    print(path, 'missing:', missing)
    assert not missing, missing
PY
```
Expected before implementation: assertion failure for each missing max-20 string.

- [ ] **Step 2: Update HTML parameter metadata**

In `/home/zccrs/projects/liquid-glass-3d-visualizer/index.html`, set:
```js
refractionMaxTan: { v: 2.75,min: 0.1, max: 20,  step: 0.05, views: 'A+B', desc: '几何斜率 tanθi 的上限。视图 A 看曲线；视图 B 中限制底部贴图位移，避免边缘尖刺。' },
```

- [ ] **Step 3: Update test_glass slider bound**

In `examples/test_glass/Main.qml`, set:
```qml
Slider { from: 0.5; to: 20; stepSize: 0.05; value: root.glassRefractionMaxTan; onMoved: root.glassRefractionMaxTan = value }
```

- [ ] **Step 4: Update DConfig range copy**

In `misc/dconfig/org.deepin.dde.treeland.user.json`, set:
```json
"description": "Cap on the geometric surface slope tangent near the silhouette. Lower values reduce edge distortion. Range: 0.1-20.0.",
"description[zh_CN]": "轮廓附近几何表面斜率正切值的上限。值越小边缘变形越小。范围：0.1-20.0。",
```

- [ ] **Step 5: Run static and build checks**

Run:
```bash
python3 - <<'PY'
from pathlib import Path
checks = {
    '/home/zccrs/projects/liquid-glass-3d-visualizer/index.html': ['refractionMaxTan: { v: 2.75,min: 0.1, max: 20'],
    '/home/zccrs/projects/treeland-liquid-glass/examples/test_glass/Main.qml': ['Slider { from: 0.5; to: 20; stepSize: 0.05; value: root.glassRefractionMaxTan'],
    '/home/zccrs/projects/treeland-liquid-glass/misc/dconfig/org.deepin.dde.treeland.user.json': ['Range: 0.1-20.0.', '范围：0.1-20.0。'],
}
for path, needles in checks.items():
    text = Path(path).read_text()
    missing = [n for n in needles if n not in text]
    print(path, 'missing:', missing)
    assert not missing, missing
PY
cmake --build --preset default --target test_glass
```
Expected: no missing strings; `test_glass` target builds.

- [ ] **Step 6: Commit max bound change**

Run:
```bash
git add examples/test_glass/Main.qml misc/dconfig/org.deepin.dde.treeland.user.json
git commit -m "fix(test_glass): raise refraction max tangent limit"
```
Commit the HTML sibling repo separately:
```bash
cd /home/zccrs/projects/liquid-glass-3d-visualizer
git add index.html
git commit -m "fix: raise refraction max tangent limit"
```

---

### Task 2: Add GlassGeometry QML type and build wiring

**Files:**
- Create: `examples/test_glass/glassgeometry.h`
- Create: `examples/test_glass/glassgeometry.cpp`
- Modify: `examples/test_glass/CMakeLists.txt`

**Interfaces:**
- Consumes: QML module `GlassExample` created by `qt_add_qml_module(test_glass ...)`.
- Produces: QML type `GlassGeometry` with properties:
  - `real width`
  - `real height`
  - `real radius`
  - `real bezelWidth`
  - `real thickness`
  - `real profilePower`

- [ ] **Step 1: Write a failing source exposure check**

Run:
```bash
python3 - <<'PY'
from pathlib import Path
root = Path('/home/zccrs/projects/treeland-liquid-glass/examples/test_glass')
required = {
    root / 'glassgeometry.h': ['class GlassGeometry', 'QML_ELEMENT', 'Q_PROPERTY(qreal width'],
    root / 'glassgeometry.cpp': ['#include "glassgeometry.h"', 'QQuick3DGeometry::Attribute::PositionSemantic'],
    root / 'CMakeLists.txt': ['Quick3D', 'glassgeometry.cpp', 'glassgeometry.h'],
}
for path, needles in required.items():
    text = path.read_text() if path.exists() else ''
    missing = [n for n in needles if n not in text]
    print(path.name, 'missing:', missing)
    assert not missing, missing
PY
```
Expected before implementation: assertion failure because files/wiring do not exist.

- [ ] **Step 2: Add `glassgeometry.h`**

Create `examples/test_glass/glassgeometry.h` with:
```cpp
// Copyright (C) 2026 UnionTech Software Technology Co., Ltd.
// SPDX-License-Identifier: Apache-2.0 OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#pragma once

#include <QQuick3DGeometry>
#include <QQmlEngine>

class GlassGeometry : public QQuick3DGeometry
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(qreal width READ width WRITE setWidth NOTIFY widthChanged)
    Q_PROPERTY(qreal height READ height WRITE setHeight NOTIFY heightChanged)
    Q_PROPERTY(qreal radius READ radius WRITE setRadius NOTIFY radiusChanged)
    Q_PROPERTY(qreal bezelWidth READ bezelWidth WRITE setBezelWidth NOTIFY bezelWidthChanged)
    Q_PROPERTY(qreal thickness READ thickness WRITE setThickness NOTIFY thicknessChanged)
    Q_PROPERTY(qreal profilePower READ profilePower WRITE setProfilePower NOTIFY profilePowerChanged)

public:
    explicit GlassGeometry(QObject *parent = nullptr);

    qreal width() const { return m_width; }
    void setWidth(qreal value);
    qreal height() const { return m_height; }
    void setHeight(qreal value);
    qreal radius() const { return m_radius; }
    void setRadius(qreal value);
    qreal bezelWidth() const { return m_bezelWidth; }
    void setBezelWidth(qreal value);
    qreal thickness() const { return m_thickness; }
    void setThickness(qreal value);
    qreal profilePower() const { return m_profilePower; }
    void setProfilePower(qreal value);

Q_SIGNALS:
    void widthChanged();
    void heightChanged();
    void radiusChanged();
    void bezelWidthChanged();
    void thicknessChanged();
    void profilePowerChanged();

private:
    void rebuild();

    qreal m_width = 300.0;
    qreal m_height = 200.0;
    qreal m_radius = 60.0;
    qreal m_bezelWidth = 60.0;
    qreal m_thickness = 50.0;
    qreal m_profilePower = 4.0;
};
```

- [ ] **Step 3: Add minimal `glassgeometry.cpp` that builds a box-like rounded glass mesh**

Create `examples/test_glass/glassgeometry.cpp` with a rebuild implementation that emits position, normal, and UV attributes. The first GREEN shape can be simple: top and bottom rings with triangle sides; Task 3 improves the profile.

```cpp
// Copyright (C) 2026 UnionTech Software Technology Co., Ltd.
// SPDX-License-Identifier: Apache-2.0 OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "glassgeometry.h"

#include <QByteArray>
#include <QVector2D>
#include <QVector3D>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace {
struct Vertex {
    QVector3D position;
    QVector3D normal;
    QVector2D uv;
};

static float clampFloat(float value, float lo, float hi)
{
    return std::max(lo, std::min(value, hi));
}

static std::vector<QVector2D> roundedRectOutline(float width, float height, float radius)
{
    const float hx = std::max(width * 0.5f, 1.0f);
    const float hy = std::max(height * 0.5f, 1.0f);
    const float r = clampFloat(radius, 0.0f, std::min(hx, hy) - 1.0f);
    const int cornerSegments = 12;
    std::vector<QVector2D> points;
    points.reserve(cornerSegments * 4);

    const QVector2D centers[] = {
        QVector2D(hx - r, hy - r),
        QVector2D(-hx + r, hy - r),
        QVector2D(-hx + r, -hy + r),
        QVector2D(hx - r, -hy + r),
    };
    const float starts[] = { 0.0f, 90.0f, 180.0f, 270.0f };

    for (int corner = 0; corner < 4; ++corner) {
        for (int i = 0; i <= cornerSegments; ++i) {
            const float degrees = starts[corner] + i * 90.0f / cornerSegments;
            const float radians = degrees * float(M_PI) / 180.0f;
            points.emplace_back(centers[corner].x() + r * std::cos(radians),
                                centers[corner].y() + r * std::sin(radians));
        }
    }

    return points;
}
} // namespace

GlassGeometry::GlassGeometry(QObject *parent)
    : QQuick3DGeometry(parent)
{
    rebuild();
}

void GlassGeometry::setWidth(qreal value) { if (qFuzzyCompare(m_width, value)) return; m_width = value; Q_EMIT widthChanged(); rebuild(); }
void GlassGeometry::setHeight(qreal value) { if (qFuzzyCompare(m_height, value)) return; m_height = value; Q_EMIT heightChanged(); rebuild(); }
void GlassGeometry::setRadius(qreal value) { if (qFuzzyCompare(m_radius, value)) return; m_radius = value; Q_EMIT radiusChanged(); rebuild(); }
void GlassGeometry::setBezelWidth(qreal value) { if (qFuzzyCompare(m_bezelWidth, value)) return; m_bezelWidth = value; Q_EMIT bezelWidthChanged(); rebuild(); }
void GlassGeometry::setThickness(qreal value) { if (qFuzzyCompare(m_thickness, value)) return; m_thickness = value; Q_EMIT thicknessChanged(); rebuild(); }
void GlassGeometry::setProfilePower(qreal value) { if (qFuzzyCompare(m_profilePower, value)) return; m_profilePower = value; Q_EMIT profilePowerChanged(); rebuild(); }

void GlassGeometry::rebuild()
{
    const float w = std::max(float(m_width), 1.0f);
    const float h = std::max(float(m_height), 1.0f);
    const float zTop = std::max(float(m_thickness), 1.0f) * 0.5f;
    const float zBottom = -zTop;
    const auto outline = roundedRectOutline(w, h, float(m_radius));

    std::vector<Vertex> vertices;
    std::vector<quint32> indices;
    vertices.reserve(outline.size() * 2 + 2);

    const quint32 topCenter = quint32(vertices.size());
    vertices.push_back({ QVector3D(0, 0, zTop), QVector3D(0, 0, 1), QVector2D(0.5f, 0.5f) });
    const quint32 bottomCenter = quint32(vertices.size());
    vertices.push_back({ QVector3D(0, 0, zBottom), QVector3D(0, 0, -1), QVector2D(0.5f, 0.5f) });

    const quint32 topStart = quint32(vertices.size());
    for (const QVector2D &p : outline)
        vertices.push_back({ QVector3D(p.x(), p.y(), zTop), QVector3D(0, 0, 1), QVector2D((p.x() / w) + 0.5f, (p.y() / h) + 0.5f) });

    const quint32 bottomStart = quint32(vertices.size());
    for (const QVector2D &p : outline)
        vertices.push_back({ QVector3D(p.x(), p.y(), zBottom), QVector3D(0, 0, -1), QVector2D((p.x() / w) + 0.5f, (p.y() / h) + 0.5f) });

    const int n = int(outline.size());
    for (int i = 0; i < n; ++i) {
        const int j = (i + 1) % n;
        indices.insert(indices.end(), { topCenter, topStart + quint32(i), topStart + quint32(j) });
        indices.insert(indices.end(), { bottomCenter, bottomStart + quint32(j), bottomStart + quint32(i) });
        indices.insert(indices.end(), { topStart + quint32(i), bottomStart + quint32(i), bottomStart + quint32(j), topStart + quint32(i), bottomStart + quint32(j), topStart + quint32(j) });
    }

    QByteArray vertexData(reinterpret_cast<const char *>(vertices.data()), qsizetype(vertices.size() * sizeof(Vertex)));
    QByteArray indexData(reinterpret_cast<const char *>(indices.data()), qsizetype(indices.size() * sizeof(quint32)));

    clear();
    setStride(sizeof(Vertex));
    addAttribute(QQuick3DGeometry::Attribute::PositionSemantic, offsetof(Vertex, position), QQuick3DGeometry::Attribute::F32Type);
    addAttribute(QQuick3DGeometry::Attribute::NormalSemantic, offsetof(Vertex, normal), QQuick3DGeometry::Attribute::F32Type);
    addAttribute(QQuick3DGeometry::Attribute::TexCoord0Semantic, offsetof(Vertex, uv), QQuick3DGeometry::Attribute::F32Type);
    addAttribute(QQuick3DGeometry::Attribute::IndexSemantic, 0, QQuick3DGeometry::Attribute::U32Type);
    setVertexData(vertexData);
    setIndexData(indexData);
    setPrimitiveType(QQuick3DGeometry::PrimitiveType::Triangles);
    setBounds(QVector3D(-w * 0.5f, -h * 0.5f, zBottom), QVector3D(w * 0.5f, h * 0.5f, zTop));
}
```

- [ ] **Step 4: Wire CMake**

In `examples/test_glass/CMakeLists.txt`, change:
```cmake
find_package(Qt6 COMPONENTS Quick QuickControls2 Quick3D ShaderTools REQUIRED)
```

Add sources to the `test_glass` executable:
```cmake
add_executable(test_glass
    main.cpp
    glassgeometry.cpp
    glassgeometry.h
)
```

Link `Qt6::Quick3D`:
```cmake
target_link_libraries(test_glass
    PRIVATE
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Quick3D
    treeland_stub
    PkgConfig::PIXMAN
    PkgConfig::WAYLAND
)
```

- [ ] **Step 5: Run exposure check and build**

Run:
```bash
python3 - <<'PY'
from pathlib import Path
root = Path('/home/zccrs/projects/treeland-liquid-glass/examples/test_glass')
required = {
    root / 'glassgeometry.h': ['class GlassGeometry', 'QML_ELEMENT', 'Q_PROPERTY(qreal width'],
    root / 'glassgeometry.cpp': ['#include "glassgeometry.h"', 'QQuick3DGeometry::Attribute::PositionSemantic'],
    root / 'CMakeLists.txt': ['Quick3D', 'glassgeometry.cpp', 'glassgeometry.h'],
}
for path, needles in required.items():
    text = path.read_text() if path.exists() else ''
    missing = [n for n in needles if n not in text]
    print(path.name, 'missing:', missing)
    assert not missing, missing
PY
cmake --build --preset default --target test_glass
```
Expected: static check passes; target builds. If configure fails with missing Quick3D, install/add Qt Quick 3D package rather than adding fallback code.

- [ ] **Step 6: Commit buildable geometry type**

Run:
```bash
git add examples/test_glass/CMakeLists.txt examples/test_glass/glassgeometry.h examples/test_glass/glassgeometry.cpp
git commit -m "feat(test_glass): add Quick 3D glass geometry"
```

---

### Task 3: Shape GlassGeometry into a beveled Liquid Glass mesh

**Files:**
- Modify: `examples/test_glass/glassgeometry.cpp`

**Interfaces:**
- Consumes: Task 2 `GlassGeometry` properties.
- Produces: same QML API, but mesh now has a profile-based top surface and side wall.

- [ ] **Step 1: Add a static geometry-quality check**

Run:
```bash
python3 - <<'PY'
from pathlib import Path
text = Path('/home/zccrs/projects/treeland-liquid-glass/examples/test_glass/glassgeometry.cpp').read_text()
required = [
    'profileHeight(float t, float power)',
    'normalFromProfile',
    'rings = 16',
    'ringT',
    'm_profilePower',
]
missing = [needle for needle in required if needle not in text]
print('missing:', missing)
assert not missing, missing
PY
```
Expected before implementation: assertion failure.

- [ ] **Step 2: Add profile helpers**

Add helpers near the top of `glassgeometry.cpp`:
```cpp
static float profileHeight(float t, float power)
{
    const float p = std::max(power, 1.0f);
    const float s = clampFloat(t, 0.0f, 1.0f);
    const float inside = 1.0f - std::pow(1.0f - s, p);
    return std::pow(std::max(inside, 0.0f), 1.0f / p);
}

static QVector3D normalFromProfile(const QVector2D &direction, float t, float power, float thickness, float bezel)
{
    const float eps = 0.01f;
    const float h0 = profileHeight(clampFloat(t - eps, 0.0f, 1.0f), power) * thickness;
    const float h1 = profileHeight(clampFloat(t + eps, 0.0f, 1.0f), power) * thickness;
    const float slope = (h1 - h0) / std::max(2.0f * eps * bezel, 1.0f);
    return QVector3D(-direction.x() * slope, -direction.y() * slope, 1.0f).normalized();
}
```

- [ ] **Step 3: Replace flat top with rings**

Inside `GlassGeometry::rebuild()`, generate `rings = 16` concentric rounded-rectangle outlines from outer footprint toward inner flat top. For each `ring`, compute:
```cpp
const int rings = 16;
const float ringT = float(ring) / float(rings - 1);
const float inset = std::min(bezel, std::min(w, h) * 0.5f - 1.0f) * ringT;
const float z = zBottom + profileHeight(ringT, float(m_profilePower)) * std::max(float(m_thickness), 1.0f);
```
Use `normalFromProfile()` for top-ring normals and connect adjacent rings with triangles. Keep side-wall triangles from outer top ring to bottom ring.

- [ ] **Step 4: Run geometry-quality check and build**

Run:
```bash
python3 - <<'PY'
from pathlib import Path
text = Path('/home/zccrs/projects/treeland-liquid-glass/examples/test_glass/glassgeometry.cpp').read_text()
required = ['profileHeight(float t, float power)', 'normalFromProfile', 'rings = 16', 'ringT', 'm_profilePower']
missing = [needle for needle in required if needle not in text]
print('missing:', missing)
assert not missing, missing
PY
cmake --build --preset default --target test_glass
```
Expected: static check passes; target builds.

- [ ] **Step 5: Commit beveled mesh**

Run:
```bash
git add examples/test_glass/glassgeometry.cpp
git commit -m "feat(test_glass): shape 3D glass bevel profile"
```

---

### Task 4: Add the QML 3D Preview mode and interactions

**Files:**
- Modify: `examples/test_glass/Main.qml`

**Interfaces:**
- Consumes: `GlassGeometry` QML type from Task 2.
- Produces: QML properties:
  - `property bool preview3DEnabled`
  - `property real previewYaw`
  - `property real previewPitch`
  - function `resetPreview3D()`

- [ ] **Step 1: Write a failing QML static check**

Run:
```bash
python3 - <<'PY'
from pathlib import Path
text = Path('/home/zccrs/projects/treeland-liquid-glass/examples/test_glass/Main.qml').read_text()
required = [
    'import QtQuick3D',
    'property bool preview3DEnabled',
    'function resetPreview3D()',
    'View3D {',
    'GlassGeometry {',
    'MouseArea {',
    'onDoubleClicked: root.resetPreview3D()',
    'yaw',
    'pitch',
]
missing = [needle for needle in required if needle not in text]
print('missing:', missing)
assert not missing, missing
PY
```
Expected before implementation: assertion failure.

- [ ] **Step 2: Add QML import and root state**

At the top of `Main.qml`, add:
```qml
import QtQuick3D
```

Near existing root properties, add:
```qml
property bool preview3DEnabled: false
property real previewYaw: 0
property real previewPitch: 0

function resetPreview3D() {
    previewYaw = 0
    previewPitch = 0
}
```

- [ ] **Step 3: Add a preview toggle button**

Near the current top-left control buttons, add:
```qml
Button {
    text: root.preview3DEnabled ? "2D Effect" : "3D Preview"
    onClicked: root.preview3DEnabled = !root.preview3DEnabled
}
```

- [ ] **Step 4: Gate 2D drag behavior**

Change the existing `MouseArea` inside `effectPanel` so it only moves the panel in 2D mode:
```qml
MouseArea {
    anchors.fill: parent
    enabled: !root.preview3DEnabled
    drag.target: effectPanel
    drag.axis: Drag.XAndYAxis
    drag.minimumX: 0
    drag.maximumX: parent.parent.width - effectPanel.width
    drag.minimumY: 0
    drag.maximumY: parent.parent.height - effectPanel.height
}
```

- [ ] **Step 5: Add the 3D component**

Add a `Component` beside `globalGlassComponent`:
```qml
Component {
    id: glass3DComponent

    Item {
        anchors.fill: parent

        View3D {
            id: view3d
            anchors.fill: parent
            environment: SceneEnvironment {
                clearColor: "transparent"
                backgroundMode: SceneEnvironment.Transparent
            }

            PerspectiveCamera {
                id: camera3d
                z: Math.max(root.effectWidth, root.effectHeight) * 2.0
            }

            DirectionalLight {
                eulerRotation.x: -35
                eulerRotation.y: 35
                brightness: 2.0
            }

            Model {
                id: glassModel3d
                eulerRotation.x: root.previewPitch
                eulerRotation.y: root.previewYaw
                geometry: GlassGeometry {
                    width: root.effectWidth
                    height: root.effectHeight
                    radius: root.effectRadius
                    bezelWidth: root.glassBezelWidth
                    thickness: root.glassThickness
                    profilePower: root.glassProfilePower
                }
                materials: PrincipledMaterial {
                    baseColor: Qt.rgba(0.75, 0.9, 1.0, Math.max(0.18, 0.55 - root.glassTint))
                    alphaMode: PrincipledMaterial.Blend
                    opacity: Math.max(0.18, 0.62 - root.glassTint)
                    specularAmount: Math.max(0.0, Math.min(1.0, root.glassSpecular))
                    roughness: 0.12
                }
            }
        }

        Rectangle {
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.margins: 8
            radius: 4
            color: "#66000000"
            border.color: "#55ffffff"
            width: angleLabel.implicitWidth + 16
            height: angleLabel.implicitHeight + 10

            Text {
                id: angleLabel
                anchors.centerIn: parent
                color: "white"
                font.pixelSize: 12
                text: "yaw " + root.previewYaw.toFixed(1) + "°  pitch " + root.previewPitch.toFixed(1) + "°"
            }
        }

        MouseArea {
            anchors.fill: parent
            property real lastX: 0
            property real lastY: 0
            onPressed: mouse => { lastX = mouse.x; lastY = mouse.y }
            onPositionChanged: mouse => {
                if (!pressed) return
                root.previewYaw += (mouse.x - lastX) * 0.45
                root.previewPitch = Math.max(-75, Math.min(75, root.previewPitch + (mouse.y - lastY) * 0.45))
                lastX = mouse.x
                lastY = mouse.y
            }
            onDoubleClicked: root.resetPreview3D()
        }
    }
}
```

- [ ] **Step 6: Switch loader source**

Change the `Loader` inside `effectPanel` to:
```qml
Loader {
    anchors.fill: parent
    sourceComponent: root.preview3DEnabled ? glass3DComponent : (root.glassMode ? globalGlassComponent : globalBlurComponent)
}
```

- [ ] **Step 7: Run QML static check and build**

Run:
```bash
python3 - <<'PY'
from pathlib import Path
text = Path('/home/zccrs/projects/treeland-liquid-glass/examples/test_glass/Main.qml').read_text()
required = ['import QtQuick3D', 'property bool preview3DEnabled', 'function resetPreview3D()', 'View3D {', 'GlassGeometry {', 'MouseArea {', 'onDoubleClicked: root.resetPreview3D()', 'yaw', 'pitch']
missing = [needle for needle in required if needle not in text]
print('missing:', missing)
assert not missing, missing
PY
cmake --build --preset default --target test_glass
```
Expected: static check passes; target builds.

- [ ] **Step 8: Commit QML 3D preview**

Run:
```bash
git add examples/test_glass/Main.qml
git commit -m "feat(test_glass): add 3D glass preview mode"
```

---

### Task 5: Smoke-test runtime behavior and finalize

**Files:**
- Modify if needed: `examples/test_glass/Main.qml`
- Modify if needed: `examples/test_glass/glassgeometry.cpp`

**Interfaces:**
- Consumes: Tasks 1-4.
- Produces: working demo and clean focused git status.

- [ ] **Step 1: Build from current tree**

Run:
```bash
cmake --build --preset default --target test_glass
```
Expected: target builds without C++ or QML cache errors.

- [ ] **Step 2: Launch smoke test**

Run:
```bash
build/examples/test_glass/test_glass --wallpaper examples/test_glass/assets/default-glass-background.jpg
```
Expected: the demo opens without QML import/type errors. If the process logs a Qt Quick 3D import error, fix CMake/module linkage instead of adding a fallback path.

- [ ] **Step 3: Manual interaction checks**

In the demo window:
- Click `3D Preview`; expected: a rounded glass model appears inside the effect panel.
- Drag inside the 3D preview; expected: yaw/pitch HUD values change and the model rotates.
- Double-click the 3D preview; expected: HUD returns to `yaw 0.0° pitch 0.0°` and the model faces front.
- Change `radius`, `bezel`, `thickness`, and `profilePower`; expected: the 3D mesh updates immediately.
- Click `2D Effect`; expected: the existing 2D `GlassEffect` path returns and panel dragging works again.

- [ ] **Step 4: Run focused status check**

Run:
```bash
git diff --stat -- examples/test_glass/CMakeLists.txt examples/test_glass/Main.qml examples/test_glass/glassgeometry.h examples/test_glass/glassgeometry.cpp misc/dconfig/org.deepin.dde.treeland.user.json
```
Expected: only the intended files are present.

- [ ] **Step 5: Final commit if smoke fixes were needed**

If Task 5 changed files, run:
```bash
git add examples/test_glass/CMakeLists.txt examples/test_glass/Main.qml examples/test_glass/glassgeometry.h examples/test_glass/glassgeometry.cpp misc/dconfig/org.deepin.dde.treeland.user.json
git commit -m "fix(test_glass): polish 3D preview smoke issues"
```

- [ ] **Step 6: Push commits**

Run:
```bash
git push https://github.com/zccrs/treeland.git HEAD:feature/liquid-glass-webgl
cd /home/zccrs/projects/liquid-glass-3d-visualizer
git push
```
Expected: both pushes complete successfully.
