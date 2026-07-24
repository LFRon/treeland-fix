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
    const float thickness = std::max(float(m_thickness), 1.0f);
    const float zBottom = -thickness * 0.5f;
    const float zTop = thickness * 0.5f;
    const float bezel = clampFloat(float(m_bezelWidth), 1.0f, std::min(w, h) * 0.5f - 1.0f);
    const float radius = clampFloat(float(m_radius), 0.0f, std::min(w, h) * 0.5f - 1.0f);
    const float edgeLift = std::min(thickness * 0.08f, bezel * 0.25f);
    const int rings = 16;
    const auto outerOutline = roundedRectOutline(w, h, radius);
    const int pointsPerRing = int(outerOutline.size());

    std::vector<Vertex> vertices;
    std::vector<quint32> indices;
    std::vector<quint32> ringStarts;
    vertices.reserve(pointsPerRing * (rings + 1) + 2);
    ringStarts.reserve(rings + 1);

    const quint32 bottomCenter = quint32(vertices.size());
    vertices.push_back({ QVector3D(0, 0, zBottom), QVector3D(0, 0, -1), QVector2D(0.5f, 0.5f) });

    const quint32 bottomStart = quint32(vertices.size());
    for (const QVector2D &p : outerOutline) {
        vertices.push_back({ QVector3D(p.x(), p.y(), zBottom),
                             QVector3D(0, 0, -1),
                             QVector2D((p.x() / w) + 0.5f, (p.y() / h) + 0.5f) });
    }

    for (int ring = 0; ring < rings; ++ring) {
        const float ringT = float(ring) / float(rings - 1);
        const float inset = bezel * ringT;
        const float ringWidth = std::max(w - inset * 2.0f, 2.0f);
        const float ringHeight = std::max(h - inset * 2.0f, 2.0f);
        const float ringRadius = std::max(radius - inset, 0.0f);
        const float z = zBottom + edgeLift + profileHeight(ringT, float(m_profilePower)) * (thickness - edgeLift);
        const auto outline = roundedRectOutline(ringWidth, ringHeight, ringRadius);
        ringStarts.push_back(quint32(vertices.size()));

        for (const QVector2D &p : outline) {
            QVector2D direction(p.x() / std::max(w * 0.5f, 1.0f), p.y() / std::max(h * 0.5f, 1.0f));
            if (!direction.isNull())
                direction.normalize();
            vertices.push_back({ QVector3D(p.x(), p.y(), z),
                                 normalFromProfile(direction, ringT, float(m_profilePower), thickness, bezel),
                                 QVector2D((p.x() / w) + 0.5f, (p.y() / h) + 0.5f) });
        }
    }

    const quint32 topCenter = quint32(vertices.size());
    vertices.push_back({ QVector3D(0, 0, zTop), QVector3D(0, 0, 1), QVector2D(0.5f, 0.5f) });

    for (int i = 0; i < pointsPerRing; ++i) {
        const int j = (i + 1) % pointsPerRing;
        indices.insert(indices.end(), { bottomCenter, bottomStart + quint32(j), bottomStart + quint32(i) });
    }

    for (int i = 0; i < pointsPerRing; ++i) {
        const int j = (i + 1) % pointsPerRing;
        indices.insert(indices.end(), { bottomStart + quint32(j), bottomStart + quint32(i), ringStarts[0] + quint32(i) });
        indices.insert(indices.end(), { bottomStart + quint32(j), ringStarts[0] + quint32(i), ringStarts[0] + quint32(j) });
    }

    for (int ring = 0; ring < rings - 1; ++ring) {
        const quint32 current = ringStarts[ring];
        const quint32 next = ringStarts[ring + 1];
        for (int i = 0; i < pointsPerRing; ++i) {
            const int j = (i + 1) % pointsPerRing;
            indices.insert(indices.end(), { current + quint32(i), next + quint32(i), next + quint32(j) });
            indices.insert(indices.end(), { current + quint32(i), next + quint32(j), current + quint32(j) });
        }
    }

    const quint32 inner = ringStarts.back();
    for (int i = 0; i < pointsPerRing; ++i) {
        const int j = (i + 1) % pointsPerRing;
        indices.insert(indices.end(), { topCenter, inner + quint32(i), inner + quint32(j) });
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
