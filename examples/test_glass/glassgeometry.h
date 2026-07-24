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
    explicit GlassGeometry(QQuick3DObject *parent = nullptr);

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
