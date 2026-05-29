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

#ifndef PN_ZIGBEE_RELAY_STATUS_H
#define PN_ZIGBEE_RELAY_STATUS_H

#include <pn-node.h>

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnZigbeeRelayStatus                                                */
/*                                                                     */
/*  Receive-only decoder for a single Zigbee on/off endpoint exposed   */
/*  by a Zigbee2MQTT bridge.  Where #PnZigbeeSwitch fuses the inbound   */
/*  state filter, the on-canvas slider and the outbound `set` builder   */
/*  into one widget, this node is the factored read-half: it carries    */
/*  no latch and no widget, it just listens.                            */
/*                                                                      */
/*  Z2M publishes a device's full state as a JSON object on the bare    */
/*  friendly-name topic (`zigbee2mqtt/<friendly_name>`), e.g.           */
/*                                                                      */
/*    { "state": "ON", "linkquality": 142, "brightness": 254 }          */
/*                                                                      */
/*  This node filters to exactly that topic for its configured device,  */
/*  reads the top-level `state` member, and reshapes the survivors into */
/*  the canonical pipnode message contract: `data.value` (1.0 / 0.0),   */
/*  `data.success = true`, `data.device` (the friendly name) and a      */
/*  human-readable `data.output`, then forwards.  A downstream LED /     */
/*  Debug / Graph therefore reads the same value form it gets from any   */
/*  other node.  Pair it with a #PnZigbeeRelayCommand when you want      */
/*  independent read and write branches instead of the fused            */
/*  #PnZigbeeSwitch.                                                     */
/* ------------------------------------------------------------------ */

#define PN_TYPE_ZIGBEE_RELAY_STATUS (pn_zigbee_relay_status_get_type ())

G_DECLARE_FINAL_TYPE (PnZigbeeRelayStatus, pn_zigbee_relay_status,
                      PN, ZIGBEE_RELAY_STATUS, PnNode)

PnZigbeeRelayStatus *pn_zigbee_relay_status_new (void);

G_END_DECLS

#endif /* PN_ZIGBEE_RELAY_STATUS_H */
