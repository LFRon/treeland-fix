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

GlassGeometry::GlassGeometry(QQuick3DObject *parent)
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
