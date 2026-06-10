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

#ifndef PN_ZIGBEE_BUTTON_H
#define PN_ZIGBEE_BUTTON_H

#include <pn-node.h>

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnZigbeeButton                                                     */
/*                                                                     */
/*  Filter / reshaping node for a Zigbee wireless button (e.g. SONOFF  */
/*  SNZB-01P) bridged through Zigbee2MQTT.  It is the button-flavoured  */
/*  sibling of #PnZigbeeRemote: structurally near-identical, it exists  */
/*  to give single-button devices their own palette entry, naming and   */
/*  output wording.  Such a button publishes each press on             */
/*  `zigbee2mqtt/<friendly_name>` with a `data.payload` JSON object      */
/*  whose `action` member names the gesture, e.g.                       */
/*                                                                      */
/*    { "action": "single", "battery": 100, "linkquality": 185 }        */
/*                                                                      */
/*  (SNZB-01P emits "single", "double" and "long".)                     */
/*                                                                      */
/*  This node sits downstream of a #PnZigbeeSource (or a plain MQTT     */
/*  Source) and does two things:                                        */
/*                                                                      */
/*    * Filters: only messages that actually carry a non-empty string   */
/*      `data.payload.action` (i.e. a real press) pass; the idle /      */
/*      state-only publishes Z2M interleaves are dropped.  Optional      */
/*      friendly-name / action filters narrow the node to one specific   */
/*      button and/or one specific gesture.                             */
/*                                                                      */
/*    * Reshapes the survivors into the canonical pipnode message       */
/*      contract (PnMessageClass::validate): it stamps                  */
/*      `data.success = true`, `data.value = 1.0`, and rewrites         */
/*      `data.output` into a human-readable line naming the button and   */
/*      the gesture ("Button <name> pressed (action: <action>)").       */
/*      The original envelope and `data.payload` are left intact, so a  */
/*      downstream node can still inspect battery, linkquality, the      */
/*      injected device block, etc.                                     */
/*                                                                      */
/*  The button's name is taken from the device block a #PnZigbeeSource  */
/*  injects (`data.payload.device.friendlyName`) when available, else   */
/*  from the envelope topic's friendly-name tail.                       */
/* ------------------------------------------------------------------ */

#define PN_TYPE_ZIGBEE_BUTTON (pn_zigbee_button_get_type ())

G_DECLARE_FINAL_TYPE (PnZigbeeButton, pn_zigbee_button,
                      PN, ZIGBEE_BUTTON, PnNode)

PnZigbeeButton *pn_zigbee_button_new (void);

G_END_DECLS

#endif /* PN_ZIGBEE_BUTTON_H */
