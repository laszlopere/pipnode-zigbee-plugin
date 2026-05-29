/*
 * Copyright (C) 2026 Laszlo Pere.  All rights reserved.
 *
 * This file is part of the pipnode-zigbee-plugin, a proprietary plugin
 * for Pipnode.  It links the pipnode host library solely through the
 * documented plugin interface (pn_plugin_init and the public pipnode
 * headers) and is therefore distributed under the additional permission
 * granted by Pipnode's LICENSE.PLUGIN-EXCEPTION, which allows a plugin
 * to be released under any license, including this proprietary one.
 *
 * SPDX-License-Identifier: LicenseRef-Proprietary
 */

#ifndef PN_ZIGBEE_REMOTE_H
#define PN_ZIGBEE_REMOTE_H

#include <pn-node.h>

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnZigbeeRemote                                                     */
/*                                                                     */
/*  Filter / reshaping node for Zigbee remote (scene controller, push  */
/*  button, panic button) events arriving from a Zigbee2MQTT bridge.   */
/*  A remote publishes its button presses on                           */
/*  `zigbee2mqtt/<friendly_name>` with a `data.payload` JSON object     */
/*  whose `action` member names the button that was pressed, e.g.       */
/*                                                                      */
/*    { "action": "emergency", "linkquality": 142 }                     */
/*                                                                      */
/*  This node sits downstream of a #PnZigbeeSource (or a plain MQTT     */
/*  Source) and does two things:                                        */
/*                                                                      */
/*    * Filters: only messages that actually carry a non-empty string   */
/*      `data.payload.action` (i.e. a real button press) pass; the      */
/*      idle / state-only publishes Z2M interleaves are dropped.        */
/*                                                                      */
/*    * Reshapes the survivors into the canonical pipnode message       */
/*      contract (PnMessageClass::validate): it stamps                  */
/*      `data.success = true`, `data.value = 1.0`, and rewrites         */
/*      `data.output` into a human-readable line naming the remote and  */
/*      the action ("Remote <name> activated (action: <action>)").      */
/*      The original envelope and `data.payload` are left intact, so a  */
/*      downstream node can still inspect linkquality, the injected     */
/*      device block, etc.                                              */
/*                                                                      */
/*  The remote's name is taken from the device block a #PnZigbeeSource  */
/*  injects (`data.payload.device.friendlyName`) when available, else   */
/*  from the envelope topic's friendly-name tail.                       */
/* ------------------------------------------------------------------ */

#define PN_TYPE_ZIGBEE_REMOTE (pn_zigbee_remote_get_type ())

G_DECLARE_FINAL_TYPE (PnZigbeeRemote, pn_zigbee_remote,
                      PN, ZIGBEE_REMOTE, PnNode)

PnZigbeeRemote *pn_zigbee_remote_new (void);

G_END_DECLS

#endif /* PN_ZIGBEE_REMOTE_H */
