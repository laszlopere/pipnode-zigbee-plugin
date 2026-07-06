/*
 * Copyright (C) 2026 Laszlo Pere
 *
 * This file is part of pipnode-zigbee-plugin, a plugin for Pipnode.  It
 * is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License version 3, or (at your option)
 * any later version, as published by the Free Software Foundation.
 *
 * The plugin links the Pipnode host solely through the documented plugin
 * interface and is covered by Pipnode's LICENSE.PLUGIN-EXCEPTION; this
 * file's own license is the GPL, version 3 or later.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; see the GNU General Public License for more
 * details.  You should have received a copy of the license in the file
 * COPYING.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef PN_ZIGBEE_NODE_GUI_H
#define PN_ZIGBEE_NODE_GUI_H

#include <glib.h>

G_BEGIN_DECLS

/**
 * zb_zigbee_nodes_gui_install:
 *
 * Install the gui-tier friendly-name editor -- an editable entry+dropdown
 * combo backed by the background name registry (pn-zigbee-name-registry.h)
 * -- onto the already-registered Zigbee node classes that carry a
 * `friendly-name` property.  Follows the host's #PnInject icon-picker
 * pattern (pn_inject_gui_install()): the headless logic module runs without
 * GTK, and this GUI companion attaches the #PnNodeClass.build_property_editor
 * vfunc afterwards.  Meant to be called once from pn_plugin_gui_init().
 */
void zb_zigbee_nodes_gui_install (void);

G_END_DECLS

#endif /* PN_ZIGBEE_NODE_GUI_H */
