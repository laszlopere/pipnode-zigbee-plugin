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

#ifndef PN_ZIGBEE_WATER_LEAK_H
#define PN_ZIGBEE_WATER_LEAK_H

#include <pn-node.h>

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnZigbeeWaterLeak                                                  */
/*                                                                     */
/*  Edge filter / reshaping node for Zigbee water-leak sensors bridged */
/*  through a Zigbee2MQTT instance.  A leak sensor publishes its state */
/*  on `zigbee2mqtt/<friendly_name>` with a `data.payload` JSON object */
/*  carrying a boolean `water_leak` member (alongside battery,         */
/*  linkquality, tamper, voltage, ...), e.g.                           */
/*                                                                     */
/*    { "water_leak": true,  "battery": 93, "linkquality": 233, ... }  */
/*    { "water_leak": false, "battery": 93, "linkquality": 229, ... }  */
/*                                                                     */
/*  The sensor republishes the full object on every routine update     */
/*  (battery, linkquality), so the bare boolean value is noisy; what    */
/*  matters is the *transition*.  This node sits downstream of a        */
/*  #PnZigbeeSource (or a plain MQTT Source) and:                       */
/*                                                                     */
/*    * Filters on the water_leak EDGE.  It remembers the last          */
/*      water_leak value per device and only forwards a message when    */
/*      that value changes -- a leak begins (false -> true) or clears   */
/*      (true -> false).  The `mode` property selects which edges pass  */
/*      (begin / end / both).  Repeated same-state updates are dropped. */
/*                                                                     */
/*    * Reshapes the survivors into the canonical pipnode message       */
/*      contract: `data.success = true`, `data.value = 1.0` on a leak   */
/*      begin / `0.0` on a leak end, and a human-readable `data.output` */
/*      naming the device and the transition.  The original envelope    */
/*      and `data.payload` are left intact.                             */
/* ------------------------------------------------------------------ */

/**
 * PnZigbeeWaterLeakMode:
 * @PN_ZIGBEE_WATER_LEAK_BEGIN: forward only leak-begin edges
 *                              (water_leak false -> true).
 * @PN_ZIGBEE_WATER_LEAK_END:   forward only leak-end edges
 *                              (water_leak true -> false).
 * @PN_ZIGBEE_WATER_LEAK_BOTH:  forward both edges (the default).
 *
 * Which water_leak transitions the node forwards.  Rendered as a
 * combo box in the node's property dialog (the host builds a combo
 * automatically from any enum-typed property).
 */
typedef enum
{
    PN_ZIGBEE_WATER_LEAK_BEGIN,
    PN_ZIGBEE_WATER_LEAK_END,
    PN_ZIGBEE_WATER_LEAK_BOTH,
} PnZigbeeWaterLeakMode;

#define PN_TYPE_ZIGBEE_WATER_LEAK_MODE (pn_zigbee_water_leak_mode_get_type ())
GType pn_zigbee_water_leak_mode_get_type (void);

#define PN_TYPE_ZIGBEE_WATER_LEAK (pn_zigbee_water_leak_get_type ())

G_DECLARE_FINAL_TYPE (PnZigbeeWaterLeak, pn_zigbee_water_leak,
                      PN, ZIGBEE_WATER_LEAK, PnNode)

PnZigbeeWaterLeak *pn_zigbee_water_leak_new (void);

G_END_DECLS

#endif /* PN_ZIGBEE_WATER_LEAK_H */
