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

#ifndef PN_ZIGBEE_RELAY_COMMAND_H
#define PN_ZIGBEE_RELAY_COMMAND_H

#include <pn-node.h>

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnZigbeeRelayCommand                                               */
/*                                                                     */
/*  Send-only command builder for a single Zigbee on/off endpoint      */
/*  exposed by a Zigbee2MQTT bridge.  Where #PnZigbeeSwitch fuses the   */
/*  inbound state filter, the on-canvas slider and the outbound `set`   */
/*  builder into one widget, this node is the factored write-half: it   */
/*  carries no latch and no widget, it just commands.                   */
/*                                                                      */
/*  It consumes the canonical pipnode value message (any node that      */
/*  emits a numeric `data.value`: an Inject, a Switch, a threshold      */
/*  filter, ...), treats value > 0.5 as ON and the rest as OFF, then    */
/*  rewrites the envelope so a downstream MQTT Sink publishes straight  */
/*  to the device's command topic:                                      */
/*                                                                      */
/*    topic   = zigbee2mqtt/<friendly_name>/set                         */
/*    payload = { "state": "ON" }   (a JSON object, not a bare string)  */
/*                                                                      */
/*  The original numeric value rides along on `data.value` and the      */
/*  standard `data.success` / `data.device` / `data.output` mirror is   */
/*  stamped too, so a parallel observer wired off the same point reads  */
/*  a value form it recognises.  Pair it with a #PnZigbeeRelayStatus    */
/*  when you want independent read and write branches instead of the    */
/*  fused #PnZigbeeSwitch.                                              */
/* ------------------------------------------------------------------ */

#define PN_TYPE_ZIGBEE_RELAY_COMMAND (pn_zigbee_relay_command_get_type ())

G_DECLARE_FINAL_TYPE (PnZigbeeRelayCommand, pn_zigbee_relay_command,
                      PN, ZIGBEE_RELAY_COMMAND, PnNode)

PnZigbeeRelayCommand *pn_zigbee_relay_command_new (void);

G_END_DECLS

#endif /* PN_ZIGBEE_RELAY_COMMAND_H */
