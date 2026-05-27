// Copyright (C) 2026 UnionTech Software Technology Co., Ltd.
// SPDX-License-Identifier: Apache-2.0 OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#pragma once

#include <QByteArray>

#include <xcb/xcb.h>

QByteArray readWindowProperty(xcb_connection_t *connection,
                              xcb_window_t win,
                              xcb_atom_t atom,
                              xcb_atom_t type);
